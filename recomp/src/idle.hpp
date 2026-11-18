// Poll loops: loops that only wait for memory to change under them.
//
// On the console an interrupt handler (or another device) changes that
// memory in the middle of the loop. Lifted code takes interrupts only at
// safe points, so the back edge of such a loop calls hooks->idle, which is
// one (docs/runtime.md, "Interrupts").
//
// A loop is a poll loop when one pass can't tell it from the last: nothing
// in it stores (except to its own stack frame), calls anything but pure
// functions, or carries a register or stack slot from one pass to the next. That last
// rule is what tells `while (flag == 0) {}` from `while (*p++) {}`: the
// scan advances a pointer, the poll recomputes everything from memory.
// A false positive costs speed (the hook may sleep up to a millisecond),
// never correctness: an interrupt may arrive at any instruction.
#pragma once

#include <climits>
#include <cstdint>
#include <functional>
#include <span>
#include <utility>
#include <vector>

#include "decoder.hpp"

namespace qilin {

// GPRs as bit masks (bit n: rn).
struct RegEffects {
    uint32_t reads = 0, writes = 0;
    // A stack slot (offset from r1) read or written: locals kept in memory
    // carry values between passes just as registers do.
    static constexpr int32_t kNone = INT32_MIN;
    int32_t stack_load = kNone, stack_store = kNone;
};

// The GPR effects of a non-branch instruction, or false if it can do more
// than read memory and compute into GPRs and CR: a store off the stack,
// anything with SPRs, FP, cache, sync, traps, or the carry chain.
bool simple_effects(const ppc::Insn& in, RegEffects& e);

// A function's effects when called, if it's pure: only simple
// instructions, branches inside it, returns, and tail calls to pure
// functions. `insns(start)` gives a function's instructions (empty if none).
using FunctionInsns = std::function<std::vector<std::pair<uint32_t, ppc::Insn>>(uint32_t)>;
bool pure_function(uint32_t start, const FunctionInsns& insns, RegEffects& e, int depth = 0);

// Is the branch back at `pc` to `target` (within `insns`, by address) a poll
// loop? `pure_call` answers for the functions it calls.
bool poll_loop(std::span<const std::pair<uint32_t, ppc::Insn>> insns, uint32_t target, uint32_t pc,
               const std::function<bool(uint32_t, RegEffects&)>& pure_call);

}  // namespace qilin
