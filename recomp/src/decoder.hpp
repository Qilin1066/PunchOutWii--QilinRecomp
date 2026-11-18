// Broadway (Wii CPU) instruction decoder.
//
// Broadway is an IBM PowerPC 750CL: the 32-bit PowerPC instruction set the
// 750 family implements, plus Nintendo's paired-single extension (Gekko's
// `ps_*`, `psq_*` and `dcbz_l`). Every instruction is one big-endian word.
//
// The decoder is table-driven: one `Spec` per base mnemonic, with its opcode,
// extended opcode and operand fields. Every bit a spec doesn't define must be
// zero, so reserved bits set, invalid forms (a load with update whose base is
// its target, a `bc` with a non-zero "z" bit, ...) and anything outside the 750CL's
// set decode as invalid. A retail compiler never emits them, and the recompiler
// fails loudly on them (see docs/roadmap-and-design-targets.md).
//
// The text form (`disasm`) follows GNU objdump's `-M broadway,raw` output, so
// tools/check/verify_decoder.py can compare the two word by word.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace qilin::ppc {

// Every Broadway instruction: identifier and mnemonic. Rc ("."), OE ("o"),
// LK ("l") and AA ("a") are bits of the same instruction, not separate entries.
#define QILIN_PPC_OPS(X)                                                                     \
    /* integer arithmetic */                                                                 \
    X(add, "add") X(addc, "addc") X(adde, "adde") X(addi, "addi") X(addic, "addic")          \
    X(addic_rc, "addic.") X(addis, "addis") X(addme, "addme") X(addze, "addze")              \
    X(divw, "divw") X(divwu, "divwu") X(mulhw, "mulhw") X(mulhwu, "mulhwu")                  \
    X(mulli, "mulli") X(mullw, "mullw") X(neg, "neg") X(subf, "subf") X(subfc, "subfc")      \
    X(subfe, "subfe") X(subfic, "subfic") X(subfme, "subfme") X(subfze, "subfze")            \
    /* integer compare */                                                                    \
    X(cmp, "cmp") X(cmpi, "cmpi") X(cmpl, "cmpl") X(cmpli, "cmpli")                          \
    /* integer logical */                                                                    \
    X(and_, "and") X(andc, "andc") X(andi_rc, "andi.") X(andis_rc, "andis.")                 \
    X(cntlzw, "cntlzw") X(eqv, "eqv") X(extsb, "extsb") X(extsh, "extsh") X(nand, "nand")    \
    X(nor, "nor") X(or_, "or") X(orc, "orc") X(ori, "ori") X(oris, "oris") X(xor_, "xor")    \
    X(xori, "xori") X(xoris, "xoris")                                                        \
    /* rotate and shift */                                                                   \
    X(rlwimi, "rlwimi") X(rlwinm, "rlwinm") X(rlwnm, "rlwnm") X(slw, "slw")                  \
    X(sraw, "sraw") X(srawi, "srawi") X(srw, "srw")                                          \
    /* floating point */                                                                     \
    X(fabs, "fabs") X(fadd, "fadd") X(fadds, "fadds") X(fcmpo, "fcmpo") X(fcmpu, "fcmpu")    \
    X(fctiw, "fctiw") X(fctiwz, "fctiwz") X(fdiv, "fdiv") X(fdivs, "fdivs")                  \
    X(fmadd, "fmadd") X(fmadds, "fmadds") X(fmr, "fmr") X(fmsub, "fmsub")                    \
    X(fmsubs, "fmsubs") X(fmul, "fmul") X(fmuls, "fmuls") X(fnabs, "fnabs")                  \
    X(fneg, "fneg") X(fnmadd, "fnmadd") X(fnmadds, "fnmadds") X(fnmsub, "fnmsub")            \
    X(fnmsubs, "fnmsubs") X(fres, "fres") X(frsp, "frsp") X(frsqrte, "frsqrte")              \
    X(fsel, "fsel") X(fsub, "fsub") X(fsubs, "fsubs")                                        \
    X(mcrfs, "mcrfs") X(mffs, "mffs") X(mtfsb0, "mtfsb0") X(mtfsb1, "mtfsb1")                \
    X(mtfsf, "mtfsf") X(mtfsfi, "mtfsfi")                                                    \
    /* integer load and store */                                                             \
    X(lbz, "lbz") X(lbzu, "lbzu") X(lbzux, "lbzux") X(lbzx, "lbzx")                          \
    X(lha, "lha") X(lhau, "lhau") X(lhaux, "lhaux") X(lhax, "lhax")                          \
    X(lhz, "lhz") X(lhzu, "lhzu") X(lhzux, "lhzux") X(lhzx, "lhzx")                          \
    X(lwz, "lwz") X(lwzu, "lwzu") X(lwzux, "lwzux") X(lwzx, "lwzx")                          \
    X(stb, "stb") X(stbu, "stbu") X(stbux, "stbux") X(stbx, "stbx")                          \
    X(sth, "sth") X(sthu, "sthu") X(sthux, "sthux") X(sthx, "sthx")                          \
    X(stw, "stw") X(stwu, "stwu") X(stwux, "stwux") X(stwx, "stwx")                          \
    X(lhbrx, "lhbrx") X(lwbrx, "lwbrx") X(sthbrx, "sthbrx") X(stwbrx, "stwbrx")              \
    X(lmw, "lmw") X(stmw, "stmw") X(lswi, "lswi") X(lswx, "lswx") X(stswi, "stswi")          \
    X(stswx, "stswx") X(lwarx, "lwarx") X(stwcx_rc, "stwcx.")                                \
    /* floating-point load and store */                                                      \
    X(lfd, "lfd") X(lfdu, "lfdu") X(lfdux, "lfdux") X(lfdx, "lfdx")                          \
    X(lfs, "lfs") X(lfsu, "lfsu") X(lfsux, "lfsux") X(lfsx, "lfsx")                          \
    X(stfd, "stfd") X(stfdu, "stfdu") X(stfdux, "stfdux") X(stfdx, "stfdx")                  \
    X(stfs, "stfs") X(stfsu, "stfsu") X(stfsux, "stfsux") X(stfsx, "stfsx")                  \
    X(stfiwx, "stfiwx")                                                                      \
    /* branch, condition register, system */                                                 \
    X(b, "b") X(bc, "bc") X(bcctr, "bcctr") X(bclr, "bclr")                                  \
    X(crand, "crand") X(crandc, "crandc") X(creqv, "creqv") X(crnand, "crnand")              \
    X(crnor, "crnor") X(cror, "cror") X(crorc, "crorc") X(crxor, "crxor") X(mcrf, "mcrf")    \
    X(sc, "sc") X(rfi, "rfi") X(tw, "tw") X(twi, "twi")                                      \
    X(mcrxr, "mcrxr") X(mfcr, "mfcr") X(mfmsr, "mfmsr") X(mfspr, "mfspr") X(mftb, "mftb")    \
    X(mtcrf, "mtcrf") X(mtmsr, "mtmsr") X(mtspr, "mtspr")                                    \
    X(mfsr, "mfsr") X(mfsrin, "mfsrin") X(mtsr, "mtsr") X(mtsrin, "mtsrin")                  \
    X(tlbie, "tlbie") X(tlbsync, "tlbsync") X(eciwx, "eciwx") X(ecowx, "ecowx")              \
    /* memory synchronisation and caches */                                                  \
    X(eieio, "eieio") X(isync, "isync") X(sync, "sync")                                      \
    X(dcbf, "dcbf") X(dcbi, "dcbi") X(dcbst, "dcbst") X(dcbt, "dcbt") X(dcbtst, "dcbtst")    \
    X(dcbz, "dcbz") X(icbi, "icbi")                                                          \
    /* Gekko/Broadway paired singles */                                                      \
    X(psq_l, "psq_l") X(psq_lu, "psq_lu") X(psq_lux, "psq_lux") X(psq_lx, "psq_lx")          \
    X(psq_st, "psq_st") X(psq_stu, "psq_stu") X(psq_stux, "psq_stux")                        \
    X(psq_stx, "psq_stx") X(ps_abs, "ps_abs") X(ps_add, "ps_add") X(ps_cmpo0, "ps_cmpo0")    \
    X(ps_cmpo1, "ps_cmpo1") X(ps_cmpu0, "ps_cmpu0") X(ps_cmpu1, "ps_cmpu1")                  \
    X(ps_div, "ps_div") X(ps_madd, "ps_madd") X(ps_madds0, "ps_madds0")                      \
    X(ps_madds1, "ps_madds1") X(ps_merge00, "ps_merge00") X(ps_merge01, "ps_merge01")        \
    X(ps_merge10, "ps_merge10") X(ps_merge11, "ps_merge11") X(ps_mr, "ps_mr")                \
    X(ps_msub, "ps_msub") X(ps_mul, "ps_mul") X(ps_muls0, "ps_muls0")                        \
    X(ps_muls1, "ps_muls1") X(ps_nabs, "ps_nabs") X(ps_neg, "ps_neg")                        \
    X(ps_nmadd, "ps_nmadd") X(ps_nmsub, "ps_nmsub") X(ps_res, "ps_res")                      \
    X(ps_rsqrte, "ps_rsqrte") X(ps_sel, "ps_sel") X(ps_sub, "ps_sub") X(ps_sum0, "ps_sum0")  \
    X(ps_sum1, "ps_sum1") X(dcbz_l, "dcbz_l")

