#include "cfg.hpp"

#include <algorithm>
#include <format>
#include <optional>
#include <ranges>
#include <tuple>

namespace qilin {

using ppc::Insn;
using ppc::Op;

namespace {

constexpr unsigned kCtr = 9;            // SPR number of CTR
constexpr uint32_t kTableWindow = 24;   // instructions searched back from a bctr
constexpr size_t kMaxHeuristicTable = 1024;

bool branch_always(unsigned bo) { return (bo & 0x14) == 0x14; }

// Does the instruction (possibly) write GPR r? A call may write the
// registers the PowerPC EABI doesn't preserve (r0, r3-r12); the string loads
// may write anything.
bool writes_gpr(const Insn& in, unsigned r) {
    const ppc::Spec& s = ppc::spec(in.op);
    if ((s.flags & ppc::kLk) && in.lk()) return r == 0 || (r >= 3 && r <= 12);
    if (in.op == Op::lswi || in.op == Op::lswx) return true;
    if ((s.flags & (ppc::kLoad | ppc::kStore)) && (s.flags & (ppc::kRaNonZero | ppc::kRaNotZeroOrRd)) &&
        in.ra() == r)
        return true;  // update forms write their base
    if (s.flags & (ppc::kStore | ppc::kFloat)) return false;
    if (in.op == Op::mtmsr || in.op == Op::mtsrin) return false;  // read rS
    if (in.op == Op::lmw) return in.rd() <= r;
    if (s.operands[0] == ppc::Opnd::rD) return in.rd() == r;
    if (s.operands[0] == ppc::Opnd::rA) return in.ra() == r;
    return false;
}

// Code above `in` can reach the instruction after it only by falling through it.
bool ends_straight_line(const Insn& in) { return Cfg::is_terminator(in); }

}  // namespace

const char* origin_name(Origin o) {
    switch (o) {
    case Origin::Entry: return "entry";
    case Origin::Call: return "call";
    case Origin::TailCall: return "tail-call";
    case Origin::ImmPtr: return "imm-ptr";
    case Origin::DataPtr: return "data-ptr";
    case Origin::Gap: return "gap";
    }
    return "?";
}

std::string percent(uint64_t part, uint64_t whole, int decimals) {
    if (whole == 0) return "n/a";
    uint64_t scale = 100;
    for (int i = 0; i < decimals; ++i) scale *= 10;
    const uint64_t v = (part * scale + whole / 2) / whole;
    uint64_t div = 1;
    for (int i = 0; i < decimals; ++i) div *= 10;
    if (decimals == 0) return std::format("{}%", v);
    return std::format("{}.{:0{}}%", v / div, v % div, decimals);
}

struct Cfg::Explored {
    Function f;
    // Tail-call targets at clean boundaries that aren't starts yet: (branch, target).
    std::vector<std::pair<uint32_t, uint32_t>> new_tails;
};

Cfg::Cfg(const Dol& dol) : dol_(dol) {
    for (const DolSection& s : dol.sections)
        if (s.text) text_.push_back(&s);
    if (!text_.empty()) {
        text_lo_ = text_.front()->address;
        text_hi_ = text_.back()->end();
        decoded_.reserve((text_hi_ - text_lo_) / 4);
        for (uint32_t a = text_lo_; a < text_hi_; a += 4)
            decoded_.push_back(dol.text_section_at(a) ? ppc::decode(dol.u32(a)) : Insn{});
        targeted_.assign(decoded_.size(), false);
        for (uint32_t a = text_lo_; a < text_hi_; a += 4) {
            const Insn& in = decoded_[(a - text_lo_) / 4];
            if ((in.op != Op::b && in.op != Op::bc) || in.lk()) continue;
            const uint32_t t = in.branch_target(a);
            if (t >= text_lo_ && t < text_hi_ && t % 4 == 0) targeted_[(t - text_lo_) / 4] = true;
        }
    }
}

bool Cfg::in_text(uint32_t a) const { return a % 4 == 0 && dol_.text_section_at(a) != nullptr; }

Insn Cfg::insn(uint32_t a) const {
    if (!in_text(a)) return {};
    return decoded_[(a - text_lo_) / 4];
}

bool Cfg::is_terminator(const Insn& in) {
    switch (in.op) {
    case Op::b: return !in.lk();
    case Op::bclr:
    case Op::bcctr: return !in.lk() && branch_always(in.rd());
    case Op::rfi: return true;
    case Op::tw:
    case Op::twi: return in.rd() == 31;  // trap unconditionally
    default: return false;
    }
}

bool Cfg::sets_up_frame(uint32_t a) const {
    // stwu r1,-n(r1), possibly after mflr r0 (mfspr r0,LR)
    for (uint32_t p = a; p < a + 8; p += 4) {
        const Insn in = insn(p);
        if (in.op == Op::stwu && in.rd() == 1 && in.ra() == 1 && in.simm() < 0) return true;
        if (!(in.op == Op::mfspr && in.spr() == 8 && in.rd() == 0)) return false;
    }
    return false;
}

// Unreachable code CodeWarrior leaves right after a function's last
// instruction, which belongs to that function: a lone blr (after a final tail
// call, a trap, or another blr), or an epilogue that tears the frame down
// (`addi r1, r1, n` ... `blr`) after a final call through `bctr`. Returns its
// length in words, 0 if `a` isn't such code. Callers check that nothing
// branches to it or points at it: then it's a function of its own.
uint32_t Cfg::leftover_words(uint32_t a) const {
    auto it = funcs_.upper_bound(a - 4);
    if (!in_text(a - 4) || it == funcs_.begin()) return 0;
    --it;
    if (it->second.end != a || !is_terminator(insn(a - 4))) return 0;
    if (insn(a).word == 0x4E800020u) return 1;
    const Insn last = insn(a - 4);
    if (!((last.op == Op::b || last.op == Op::bcctr) && !last.lk())) return 0;
    bool teardown = false;
    for (uint32_t p = a, n = 1; n <= 16 && in_text(p); p += 4, ++n) {
        const Insn in = insn(p);
        if (!in.valid() || (in.op == Op::stwu && in.ra() == 1)) return 0;
        if (in.op == Op::addi && in.rd() == 1 && in.ra() == 1 && in.simm() > 0) teardown = true;
        if (is_terminator(in)) return teardown && in.op == Op::bclr ? n : 0;
    }
    return 0;
}

bool Cfg::clean_boundary(uint32_t a) const {
    const DolSection* s = in_text(a) ? dol_.text_section_at(a) : nullptr;
    if (!s || !insn(a).valid()) return false;
    if (a == s->address) return true;
    const Insn prev = insn(a - 4);
    return prev.word == 0 || is_terminator(prev);
}

uint32_t Cfg::bound_of(uint32_t start) const {
    const DolSection* s = dol_.text_section_at(start);
    auto next = starts_.upper_bound(start);
    if (next != starts_.end() && next->first < s->end()) return next->first;
    return s->end();
}

void Cfg::add_start(uint32_t a, Origin o) {
    if (dropped_.contains(a) || !in_text(a)) return;
    auto [it, inserted] = starts_.try_emplace(a, o);
    if (!inserted && o < it->second) it->second = o;
}

// CodeWarrior's switch:
//     cmplwi  crN, rI, N        (or a bound of N + 1 checked with bge)
//     bgt     crN, default
//     lis     rX, table@ha
//     slwi    rB, rI, 2
//     addi    rA, rX, table@l
//     lwzx    rT, rA, rB
//     mtctr   rT
//     bctr
// in any order the scheduler chose, all on the straight line into the bctr.
// Where the function containing `a` most likely starts, before any analysis
// has found it: the nearest stack-frame set-up (stwu r1, -n(r1)) at a clean
// boundary, or the section start.
uint32_t Cfg::prologue_before(uint32_t lo, uint32_t a) const {
    for (uint32_t p = a; p > lo;) {
        p -= 4;
        const Insn in = insn(p);
        if (in.op == Op::stwu && in.rd() == 1 && in.ra() == 1 && in.simm() < 0 && clean_boundary(p)) return p;
    }
    return lo;
}

// The value of a callee-saved register that the code from `start` to `at`
// sets once, to an address: `lis rX, hi`, possibly followed by
// `addi rX, rX, lo` (or `ori`), as CodeWarrior does for a function's data pool. Calls preserve
// r14-r31 under the EABI, so they don't count as writes.
std::optional<uint32_t> Cfg::constant_reg(uint32_t start, uint32_t at, unsigned r) const {
    if (r == 0 || r == 1 || r == 2 || r == 13) return std::nullopt;
    const bool preserved = r >= 14;  // a volatile register qualifies only with no call after its writer
    std::vector<uint32_t> writers;
    for (uint32_t p = start; p < at; p += 4) {
        const Insn in = insn(p);
        if (!in.valid()) return std::nullopt;
        const bool call = (ppc::spec(in.op).flags & ppc::kLk) && in.lk() && !not_calls_.contains(p);
        if (call && !preserved && !writers.empty()) return std::nullopt;
        if (!call && writes_gpr(in, r)) writers.push_back(p);
    }
    if (writers.size() == 1) {  // lis rX, hi alone: the low half is added where it's used
        const Insn hi = insn(writers[0]);
        if (hi.op == Op::addis && hi.ra() == 0) return hi.uimm() << 16;
        return std::nullopt;
    }
    if (writers.size() != 2) return std::nullopt;
    const Insn hi = insn(writers[0]), lo = insn(writers[1]);
    if (hi.op != Op::addis || hi.ra() != 0) return std::nullopt;
    if (lo.op == Op::addi && lo.ra() == r) return (hi.uimm() << 16) + static_cast<uint32_t>(lo.simm());
    if (lo.op == Op::ori && lo.rd() == r) return (hi.uimm() << 16) | lo.uimm();
    return std::nullopt;
}

// The next stack-frame set-up after `a` at a clean boundary, or `hi`.
uint32_t Cfg::prologue_after(uint32_t a, uint32_t hi) const {
    for (uint32_t p = a + 4; p < hi; p += 4) {
        const Insn in = insn(p);
        if (in.op == Op::stwu && in.rd() == 1 && in.ra() == 1 && in.simm() < 0 && clean_boundary(p)) return p;
    }
    return hi;
}

// Does a direct branch in [start, fend), from outside [lo, hi], land in (lo, hi]?
// Then code in (lo, hi] can be reached without running `lo` first.
bool Cfg::branched_into(uint32_t lo, uint32_t hi, uint32_t start, uint32_t fend) const {
    for (uint32_t q = start; q < fend; q += 4) {
        if (q >= lo && q <= hi) continue;
        const Insn b = insn(q);
        if ((b.op != Op::b && b.op != Op::bc) || b.lk()) continue;
        const uint32_t t = b.branch_target(q);
        if (t > lo && t <= hi) return true;
    }
    return false;
}

// A callee-saved register restored from the stack on the way out
// (`lmw rN, d(r1)` or `lwz rN, d(r1)`, then straight on to `blr`): the
// function's value of the register is dead there.
bool Cfg::epilogue_restore(uint32_t a) const {
    const Insn in = insn(a);
    if (!((in.op == Op::lmw || in.op == Op::lwz) && in.ra() == 1)) return false;
    for (uint32_t p = a + 4, n = 0; n < 12 && in_text(p); p += 4, ++n) {
        const Insn x = insn(p);
        if (x.op == Op::bclr) return !x.lk() && branch_always(x.rd());
        const bool restore = ((x.op == Op::lmw || x.op == Op::lwz) && x.ra() == 1) || (x.op == Op::mtspr && x.spr() == 8) ||
                             (x.op == Op::addi && x.rd() == 1 && x.ra() == 1);
        if (!restore) return false;
    }
    return false;
}

// Is `def` the only write of GPR r that reaches `use`? `def` is above `use`
// on a path the caller walked, which crossed a join (a branch target). The
// region from `def` down to `use` grows to cover every branch back into it;
// if anything enters it from above `def`, or anything in it writes r, another
// value could reach `use`.
bool Cfg::reaches_alone(uint32_t def, uint32_t use, unsigned r, uint32_t start, uint32_t fend) const {
    uint32_t end = use;
    for (bool grew = true; grew;) {
        grew = false;
        for (uint32_t q = start; q < fend; q += 4) {
            const Insn b = insn(q);
            if ((b.op != Op::b && b.op != Op::bc) || b.lk()) continue;
            const uint32_t t = b.branch_target(q);
            if (t <= def || t > end) continue;
            if (q < def) return false;
            if (q > end) {
                end = q;
                grew = true;
            }
        }
    }
    for (uint32_t p = def + 4; p <= end; p += 4) {
        if (p == use) continue;
        const Insn in = insn(p);
        if (!in.valid() || (writes_gpr(in, r) && !epilogue_restore(p))) return false;
    }
    return true;
}

// The value of GPR r at `at` when the function sets it more than once, but
// the last time before `at` is `lis rX, hi` (possibly with `addi`/`ori rX,
// rX, lo` right after) at the head of a region that can only be entered
// through that head: a loop set up after earlier, unrelated uses of the
// register. The region [head, end] grows to cover every branch back into it;
// any branch into it from before the head, or any other write in it, fails.
std::optional<uint32_t> Cfg::region_constant(uint32_t start, uint32_t fend, uint32_t at, unsigned r,
                                             Regions& regions) const {
    if (r < 14) return std::nullopt;  // calls must preserve it
    std::optional<uint32_t> last;
    for (uint32_t p = start; p < at; p += 4) {
        const Insn in = insn(p);
        if (!in.valid()) return std::nullopt;
        const bool call = (ppc::spec(in.op).flags & ppc::kLk) && in.lk() && !not_calls_.contains(p);
        if (!call && writes_gpr(in, r)) last = p;
    }
    if (!last) return std::nullopt;
    uint32_t head = *last, value = 0;
    const Insn w = insn(*last);
    if (w.op == Op::addis && w.ra() == 0) {
        value = w.uimm() << 16;
    } else if (((w.op == Op::addi && w.ra() == r) || (w.op == Op::ori && w.rd() == r && w.ra() == r)) && *last > start) {
        // The `lis` is the previous writer of r on the straight line.
        std::optional<uint32_t> hi;
        for (uint32_t p = *last; p > start;) {
            p -= 4;
            const Insn x = insn(p);
            if (!x.valid() || ends_straight_line(x)) return std::nullopt;
            if (writes_gpr(x, r)) {
                hi = p;
                break;
            }
        }
        if (!hi) return std::nullopt;
        const Insn h = insn(*hi);
        if (h.op != Op::addis || h.ra() != 0) return std::nullopt;
        if (branched_into(*hi, *last, start, fend)) return std::nullopt;
        head = *hi;
        value = w.op == Op::addi ? (h.uimm() << 16) + static_cast<uint32_t>(w.simm()) : (h.uimm() << 16) | w.uimm();
    } else {
        return std::nullopt;
    }
    // Grow the region to cover every branch into it.
    uint32_t end = at;
    for (bool grew = true; grew;) {
        grew = false;
        for (uint32_t q = start; q < fend; q += 4) {
            const Insn b = insn(q);
            if ((b.op != Op::b && b.op != Op::bc) || b.lk()) continue;
            const uint32_t t = b.branch_target(q);
            if (t <= head || t > end) continue;
            if (q < head) return std::nullopt;  // enters past the head
            if (q > end) {
                end = q;
                grew = true;
            }
        }
    }
    for (uint32_t p = *last + 4; p <= end; p += 4) {
        const Insn in = insn(p);
        if (!in.valid()) return std::nullopt;
        const bool call = (ppc::spec(in.op).flags & ppc::kLk) && in.lk() && !not_calls_.contains(p);
        if (!call && writes_gpr(in, r) && !epilogue_restore(p)) return std::nullopt;
    }
    regions.emplace_back(head, end);
    return value;
}

// A switch inside a loop whose index the compiler strength-reduced: the
// table is indexed by rB, which steps by 4, and bounds-checked on rI, which
// steps by 1. Proven when the last writes of both before the switch zero them
// together (nothing can branch in between), and that zeroing heads a region
// holding the switch that can only be entered through it: the region grows to
// cover every branch back into it, a branch into it from before the zeroing
// fails, and inside it the only writes to either register are one step of
// each, again together. Then rB == 4 * rI wherever the switch runs. Calls in
// the region are allowed only if both registers survive calls. Returns rI.
std::optional<unsigned> Cfg::scaled_index(uint32_t start, uint32_t fend, uint32_t at, unsigned rb, unsigned ri,
                                          Regions& regions) const {
    if (rb == ri || rb == 0 || ri == 0 || rb == 1 || ri == 1) return std::nullopt;
    auto is_call = [&](uint32_t p, const Insn& in) {
        return (ppc::spec(in.op).flags & ppc::kLk) && in.lk() && !not_calls_.contains(p);
    };
    // The last writes before the switch.
    std::optional<uint32_t> ib, ii;
    for (uint32_t p = start; p < at; p += 4) {
        const Insn in = insn(p);
        if (!in.valid() || is_call(p, in)) continue;
        if (writes_gpr(in, rb)) ib = p;
        if (writes_gpr(in, ri)) ii = p;
    }
    if (!ib || !ii) return std::nullopt;
    auto is_li0 = [&](uint32_t p, unsigned r) {
        const Insn x = insn(p);
        return x.op == Op::addi && x.rd() == r && x.ra() == 0 && x.simm() == 0;
    };
    auto is_step = [&](uint32_t p, unsigned r, int32_t by) {
        const Insn x = insn(p);
        return x.op == Op::addi && x.rd() == r && x.ra() == r && x.simm() == by;
    };
    // Two instructions on one straight line that nothing branches into.
    auto together = [&](uint32_t x, uint32_t y) {
        const uint32_t lo = std::min(x, y), hi = std::max(x, y);
        for (uint32_t p = lo; p < hi; p += 4) {
            const Insn in = insn(p);
            if (!in.valid() || ends_straight_line(in) || in.op == Op::bc || in.op == Op::bclr || in.op == Op::bcctr ||
                is_call(p, in))
                return false;
        }
        return !branched_into(lo, hi, start, fend) && !(at >= lo && at <= hi);
    };
    // Either order: the step written last before the switch, at the loop's
    // foot, would also be a last writer if the loop runs downwards. Only
    // zeroing qualifies here.
    if (!is_li0(*ib, rb) || !is_li0(*ii, ri) || !together(*ib, *ii)) return std::nullopt;
    const uint32_t init_lo = std::min(*ib, *ii), head = std::max(*ib, *ii) + 4;
    if (head > at) return std::nullopt;
    uint32_t end = at;
    for (bool grew = true; grew;) {
        grew = false;
        for (uint32_t q = start; q < fend; q += 4) {
            const Insn b = insn(q);
            if ((b.op != Op::b && b.op != Op::bc) || b.lk()) continue;
            const uint32_t t = b.branch_target(q);
            if (t < head || t > end) continue;
            if (q < init_lo) return std::nullopt;  // enters without the zeroing
            if (q > end) {
                end = q;
                grew = true;
            }
        }
    }
    std::vector<uint32_t> sb, si;
    bool calls = false;
    for (uint32_t p = head; p <= end; p += 4) {
        const Insn in = insn(p);
        if (!in.valid()) return std::nullopt;
        if (is_call(p, in)) {
            calls = true;
            continue;
        }
        if (epilogue_restore(p)) continue;
        if (writes_gpr(in, rb)) sb.push_back(p);
        if (writes_gpr(in, ri)) si.push_back(p);
    }
    if (calls && (rb < 14 || ri < 14)) return std::nullopt;
    if (sb.size() != 1 || si.size() != 1 || !is_step(sb[0], rb, 4) || !is_step(si[0], ri, 1) || !together(sb[0], si[0]))
        return std::nullopt;
    regions.emplace_back(head, end);
    return ri;
}

// Does a word in data, outside [table_lo, table_hi), hold an address in one
// of the regions? A switch elsewhere, or a pointer, could then enter it
// without going through its head.
bool Cfg::pointed_into(const Regions& regions, uint32_t table_lo, uint32_t table_hi) const {
    if (regions.empty()) return false;
    for (const DolSection& s : dol_.sections) {
        if (s.text) continue;
        for (uint32_t a = (s.address + 3) & ~3u; a + 4 <= s.end(); a += 4) {
            if (a >= table_lo && a < table_hi) continue;
            const uint32_t v = dol_.u32(a);
            for (const auto& [head, end] : regions)
                if (v >= head && v <= end) return true;
        }
    }
    return false;
}

bool Cfg::resolve_table(uint32_t start, uint32_t bound, uint32_t fend, uint32_t at, JumpTable& out) const {
    Regions regions;
    const uint32_t lo = std::max(start, at >= kTableWindow * 4 ? at - kTableWindow * 4 : 0u);
    // The latest instruction before `before` that writes GPR r, walking back
    // on the straight line. Where the code above can't fall through (an early
    // return, say), the walk continues from the one branch in the function
    // that jumps there, if there is exactly one.
    // Crossing a join (a word some branch lands on) means other paths reach
    // `before` too: then the write found must be the only one that can.
    auto def_of = [&](unsigned r, uint32_t before) -> std::optional<uint32_t> {
        uint32_t p = before;
        bool joined = false;
        for (int steps = 0; steps < 64 && p > start; ++steps) {
            const Insn in = insn(p - 4);
            if (!in.valid()) return std::nullopt;
            if (targeted_[(p - text_lo_) / 4]) joined = true;
            if (ends_straight_line(in)) {
                std::optional<uint32_t> from;
                for (uint32_t q = start; q < at; q += 4) {
                    const Insn b = insn(q);
                    if ((b.op == Op::b || b.op == Op::bc) && !b.lk() && b.branch_target(q) == p) {
                        if (from) return std::nullopt;
                        from = q;
                    }
                }
                if (!from) return std::nullopt;
                p = *from;
                continue;
            }
            p -= 4;
            if (writes_gpr(in, r)) {
                if (joined && !reaches_alone(p, before, r, start, fend)) return std::nullopt;
                return p;
            }
        }
        return std::nullopt;
    };
    // mtctr rT, with nothing else writing CTR in between.
    std::optional<uint32_t> mt;
    for (uint32_t p = at; p > lo && !mt;) {
        p -= 4;
        const Insn in = insn(p);
        if (!in.valid() || ends_straight_line(in)) break;
        if (in.op == Op::mtspr && in.spr() == kCtr) mt = p;
        else if (in.op == Op::bc || in.op == Op::bcctr || in.op == Op::bclr) {
            if (!(in.rd() & 4)) break;  // decrements CTR
        }
    }
    if (!mt) return false;
    const auto lw = def_of(insn(*mt).rd(), *mt);
    if (!lw || insn(*lw).op != Op::lwzx) return false;
    const Insn load = insn(*lw);
    // The table address: lis rX, hi; addi rA, rX, lo (or ori).
    const auto base_def = def_of(load.ra(), *lw);
    if (!base_def) return false;
    const Insn base = insn(*base_def);
    if (!((base.op == Op::addi && base.ra() != 0) || base.op == Op::ori)) return false;
    const unsigned high_reg = base.op == Op::addi ? base.ra() : base.rd();  // ori rA, rS, uimm: rS is in the rD field
    const uint32_t low = base.op == Op::addi ? static_cast<uint32_t>(base.simm()) : base.uimm();
    std::optional<uint32_t> high_value;
    if (const auto hi_def = def_of(high_reg, *base_def)) {
        const Insn high = insn(*hi_def);
        if (high.op != Op::addis || high.ra() != 0) return false;
        high_value = high.uimm() << 16;
    } else {
        high_value = constant_reg(start, at, high_reg);  // a data-pool base set up earlier
        if (!high_value) high_value = region_constant(start, fend, at, high_reg, regions);  // ... or at a loop's head
    }
    if (!high_value) return false;
    const uint32_t table = *high_value + low;
    const DolSection* data = dol_.section_at(table);
    if (!data || data->text || table % 4) return false;
    // The index: slwi rB, rI, 2; or rB stepping by 4 alongside rI (scaled_index).
    // The scheduler may put the bound check before the shift or after it.
    std::optional<unsigned> index;
    std::optional<uint32_t> shift_at;
    if (const auto shift_def = def_of(load.rb(), *lw)) {
        const Insn shift = insn(*shift_def);
        if (shift.op == Op::rlwinm && shift.rb() == 2 && shift.mb() == 0 && shift.me() == 29) {
            index = shift.rd();  // rlwinm rA, rS, ...: rS is in the rD field
            shift_at = shift_def;
        }
    }

    // The bound: cmplwi crN, rI, N, then a conditional branch out of the
    // straight line: bgt crN (index > N) or bge crN (index >= N). Nothing
    // may branch in between the check and the bctr.
    std::optional<uint32_t> count;
    std::optional<std::pair<unsigned, unsigned>> exit;  // CR bit, branch-if-true
    for (uint32_t p = *lw; p > lo && !count;) {
        p -= 4;
        const Insn in = insn(p);
        if (!in.valid() || ends_straight_line(in)) break;
        if ((in.op == Op::bc || in.op == Op::bclr) && !in.lk() && !exit) {  // bgt default, or bgtlr
            exit = std::pair{in.ra(), (in.rd() & 0x1E) == 12 ? 1u : (in.rd() & 0x1E) == 4 ? 0u : 2u};
            continue;
        }
        // Above the shift, the index must not change on the way down to it
        // (below it, the shifted copy is what's used).
        if (index && p < *shift_at && writes_gpr(in, *index)) break;
        if (!index && writes_gpr(in, load.rb())) break;
        if (in.op == Op::cmpli && exit && (!index || in.ra() == *index)) {
            if (index && p > *shift_at) {  // the check after the shift: the index unchanged in between
                bool changed = false;
                for (uint32_t q = *shift_at + 4; q < p; q += 4) changed |= writes_gpr(insn(q), *index);
                if (changed) break;
            }
            if (!index && !scaled_index(start, fend, at, load.rb(), in.ra(), regions)) break;
            const unsigned crf = in.crfd();
            if (exit->first == 4 * crf + 1 && exit->second == 1) count = in.uimm() + 1;  // bgt
            else if (exit->first == 4 * crf + 0 && exit->second == 0) count = in.uimm();  // bge
            if (count && branched_into(p, at, start, fend)) count.reset();
            break;
        }
    }
    if (!index && !count) return false;  // a strength-reduced index needs its proof

    JumpTable t;
    t.at = at;
    t.table = table;
    auto plausible = [&](uint32_t target) { return target >= start && target < bound && in_text(target) && insn(target).valid(); };
    if (count) {
        if (*count == 0 || !dol_.mapped(table, 4 * *count)) return false;
        for (uint32_t k = 0; k < *count; ++k) {
            const uint32_t target = dol_.u32(table + 4 * k);
            if (!plausible(target)) return false;
            t.entries.push_back(target);
        }
        if (pointed_into(regions, table, table + 4 * *count)) return false;
        t.proven = true;
    } else {
        if (!regions.empty()) return false;  // a region proof needs its pointer check, which needs the size
        for (uint32_t k = 0; k < kMaxHeuristicTable && dol_.mapped(table + 4 * k, 4); ++k) {
            const uint32_t target = dol_.u32(table + 4 * k);
            if (!plausible(target)) break;
            t.entries.push_back(target);
        }
        if (t.entries.empty()) return false;
    }
    t.targets = t.entries;
    std::ranges::sort(t.targets);
    t.targets.erase(std::ranges::unique(t.targets).begin(), t.targets.end());
    out = std::move(t);
    return true;
}

Cfg::Explored Cfg::explore(uint32_t start, uint32_t bound) const {
    Explored r;
    Function& f = r.f;
    f.start = start;
    f.bound = bound;
    f.origin = starts_.at(start);
    std::vector<bool> seen((bound - start) / 4, false);
    std::vector<uint32_t> work{start};
    f.leaders.insert(start);
    const DolSection* section = dol_.text_section_at(start);

    // Where an unconditional or conditional branch goes; returns the address
    // to explore, if it's inside this function.
    auto branch_to = [&](uint32_t from, uint32_t target) -> std::optional<uint32_t> {
        // A stack frame is only ever set up at a function's entry, so a
        // branch to a frame set-up at a clean boundary is a tail call, even
        // into what looked like this function.
        if (target > start && target < bound && sets_up_frame(target) && clean_boundary(target)) {
            f.tails.insert(target);
            r.new_tails.emplace_back(from, target);
            return std::nullopt;
        }
        if (target >= start && target < bound && in_text(target)) {
            f.leaders.insert(target);
            return target;
        }
        if (starts_.contains(target)) f.tails.insert(target);
        else if (clean_boundary(target)) {
            f.tails.insert(target);
            r.new_tails.emplace_back(from, target);
        } else {
            f.escapes.push_back(from);
        }
        return std::nullopt;
    };

    while (!work.empty()) {
        uint32_t a = work.back();
        work.pop_back();
        for (;;) {
            const size_t i = (a - start) / 4;
            if (seen[i]) break;
            seen[i] = true;
            f.insns.push_back(a);
            const Insn in = insn(a);
            if (!in.valid()) {
                f.bad.push_back(a);
                break;
            }
            bool next = true;
            switch (in.op) {
            case Op::b:
            case Op::bc: {
                const uint32_t target = in.branch_target(a);
                const bool always = in.op == Op::b || branch_always(in.rd());
                if (in.lk()) {
                    if (not_calls_.contains(a)) break;  // reads the PC; execution continues
                    if (in_text(target)) f.calls.insert(target);
                    else f.escapes.push_back(a);
                    break;
                }
                if (auto t = branch_to(a, target)) work.push_back(*t);
                if (always) next = false;
                else f.leaders.insert(a + 4);
                break;
            }
            case Op::bclr:
                if (in.lk()) f.icalls.push_back(a);
                else if (branch_always(in.rd())) next = false;
                else f.leaders.insert(a + 4);
                break;
            case Op::bcctr:
                if (in.lk()) {
                    f.icalls.push_back(a);
                } else if (branch_always(in.rd())) {
                    JumpTable t;
                    if (resolve_table(start, bound, bound, a, t)) {
                        for (uint32_t target : t.targets) {
                            f.leaders.insert(target);
                            work.push_back(target);
                        }
                        f.jtabs.push_back(std::move(t));
                    } else {
                        f.itails.push_back(a);
                    }
                    next = false;
                } else {
                    f.itails.push_back(a);  // conditional branch through CTR
                    f.leaders.insert(a + 4);
                }
                break;
            default:
                if (is_terminator(in)) next = false;
                break;
            }
            if (!next) break;
            a += 4;
            if (a >= bound) {
                if (a < section->end() && starts_.contains(a)) f.falls_into = a;
                else f.bad.push_back(a - 4);  // runs off the end of its section
                break;
            }
        }
    }
    std::ranges::sort(f.insns);
    f.end = f.insns.empty() ? start : f.insns.back() + 4;

    // lis/addi (or lis/ori) pairs that build an address in text: callbacks,
    // thread entry points, function pointers stored at run time.
    for (uint32_t a : f.insns) {
        const Insn in = insn(a);
        if (!((in.op == Op::addi && in.ra() != 0) || in.op == Op::ori)) continue;
        const unsigned high_reg = in.op == Op::addi ? in.ra() : in.rd();
        for (uint32_t p = a; p > start && a - p < 64;) {
            p -= 4;
            const Insn h = insn(p);
            if (!h.valid() || is_terminator(h)) break;
            if (!writes_gpr(h, high_reg)) continue;
            if (h.op == Op::addis && h.ra() == 0) {
                const uint32_t v = (h.uimm() << 16) + (in.op == Op::addi ? static_cast<uint32_t>(in.simm()) : in.uimm());
                if (in_text(v)) f.imm_ptrs.push_back(v);
            }
            break;
        }
    }
    return r;
}

std::vector<bool> Cfg::owned_words() const {
    std::vector<bool> owned(decoded_.size(), false);
    for (const auto& [start, f] : funcs_)
        for (uint32_t a : f.insns) owned[(a - text_lo_) / 4] = true;
    return owned;
}

bool Cfg::inside_span(uint32_t a) const {
    auto it = funcs_.upper_bound(a);
    if (it == funcs_.begin()) return false;
    --it;
    return a > it->second.start && a < it->second.end;
}

bool Cfg::explore_all() {
    funcs_.clear();
    std::vector<std::tuple<uint32_t, uint32_t, uint32_t>> tails;  // (function, branch, target)
    std::vector<uint32_t> imms;
    for (const auto& [start, origin] : starts_) {
        Explored e = explore(start, bound_of(start));
        for (const auto& [from, target] : e.new_tails) tails.emplace_back(start, from, target);
        imms.insert(imms.end(), e.f.imm_ptrs.begin(), e.f.imm_ptrs.end());
        funcs_.emplace(start, std::move(e.f));
    }
    // A new start must not fall inside a function's span: CodeWarrior's
    // functions are contiguous, so that would be a label, not a function.
    // A tail call that can't become a start is an escape.
    const size_t before = starts_.size();
    // A tail call to a clean boundary inside another function's span means
    // that span was too long (it was explored before this start was known):
    // CodeWarrior never branches between functions except to an entry. Only
    // weakly seeded functions can't split another this way.
    for (const auto& [start, from, target] : tails) {
        const Function& f = funcs_.at(start);
        if (!inside_span(target) || f.origin == Origin::Entry || f.origin == Origin::Call ||
            f.origin == Origin::TailCall) {
            add_start(target, Origin::TailCall);
        } else {
            Function& g = funcs_.at(start);
            g.tails.erase(target);
            g.escapes.push_back(from);
        }
    }
    for (uint32_t v : imms)
        if (!starts_.contains(v) && clean_boundary(v) && !inside_span(v)) add_start(v, Origin::ImmPtr);
    return starts_.size() != before;
}

void Cfg::seed_data_pointers() {
    // Jump tables are runs of pointers into text too. Exclude the ones found
    // so far, and every one a linear scan of the code can prove: a function
    // reached only through a vtable hasn't been explored yet, and its switch
    // tables' entries must not become function starts.
    std::vector<std::pair<uint32_t, uint32_t>> tables;
    // Case labels of proven switches: never a function entry, even when some
    // other word in data happens to hold the same value (binary data can look
    // like a pointer). Seeding one would end the switch's function there,
    // before its own table could be proven.
    std::set<uint32_t> case_labels;
    for (const auto& [start, f] : funcs_)
        for (const JumpTable& t : f.jtabs) tables.emplace_back(t.table, t.end());
    for (const DolSection* sec : text_)
        for (uint32_t a = sec->address; a < sec->end(); a += 4) {
            const Insn in = insn(a);
            if (in.op != Op::bcctr || in.lk() || !branch_always(in.rd())) continue;
            JumpTable t;
            if (resolve_table(prologue_before(sec->address, a), sec->end(), prologue_after(a, sec->end()), a, t) &&
                t.proven) {
                tables.emplace_back(t.table, t.end());
                case_labels.insert(t.targets.begin(), t.targets.end());
            }
        }
    std::ranges::sort(tables);
    auto in_table = [&](uint32_t a) {
        auto it = std::ranges::upper_bound(tables, std::pair{a, UINT32_MAX});
        return it != tables.begin() && a < std::prev(it)->second;
    };
    for (const DolSection& s : dol_.sections) {
        if (s.text) continue;
        for (uint32_t a = (s.address + 3) & ~3u; a + 4 <= s.end(); a += 4) {
            const uint32_t v = dol_.u32(a);
            if (!in_text(v) || starts_.contains(v) || in_table(a) || case_labels.contains(v)) continue;
            if (clean_boundary(v) && !inside_span(v)) add_start(v, Origin::DataPtr);
        }
    }
}

void Cfg::seed_gaps() {
    const std::vector<bool> owned = owned_words();
    for (const DolSection* s : text_) {
        bool after_boundary = true;  // the section start is a boundary
        for (uint32_t a = s->address; a < s->end(); a += 4) {
            const size_t i = (a - text_lo_) / 4;
            const Insn in = decoded_[i];
            if (owned[i]) {
                after_boundary = is_terminator(in);
                continue;
            }
            if (in.word == 0) {
                after_boundary = true;
                continue;
            }
            // The first word of each run of unreached code, if it starts at
            // a boundary and isn't unreachable code inside a function. The
            // next round sees the new function's extent and seeds the rest.
            if (const uint32_t n = leftover_words(a)) {  // part of the previous function; ends in blr
                a += 4 * (n - 1);
                after_boundary = true;
                continue;
            }
            if (after_boundary && in.valid() && !starts_.contains(a) && !inside_span(a)) add_start(a, Origin::Gap);
            after_boundary = false;
        }
    }
}

void Cfg::validate() {
    for (;;) {
        std::vector<uint32_t> drop;
        for (const auto& [start, f] : funcs_)
            if (!f.bad.empty() && !strong(f.origin)) drop.push_back(start);
        if (drop.empty()) break;
        for (uint32_t a : drop) {
            starts_.erase(a);
            dropped_.insert(a);
            rejected_.push_back(a);
        }
        while (explore_all()) {
        }
    }
    suspects_.clear();
    for (const auto& [start, f] : funcs_) {
        for (uint32_t a : f.bad)
            suspects_.push_back({start, std::format("reaches {:08X}: {}", a,
                                                    insn(a).valid() ? "the end of its section" : "an invalid word")});
        for (uint32_t a : f.escapes)
            suspects_.push_back({start, std::format("branch at {:08X} to {:08X}, inside another function or not code", a,
                                                    insn(a).branch_target(a))});
        for (const JumpTable& t : f.jtabs)
            if (!t.proven)
                suspects_.push_back({start, std::format("jump table at {:08X} ({} entries from {:08X}) has no bound check",
                                                        t.at, t.entries.size(), t.table)});
    }
}

void Cfg::run() {
    if (text_.empty()) return;
    add_start(dol_.entry, Origin::Entry);
    for (uint32_t a = text_lo_; a < text_hi_; a += 4) {
        const Insn in = decoded_[(a - text_lo_) / 4];
        if ((in.op != Op::b && in.op != Op::bc) || !in.lk()) continue;
        const uint32_t target = in.branch_target(a);
        if (target == a + 4) not_calls_.insert(a);  // bl to the next instruction: the PC into LR
        else if (in_text(target)) add_start(target, Origin::Call);
    }
    while (explore_all()) {
    }
    for (;;) {
        const size_t before = starts_.size();
        seed_data_pointers();
        while (explore_all()) {
        }
        seed_gaps();
        while (explore_all()) {
        }
        if (starts_.size() == before) break;
    }
    validate();
}

std::vector<Coverage> Cfg::coverage() const {
    const std::vector<bool> owned = owned_words();
    // Every address reached code or data could transfer control to: direct
    // branch and call targets in reached code, and pointers into text stored
    // in data. (A branch in unreached code can't make its target reachable.)
    std::set<uint32_t> referenced;
    for (uint32_t a = text_lo_; a < text_hi_; a += 4) {
        const Insn in = decoded_[(a - text_lo_) / 4];
        if ((in.op == Op::b || in.op == Op::bc) && owned[(a - text_lo_) / 4]) referenced.insert(in.branch_target(a));
    }
    for (const DolSection& s : dol_.sections) {
        if (s.text) continue;
        for (uint32_t a = (s.address + 3) & ~3u; a + 4 <= s.end(); a += 4)
            if (const uint32_t v = dol_.u32(a); in_text(v)) referenced.insert(v);
    }
    auto padding = [](const Insn& in) { return in.word == 0 || in.word == 0x60000000u; };  // zero, nop

    std::vector<Coverage> out;
    for (const DolSection* s : text_) {
        Coverage c{s->name(), s->address, s->end(), 0, 0, 0, 0, {}, {}};
        for (uint32_t a = s->address; a < s->end();) {
            const size_t i = (a - text_lo_) / 4;
            if (owned[i]) {
                ++c.code;
                a += 4;
                continue;
            }
            if (padding(decoded_[i])) {
                ++c.padding;
                a += 4;
                continue;
            }
            // A run of unreached, non-padding words.
            uint32_t b = a;
            bool targeted = false;
            while (b < s->end() && !owned[(b - text_lo_) / 4] && !padding(decoded_[(b - text_lo_) / 4])) {
                targeted |= referenced.contains(b);
                b += 4;
            }
            const uint32_t words = (b - a) / 4;
            uint32_t before = a;  // the last word before the run that isn't padding
            while (before > s->address && padding(insn(before - 4))) before -= 4;
            const bool dead = !targeted && ((before > s->address && is_terminator(insn(before - 4)) && inside_span(a)) ||
                                            leftover_words(a) == words);
            if (dead) {
                c.dead += words;
                c.dead_spans.emplace_back(a, b);
            } else {
                c.unknown += words;
                c.unknown_spans.emplace_back(a, b);
            }
            a = b;
        }
        out.push_back(std::move(c));
    }
    return out;
}

std::string Cfg::summary() const {
    std::map<Origin, size_t> by_origin;
    size_t insns = 0, proven = 0, heuristic = 0, icalls = 0, itails = 0, falls = 0, tails = 0;
    for (const auto& [start, f] : funcs_) {
        ++by_origin[f.origin];
        insns += f.insns.size();
        icalls += f.icalls.size();
        itails += f.itails.size();
        tails += f.tails.size();
        falls += f.falls_into != 0;
        for (const JumpTable& t : f.jtabs) ++(t.proven ? proven : heuristic);
    }
    std::string s = std::format("{} functions:", funcs_.size());
    for (const auto& [o, n] : by_origin) s += std::format(" {} {},", n, origin_name(o));
    s.back() = '\n';
    s += std::format("{} reachable instructions; {} jump tables ({} proven, {} heuristic); "
                     "{} indirect calls; {} indirect tail calls; {} tail calls; {} fall into the next function\n",
                     insns, proven + heuristic, proven, heuristic, icalls, itails, tails, falls);
    s += std::format("{} suspects, {} rejected starts\n", suspects_.size(), rejected_.size());
    for (const Coverage& c : coverage()) {
        const uint64_t words = c.code + c.padding + c.dead + c.unknown;
        s += std::format("{} {:08X}-{:08X}: code {} ({}), padding {}, dead {}, unknown {} in {} spans\n", c.section, c.lo,
                         c.hi, c.code, percent(c.code, words, 2), c.padding, c.dead, c.unknown, c.unknown_spans.size());
    }
    return s;
}

std::string Cfg::to_json() const {
    std::string j = "{\n  \"functions\": [\n";
    size_t n = 0;
    for (const auto& [start, f] : funcs_) {
        j += std::format("    {{\"start\": \"0x{:08X}\", \"end\": \"0x{:08X}\", \"origin\": \"{}\", \"instructions\": {}",
                         f.start, f.end, origin_name(f.origin), f.insns.size());
        auto list = [&](const char* name, const auto& v) {
            if (v.empty()) return;
            j += std::format(", \"{}\": [", name);
            bool first = true;
            for (uint32_t a : v) {
                j += std::format("{}\"0x{:08X}\"", first ? "" : ", ", a);
                first = false;
            }
            j += ']';
        };
        list("calls", f.calls);
        list("tails", f.tails);
        list("indirect_calls", f.icalls);
        list("indirect_tails", f.itails);
        if (f.falls_into) j += std::format(", \"falls_into\": \"0x{:08X}\"", f.falls_into);
        if (!f.jtabs.empty()) {
            j += ", \"jump_tables\": [";
            for (size_t k = 0; k < f.jtabs.size(); ++k) {
                const JumpTable& t = f.jtabs[k];
                j += std::format("{}{{\"at\": \"0x{:08X}\", \"table\": \"0x{:08X}\", \"entries\": {}, \"proven\": {}}}",
                                 k ? ", " : "", t.at, t.table, t.entries.size(), t.proven);
            }
            j += ']';
        }
        j += std::format("}}{}\n", ++n < funcs_.size() ? "," : "");
    }
    j += "  ],\n  \"suspects\": [\n";
    for (size_t k = 0; k < suspects_.size(); ++k)
        j += std::format("    {{\"function\": \"0x{:08X}\", \"why\": \"{}\"}}{}\n", suspects_[k].start, suspects_[k].why,
                         k + 1 < suspects_.size() ? "," : "");
    j += "  ]\n}\n";
    return j;
}

}  // namespace qilin
