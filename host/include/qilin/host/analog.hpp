// Analogue inputs without SDL: shared by the host and regression tests.
#pragma once
#include <algorithm>
#include <cmath>

namespace qilin::host {
// Separate press and release thresholds prevent a noisy axis repeatedly
// pressing a dodge or a trigger at the threshold. Percentages of full travel.
inline bool axis_pressed(int value, unsigned threshold, bool held) {
    const int press = static_cast<int>(std::clamp(threshold, 5u, 95u)) * 32767 / 100;
    const int release = std::max(0, press - 32767 * 5 / 100);
    return value > (held ? release : press);
}
inline float pointer_axis(int value, unsigned deadzone) {
    const float zone = float(std::min(deadzone, 95u)) / 100.0f;
    const float f = std::clamp(float(value) / 32767.0f, -1.0f, 1.0f);
    return std::fabs(f) <= zone ? 0.0f : (f - std::copysign(zone, f)) / (1.0f - zone);
}
}  // namespace qilin::host