enum class Op : uint16_t {
    invalid,
#define QILIN_PPC_ENUM(id, name) id,
    QILIN_PPC_OPS(QILIN_PPC_ENUM)
#undef QILIN_PPC_ENUM
    count_
};

// Operand fields, in the order objdump prints them. `zero` prints "0" and
// defines no bits: it stands for a field later Power ISA versions added (a
// branch hint, a cache-line size) that is reserved on Broadway.
enum class Opnd : uint8_t {
    none,
    rD,         // GPR in bits 6-10 (also rS)
    rA,         // GPR in bits 11-15
    rA0,        // bits 11-15, where 0 means the value 0, not r0
    rB,         // GPR in bits 16-20
    fD,         // FPR in bits 6-10 (also frS)
    fA, fB, fC, // FPRs in bits 11-15, 16-20, 21-25
    crfD,       // CR field in bits 6-8
    crfS,       // CR field in bits 11-13
    bf,         // FPSCR field in bits 6-8 (mtfsfi), printed as a number
    simm,       // signed 16-bit immediate
    uimm,       // unsigned 16-bit immediate
    d_rA0,      // 16-bit signed displacement and base: d(rA|0)
    sh, mb, me, // rotate amount and mask bounds
    nb,         // byte count for lswi/stswi (0 means 32)
    spr, tbr,   // special-purpose / time-base register number (halves swapped)
    to,         // trap condition
    bo, bi,     // branch options and condition bit
    bd,         // conditional branch target (relative unless AA)
    li,         // branch target (relative unless AA)
    crbD, crbA, crbB,  // condition-register bits
    fpscr_bit,  // FPSCR bit for mtfsb0/mtfsb1
    crm,        // CR field mask for mtcrf
    fm,         // FPSCR field mask for mtfsf
    imm,        // 4-bit immediate for mtfsfi
    sr,         // segment register
    psq_d_rA0,  // 12-bit signed displacement and base for psq_l/psq_st
    psq_w, psq_i,    // W and I fields of psq_l/psq_st
    psqx_w, psqx_i,  // W and I fields of the indexed forms
    zero,
};

