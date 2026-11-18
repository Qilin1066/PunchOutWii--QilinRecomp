// A reference interpreter for the lifter's slice, written separately from the
// lifter: from the PowerPC manual's definitions, on the raw instruction word's
// fields, with CR and XER held as the hardware holds them (packed words) and
// memory read and written byte by byte, big-endian. It shares only the
// decoder's identification of the opcode with the lifter, and for floating
// point the arithmetic primitives of rt::fp (rounding, estimates, the bitwise
// single conversions), which have known-answer tests of their own
// (recomp/tests/tests.cpp); what it checks there is the lifter's wiring: the
// operand fields, which half of an FPR is written, flushing, update forms.
#pragma once

#include <cstdint>

#include "decoder.hpp"

namespace qilin::ref {

struct State {
    uint32_t gpr[32] = {};
    uint32_t cr = 0, xer = 0, lr = 0, ctr = 0;
    uint32_t gqr[8] = {};
    uint64_t ps0[32] = {}, ps1[32] = {};  // FPRs as bit patterns
    bool reserve = false;
    uint32_t reserve_addr = 0;
    uint8_t* mem = nullptr;  // guest address a is mem[a]
    bool trapped = false;
};

// Executes one non-branch instruction at pc. Returns false if it isn't one the
// reference knows (the test then fails: every slice instruction must be here).
bool step(State& s, const ppc::Insn& in, uint32_t pc);

// A conditional branch's decision (and its CTR update): taken or not.
bool branch_taken(State& s, uint32_t bo, uint32_t bi);

}  // namespace qilin::ref
