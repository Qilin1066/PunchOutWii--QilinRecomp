#include "ref.hpp"

#include <cstdint>

namespace qilin::ref {

using ppc::Op;

namespace {

// Fields, by IBM bit numbers (0 = most significant).
uint32_t bits(uint32_t w, unsigned from, unsigned to) { return (w >> (31 - to)) & ((1u << (to - from + 1)) - 1); }
int32_t exts16(uint32_t v) { return static_cast<int32_t>(static_cast<int16_t>(static_cast<uint16_t>(v))); }

uint32_t load(const State& s, uint32_t a, unsigned n) {
    uint32_t v = 0;
    for (unsigned k = 0; k < n; ++k) v = (v << 8) | s.mem[a + k];
    return v;
}
void store(State& s, uint32_t a, uint32_t v, unsigned n) {
    for (unsigned k = 0; k < n; ++k) s.mem[a + k] = static_cast<uint8_t>(v >> (8 * (n - 1 - k)));
}

uint32_t so(const State& s) { return s.xer >> 31; }
uint32_t ca(const State& s) { return (s.xer >> 29) & 1; }
void set_ca(State& s, bool c) { s.xer = c ? (s.xer | 0x20000000u) : (s.xer & ~0x20000000u); }

// CR field n (0..7) set to a 4-bit value.
void set_crf(State& s, unsigned n, uint32_t v) {
    const unsigned shift = 28 - 4 * n;
    s.cr = (s.cr & ~(15u << shift)) | ((v & 15) << shift);
}
uint32_t crbit(const State& s, unsigned n) { return (s.cr >> (31 - n)) & 1; }
void set_crbit(State& s, unsigned n, uint32_t v) { s.cr = (s.cr & ~(1u << (31 - n))) | ((v & 1) << (31 - n)); }

void cr0_from(State& s, uint32_t r) {
    const int64_t v = static_cast<int32_t>(r);
    set_crf(s, 0, (v < 0 ? 8 : v > 0 ? 4 : 2) | so(s));
}

// ROTL32 and MASK(mb, me) as the manual defines them.
uint32_t rotl(uint32_t x, unsigned n) {
    n &= 31;
    return n ? (x << n) | (x >> (32 - n)) : x;
}
uint32_t mask(unsigned mb, unsigned me) {
    uint32_t m = 0;
    for (unsigned i = mb;; i = (i + 1) & 31) {  // bits mb..me, wrapping
        m |= 0x80000000u >> i;
        if (i == me) break;
    }
    return m;
}

}  // namespace

bool branch_taken(State& s, uint32_t bo, uint32_t bi) {
    // BO bits, IBM numbering 0..4 (bit 0 is the 16s place).
    const bool b0 = bo & 16, b1 = bo & 8, b2 = bo & 4, b3 = bo & 2;
    if (!b2) s.ctr = s.ctr - 1;
    const bool ctr_ok = b2 || ((s.ctr != 0) != b3);
    const bool cond_ok = b0 || (crbit(s, bi) == static_cast<uint32_t>(b1));
    return ctr_ok && cond_ok;
}

bool step(State& s, const ppc::Insn& in, uint32_t pc) {
    (void)pc;
    const uint32_t w = in.word;
    const unsigned D = bits(w, 6, 10), A = bits(w, 11, 15), B = bits(w, 16, 20);
    const bool Rc = w & 1;
    uint32_t* r = s.gpr;
    const uint32_t a0 = A ? r[A] : 0;  // (rA|0)
    const uint32_t simm = static_cast<uint32_t>(exts16(bits(w, 16, 31)));
    const uint32_t uimm = bits(w, 16, 31);
    const uint32_t ea_d = a0 + simm, ea_x = a0 + r[B];
    const unsigned SH = bits(w, 16, 20), MB = bits(w, 21, 25), ME = bits(w, 26, 30);
    auto rec_d = [&] { if (Rc) cr0_from(s, r[D]); };
    auto rec_a = [&] { if (Rc) cr0_from(s, r[A]); };
    // Carrying add: x + y + c, with the carry out into XER[CA].
    auto adde = [&](uint32_t x, uint32_t y, uint32_t c) {
        const uint64_t t = static_cast<uint64_t>(x) + y + c;
        set_ca(s, t >> 32);
        return static_cast<uint32_t>(t);
    };
    auto cmp_s = [&](int64_t x, int64_t y) { return (x < y ? 8u : x > y ? 4u : 2u) | so(s); };
    switch (in.op) {
    case Op::addi: r[D] = a0 + simm; return true;
    case Op::addis: r[D] = a0 + (uimm << 16); return true;
    case Op::add: r[D] = r[A] + r[B]; rec_d(); return true;
    case Op::subf: r[D] = ~r[A] + r[B] + 1; rec_d(); return true;
    case Op::neg: r[D] = ~r[A] + 1; rec_d(); return true;
    case Op::addic: r[D] = adde(r[A], simm, 0); return true;
    case Op::addic_rc: r[D] = adde(r[A], simm, 0); cr0_from(s, r[D]); return true;
    case Op::addc: r[D] = adde(r[A], r[B], 0); rec_d(); return true;
    case Op::adde: r[D] = adde(r[A], r[B], ca(s)); rec_d(); return true;
    case Op::addme: r[D] = adde(r[A], 0xFFFFFFFFu, ca(s)); rec_d(); return true;
    case Op::addze: r[D] = adde(r[A], 0, ca(s)); rec_d(); return true;
    case Op::subfc: r[D] = adde(~r[A], r[B], 1); rec_d(); return true;
    case Op::subfe: r[D] = adde(~r[A], r[B], ca(s)); rec_d(); return true;
    case Op::subfic: r[D] = adde(~r[A], simm, 1); return true;
    case Op::subfme: r[D] = adde(~r[A], 0xFFFFFFFFu, ca(s)); rec_d(); return true;
    case Op::subfze: r[D] = adde(~r[A], 0, ca(s)); rec_d(); return true;
    case Op::mulli: r[D] = static_cast<uint32_t>(static_cast<int64_t>(static_cast<int32_t>(r[A])) * static_cast<int32_t>(simm)); return true;
    case Op::mullw: r[D] = static_cast<uint32_t>(static_cast<int64_t>(static_cast<int32_t>(r[A])) * static_cast<int32_t>(r[B])); rec_d(); return true;
    case Op::mulhw: r[D] = static_cast<uint32_t>((static_cast<int64_t>(static_cast<int32_t>(r[A])) * static_cast<int32_t>(r[B])) >> 32); rec_d(); return true;
    case Op::mulhwu: r[D] = static_cast<uint32_t>((static_cast<uint64_t>(r[A]) * r[B]) >> 32); rec_d(); return true;
    case Op::divw: {
        const int64_t x = static_cast<int32_t>(r[A]), y = static_cast<int32_t>(r[B]);
        const int64_t q = y == 0 ? 0 : x / y;
        // Undefined in the architecture: no quotient, or one that doesn't fit.
        r[D] = (y == 0 || q > INT32_MAX) ? (x < 0 ? 0xFFFFFFFFu : 0u) : static_cast<uint32_t>(q);
        rec_d();
        return true;
    }
    case Op::divwu: r[D] = r[B] ? r[A] / r[B] : 0; rec_d(); return true;
    case Op::cmp: set_crf(s, bits(w, 6, 8), cmp_s(static_cast<int32_t>(r[A]), static_cast<int32_t>(r[B]))); return true;
    case Op::cmpi: set_crf(s, bits(w, 6, 8), cmp_s(static_cast<int32_t>(r[A]), static_cast<int32_t>(simm))); return true;
    case Op::cmpl: set_crf(s, bits(w, 6, 8), cmp_s(r[A], r[B])); return true;
    case Op::cmpli: set_crf(s, bits(w, 6, 8), cmp_s(r[A], uimm)); return true;
    case Op::and_: r[A] = r[D] & r[B]; rec_a(); return true;
    case Op::andc: r[A] = r[D] & ~r[B]; rec_a(); return true;
    case Op::or_: r[A] = r[D] | r[B]; rec_a(); return true;
    case Op::orc: r[A] = r[D] | ~r[B]; rec_a(); return true;
    case Op::xor_: r[A] = r[D] ^ r[B]; rec_a(); return true;
    case Op::nor: r[A] = ~(r[D] | r[B]); rec_a(); return true;
    case Op::nand: r[A] = ~(r[D] & r[B]); rec_a(); return true;
    case Op::eqv: r[A] = ~(r[D] ^ r[B]); rec_a(); return true;
    case Op::andi_rc: r[A] = r[D] & uimm; cr0_from(s, r[A]); return true;
    case Op::andis_rc: r[A] = r[D] & (uimm << 16); cr0_from(s, r[A]); return true;
    case Op::ori: r[A] = r[D] | uimm; return true;
    case Op::oris: r[A] = r[D] | (uimm << 16); return true;
    case Op::xori: r[A] = r[D] ^ uimm; return true;
    case Op::xoris: r[A] = r[D] ^ (uimm << 16); return true;
    case Op::cntlzw: {
        uint32_t n = 0;
        while (n < 32 && !(r[D] & (0x80000000u >> n))) ++n;
        r[A] = n;
        rec_a();
        return true;
    }
    case Op::extsb: r[A] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(r[D] & 0xFF))); rec_a(); return true;
    case Op::extsh: r[A] = static_cast<uint32_t>(exts16(r[D])); rec_a(); return true;
    case Op::rlwinm: r[A] = rotl(r[D], SH) & mask(MB, ME); rec_a(); return true;
    case Op::rlwnm: r[A] = rotl(r[D], r[B] & 31) & mask(MB, ME); rec_a(); return true;
    case Op::rlwimi: {
        const uint32_t m = mask(MB, ME);
        r[A] = (rotl(r[D], SH) & m) | (r[A] & ~m);
        rec_a();
        return true;
    }
    case Op::slw: { const uint32_t n = r[B] & 63; r[A] = n > 31 ? 0 : r[D] << n; rec_a(); return true; }
    case Op::srw: { const uint32_t n = r[B] & 63; r[A] = n > 31 ? 0 : r[D] >> n; rec_a(); return true; }
    case Op::sraw: case Op::srawi: {
        const uint32_t n = in.op == Op::srawi ? SH : (r[B] & 63);
        const int64_t v = static_cast<int32_t>(r[D]);
        const int64_t res = n > 31 ? (v < 0 ? -1 : 0) : (v >> n);
        // CA: negative, and a one shifted out.
        const bool lost = n > 31 ? (r[D] != 0) : (n && (r[D] << (32 - n)) != 0);
        set_ca(s, v < 0 && lost);
        r[A] = static_cast<uint32_t>(res);
        rec_a();
        return true;
    }
    // Loads and stores.
    case Op::lbz: r[D] = load(s, ea_d, 1); return true;
    case Op::lhz: r[D] = load(s, ea_d, 2); return true;
    case Op::lha: r[D] = static_cast<uint32_t>(exts16(load(s, ea_d, 2))); return true;
    case Op::lwz: r[D] = load(s, ea_d, 4); return true;
    case Op::lbzx: r[D] = load(s, ea_x, 1); return true;
    case Op::lhzx: r[D] = load(s, ea_x, 2); return true;
    case Op::lhax: r[D] = static_cast<uint32_t>(exts16(load(s, ea_x, 2))); return true;
    case Op::lwzx: r[D] = load(s, ea_x, 4); return true;
    case Op::lbzu: { const uint32_t ea = r[A] + simm; r[D] = load(s, ea, 1); r[A] = ea; return true; }
    case Op::lhzu: { const uint32_t ea = r[A] + simm; r[D] = load(s, ea, 2); r[A] = ea; return true; }
    case Op::lhau: { const uint32_t ea = r[A] + simm; r[D] = static_cast<uint32_t>(exts16(load(s, ea, 2))); r[A] = ea; return true; }
    case Op::lwzu: { const uint32_t ea = r[A] + simm; r[D] = load(s, ea, 4); r[A] = ea; return true; }
    case Op::lbzux: { const uint32_t ea = r[A] + r[B]; r[D] = load(s, ea, 1); r[A] = ea; return true; }
    case Op::lhzux: { const uint32_t ea = r[A] + r[B]; r[D] = load(s, ea, 2); r[A] = ea; return true; }
    case Op::lhaux: { const uint32_t ea = r[A] + r[B]; r[D] = static_cast<uint32_t>(exts16(load(s, ea, 2))); r[A] = ea; return true; }
    case Op::lwzux: { const uint32_t ea = r[A] + r[B]; r[D] = load(s, ea, 4); r[A] = ea; return true; }
    case Op::lhbrx: { const uint32_t v = load(s, ea_x, 2); r[D] = ((v & 0xFF) << 8) | (v >> 8); return true; }
    case Op::lwbrx: {
        const uint32_t v = load(s, ea_x, 4);
        r[D] = (v << 24) | ((v & 0xFF00) << 8) | ((v >> 8) & 0xFF00) | (v >> 24);
        return true;
    }
    case Op::stb: store(s, ea_d, r[D], 1); return true;
    case Op::sth: store(s, ea_d, r[D], 2); return true;
    case Op::stw: store(s, ea_d, r[D], 4); return true;
    case Op::stbx: store(s, ea_x, r[D], 1); return true;
    case Op::sthx: store(s, ea_x, r[D], 2); return true;
    case Op::stwx: store(s, ea_x, r[D], 4); return true;
    case Op::stbu: { const uint32_t ea = r[A] + simm; store(s, ea, r[D], 1); r[A] = ea; return true; }
    case Op::sthu: { const uint32_t ea = r[A] + simm; store(s, ea, r[D], 2); r[A] = ea; return true; }
    case Op::stwu: { const uint32_t ea = r[A] + simm; store(s, ea, r[D], 4); r[A] = ea; return true; }
    case Op::stbux: { const uint32_t ea = r[A] + r[B]; store(s, ea, r[D], 1); r[A] = ea; return true; }
    case Op::sthux: { const uint32_t ea = r[A] + r[B]; store(s, ea, r[D], 2); r[A] = ea; return true; }
    case Op::stwux: { const uint32_t ea = r[A] + r[B]; store(s, ea, r[D], 4); r[A] = ea; return true; }
    case Op::sthbrx: store(s, ea_x, ((r[D] & 0xFF) << 8) | ((r[D] >> 8) & 0xFF), 2); return true;
    case Op::stwbrx: {
        const uint32_t v = r[D];
        store(s, ea_x, (v << 24) | ((v & 0xFF00) << 8) | ((v >> 8) & 0xFF00) | (v >> 24), 4);
        return true;
    }
    case Op::lmw: for (unsigned k = D, ea = ea_d; k < 32; ++k, ea += 4) r[k] = load(s, ea, 4); return true;
    case Op::stmw: for (unsigned k = D, ea = ea_d; k < 32; ++k, ea += 4) store(s, ea, r[k], 4); return true;
    case Op::lwarx: r[D] = load(s, ea_x, 4); s.reserve = true; s.reserve_addr = ea_x; return true;
    case Op::stwcx_rc: {
        const bool ok = s.reserve && s.reserve_addr == ea_x;
        if (ok) store(s, ea_x, r[D], 4);
        set_crf(s, 0, (ok ? 2u : 0u) | so(s));
        s.reserve = false;
        return true;
    }
    case Op::dcbz: { const uint32_t ea = ea_x & ~31u; for (unsigned k = 0; k < 32; ++k) s.mem[ea + k] = 0; return true; }
    case Op::dcbf: case Op::dcbst: case Op::dcbt: case Op::dcbtst: case Op::icbi:
    case Op::sync: case Op::isync: case Op::eieio: return true;
    // Condition register.
    case Op::crand: set_crbit(s, D, crbit(s, A) & crbit(s, B)); return true;
    case Op::crandc: set_crbit(s, D, crbit(s, A) & !crbit(s, B)); return true;
    case Op::creqv: set_crbit(s, D, crbit(s, A) == crbit(s, B)); return true;
    case Op::crnand: set_crbit(s, D, !(crbit(s, A) & crbit(s, B))); return true;
    case Op::crnor: set_crbit(s, D, !(crbit(s, A) | crbit(s, B))); return true;
    case Op::cror: set_crbit(s, D, crbit(s, A) | crbit(s, B)); return true;
    case Op::crorc: set_crbit(s, D, crbit(s, A) | !crbit(s, B)); return true;
    case Op::crxor: set_crbit(s, D, crbit(s, A) ^ crbit(s, B)); return true;
    case Op::mcrf: set_crf(s, bits(w, 6, 8), (s.cr >> (28 - 4 * bits(w, 11, 13))) & 15); return true;
    case Op::mcrxr: set_crf(s, bits(w, 6, 8), s.xer >> 28); s.xer &= 0x0FFFFFFFu; return true;
    case Op::mfcr: r[D] = s.cr; return true;
    case Op::mtcrf: {
        const unsigned crm = bits(w, 12, 19);
        uint32_t m = 0;
        for (unsigned n = 0; n < 8; ++n)
            if (crm & (0x80u >> n)) m |= 0xF0000000u >> (4 * n);
        s.cr = (r[D] & m) | (s.cr & ~m);
        return true;
    }
    case Op::mfspr: case Op::mtspr: {
        const unsigned n = (bits(w, 16, 20) << 5) | bits(w, 11, 15);
        uint32_t* reg = n == 1 ? &s.xer : n == 8 ? &s.lr : n == 9 ? &s.ctr : (n >= 912 && n < 920) ? &s.gqr[n - 912] : nullptr;
        if (!reg) return false;
        if (in.op == Op::mfspr) r[D] = n == 1 ? (*reg & 0xE000007Fu) : *reg;
        else *reg = n == 1 ? (r[D] & 0xE000007Fu) : r[D];
        return true;
    }
    case Op::tw: case Op::twi: {
        const int32_t x = static_cast<int32_t>(r[A]);
        const int32_t y = static_cast<int32_t>(in.op == Op::tw ? r[B] : simm);
        const unsigned to = D;
        if (((to & 16) && x < y) || ((to & 8) && x > y) || ((to & 4) && x == y) ||
            ((to & 2) && static_cast<uint32_t>(x) < static_cast<uint32_t>(y)) ||
            ((to & 1) && static_cast<uint32_t>(x) > static_cast<uint32_t>(y)))
            s.trapped = true;
        return true;
    }
    default: return false;
    }
}

}  // namespace qilin::ref
