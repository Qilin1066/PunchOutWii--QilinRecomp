// qilin_difftest: every generated case, run lifted and on the reference
// interpreter from the same random states; registers, CR, XER, LR, CTR,
// GQRs, the reservation, traps, indirect calls and memory must all agree.
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <print>
#include <random>
#include <string>
#include <vector>

#include "cases.hpp"
#include "decoder.hpp"
#include "qilin/rt/memory.hpp"
#include "ref.hpp"

using namespace qilin;
using difftest::Case;

namespace {

constexpr uint32_t kWin = 0x80100000u, kWinSize = 0x4000u;  // the memory tests touch
constexpr int kTrials = 64;

struct Trapped {};
uint32_t g_called = 0;
bool g_did_call = false;

uint32_t slow_read(rt::Cpu&, uint32_t a, unsigned) {
    std::println(stderr, "difftest: unexpected access to {:08X}", a);
    std::abort();
}
void slow_write(rt::Cpu&, uint32_t a, uint32_t, unsigned) {
    std::println(stderr, "difftest: unexpected store to {:08X}", a);
    std::abort();
}
void on_call(rt::Cpu&, uint32_t target) {
    g_did_call = true;
    g_called = target;
}
void on_trap(rt::Cpu&, uint32_t, const char*) { throw Trapped{}; }
void on_replaced(rt::Cpu&, uint32_t f) {
    std::println(stderr, "difftest: unexpected call to replaced function {:08X}", f);
    std::abort();
}
const rt::CpuHooks kHooks{slow_read, slow_write, on_call, on_trap, on_replaced, [](rt::Cpu&) {}};

bool memory_op(ppc::Op op) {
    const auto f = ppc::spec(op).flags;
    return (f & (ppc::kLoad | ppc::kStore)) || op == ppc::Op::dcbz || op == ppc::Op::lwarx || op == ppc::Op::stwcx_rc;
}
bool indexed(ppc::Op op) {
    for (ppc::Opnd o : ppc::spec(op).operands)
        if (o == ppc::Opnd::d_rA0 || o == ppc::Opnd::psq_d_rA0) return false;
    return true;
}

}  // namespace

