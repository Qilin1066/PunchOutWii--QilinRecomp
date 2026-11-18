#include "idle.hpp"

#include <set>
#include <string_view>

namespace qilin {
using ppc::Insn;
using ppc::Op;
using ppc::Opnd;

namespace {
uint32_t bit(unsigned r) { return 1u << r; }

bool decrements_ctr(const Insn& in) { return !(in.rd() & 4); }  // BO bit 2 clear: CTR is decremented
}  // namespace

bool simple_effects(const Insn& in, RegEffects& e) {
    const ppc::Spec& s = ppc::spec(in.op);
    if (s.flags & (ppc::kBranch | ppc::kFloat | ppc::kPaired | ppc::kPrivileged | ppc::kCache | ppc::kSync)) return false;
    const std::string_view n = s.name;
    switch (in.op) {
        case Op::sc: case Op::tw: case Op::twi: case Op::mtspr: case Op::mfspr: case Op::mtcrf: case Op::mcrxr:
        case Op::lmw: case Op::stmw: case Op::lswi: case Op::lswx: case Op::stswi: case Op::stswx:
        case Op::eciwx: case Op::ecowx:
            return false;
        default: break;
    }
    // The carry chain reads XER: a value carried between passes.
    if (n.starts_with("adde") || n.starts_with("addze") || n.starts_with("addme") || n.starts_with("subfe") ||
        n.starts_with("subfze") || n.starts_with("subfme"))
        return false;
    const auto gpr = [&](Opnd o, unsigned& r) {
        switch (o) {
            case Opnd::rD: r = in.rd(); return true;
            case Opnd::rA: r = in.ra(); return true;
            case Opnd::rB: r = in.rb(); return true;
            case Opnd::rA0: case Opnd::d_rA0: r = in.ra(); return r != 0;
            default: return false;
        }
    };
    const bool store = s.flags & ppc::kStore;
    if (store && (in.ra() != 1 || n.ends_with("u") || n.ends_with("ux"))) return false;  // only into its own frame
    if ((store || (s.flags & ppc::kLoad)) && in.ra() == 1) {  // a stack slot: by displacement only
        if (s.operands[1] != Opnd::d_rA0) return false;
        (store ? e.stack_store : e.stack_load) = in.simm();
    }
    bool first = true;
    for (Opnd o : s.operands) {
        if (o == Opnd::none) break;
        unsigned r;
        const bool is_reg = gpr(o, r);
        if (is_reg) {
            if (first && !store && o != Opnd::rA0 && o != Opnd::d_rA0) e.writes |= bit(r);
            else e.reads |= bit(r);
        }
        first = false;
    }
    if ((s.flags & ppc::kLoad) && (n.ends_with("u") || n.ends_with("ux"))) {  // update forms write the base
        e.reads |= bit(in.ra());
        e.writes |= bit(in.ra());
    }
    return true;
}

bool pure_function(uint32_t start, const FunctionInsns& insns, RegEffects& e, int depth) {
    if (depth > 3) return false;
    const auto code = insns(start);
    if (code.empty()) return false;
    const uint32_t lo = code.front().first, hi = code.back().first;
    uint32_t written = 0;
    for (const auto& [a, in] : code) {
        RegEffects x;
        switch (in.op) {
            case Op::bclr:
                if (in.lk()) return false;
                continue;
            case Op::b: case Op::bc: {
                if (in.lk() || (in.op == Op::bc && decrements_ctr(in))) return false;
                const uint32_t t = in.branch_target(a);
                if (t >= lo && t <= hi) continue;
                if (in.op == Op::bc) return false;
                if (!pure_function(t, insns, x, depth + 1)) return false;  // a tail call
                break;
            }
            default:
                if (!simple_effects(in, x)) return false;
        }
        e.reads |= x.reads & ~written;
        written |= x.writes;
    }
    e.writes |= written;
    return true;
}

bool poll_loop(std::span<const std::pair<uint32_t, Insn>> insns, uint32_t target, uint32_t pc,
               const std::function<bool(uint32_t, RegEffects&)>& pure_call) {
    if (target > pc || pc - target > 64 * 4) return false;
    struct Step {
        uint32_t a;
        const Insn* in;
        RegEffects fx;
    };
    std::vector<Step> body;
    uint32_t expect = target, all_writes = 0;
    std::set<int32_t> slots_stored;
    bool reads_memory = false;
    for (const auto& [a, in] : insns) {
        if (a < target || a > pc) continue;
        if (a != expect) return false;  // not one straight run
        expect = a + 4;
        RegEffects x;
        switch (in.op) {
            case Op::bclr:
                if (in.lk()) return false;
                break;  // a return: leaves the loop
            case Op::b: case Op::bc: {
                if (in.op == Op::bc && decrements_ctr(in)) return false;
                if (in.lk()) {
                    if (!pure_call(in.branch_target(a), x)) return false;
                    reads_memory = true;
                    break;
                }
                const uint32_t t = in.branch_target(a);
                if (t <= a && t != target) return false;  // an inner loop
                break;
            }
            default:
                if (!simple_effects(in, x)) return false;
                if (ppc::spec(in.op).flags & ppc::kLoad) reads_memory = true;
        }
        body.push_back({a, &in, x});
        all_writes |= x.writes;
        if (x.stack_store != RegEffects::kNone) slots_stored.insert(x.stack_store);
    }
    if (expect != pc + 4 || !reads_memory) return false;

    // Nothing read in a pass may come from the pass before: what's defined on
    // every path from the loop's head to an instruction (registers and stack
    // slots), against what it reads. The body branches only forward, so one
    // walk in address order is the whole dataflow.
    struct Defined {
        bool reached = false;
        uint32_t regs = 0;
        std::set<int32_t> slots;
    };
    std::vector<Defined> in_state(body.size());
    in_state[0].reached = true;
    const auto index = [&](uint32_t a) { return static_cast<size_t>((a - target) / 4); };
    const auto merge = [&](size_t i, const Defined& d) {
        Defined& s = in_state[i];
        if (!s.reached) {
            s = d;
            s.reached = true;
            return;
        }
        s.regs &= d.regs;
        std::set<int32_t> both;
        for (int32_t k : s.slots)
            if (d.slots.contains(k)) both.insert(k);
        s.slots = std::move(both);
    };
    for (size_t i = 0; i < body.size(); ++i) {
        if (!in_state[i].reached) continue;
        const Step& st = body[i];
        Defined d = in_state[i];
        if (st.fx.reads & all_writes & ~d.regs) return false;
        if (st.fx.stack_load != RegEffects::kNone && slots_stored.contains(st.fx.stack_load) && !d.slots.contains(st.fx.stack_load))
            return false;
        d.regs |= st.fx.writes;
        if (st.fx.stack_store != RegEffects::kNone) d.slots.insert(st.fx.stack_store);
        const Insn& in = *st.in;
        bool falls_through = true;
        if ((in.op == Op::b || in.op == Op::bc) && !in.lk()) {
            const uint32_t t = in.branch_target(st.a);
            if (t > st.a && t <= pc) merge(index(t), d);  // forward, inside; others leave or go round
            if (in.op == Op::b) falls_through = false;
            if (in.op == Op::bc && (in.rd() & 0x14) == 0x14) falls_through = false;  // branch always
        } else if (in.op == Op::bclr && (in.rd() & 0x14) == 0x14) {
            falls_through = false;
        }
        if (falls_through && i + 1 < body.size()) merge(i + 1, d);
    }
    return true;
}

}  // namespace qilin
