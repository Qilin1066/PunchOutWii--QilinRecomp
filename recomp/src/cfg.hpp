// Function discovery and recursive-descent control-flow recovery for
// Broadway code compiled by CodeWarrior.
//
// What makes this simpler than on x86: every instruction is one aligned word,
// CodeWarrior keeps switch tables and constants in data sections, so text
// sections hold code and zero padding only, and a function's code is one
// contiguous range. What it has to handle: functions reached only through
// pointers (vtables, callbacks), tail calls, switch tables, CodeWarrior's
// register save/restore routines (one routine, many entry points, each
// falling through into the next) and the `bl` to the next instruction that
// hand-written SDK code uses to read the program counter.
//
// Pipeline:
//   1. Decode every word of the text sections. Seed function starts with the
//      entry point and every direct call target (strong evidence).
//   2. Explore each function by recursive descent, bounded by the next known
//      start, resolving CodeWarrior jump tables (proven by their bound check).
//      A branch past the bound to a clean boundary is a tail call to a function
//      nothing calls directly: it becomes a start, and so do `lis`/`addi`
//      address pairs in code that point at a clean boundary. Repeat until
//      nothing changes.
//   3. Weaker evidence, in rounds, each followed by step 2 again: pointers into
//      text stored in data sections (outside jump tables), then code in the
//      remaining gaps that starts at a clean boundary.
//   4. Validation: a function that reaches an invalid word or leaves its text
//      section is dropped if only weak evidence made it, kept as a suspect if
//      it's an entry point or call target.
//
// A clean boundary is a word that decodes, and either starts its section or
// follows code that can't fall through (blr, bctr, b, rfi, an unconditional
// trap) or zero padding.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "decoder.hpp"
#include "dol.hpp"

namespace qilin {

struct JumpTable {
    uint32_t at = 0;                // the bctr
    uint32_t table = 0;             // table address (in a data section)
    std::vector<uint32_t> entries;  // table contents in index order
    std::vector<uint32_t> targets;  // unique, sorted
    bool proven = false;            // size from a cmplwi/bgt bound on the index; else read while plausible
    uint32_t end() const { return table + 4 * static_cast<uint32_t>(entries.size()); }
};

// How a function was found, strongest evidence first.
enum class Origin : uint8_t { Entry, Call, TailCall, ImmPtr, DataPtr, Gap };
const char* origin_name(Origin o);
inline bool strong(Origin o) { return o == Origin::Entry || o == Origin::Call; }

struct Function {
    uint32_t start = 0;
    uint32_t bound = 0;                // next known start (or section end) when explored
    uint32_t end = 0;                  // one past the highest reachable instruction
    Origin origin = Origin::Call;
    std::vector<uint32_t> insns;       // reachable instruction addresses, sorted
    std::set<uint32_t> leaders;        // basic-block starts
    std::set<uint32_t> calls;          // direct call targets (bl)
    std::set<uint32_t> tails;          // tail-call targets (b, or bc, to another function)
    uint32_t falls_into = 0;           // next function that execution runs into, if any
    std::vector<uint32_t> icalls;      // bctrl/blrl: indirect calls
    std::vector<uint32_t> itails;      // bctr not resolved as a jump table: indirect tail calls
    std::vector<JumpTable> jtabs;
    std::vector<uint32_t> bad;         // reached an invalid word, or left the text section
    std::vector<uint32_t> escapes;     // branch targets outside the function that aren't clean boundaries
    std::vector<uint32_t> imm_ptrs;    // lis/addi pairs pointing into text
    uint32_t bytes() const { return 4 * static_cast<uint32_t>(insns.size()); }
};

struct Suspect {
    uint32_t start = 0;
    std::string why;
};

// Word-level accounting of each text section. Every word is
//   code     reached by some function;
//   padding  not reached, and zero or a nop (CodeWarrior's alignment);
//   dead     not reached, and no branch in reached code nor any pointer in data
//            targets any word of it: inside a function right after code that
//            can't fall through, or CodeWarrior's leftovers just past a
//            function's end (a lone blr, or an epilogue after a final bctr);
//   unknown  anything else. Done means unknown == 0 in every lifted library.
struct Coverage {
    std::string section;
    uint32_t lo = 0, hi = 0;
    uint64_t code = 0, padding = 0, dead = 0, unknown = 0;
    std::vector<std::pair<uint32_t, uint32_t>> unknown_spans;  // [lo, hi)
    std::vector<std::pair<uint32_t, uint32_t>> dead_spans;     // [lo, hi): code nothing reaches or points at
};

// part / whole as a percentage with `decimals` places, in integer arithmetic,
// so reports are identical on every host.
std::string percent(uint64_t part, uint64_t whole, int decimals);

class Cfg {
public:
    explicit Cfg(const Dol& dol);
    void run();

