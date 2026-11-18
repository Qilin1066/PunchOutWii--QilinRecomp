// qilin_difftest_gen: writes the differential test's cases as C++ (run at build
// time; the output is compiled into qilin_difftest).
//
// For every instruction the lifter's slice covers, random valid encodings
// (the decoder decides validity), each lifted as a one-instruction function
// ending in blr. For the branches, small programs whose result shows whether
// the branch was taken, over every BO the decoder accepts and a spread of BI.
//
//   qilin_difftest_gen <out.cpp> [cases-per-instruction]
#include <cstdio>
#include <cstdlib>
#include <format>
#include <fstream>
#include <print>
#include <iterator>
#include <random>
#include <string>
#include <vector>

#include "decoder.hpp"
#include "lift.hpp"

using namespace qilin;
using ppc::Insn;
using ppc::Op;

namespace {

constexpr uint32_t kBase = 0x81000000u;  // case k's code lives at kBase + 32 * k
constexpr uint32_t kBlr = 0x4E800020u;

bool memory_op(Op op) {
    const auto f = ppc::spec(op).flags;
    return (f & (ppc::kLoad | ppc::kStore)) || op == Op::dcbz || op == Op::lwarx || op == Op::stwcx_rc;
}
bool indexed(const ppc::Spec& s) {
    for (ppc::Opnd o : s.operands)
        if (o == ppc::Opnd::d_rA0) return false;
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::println(stderr, "usage: qilin_difftest_gen <out.cpp> [cases-per-instruction]");
        return 2;
    }
    const int per = argc > 2 ? std::atoi(argv[2]) : 24;
    std::mt19937 rng(20261118);
    std::string fns, table;
    unsigned k = 0;
    auto add = [&](unsigned kind, std::vector<uint32_t> words) -> bool {
        LiftInput in;
        in.start = kBase + 32 * k;
        for (size_t i = 0; i < words.size(); ++i) in.insns.emplace_back(in.start + 4 * static_cast<uint32_t>(i), ppc::decode(words[i]));
        in.is_function = [](uint32_t) { return false; };
        const Lifted l = lift(in);
        if (!l.ok) {
            std::println(stderr, "difftest_gen: {} at {:08X}: {}", l.why, l.why_at, ppc::disasm(in.insns[0].second, in.start));
            return false;
        }
        fns += l.code + "\n";
        words.resize(4, 0);
        table += std::format("    {{{}, {{0x{:08X}u, 0x{:08X}u, 0x{:08X}u, 0x{:08X}u}}, ::qilin::gen::{}}},\n", kind, words[0], words[1], words[2], words[3],
                             fn_name(in.start));
        ++k;
        return true;
    };

    unsigned ops = 0;
    for (const ppc::Spec& s : ppc::specs()) {
        if (s.op == Op::invalid || (s.flags & ppc::kBranch)) continue;
        Insn bare;  // the op with every field zero: in the slice unless the op itself isn't
        bare.op = s.op;
        bare.word = s.match;
        if (!in_slice(bare, nullptr) && s.op != Op::mfspr && s.op != Op::mtspr) continue;  // SPRs are picked below
        int made = 0;
        for (int tries = 0; made < per && tries < per * 400; ++tries) {
            uint32_t w = s.match;
            for (ppc::Opnd o : s.operands) w |= rng() & ppc::field_mask(o);
            if (s.flags & ppc::kRc) w |= rng() & 1;
            if (s.op == Op::mfspr || s.op == Op::mtspr) {
                static const unsigned sprs[] = {1, 8, 9, 912, 913, 914, 915, 916, 917, 918, 919};
                const unsigned n = sprs[rng() % std::size(sprs)];
                w = (w & ~0x001FF800u) | ((n & 31) << 16) | ((n >> 5) << 11);
            }
            const Insn in = ppc::decode(w);
            if (in.op != s.op || !in_slice(in, nullptr)) continue;
            if (memory_op(s.op)) {
                if (!indexed(s)) {
                    if (in.ra() == 0) continue;  // the address would be the displacement alone
                    // Keep displacements small, so the address stays in the test window.
                    w = (w & 0xFFFF0000u) | (static_cast<uint32_t>(static_cast<int32_t>(rng() % 1024) - 512) & 0xFFFF);
                    if (ppc::decode(w).op != s.op) continue;
                } else if (in.ra() == in.rb()) {
                    continue;  // base and index the same register: no one value serves as both
                }
            }
            if (!add(0, {w, kBlr})) return 1;
            ++made;
        }
        if (made) ++ops;
    }

    // Branches: kind 1 is bc over an increment, kind 2 a conditional return, kind 3 bcctr.
    unsigned branches = 0;
    for (unsigned bo = 0; bo < 32; ++bo)
        for (unsigned bi = 0; bi < 32; bi += 3) {
            const uint32_t bc = 0x40000000u | (bo << 21) | (bi << 16) | 12u;    // bc bo,bi,+12
            const uint32_t addi1 = 0x38630001u, addi2 = 0x38630002u;          // addi r3,r3,1 / 2
            if (ppc::decode(bc).op != Op::bc) continue;
            if (!add(1, {bc, addi1, kBlr, addi2, kBlr})) return 1;
            const uint32_t bclr = 0x4C000020u | (bo << 21) | (bi << 16);
            if (ppc::decode(bclr).op == Op::bclr && !add(2, {bclr, addi1, kBlr})) return 1;
            const uint32_t bcctr = 0x4C000420u | (bo << 21) | (bi << 16);
            if (ppc::decode(bcctr).op == Op::bcctr && !add(3, {bcctr, addi1, kBlr})) return 1;
            branches += 1;
        }

    std::ofstream f(argv[1], std::ios::binary);
    f << "// Generated by qilin_difftest_gen: the differential test's cases.\n"
         "#include \"qilin/rt/cpu.hpp\"\n#include \"cases.hpp\"\n\n"
         "namespace qilin::gen {\nusing namespace ::qilin::rt;\n\n"
      << fns
      << "}  // namespace qilin::gen\n\nnamespace qilin::difftest {\nextern const Case kCases[] = {\n"
      << table << std::format("}};\nextern const unsigned kCaseCount = {};\n}}  // namespace qilin::difftest\n", k);
    if (!f) {
        std::println(stderr, "difftest_gen: cannot write {}", argv[1]);
        return 1;
    }
    std::println("difftest_gen: {} cases: {} instructions, branches over {} BO/BI pairs", k, ops, branches);
    return 0;
}
