#include "decoder.hpp"

#include <array>
#include <format>
#include <stdexcept>

namespace qilin::ppc {

namespace {

constexpr std::string_view kNames[] = {
    "(invalid)",
#define QILIN_PPC_NAME(id, name) name,
    QILIN_PPC_OPS(QILIN_PPC_NAME)
#undef QILIN_PPC_NAME
};
static_assert(std::size(kNames) == static_cast<size_t>(Op::count_));

constexpr uint32_t kPrimary = 0xFC000000u;

struct Match {
    uint32_t match, mask;
};
// Primary opcode only (D, I, B, M forms).
constexpr Match P(unsigned p) { return {p << 26, kPrimary}; }
// 10-bit extended opcode in bits 1-10 (X, XL, XFX, XFL forms).
constexpr Match X(unsigned p, unsigned xo) { return {(p << 26) | (xo << 1), kPrimary | (0x3FFu << 1)}; }
// 9-bit extended opcode, bit 10 is OE (XO form).
constexpr Match XO(unsigned p, unsigned xo) { return {(p << 26) | (xo << 1), kPrimary | (0x1FFu << 1)}; }
// 6-bit extended opcode in bits 1-6 (paired-single indexed loads and stores).
constexpr Match X6(unsigned p, unsigned xo) { return {(p << 26) | (xo << 1), kPrimary | (0x3Fu << 1)}; }
// 5-bit extended opcode in bits 1-5 (A form).
constexpr Match A(unsigned p, unsigned xo) { return {(p << 26) | (xo << 1), kPrimary | (0x1Fu << 1)}; }
// The same with fixed bits added.
constexpr Match with(Match m, uint32_t bits) { return {m.match | bits, m.mask | bits}; }

using enum Opnd;

struct Row {
    Op op;
    Match m;
    std::array<Opnd, 5> ops;
    uint32_t flags;
};

constexpr uint32_t kRcOe = kRc | kOe;
constexpr uint32_t kLd = kLoad;
constexpr uint32_t kSt = kStore;
constexpr uint32_t kFL = kFloat | kLoad;
constexpr uint32_t kFS = kFloat | kStore;
constexpr uint32_t kF = kFloat;
constexpr uint32_t kFRc = kFloat | kRc;
constexpr uint32_t kPS = kFloat | kPaired;
constexpr uint32_t kPSRc = kFloat | kPaired | kRc;
constexpr uint32_t kPriv = kPrivileged;

// In decode-table order: within an extended primary opcode, the forms with
// the widest extended opcode come first. No two rows share an encoding
// (checked when the tables are built).
constexpr Row kRows[] = {
    // ---- primary opcodes ----
    {Op::twi, P(3), {to, rA, simm}, 0},
    {Op::mulli, P(7), {rD, rA, simm}, 0},
    {Op::subfic, P(8), {rD, rA, simm}, 0},
    {Op::cmpli, P(10), {crfD, zero, rA, uimm}, 0},
    {Op::cmpi, P(11), {crfD, zero, rA, simm}, 0},
    {Op::addic, P(12), {rD, rA, simm}, 0},
    {Op::addic_rc, P(13), {rD, rA, simm}, 0},
    {Op::addi, P(14), {rD, rA0, simm}, 0},
    {Op::addis, P(15), {rD, rA0, simm}, 0},
    {Op::bc, P(16), {bo, bi, bd}, kBranch | kLk | kAa | kValidBo},
    {Op::sc, with(P(17), 2), {zero}, 0},
    {Op::b, P(18), {li}, kBranch | kLk | kAa},
    {Op::rlwimi, P(20), {rA, rD, sh, mb, me}, kRc},
    {Op::rlwinm, P(21), {rA, rD, sh, mb, me}, kRc},
    {Op::rlwnm, P(23), {rA, rD, rB, mb, me}, kRc},
    {Op::ori, P(24), {rA, rD, uimm}, 0},
    {Op::oris, P(25), {rA, rD, uimm}, 0},
    {Op::xori, P(26), {rA, rD, uimm}, 0},
    {Op::xoris, P(27), {rA, rD, uimm}, 0},
    {Op::andi_rc, P(28), {rA, rD, uimm}, 0},
    {Op::andis_rc, P(29), {rA, rD, uimm}, 0},
    {Op::lwz, P(32), {rD, d_rA0}, kLd},
    {Op::lwzu, P(33), {rD, d_rA0}, kLd | kRaNotZeroOrRd},
    {Op::lbz, P(34), {rD, d_rA0}, kLd},
    {Op::lbzu, P(35), {rD, d_rA0}, kLd | kRaNotZeroOrRd},
    {Op::stw, P(36), {rD, d_rA0}, kSt},
    {Op::stwu, P(37), {rD, d_rA0}, kSt | kRaNonZero},
    {Op::stb, P(38), {rD, d_rA0}, kSt},
    {Op::stbu, P(39), {rD, d_rA0}, kSt | kRaNonZero},
    {Op::lhz, P(40), {rD, d_rA0}, kLd},
    {Op::lhzu, P(41), {rD, d_rA0}, kLd | kRaNotZeroOrRd},
    {Op::lha, P(42), {rD, d_rA0}, kLd},
    {Op::lhau, P(43), {rD, d_rA0}, kLd | kRaNotZeroOrRd},
    {Op::sth, P(44), {rD, d_rA0}, kSt},
    {Op::sthu, P(45), {rD, d_rA0}, kSt | kRaNonZero},
    {Op::lmw, P(46), {rD, d_rA0}, kLd | kLmw},
    {Op::stmw, P(47), {rD, d_rA0}, kSt},
    {Op::lfs, P(48), {fD, d_rA0}, kFL},
    {Op::lfsu, P(49), {fD, d_rA0}, kFL | kRaNonZero},
    {Op::lfd, P(50), {fD, d_rA0}, kFL},
    {Op::lfdu, P(51), {fD, d_rA0}, kFL | kRaNonZero},
    {Op::stfs, P(52), {fD, d_rA0}, kFS},
    {Op::stfsu, P(53), {fD, d_rA0}, kFS | kRaNonZero},
    {Op::stfd, P(54), {fD, d_rA0}, kFS},
    {Op::stfdu, P(55), {fD, d_rA0}, kFS | kRaNonZero},
    {Op::psq_l, P(56), {fD, psq_d_rA0, psq_w, psq_i}, kPS | kLoad},
    {Op::psq_lu, P(57), {fD, psq_d_rA0, psq_w, psq_i}, kPS | kLoad | kRaNonZero},
    {Op::psq_st, P(60), {fD, psq_d_rA0, psq_w, psq_i}, kPS | kStore},
    {Op::psq_stu, P(61), {fD, psq_d_rA0, psq_w, psq_i}, kPS | kStore | kRaNonZero},

    // ---- 4: paired singles ----
    {Op::ps_cmpu0, X(4, 0), {crfD, fA, fB}, kPS},
    {Op::ps_cmpo0, X(4, 32), {crfD, fA, fB}, kPS},
    {Op::ps_neg, X(4, 40), {fD, fB}, kPSRc},
    {Op::ps_cmpu1, X(4, 64), {crfD, fA, fB}, kPS},
    {Op::ps_mr, X(4, 72), {fD, fB}, kPSRc},
    {Op::ps_cmpo1, X(4, 96), {crfD, fA, fB}, kPS},
    {Op::ps_nabs, X(4, 136), {fD, fB}, kPSRc},
    {Op::ps_abs, X(4, 264), {fD, fB}, kPSRc},
    {Op::ps_merge00, X(4, 528), {fD, fA, fB}, kPSRc},
    {Op::ps_merge01, X(4, 560), {fD, fA, fB}, kPSRc},
    {Op::ps_merge10, X(4, 592), {fD, fA, fB}, kPSRc},
    {Op::ps_merge11, X(4, 624), {fD, fA, fB}, kPSRc},
    {Op::dcbz_l, X(4, 1014), {rA, rB}, kPaired | kCache},
    {Op::psq_lx, X6(4, 6), {fD, rA, rB, psqx_w, psqx_i}, kPS | kLoad},
    {Op::psq_stx, X6(4, 7), {fD, rA, rB, psqx_w, psqx_i}, kPS | kStore},
    {Op::psq_lux, X6(4, 38), {fD, rA, rB, psqx_w, psqx_i}, kPS | kLoad | kRaNonZero},
    {Op::psq_stux, X6(4, 39), {fD, rA, rB, psqx_w, psqx_i}, kPS | kStore | kRaNonZero},
    {Op::ps_sum0, A(4, 10), {fD, fA, fC, fB}, kPSRc},
    {Op::ps_sum1, A(4, 11), {fD, fA, fC, fB}, kPSRc},
    {Op::ps_muls0, A(4, 12), {fD, fA, fC}, kPSRc},
    {Op::ps_muls1, A(4, 13), {fD, fA, fC}, kPSRc},
    {Op::ps_madds0, A(4, 14), {fD, fA, fC, fB}, kPSRc},
    {Op::ps_madds1, A(4, 15), {fD, fA, fC, fB}, kPSRc},
    {Op::ps_div, A(4, 18), {fD, fA, fB}, kPSRc},
    {Op::ps_sub, A(4, 20), {fD, fA, fB}, kPSRc},
    {Op::ps_add, A(4, 21), {fD, fA, fB}, kPSRc},
    {Op::ps_sel, A(4, 23), {fD, fA, fC, fB}, kPSRc},
    {Op::ps_res, A(4, 24), {fD, fB}, kPSRc},
    {Op::ps_mul, A(4, 25), {fD, fA, fC}, kPSRc},
    {Op::ps_rsqrte, A(4, 26), {fD, fB}, kPSRc},
    {Op::ps_msub, A(4, 28), {fD, fA, fC, fB}, kPSRc},
    {Op::ps_madd, A(4, 29), {fD, fA, fC, fB}, kPSRc},
    {Op::ps_nmsub, A(4, 30), {fD, fA, fC, fB}, kPSRc},
    {Op::ps_nmadd, A(4, 31), {fD, fA, fC, fB}, kPSRc},

    // ---- 19: branch to LR/CTR, CR logical, rfi, isync ----
    {Op::mcrf, X(19, 0), {crfD, crfS}, 0},
    {Op::bclr, X(19, 16), {bo, bi, zero}, kBranch | kLk | kValidBo},
    {Op::crnor, X(19, 33), {crbD, crbA, crbB}, 0},
    {Op::rfi, X(19, 50), {}, kBranch | kPriv},
    {Op::crandc, X(19, 129), {crbD, crbA, crbB}, 0},
    {Op::isync, X(19, 150), {}, kSync},
    {Op::crxor, X(19, 193), {crbD, crbA, crbB}, 0},
    {Op::crnand, X(19, 225), {crbD, crbA, crbB}, 0},
    {Op::crand, X(19, 257), {crbD, crbA, crbB}, 0},
    {Op::creqv, X(19, 289), {crbD, crbA, crbB}, 0},
    {Op::crorc, X(19, 417), {crbD, crbA, crbB}, 0},
    {Op::cror, X(19, 449), {crbD, crbA, crbB}, 0},
    {Op::bcctr, X(19, 528), {bo, bi, zero}, kBranch | kLk | kValidBo | kBoNoDecrement},

    // ---- 31: integer register forms, indexed loads and stores, system ----
    {Op::cmp, X(31, 0), {crfD, zero, rA, rB}, 0},
    {Op::tw, X(31, 4), {to, rA, rB}, 0},
    {Op::mfcr, X(31, 19), {rD}, 0},
    {Op::lwarx, X(31, 20), {rD, rA0, rB, zero}, kLd},
    {Op::lwzx, X(31, 23), {rD, rA0, rB}, kLd},
    {Op::slw, X(31, 24), {rA, rD, rB}, kRc},
    {Op::cntlzw, X(31, 26), {rA, rD}, kRc},
    {Op::and_, X(31, 28), {rA, rD, rB}, kRc},
    {Op::cmpl, X(31, 32), {crfD, zero, rA, rB}, 0},
    {Op::dcbst, X(31, 54), {rA0, rB}, kCache},
    {Op::lwzux, X(31, 55), {rD, rA, rB}, kLd | kRaNotZeroOrRd},
    {Op::andc, X(31, 60), {rA, rD, rB}, kRc},
    {Op::mfmsr, X(31, 83), {rD}, kPriv},
    {Op::dcbf, X(31, 86), {rA0, rB, zero}, kCache},
    {Op::lbzx, X(31, 87), {rD, rA0, rB}, kLd},
    {Op::lbzux, X(31, 119), {rD, rA, rB}, kLd | kRaNotZeroOrRd},
    {Op::nor, X(31, 124), {rA, rD, rB}, kRc},
    {Op::mtcrf, X(31, 144), {crm, rD}, 0},
    {Op::mtmsr, X(31, 146), {rD, zero}, kPriv},
    {Op::stwcx_rc, with(X(31, 150), 1), {rD, rA0, rB}, kSt},
    {Op::stwx, X(31, 151), {rD, rA0, rB}, kSt},
    {Op::stwux, X(31, 183), {rD, rA, rB}, kSt | kRaNonZero},
    {Op::mtsr, X(31, 210), {sr, rD}, kPriv},
    {Op::stbx, X(31, 215), {rD, rA0, rB}, kSt},
    {Op::mtsrin, X(31, 242), {rD, rB}, kPriv},
    {Op::dcbtst, X(31, 246), {rA0, rB}, kCache},
    {Op::stbux, X(31, 247), {rD, rA, rB}, kSt | kRaNonZero},
    {Op::dcbt, X(31, 278), {rA0, rB}, kCache},
    {Op::lhzx, X(31, 279), {rD, rA0, rB}, kLd},
    {Op::eqv, X(31, 284), {rA, rD, rB}, kRc},
    {Op::tlbie, X(31, 306), {rB, zero}, kPriv},
    {Op::eciwx, X(31, 310), {rD, rA0, rB}, kLd},
    {Op::lhzux, X(31, 311), {rD, rA, rB}, kLd | kRaNotZeroOrRd},
    {Op::xor_, X(31, 316), {rA, rD, rB}, kRc},
    {Op::mfspr, X(31, 339), {rD, spr}, 0},
    {Op::lhax, X(31, 343), {rD, rA0, rB}, kLd},
    {Op::mftb, X(31, 371), {rD, tbr}, 0},
    {Op::lhaux, X(31, 375), {rD, rA, rB}, kLd | kRaNotZeroOrRd},
    {Op::sthx, X(31, 407), {rD, rA0, rB}, kSt},
    {Op::orc, X(31, 412), {rA, rD, rB}, kRc},
    {Op::ecowx, X(31, 438), {rD, rA0, rB}, kSt},
    {Op::sthux, X(31, 439), {rD, rA, rB}, kSt | kRaNonZero},
    {Op::or_, X(31, 444), {rA, rD, rB}, kRc},
    {Op::mtspr, X(31, 467), {spr, rD}, 0},
    {Op::dcbi, X(31, 470), {rA0, rB}, kCache | kPriv},
    {Op::nand, X(31, 476), {rA, rD, rB}, kRc},
    {Op::mcrxr, X(31, 512), {crfD}, 0},
    {Op::lswx, X(31, 533), {rD, rA0, rB}, kLd | kLswx},
    {Op::lwbrx, X(31, 534), {rD, rA0, rB}, kLd},
    {Op::lfsx, X(31, 535), {fD, rA0, rB}, kFL},
    {Op::srw, X(31, 536), {rA, rD, rB}, kRc},
    {Op::tlbsync, X(31, 566), {}, kPriv | kSync},
    {Op::lfsux, X(31, 567), {fD, rA, rB}, kFL | kRaNonZero},
    {Op::mfsr, X(31, 595), {rD, sr}, kPriv},
    {Op::lswi, X(31, 597), {rD, rA0, nb}, kLd | kLswi},
    {Op::sync, X(31, 598), {zero}, kSync},
    {Op::lfdx, X(31, 599), {fD, rA0, rB}, kFL},
    {Op::lfdux, X(31, 631), {fD, rA, rB}, kFL | kRaNonZero},
    {Op::mfsrin, X(31, 659), {rD, rB}, kPriv},
    {Op::stswx, X(31, 661), {rD, rA0, rB}, kSt},
    {Op::stwbrx, X(31, 662), {rD, rA0, rB}, kSt},
    {Op::stfsx, X(31, 663), {fD, rA0, rB}, kFS},
    {Op::stfsux, X(31, 695), {fD, rA, rB}, kFS | kRaNonZero},
    {Op::stswi, X(31, 725), {rD, rA0, nb}, kSt},
    {Op::stfdx, X(31, 727), {fD, rA0, rB}, kFS},
    {Op::stfdux, X(31, 759), {fD, rA, rB}, kFS | kRaNonZero},
    {Op::lhbrx, X(31, 790), {rD, rA0, rB}, kLd},
    {Op::sraw, X(31, 792), {rA, rD, rB}, kRc},
    {Op::srawi, X(31, 824), {rA, rD, sh}, kRc},
    {Op::eieio, X(31, 854), {}, kSync},
    {Op::sthbrx, X(31, 918), {rD, rA0, rB}, kSt},
    {Op::extsh, X(31, 922), {rA, rD}, kRc},
    {Op::extsb, X(31, 954), {rA, rD}, kRc},
    {Op::icbi, X(31, 982), {rA0, rB}, kCache},
    {Op::stfiwx, X(31, 983), {fD, rA0, rB}, kFS},
    {Op::dcbz, X(31, 1014), {rA0, rB}, kCache | kStore},
    // XO form: bit 10 is OE where the instruction has it, reserved otherwise.
    {Op::subfc, XO(31, 8), {rD, rA, rB}, kRcOe},
    {Op::addc, XO(31, 10), {rD, rA, rB}, kRcOe},
    {Op::mulhwu, XO(31, 11), {rD, rA, rB}, kRc},
    {Op::subf, XO(31, 40), {rD, rA, rB}, kRcOe},
    {Op::mulhw, XO(31, 75), {rD, rA, rB}, kRc},
    {Op::neg, XO(31, 104), {rD, rA}, kRcOe},
    {Op::subfe, XO(31, 136), {rD, rA, rB}, kRcOe},
    {Op::adde, XO(31, 138), {rD, rA, rB}, kRcOe},
    {Op::subfze, XO(31, 200), {rD, rA}, kRcOe},
    {Op::addze, XO(31, 202), {rD, rA}, kRcOe},
    {Op::subfme, XO(31, 232), {rD, rA}, kRcOe},
    {Op::addme, XO(31, 234), {rD, rA}, kRcOe},
    {Op::mullw, XO(31, 235), {rD, rA, rB}, kRcOe},
    {Op::add, XO(31, 266), {rD, rA, rB}, kRcOe},
    {Op::divwu, XO(31, 459), {rD, rA, rB}, kRcOe},
    {Op::divw, XO(31, 491), {rD, rA, rB}, kRcOe},

    // ---- 59: single-precision arithmetic ----
    {Op::fdivs, A(59, 18), {fD, fA, fB}, kFRc},
    {Op::fsubs, A(59, 20), {fD, fA, fB}, kFRc},
    {Op::fadds, A(59, 21), {fD, fA, fB}, kFRc},
    {Op::fres, A(59, 24), {fD, fB, zero}, kFRc},
    {Op::fmuls, A(59, 25), {fD, fA, fC}, kFRc},
    {Op::fmsubs, A(59, 28), {fD, fA, fC, fB}, kFRc},
    {Op::fmadds, A(59, 29), {fD, fA, fC, fB}, kFRc},
    {Op::fnmsubs, A(59, 30), {fD, fA, fC, fB}, kFRc},
    {Op::fnmadds, A(59, 31), {fD, fA, fC, fB}, kFRc},

    // ---- 63: double-precision arithmetic, moves, compares, FPSCR ----
    {Op::fcmpu, X(63, 0), {crfD, fA, fB}, kF},
    {Op::frsp, X(63, 12), {fD, fB}, kFRc},
    {Op::fctiw, X(63, 14), {fD, fB}, kFRc},
    {Op::fctiwz, X(63, 15), {fD, fB}, kFRc},
    {Op::fcmpo, X(63, 32), {crfD, fA, fB}, kF},
    {Op::mtfsb1, X(63, 38), {fpscr_bit}, kFRc},
    {Op::fneg, X(63, 40), {fD, fB}, kFRc},
    {Op::mcrfs, X(63, 64), {crfD, crfS}, kF},
    {Op::mtfsb0, X(63, 70), {fpscr_bit}, kFRc},
    {Op::fmr, X(63, 72), {fD, fB}, kFRc},
    {Op::mtfsfi, X(63, 134), {bf, imm}, kFRc},
    {Op::fnabs, X(63, 136), {fD, fB}, kFRc},
    {Op::fabs, X(63, 264), {fD, fB}, kFRc},
    {Op::mffs, X(63, 583), {fD}, kFRc},
    {Op::mtfsf, X(63, 711), {fm, fB}, kFRc},
    {Op::fdiv, A(63, 18), {fD, fA, fB}, kFRc},
    {Op::fsub, A(63, 20), {fD, fA, fB}, kFRc},
    {Op::fadd, A(63, 21), {fD, fA, fB}, kFRc},
    {Op::fsel, A(63, 23), {fD, fA, fC, fB}, kFRc},
    {Op::fmul, A(63, 25), {fD, fA, fC}, kFRc},
    {Op::frsqrte, A(63, 26), {fD, fB, zero}, kFRc},
    {Op::fmsub, A(63, 28), {fD, fA, fC, fB}, kFRc},
    {Op::fmadd, A(63, 29), {fD, fA, fC, fB}, kFRc},
    {Op::fnmsub, A(63, 30), {fD, fA, fC, fB}, kFRc},
    {Op::fnmadd, A(63, 31), {fD, fA, fC, fB}, kFRc},
};
constexpr size_t kRowCount = std::size(kRows);
static_assert(kRowCount == static_cast<size_t>(Op::count_) - 1, "one row per op");

constexpr bool is_extended(unsigned p) { return p == 4 || p == 19 || p == 31 || p == 59 || p == 63; }

struct Tables {
    std::array<Spec, static_cast<size_t>(Op::count_)> by_op{};  // indexed by Op
    std::array<Spec, kRowCount> in_order{};
    std::array<uint32_t, static_cast<size_t>(Op::count_)> defined{};  // bits each op defines
    std::array<uint16_t, 64> primary{};                  // Op for primary-only opcodes
    std::array<std::array<uint16_t, 1024>, 64> ext{};    // Op by bits 1-10, extended opcodes
};

Tables build() {
    Tables t;
    t.by_op[0] = Spec{Op::invalid, kNames[0], 0, 0, {}, 0};
    for (size_t i = 0; i < kRowCount; ++i) {
        const Row& r = kRows[i];
        const auto idx = static_cast<size_t>(r.op);
        if (t.by_op[idx].op != Op::invalid)
            throw std::logic_error(std::format("decoder: {} has two rows", kNames[idx]));
        Spec s{r.op, kNames[idx], r.m.match, r.m.mask, {}, r.flags};
        uint32_t defined = r.m.mask;
        for (size_t k = 0; k < r.ops.size(); ++k) {
            s.operands[k] = r.ops[k];
            defined |= field_mask(r.ops[k]);
        }
        if (r.flags & (kRc | kLk)) defined |= 1;
        if (r.flags & kOe) defined |= 0x400;
        if (r.flags & kAa) defined |= 2;
        t.by_op[idx] = s;
        t.in_order[i] = s;
        t.defined[idx] = defined;

        const unsigned p = r.m.match >> 26;
        const auto code = static_cast<uint16_t>(r.op);
        if (!is_extended(p)) {
            if (t.primary[p])
                throw std::logic_error(std::format("decoder: opcode {} has two rows", p));
            t.primary[p] = code;
            continue;
        }
        const uint32_t xo_mask = r.m.mask & 0x7FEu;  // bits 1-10
        for (uint32_t xo = 0; xo < 1024; ++xo) {
            if (((xo << 1) & xo_mask) != (r.m.match & xo_mask)) continue;
            if (t.ext[p][xo])
                throw std::logic_error(std::format("decoder: {} overlaps {}", kNames[idx], kNames[t.ext[p][xo]]));
            t.ext[p][xo] = code;
        }
    }
    for (size_t i = 1; i < t.by_op.size(); ++i)
        if (t.by_op[i].op == Op::invalid)
            throw std::logic_error(std::format("decoder: {} has no row", kNames[i]));
    return t;
}

const Tables& tables() {
    static const Tables t = build();
    return t;
}

// The BO encodings the 32-bit PowerPC architecture defines; "z" bits must be
// zero ("y", the prediction hint, may be either):
//   0000y 0001y 001zy 0100y 0101y 011zy 1z00y 1z01y 1z1zz
bool valid_bo(unsigned bo) {
    if ((bo & 0x14) == 0) return true;
    if ((bo & 0x14) == 0x04) return (bo & 0x02) == 0;
    if ((bo & 0x14) == 0x10) return (bo & 0x08) == 0;
    return bo == 0x14;
}

bool constraints_hold(uint32_t flags, const Insn& in) {
    const unsigned ra = in.ra(), rd = in.rd();
    if ((flags & kRaNonZero) && ra == 0) return false;
    if ((flags & kRaNotZeroOrRd) && (ra == 0 || ra == rd)) return false;
    if ((flags & kLmw) && ra >= rd) return false;
    if (flags & kLswi) {
        const unsigned bytes = in.rb() ? in.rb() : 32;
        const unsigned regs = (bytes + 3) / 4;
        if (((ra - rd) & 31) < regs) return false;  // rA is among rD .. rD+regs-1 (mod 32)
    }
    if ((flags & kLswx) && (rd == ra || rd == in.rb())) return false;
    if ((flags & kValidBo) && !valid_bo(in.rd())) return false;
    if ((flags & kBoNoDecrement) && !(in.rd() & 4)) return false;
    return true;
}

std::string cr_bit(unsigned bit) {
    static constexpr std::string_view kBits[] = {"lt", "gt", "eq", "so"};
    if (bit < 4) return std::string(kBits[bit]);
    return std::format("4*cr{}+{}", bit / 4, kBits[bit % 4]);
}

std::string gpr_or_zero(unsigned r) { return r ? std::format("r{}", r) : std::string("0"); }

}  // namespace

uint32_t field_mask(Opnd o) {
    switch (o) {
    case rD: case fD: case to: case bo: case crbD: case fpscr_bit: return 0x03E00000u;
    case rA: case rA0: case fA: case bi: case crbA: return 0x001F0000u;
    case rB: case fB: case crbB: case sh: case nb: return 0x0000F800u;
    case fC: case mb: return 0x000007C0u;
    case me: return 0x0000003Eu;
    case crfD: case bf: return 0x03800000u;
    case crfS: return 0x001C0000u;
    case simm: case uimm: return 0x0000FFFFu;
    case d_rA0: return 0x001FFFFFu;
    case spr: case tbr: return 0x001FF800u;
    case bd: return 0x0000FFFCu;
    case li: return 0x03FFFFFCu;
    case crm: return 0x000FF000u;
    case fm: return 0x01FE0000u;
    case imm: return 0x0000F000u;
    case sr: return 0x000F0000u;
    case psq_d_rA0: return 0x001F0FFFu;
    case psq_w: return 0x00008000u;
    case psq_i: return 0x00007000u;
    case psqx_w: return 0x00000400u;
    case psqx_i: return 0x00000380u;
    case none: case zero: return 0;
    }
    return 0;
}

uint32_t Insn::branch_target(uint32_t pc) const {
    const int32_t disp = op == Op::b ? static_cast<int32_t>((word & 0x03FFFFFCu) << 6) >> 6
                                     : static_cast<int16_t>(word & 0xFFFCu);
    return aa() ? static_cast<uint32_t>(disp) : pc + static_cast<uint32_t>(disp);
}

const Spec& spec(Op op) { return tables().by_op[static_cast<size_t>(op)]; }

std::span<const Spec> specs() { return tables().in_order; }

Insn decode(uint32_t word) {
    const Tables& t = tables();
    const unsigned p = word >> 26;
    const uint16_t code = is_extended(p) ? t.ext[p][(word >> 1) & 0x3FF] : t.primary[p];
    if (!code) return {word, Op::invalid};
    const Spec& s = t.by_op[code];
    if ((word & s.mask) != s.match || (word & ~t.defined[code]) != 0) return {word, Op::invalid};
    const Insn in{word, s.op};
    if (!constraints_hold(s.flags, in)) return {word, Op::invalid};
    return in;
}

std::string mnemonic(const Insn& in) {
    const Spec& s = spec(in.op);
    std::string m(s.name);
    if ((s.flags & kOe) && in.oe()) m += 'o';
    if ((s.flags & kRc) && in.rc()) m += '.';
    if ((s.flags & kLk) && in.lk()) m += 'l';
    if ((s.flags & kAa) && in.aa()) m += 'a';
    return m;
}

std::string disasm(const Insn& in, uint32_t pc) {
    if (!in.valid()) return std::format(".long 0x{:x}", in.word);
    const Spec& s = spec(in.op);
    std::string text = mnemonic(in);
    bool first = true;
    for (Opnd o : s.operands) {
        if (o == none) break;
        text += first ? ' ' : ',';
        first = false;
        switch (o) {
        case rD: text += std::format("r{}", in.rd()); break;
        case rA: text += std::format("r{}", in.ra()); break;
        case rA0: text += gpr_or_zero(in.ra()); break;
        case rB: text += std::format("r{}", in.rb()); break;
        case fD: text += std::format("f{}", in.rd()); break;
        case fA: text += std::format("f{}", in.ra()); break;
        case fB: text += std::format("f{}", in.rb()); break;
        case fC: text += std::format("f{}", in.rc_reg()); break;
        case crfD: text += std::format("cr{}", in.crfd()); break;
        case crfS: text += std::format("cr{}", in.crfs()); break;
        case bf: text += std::format("{}", in.crfd()); break;
        case simm: text += std::format("{}", in.simm()); break;
        case uimm: text += std::format("{}", in.uimm()); break;
        case d_rA0: text += std::format("{}({})", in.simm(), gpr_or_zero(in.ra())); break;
        case sh: text += std::format("{}", in.rb()); break;
        case nb: text += std::format("{}", in.rb() ? in.rb() : 32); break;
        case mb: text += std::format("{}", in.mb()); break;
        case me: text += std::format("{}", in.me()); break;
        case spr: case tbr: text += std::format("{}", in.spr()); break;
        case to: case bo: case fpscr_bit: text += std::format("{}", in.rd()); break;
        case bi: text += cr_bit(in.ra()); break;
        case crbD: text += cr_bit(in.rd()); break;
        case crbA: text += cr_bit(in.ra()); break;
        case crbB: text += cr_bit(in.rb()); break;
        case bd: case li: text += std::format("0x{:x}", in.branch_target(pc)); break;
        case crm: text += std::format("{}", in.crm()); break;
        case fm: text += std::format("{}", in.fm()); break;
        case imm: text += std::format("{}", (in.word >> 12) & 15); break;
        case sr: text += std::format("{}", in.sr()); break;
        // objdump prints r0 here although rA = 0 means 0, as in d(rA|0).
        case psq_d_rA0: text += std::format("{}(r{})", in.psq_d(), in.ra()); break;
        case psq_w: text += std::format("{}", in.psq_w()); break;
        case psq_i: text += std::format("{}", in.psq_i()); break;
        case psqx_w: text += std::format("{}", in.psqx_w()); break;
        case psqx_i: text += std::format("{}", in.psqx_i()); break;
        case zero: text += '0'; break;
        case none: break;
        }
    }
    return text;
}

}  // namespace qilin::ppc