// Instruction properties, for the analysis passes and the lifter.
enum Flag : uint32_t {
    kRc = 1u << 0,          // has a record bit (bit 31): "." sets CR0/CR1
    kOe = 1u << 1,          // has an overflow-enable bit (bit 21): "o"
    kLk = 1u << 2,          // has a link bit (bit 31): "l"
    kAa = 1u << 3,          // has an absolute-address bit (bit 30): "a"
    kBranch = 1u << 4,
    kLoad = 1u << 5,
    kStore = 1u << 6,
    kFloat = 1u << 7,       // reads or writes FPRs/FPSCR
    kPaired = 1u << 8,      // paired-single extension
    kPrivileged = 1u << 9,  // supervisor only (MSR, segment registers, TLB, rfi, dcbi)
    kCache = 1u << 10,      // cache management
    kSync = 1u << 11,       // memory or instruction synchronisation
    // Operand constraints that are invalid forms when broken:
    kRaNonZero = 1u << 16,      // rA != 0 (stores, FP and paired loads with update)
    kRaNotZeroOrRd = 1u << 17,  // rA != 0 and rA != rD (integer loads with update)
    kLmw = 1u << 18,            // rA outside the registers lmw loads
    kLswi = 1u << 19,           // rA outside the registers lswi loads
    kValidBo = 1u << 20,        // BO is one of the defined encodings
    kBoNoDecrement = 1u << 21,  // bcctr: BO must not decrement CTR
    kLswx = 1u << 22,           // rD differs from rA and rB
};

