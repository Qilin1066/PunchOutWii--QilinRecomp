// Evaluates the runtime's floating-point helpers (rt::fp, used by lifted
// code) on inputs from stdin, one case per line: "<op> <hex> [<hex> ...]", and
// prints each result in hex. fp_check.py drives it and checks every result
// against exact rational arithmetic. Each op composes the helpers exactly as
// recomp/src/lift.cpp emits them for the instruction it's named after.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>

#include "qilin/rt/cpu.hpp"

namespace fp = qilin::rt::fp;

namespace {

double d(uint64_t b) { return fp::from_bits(b); }
uint64_t b(double v) { return fp::bits(v); }

uint64_t eval(const std::string& op, const uint64_t* x) {
    // double precision (fadd, fsub, fmul, fdiv, fmadd, fmsub)
    if (op == "fadd") return b(fp::flush(fp::add(d(x[0]), d(x[1]))));
    if (op == "fsub") return b(fp::flush(fp::sub(d(x[0]), d(x[1]))));
    if (op == "fmul") return b(fp::flush(fp::mul(d(x[0]), d(x[1]))));
    if (op == "fdiv") return b(fp::flush(fp::div(d(x[0]), d(x[1]))));
    if (op == "fmadd") return b(fp::flush(fp::madd(d(x[0]), d(x[1]), d(x[2]), false)));
    if (op == "fmsub") return b(fp::flush(fp::madd(d(x[0]), d(x[1]), d(x[2]), true)));
    // single precision (fadds ... fmsubs; ps_add and the rest compute each half the same way)
    if (op == "fadds") return b(fp::single(fp::add(d(x[0]), d(x[1]))));
    if (op == "fsubs") return b(fp::single(fp::sub(d(x[0]), d(x[1]))));
    if (op == "fmuls") return b(fp::single(fp::mul(d(x[0]), fp::round25(d(x[1])))));
    if (op == "fdivs") return b(fp::single(fp::div(d(x[0]), d(x[1]))));
    if (op == "fmadds") return b(fp::single(fp::madd_single(d(x[0]), d(x[1]), d(x[2]), false)));
    if (op == "fmsubs") return b(fp::single(fp::madd_single(d(x[0]), d(x[1]), d(x[2]), true)));
    if (op == "frsp") return b(fp::single(d(x[0])));
    if (op == "round25") return b(fp::round25(d(x[0])));
    // conversions
    if (op == "fctiw") return b(fp::to_int(d(x[0]), false));
    if (op == "fctiwz") return b(fp::to_int(d(x[0]), true));
    if (op == "lfs") return b(fp::load_single(static_cast<uint32_t>(x[0])));
    if (op == "stfs") return fp::store_single(d(x[0]));
    // estimates
    if (op == "fres") return b(fp::estimate_reciprocal(d(x[0])));
    if (op == "frsqrte") return b(fp::estimate_rsqrt(d(x[0])));
    // paired-single quantisation: type, scale, then the raw value or the double
    if (op == "dequant") return b(fp::dequantize(static_cast<unsigned>(x[0]), static_cast<unsigned>(x[1]), static_cast<uint32_t>(x[2])));
    if (op == "quant") return fp::quantize(static_cast<unsigned>(x[0]), static_cast<unsigned>(x[1]), d(x[2]));
    std::fprintf(stderr, "fp_eval: unknown op %s\n", op.c_str());
    std::exit(2);
}

}  // namespace

int main() {
    std::ios::sync_with_stdio(false);
    std::string line, op;
    while (std::getline(std::cin, line)) {
        std::istringstream in(line);
        uint64_t x[3] = {};
        in >> op;
        for (uint64_t& v : x)
            if (!(in >> std::hex >> v)) break;
        std::printf("%016llx\n", static_cast<unsigned long long>(eval(op, x)));
    }
    return 0;
}
