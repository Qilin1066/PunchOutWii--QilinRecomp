// QILIN_NATIVE with MSVC (CMakeLists.txt): exits 0 if this CPU and the OS
// support AVX2 and FMA (so /arch:AVX2 code runs here), 1 otherwise.
#include <intrin.h>
int main() {
    int r[4];
    __cpuid(r, 0);
    if (r[0] < 7) return 1;
    __cpuid(r, 1);
    const bool fma = (r[2] >> 12) & 1, osxsave = (r[2] >> 27) & 1, avx = (r[2] >> 28) & 1;
    if (!fma || !osxsave || !avx) return 1;
    if ((_xgetbv(0) & 6) != 6) return 1;  // the OS saves the YMM registers
    __cpuidex(r, 7, 0);
    return ((r[1] >> 5) & 1) ? 0 : 1;  // AVX2
}