struct Spec {
    Op op = Op::invalid;
    std::string_view name;
    uint32_t match = 0;  // opcode bits
    uint32_t mask = 0;   // which bits `match` covers
    Opnd operands[5] = {};
    uint32_t flags = 0;
};

// The bits an operand field occupies, as a mask. (Comments here number bits
// as IBM's manuals do, from 0 at the most significant.)
uint32_t field_mask(Opnd o);

struct Insn {
    uint32_t word = 0;
    Op op = Op::invalid;

    bool valid() const { return op != Op::invalid; }

    // Fields. The caller knows which ones the instruction has.
    unsigned primary() const { return word >> 26; }
    unsigned rd() const { return (word >> 21) & 31; }  // also rS, frD, frS, TO, BO, crbD
    unsigned ra() const { return (word >> 16) & 31; }  // also frA, BI, crbA
    unsigned rb() const { return (word >> 11) & 31; }  // also frB, crbB, SH, NB
    unsigned rc_reg() const { return (word >> 6) & 31; }  // frC
    unsigned crfd() const { return (word >> 23) & 7; }
    unsigned crfs() const { return (word >> 18) & 7; }
    unsigned mb() const { return (word >> 6) & 31; }
    unsigned me() const { return (word >> 1) & 31; }
    int32_t simm() const { return static_cast<int16_t>(word & 0xFFFF); }
    uint32_t uimm() const { return word & 0xFFFF; }
    unsigned spr() const { return ((word >> 16) & 31) | (((word >> 11) & 31) << 5); }
    unsigned crm() const { return (word >> 12) & 0xFF; }
    unsigned fm() const { return (word >> 17) & 0xFF; }
    unsigned sr() const { return (word >> 16) & 15; }
    bool rc() const { return word & 1; }   // also LK
    bool oe() const { return word & 0x400; }
    bool aa() const { return word & 2; }
    bool lk() const { return word & 1; }
    // psq_l / psq_st: 12-bit signed displacement, W and I.
    int32_t psq_d() const { return static_cast<int32_t>(word << 20) >> 20; }
    unsigned psq_w() const { return (word >> 15) & 1; }
    unsigned psq_i() const { return (word >> 12) & 7; }
    // psq_lx / psq_stx and their update forms.
    unsigned psqx_w() const { return (word >> 10) & 1; }
    unsigned psqx_i() const { return (word >> 7) & 7; }

    // Target of b / bc at address pc.
    uint32_t branch_target(uint32_t pc) const;
};

// The spec of every op (index by Op), and all of them in table order.
const Spec& spec(Op op);
std::span<const Spec> specs();

Insn decode(uint32_t word);

// Mnemonic with its o / . / l / a suffixes, e.g. "addo.", "bcl", "bla".
std::string mnemonic(const Insn& insn);
// Text of a valid instruction at address pc, objdump `-M broadway,raw` style:
// "addi r3,r1,8", "bc 12,eq,0x80003104", "psq_l f1,8(r1),0,0".
std::string disasm(const Insn& insn, uint32_t pc);

}  // namespace qilin::ppc