    const std::map<uint32_t, Function>& functions() const { return funcs_; }
    const std::vector<Suspect>& suspects() const { return suspects_; }
    const std::vector<uint32_t>& rejected() const { return rejected_; }
    std::vector<Coverage> coverage() const;
    std::string summary() const;
    std::string to_json() const;

    // Decoded word at a text address (invalid outside text).
    ppc::Insn insn(uint32_t a) const;
    bool in_text(uint32_t a) const;
    // Does execution never continue past this instruction?
    static bool is_terminator(const ppc::Insn& in);

private:
    struct Explored;
    Explored explore(uint32_t start, uint32_t bound) const;
    // `fend`: where the function most likely ends (the bound when exploring;
    // the next frame set-up in the linear scan), for the region checks.
    bool resolve_table(uint32_t start, uint32_t bound, uint32_t fend, uint32_t at, JumpTable& out) const;
    std::optional<uint32_t> constant_reg(uint32_t start, uint32_t at, unsigned r) const;
    // The region proofs: each appends the region [head, end] it relies on,
    // which no pointer in data (but the switch's own table) may point into.
    using Regions = std::vector<std::pair<uint32_t, uint32_t>>;
    std::optional<uint32_t> region_constant(uint32_t start, uint32_t fend, uint32_t at, unsigned r, Regions& regions) const;
    std::optional<unsigned> scaled_index(uint32_t start, uint32_t fend, uint32_t at, unsigned rb, unsigned ri,
                                         Regions& regions) const;
    bool pointed_into(const Regions& regions, uint32_t table_lo, uint32_t table_hi) const;
    bool branched_into(uint32_t lo, uint32_t hi, uint32_t start, uint32_t fend) const;
    bool epilogue_restore(uint32_t a) const;
    bool reaches_alone(uint32_t def, uint32_t use, unsigned r, uint32_t start, uint32_t fend) const;
    uint32_t prologue_before(uint32_t lo, uint32_t a) const;
    uint32_t prologue_after(uint32_t a, uint32_t hi) const;
    bool clean_boundary(uint32_t a) const;
    bool inside_span(uint32_t a) const;  // strictly inside an explored function's [start, end)
    bool sets_up_frame(uint32_t a) const;
    uint32_t leftover_words(uint32_t a) const;
    uint32_t bound_of(uint32_t start) const;
    bool explore_all();  // returns true if new starts were found
    void add_start(uint32_t a, Origin o);
    void seed_data_pointers();
    void seed_gaps();
    void validate();
    std::vector<bool> owned_words() const;

    const Dol& dol_;
    std::vector<const DolSection*> text_;  // text sections, by address
    uint32_t text_lo_ = 0, text_hi_ = 0;   // span of all text sections
    std::vector<ppc::Insn> decoded_;       // one per word of [text_lo_, text_hi_)
    std::vector<bool> targeted_;           // per word: some direct branch (not a call) lands there

    std::map<uint32_t, Origin> starts_;    // every known function start
    std::map<uint32_t, Function> funcs_;
    std::set<uint32_t> not_calls_;         // `bl` to the next instruction: reads the PC, isn't a call
    std::vector<Suspect> suspects_;
    std::vector<uint32_t> rejected_;
    std::set<uint32_t> dropped_;           // rejected starts, never re-seeded
};

}  // namespace qilin
