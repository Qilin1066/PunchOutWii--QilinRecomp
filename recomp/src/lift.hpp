// The lifter: one guest function to one C++ function over qilin::rt::Cpu
// (runtime/include/qilin/rt/cpu.hpp).
//
// Slice 1 (v0.4.0): integer arithmetic, logic, rotates and shifts, compares
// and the condition register, integer loads and stores (with update, indexed,
// byte-reversed, multiple, lwarx/stwcx.), dcbz and the cache hints, LR, CTR,
// XER and the GQRs, traps, and all branches and calls. Anything else (floating
// point, paired singles, privileged and system instructions, overflow-enable
// and string forms) makes the function a stub that traps with the reason when
// it's called, so nothing is skipped silently (docs/recompiler-core.md).
#pragma once

#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "cfg.hpp"
#include "decoder.hpp"
#include "idle.hpp"

namespace qilin {

struct LiftInput {
    uint32_t start = 0;
    std::vector<std::pair<uint32_t, ppc::Insn>> insns;  // reachable instructions, by address
    std::vector<JumpTable> jtabs;
    std::set<uint32_t> not_calls;                       // `bl` to the next instruction (reads the PC)
    uint32_t falls_into = 0;                            // the next function, if execution runs into it
    std::function<bool(uint32_t)> is_function;          // a known function start (tail and call targets)
    // A call target that is pure (idle.hpp), with its effects: lets poll loops call getters.
    std::function<bool(uint32_t, RegEffects&)> pure_call;
};

struct Lifted {
    bool ok = false;
    std::string code;   // the definition, when ok
    std::string why;    // why not, when !ok: the first instruction outside the slice
    uint32_t why_at = 0;
};

std::string fn_name(uint32_t address);  // "fn_80004000"

// Is this instruction in the current slice?
bool in_slice(const ppc::Insn& in, std::string* why = nullptr);

Lifted lift(const LiftInput& f);

// The input for one of Cfg's functions.
LiftInput lift_input(const Cfg& cfg, const Function& f);

}  // namespace qilin
