// qilin_difftest: every generated case, run lifted and on the reference
// interpreter from the same random states; registers, CR, XER, LR, CTR,
// GQRs, the reservation, traps, indirect calls and memory must all agree.
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
const rt::CpuHooks kHooks{slow_read, slow_write, on_call, on_trap};

bool memory_op(ppc::Op op) {
    const auto f = ppc::spec(op).flags;
    return (f & (ppc::kLoad | ppc::kStore)) || op == ppc::Op::dcbz || op == ppc::Op::lwarx || op == ppc::Op::stwcx_rc;
}
bool indexed(ppc::Op op) {
    for (ppc::Opnd o : ppc::spec(op).operands)
        if (o == ppc::Opnd::d_rA0) return false;
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

    unsigned failures = 0, runs = 0;
    for (unsigned k = 0; k < difftest::kCaseCount; ++k) {
        const Case& t = difftest::kCases[k];
        const ppc::Insn in = ppc::decode(t.words[0]);
        const uint32_t pc = 0x81000000u + 32 * k;
        for (int trial = 0; trial < kTrials; ++trial, ++runs) {
            rt::Cpu c;
            for (uint32_t& r : c.r) r = value();
            if (t.kind == 0 && memory_op(in.op)) {
                const uint32_t addr = kWin + 0x800 + (rng() % 0x2800);  // room for d (+-512), an index (< 0x400) and lmw
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
            for (uint32_t& g : c.gqr) g = static_cast<uint32_t>(rng());
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
