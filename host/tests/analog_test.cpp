#include "qilin/host/analog.hpp"
#include <cstdio>
#include <cmath>
int main() {
    using namespace qilin::host;
    int failures = 0;
    const auto check = [&](bool ok, const char* what) {
        std::printf("%s %s\n", ok ? "ok" : "FAIL", what);
        if (!ok) ++failures;
    };
    check(!axis_pressed(16000, 50, false), "resting below threshold");
    check(axis_pressed(17000, 50, false), "crossing press threshold");
    check(axis_pressed(15500, 50, true), "jitter does not release a held input");
    check(!axis_pressed(14000, 50, true), "releasing below hysteresis");
    check(!axis_pressed(15500, 50, false), "jitter cannot press it again");
    check(axis_pressed(32768, 95, false), "negative stick endpoint (negated INT16_MIN)");
    check(!axis_pressed(0, 5, true), "centre always releases");
    check(pointer_axis(8000, 25) == 0, "pointer drift removed");
    check(pointer_axis(-32768, 25) == -1 && pointer_axis(32767, 25) == 1, "both endpoints normalised");
    check(std::fabs(pointer_axis(20000, 25) + pointer_axis(-20000, 25)) < 1e-6f, "pointer symmetry");
    check(std::isfinite(pointer_axis(32767, 100)), "invalid dead zone cannot divide by zero");
    return failures ? 1 : 0;
}