int main() {
    rt::GuestMemory memory;
    if (!memory.reserve() || !memory.commit(kWin, kWinSize)) {
        std::println(stderr, "difftest: cannot reserve guest memory");
        return 1;
    }
    uint8_t* const win = memory.base() + kWin;
    std::vector<uint8_t> init(kWinSize), after(kWinSize);
    std::mt19937 rng(18112026);
    auto value = [&]() -> uint32_t {
        static const uint32_t special[] = {0, 1, 2, 31, 32, 33, 63, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu, 0xFFFFFFFEu, 0x8000u, 0xFFFF8000u};
        return rng() % 4 == 0 ? special[rng() % std::size(special)] : static_cast<uint32_t>(rng());
    };

    // FPR values: the edge cases, values a single holds exactly (what single-
    // precision code sees), ordinary doubles, and raw bit patterns.
    auto fvalue = [&]() -> uint64_t {
        static const uint64_t special[] = {
            0, 0x8000000000000000ull, 0x3FF0000000000000ull, 0xBFF0000000000000ull, 0x7FF0000000000000ull,
            0xFFF0000000000000ull, 0x7FF8000000000000ull, 0xFFF8000000000001ull, 0x7FF0000000000001ull,  // QNaN, -QNaN, SNaN
            0xFFF7FFFFFFFFFFFFull, 1, 0x800FFFFFFFFFFFFFull, 0x0010000000000000ull,                      // -SNaN, subnormals, min normal
            0x3810000000000000ull, 0x3800000000000000ull, 0x36A0000000000000ull, 0x3680000000000000ull,   // single boundaries
            0x47EFFFFFE0000000ull, 0x47F0000000000000ull, 0x41E0000000000000ull, 0xC1E0000000000000ull,   // FLT_MAX, 2^128, +-2^31
            0x3FE0000000000000ull, 0xBFE0000000000000ull, 0x4330000000000000ull};
        switch (rng() % 6) {
        case 0: return special[rng() % std::size(special)];
        case 1: case 2: {  // a single
            const uint32_t f = static_cast<uint32_t>(rng());
            return std::bit_cast<uint64_t>(static_cast<double>(std::bit_cast<float>(f)));
        }
        case 3: {  // an ordinary double around 1
            const double v = std::ldexp(static_cast<double>(rng()) / 4294967296.0 + 0.5, static_cast<int>(rng() % 40) - 20);
            return std::bit_cast<uint64_t>(rng() % 2 ? -v : v);
        }
        default: return (uint64_t{static_cast<uint32_t>(rng())} << 32) | static_cast<uint32_t>(rng());
        }
    };

    // A single-precision value as an FPR holds it (lfs's conversion): edge cases, ordinary
    // numbers of like size (the host's own arithmetic, in single mode), the bottom of the
    // range (NI mode's zeros) and raw bit patterns.
    auto svalue = [&]() -> uint64_t {
        static const uint32_t special[] = {0, 0x80000000u, 0x3F800000u, 0xBF800000u, 0x7F800000u, 0xFF800000u, 0x7FC00000u, 0xFFC00001u,
                                           0x7F800001u, 0xFFBFFFFFu, 1, 0x807FFFFFu, 0x00800000u, 0x80800000u, 0x00800001u, 0x7F7FFFFFu,
                                           0xFF7FFFFFu, 0x3F7FFFFFu, 0x3F800001u, 0x4F000000u, 0xCF000000u};
        uint32_t f;
        switch (rng() % 8) {
        case 0: f = special[rng() % std::size(special)]; break;
        case 1: f = static_cast<uint32_t>(rng()); break;
        case 2: f = (static_cast<uint32_t>(rng()) & 0x807FFFFFu) | ((rng() % 4) << 23); break;
        case 3: f = (static_cast<uint32_t>(rng()) & 0x807FFFFFu) | ((60 + rng() % 10) << 23); break;
        default: f = (static_cast<uint32_t>(rng()) & 0x807FFFFFu) | ((117 + rng() % 20) << 23); break;
        }
        return rt::fp::bits(rt::fp::load_single(f));
    };

    unsigned failures = 0, runs = 0;
    for (unsigned k = 0; k < difftest::kCaseCount; ++k) {
        const Case& t = difftest::kCases[k];
        const ppc::Insn in = ppc::decode(t.words[0]);
        const uint32_t pc = 0x81000000u + 32 * k;
        for (int trial = 0; trial < kTrials; ++trial, ++runs) {
            rt::Cpu c;
            for (uint32_t& r : c.r) r = value();
            if (t.kind == 0 && memory_op(in.op)) {
                const uint32_t addr = kWin + 0x900 + (rng() % 0x2600);  // room for d (+-512, psq +-2048), an index (< 0x400) and lmw
                if (!indexed(in.op)) {
                    c.r[in.ra()] = addr;
                } else if (in.ra() == 0) {
                    c.r[in.rb()] = addr;
                } else {
                    c.r[in.ra()] = addr;
                    c.r[in.rb()] = rng() % 0x400;
                }
            }
            c.cr = static_cast<uint32_t>(rng());
            c.set_xer(static_cast<uint32_t>(rng()));
            c.lr = value();
            c.ctr = t.kind ? (rng() % 2 ? rng() % 3 : value()) : value();
            // GQRs: a valid type (float, u8, u16, s8, s16) in both halves 15 times in 16,
            // anything (invalid types trap) the rest.
            for (uint32_t& g : c.gqr) {
                static const unsigned types[] = {0, 4, 5, 6, 7};
                g = static_cast<uint32_t>(rng());
                if (rng() % 16) g = (g & ~0x00070007u) | types[rng() % 5] | (types[rng() % 5] << 16);
            }
            // Every other trial, every FPR half holds a single-precision value: the lifter's
            // single mode (cpu.hpp) runs then, and is compared like the rest.
            const bool singles = trial % 2 == 1;
            uint64_t f0[32], f1[32];
            for (unsigned n = 0; n < 32; ++n) {
                f0[n] = singles ? svalue() : fvalue();
                f1[n] = singles ? svalue() : fvalue();
                c.ps0[n] = std::bit_cast<double>(f0[n]);
                c.ps1[n] = std::bit_cast<double>(f1[n]);
            }
            c.reserve = rng() % 2;
            if (in.op == ppc::Op::stwcx_rc) {
                const uint32_t ea = (in.ra() ? c.r[in.ra()] : 0) + c.r[in.rb()];
                c.reserved = rng() % 2 ? ea : ea + 4;
            } else {
                c.reserved = kWin + (rng() % kWinSize);
            }
            for (uint8_t& b : init) b = static_cast<uint8_t>(rng());

            ref::State s;
            std::memcpy(s.gpr, c.r, sizeof s.gpr);
            s.cr = c.cr;
            s.xer = c.xer();
            s.lr = c.lr;
            s.ctr = c.ctr;
            std::memcpy(s.gqr, c.gqr, sizeof s.gqr);
            std::memcpy(s.ps0, f0, sizeof f0);
            std::memcpy(s.ps1, f1, sizeof f1);
            s.reserve = c.reserve;
            s.reserve_addr = c.reserved;
            s.mem = memory.base();

            // Lifted.
            std::memcpy(win, init.data(), kWinSize);
            c.mem = memory.base();
            c.hooks = &kHooks;
            g_did_call = false;
            bool trapped = false;
            try {
                t.fn(c);
            } catch (const Trapped&) {
                trapped = true;
            }
            const bool did_call = g_did_call;
            const uint32_t called = g_called;
            std::memcpy(after.data(), win, kWinSize);

            // Reference.
            std::memcpy(win, init.data(), kWinSize);
            bool ref_call = false;
            uint32_t ref_called = 0;
            const unsigned bo = in.rd(), bi = in.ra();
            switch (t.kind) {
            case 0:
                if (!ref::step(s, in, pc)) {
                    std::println(stderr, "difftest: the reference doesn't know {}", ppc::disasm(in, pc));
                    return 1;
                }
                break;
            case 1: s.gpr[3] += ref::branch_taken(s, bo, bi) ? 2 : 1; break;
            case 2: if (!ref::branch_taken(s, bo, bi)) s.gpr[3] += 1; break;
            default:
                if (ref::branch_taken(s, bo, bi)) {
                    ref_call = true;
                    ref_called = s.ctr & ~3u;
                } else {
                    s.gpr[3] += 1;
                }
                break;
            }

            // Compare.
            std::vector<std::string> diffs;
            for (unsigned r = 0; r < 32; ++r)
                if (c.r[r] != s.gpr[r]) diffs.push_back(std::format("r{} {:08X} vs {:08X}", r, c.r[r], s.gpr[r]));
            if (c.cr != s.cr) diffs.push_back(std::format("cr {:08X} vs {:08X}", c.cr, s.cr));
            if (c.xer() != (s.xer & 0xE000007Fu)) diffs.push_back(std::format("xer {:08X} vs {:08X}", c.xer(), s.xer & 0xE000007Fu));
            if (c.lr != s.lr) diffs.push_back(std::format("lr {:08X} vs {:08X}", c.lr, s.lr));
            if (c.ctr != s.ctr) diffs.push_back(std::format("ctr {:08X} vs {:08X}", c.ctr, s.ctr));
            for (unsigned n = 0; n < 32; ++n) {
                const uint64_t x0 = std::bit_cast<uint64_t>(c.ps0[n]), x1 = std::bit_cast<uint64_t>(c.ps1[n]);
                if (x0 != s.ps0[n]) diffs.push_back(std::format("f{}.ps0 {:016X} vs {:016X}", n, x0, s.ps0[n]));
                if (x1 != s.ps1[n]) diffs.push_back(std::format("f{}.ps1 {:016X} vs {:016X}", n, x1, s.ps1[n]));
            }
            for (unsigned g = 0; g < 8; ++g)
                if (c.gqr[g] != s.gqr[g]) diffs.push_back(std::format("gqr{} differs", g));
            if (static_cast<bool>(c.reserve) != s.reserve || (s.reserve && c.reserved != s.reserve_addr))
                diffs.push_back("reservation differs");
            if (trapped != s.trapped) diffs.push_back(std::format("trap {} vs {}", trapped, s.trapped));
            if (did_call != ref_call || (did_call && (called & ~3u) != ref_called))
                diffs.push_back(std::format("indirect call {}:{:08X} vs {}:{:08X}", did_call, called, ref_call, ref_called));
            if (std::memcmp(after.data(), win, kWinSize) != 0) {
                size_t i = 0;
                while (after[i] == win[i]) ++i;
                diffs.push_back(std::format("memory at {:08X}: {:02X} vs {:02X}", kWin + i, after[i], win[i]));
            }
            if (!diffs.empty()) {
                if (++failures <= 20) {
                    std::string text = ppc::disasm(in, pc);
                    for (unsigned w = 1; w < 4 && t.words[w]; ++w) text += "; " + ppc::disasm(ppc::decode(t.words[w]), pc + 4 * w);
                    std::println(stderr, "FAIL case {} trial {}: {}", k, trial, text);
                    for (const std::string& d : diffs) std::println(stderr, "    {} (lifted vs reference)", d);
                }
                break;  // one report per case
            }
        }
    }
    std::println("difftest: {} cases, {} runs, {} failing cases", difftest::kCaseCount, runs, failures);
    return failures ? 1 : 0;
}
