// The guest CPU state and memory access that lifted code compiles against.
//
// Lifted functions take a `Cpu&`. Inside a function the guest registers live in
// host locals; they are written back to the Cpu, and read again, only around
// calls and returns, where another function can see them
// (docs/recompiler-core.md, "Lifter").
//
// Runtime code: no std::format here (macOS 10.15, see CMakeLists.txt).
#pragma once

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

namespace qilin::rt {

static_assert(std::endian::native == std::endian::little, "hosts are little-endian; the guest is big-endian");

struct Cpu;
using GuestFn = void (*)(Cpu&);

// Called for an access outside cached MEM1 and MEM2 (hardware registers, the
// write-gather pipe, uncached mirrors, the locked cache), for an indirect call,
// and when guest code traps. Each must not return if it can't handle the case.
struct CpuHooks {
    uint32_t (*read)(Cpu&, uint32_t addr, unsigned size);
    void (*write)(Cpu&, uint32_t addr, uint32_t value, unsigned size);
    void (*call)(Cpu&, uint32_t target);  // bctrl / bctr / blrl to a computed address
    void (*trap)(Cpu&, uint32_t pc, const char* why);  // must not return
};

struct Cpu {
    uint32_t r[32] = {};
    uint32_t cr = 0;  // CR0 in the top four bits, as mfcr reads it
    uint32_t lr = 0, ctr = 0;
    uint32_t xer_so = 0, xer_ov = 0, xer_ca = 0, xer_bc = 0;  // XER's bits, one per word
    uint32_t gqr[8] = {};
    double ps0[32] = {}, ps1[32] = {};  // FPRs: ps0 is the double the FP instructions use, ps1 the paired-single half
    uint32_t fpscr = 0;
    uint32_t reserve = 0, reserved = 0;  // lwarx's reservation
    uint8_t* mem = nullptr;              // base of the 4 GB guest address space
    const CpuHooks* hooks = nullptr;

    uint32_t xer() const { return xer_so << 31 | xer_ov << 30 | xer_ca << 29 | (xer_bc & 0x7F); }
    void set_xer(uint32_t v) {
        xer_so = v >> 31;
        xer_ov = (v >> 30) & 1;
        xer_ca = (v >> 29) & 1;
        xer_bc = v & 0x7F;
    }
};

// Cached MEM1 (0x80000000, 24 MB) and MEM2 (0x90000000, 64 MB) are plain host
// memory at mem + address, kept in the guest's byte order. Everything at
// 0xC0000000 and above goes to the hooks.
constexpr uint32_t kSlowFrom = 0xC0000000u;

inline uint32_t read8(Cpu& c, uint32_t a) {
    if (a >= kSlowFrom) [[unlikely]]
        return c.hooks->read(c, a, 1);
    return c.mem[a];
}
inline uint32_t read16(Cpu& c, uint32_t a) {
    if (a >= kSlowFrom) [[unlikely]]
        return c.hooks->read(c, a, 2);
    uint16_t v;
    std::memcpy(&v, c.mem + a, 2);
    return std::byteswap(v);
}
inline uint32_t read32(Cpu& c, uint32_t a) {
    if (a >= kSlowFrom) [[unlikely]]
        return c.hooks->read(c, a, 4);
    uint32_t v;
    std::memcpy(&v, c.mem + a, 4);
    return std::byteswap(v);
}
inline uint64_t read64(Cpu& c, uint32_t a) { return uint64_t{read32(c, a)} << 32 | read32(c, a + 4); }
inline void write8(Cpu& c, uint32_t a, uint32_t v) {
    if (a >= kSlowFrom) [[unlikely]]
        return c.hooks->write(c, a, v & 0xFF, 1);
    c.mem[a] = static_cast<uint8_t>(v);
}
inline void write16(Cpu& c, uint32_t a, uint32_t v) {
    if (a >= kSlowFrom) [[unlikely]]
        return c.hooks->write(c, a, v & 0xFFFF, 2);
    const uint16_t s = std::byteswap(static_cast<uint16_t>(v));
    std::memcpy(c.mem + a, &s, 2);
}
inline void write32(Cpu& c, uint32_t a, uint32_t v) {
    if (a >= kSlowFrom) [[unlikely]]
        return c.hooks->write(c, a, v, 4);
    const uint32_t s = std::byteswap(v);
    std::memcpy(c.mem + a, &s, 4);
}
inline void write64(Cpu& c, uint32_t a, uint64_t v) {
    write32(c, a, static_cast<uint32_t>(v >> 32));
    write32(c, a + 4, static_cast<uint32_t>(v));
}
inline void zero_block(Cpu& c, uint32_t a) {  // dcbz: the 32-byte block holding a
    a &= ~31u;
    if (a >= kSlowFrom) [[unlikely]] {
        for (uint32_t k = 0; k < 32; k += 4) c.hooks->write(c, a + k, 0, 4);
        return;
    }
    std::memset(c.mem + a, 0, 32);
}

// CR0 after a record form ("."): the sign of the result, and XER's SO.
inline uint32_t record(uint32_t v, uint32_t so) {
    const int32_t s = static_cast<int32_t>(v);
    return (s < 0 ? 8u : s > 0 ? 4u : 2u) | so;
}
inline uint32_t compare_signed(int32_t a, int32_t b, uint32_t so) { return (a < b ? 8u : a > b ? 4u : 2u) | so; }
inline uint32_t compare_unsigned(uint32_t a, uint32_t b, uint32_t so) { return (a < b ? 8u : a > b ? 4u : 2u) | so; }

// a + b + carry_in, and the carry out.
inline uint32_t add_carry(uint32_t a, uint32_t b, uint32_t cin, uint32_t& cout) {
    const uint64_t t = uint64_t{a} + b + cin;
    cout = static_cast<uint32_t>(t >> 32);
    return static_cast<uint32_t>(t);
}

inline uint32_t sraw(uint32_t s, uint32_t n, uint32_t& ca) {
    n &= 63;
    const int32_t v = static_cast<int32_t>(s);
    if (n >= 32) {
        ca = v < 0;
        return v < 0 ? 0xFFFFFFFFu : 0;
    }
    ca = v < 0 && n && (s & ((1u << n) - 1));
    return static_cast<uint32_t>(v >> n);
}
inline uint32_t slw(uint32_t s, uint32_t n) { return (n & 32) ? 0 : s << (n & 31); }
inline uint32_t srw(uint32_t s, uint32_t n) { return (n & 32) ? 0 : s >> (n & 31); }

// divw and divwu where the architecture leaves the result undefined (division
// by zero, INT_MIN / -1): all ones for a negative dividend, else zero, as
// Dolphin's interpreter has it (Interpreter_Integer.cpp, divwx; PR #6943).
inline uint32_t divw(uint32_t a, uint32_t b) {
    const int32_t x = static_cast<int32_t>(a), y = static_cast<int32_t>(b);
    if (y == 0 || (x == INT32_MIN && y == -1)) return x < 0 ? 0xFFFFFFFFu : 0;
    return static_cast<uint32_t>(x / y);
}
inline uint32_t divwu(uint32_t a, uint32_t b) { return b ? a / b : 0; }
inline uint32_t mulhw(uint32_t a, uint32_t b) {
    return static_cast<uint32_t>(static_cast<uint64_t>(int64_t{static_cast<int32_t>(a)} * static_cast<int32_t>(b)) >> 32);
}
inline uint32_t mulhwu(uint32_t a, uint32_t b) { return static_cast<uint32_t>((uint64_t{a} * b) >> 32); }

// ---------------------------------------------------------------- floating point
//
// Broadway's floating-point results, bit for bit, as Dolphin's interpreter
// records them from hardware tests (Source/Core/Core/PowerPC/Interpreter/
// Interpreter_FPUtils.h, Common/FloatUtils.cpp; written here from those rules).
// The game runs with FPSCR[NI] set (OSInit calls PPCSetFpNonIEEEMode), round
// to nearest, and every exception disabled, so those are fixed here; the FPSCR
// flags themselves aren't kept (only the replaced OS reads them).
namespace fp {

constexpr uint64_t kSign = 0x8000000000000000ull, kExp = 0x7FF0000000000000ull, kFrac = 0x000FFFFFFFFFFFFFull;
constexpr uint64_t kQuiet = 0x0008000000000000ull, kDefaultNan = 0x7FF8000000000000ull;

inline uint64_t bits(double d) { return std::bit_cast<uint64_t>(d); }
inline double from_bits(uint64_t b) { return std::bit_cast<double>(b); }
inline bool is_nan(double d) { return (bits(d) & ~kSign) > kExp; }
inline double quiet(double d) { return from_bits(bits(d) | kQuiet); }

// A NaN result: the first NaN operand, made quiet, else the default NaN.
inline double nan2(double a, double b) { return is_nan(a) ? quiet(a) : is_nan(b) ? quiet(b) : from_bits(kDefaultNan); }
inline double nan3(double a, double b, double c) { return is_nan(a) || is_nan(b) ? nan2(a, b) : is_nan(c) ? quiet(c) : from_bits(kDefaultNan); }

// FPSCR[NI]: a subnormal double result is a signed zero.
inline double flush(double d) { return (bits(d) & kExp) ? d : from_bits(bits(d) & kSign); }

// Rounding to single precision under NI: anything below the smallest normal
// single before rounding becomes a signed zero.
inline double single(double v) {
    if ((bits(v) & ~kSign) < 0x3810000000000000ull) return from_bits(bits(v) & kSign);
    return static_cast<double>(static_cast<float>(v));
}

// frC of single-precision multiplies, rounded to 25 significant bits.
inline double round25(double d) {
    uint64_t i = bits(d);
    const uint64_t e = i & kExp, f = i & kFrac;
    uint64_t keep = 0xFFFFFFFFF8000000ull, round = 0x8000000ull;
    if (e == 0 && f != 0) {  // a subnormal rounds as if normalised: at a lower bit
        const unsigned shift = static_cast<unsigned>(std::countl_zero(f)) - 11;
        keep = static_cast<uint64_t>(static_cast<int64_t>(keep) >> shift);
        round >>= shift;
    }
    return from_bits((i & keep) + (i & round));
}

inline double add(double a, double b) {
    const double r = a + b;
    return is_nan(r) ? nan2(a, b) : r;
}
inline double sub(double a, double b) {
    const double r = a - b;
    return is_nan(r) ? nan2(a, b) : r;
}
inline double mul(double a, double b) {
    const double r = a * b;
    return is_nan(r) ? nan2(a, b) : r;
}
inline double div(double a, double b) {
    const double r = a / b;
    return is_nan(r) ? nan2(a, b) : r;
}
// a * c + b (or - b) with one rounding; NaNs looked at in the order a, b, c,
// with b as it was before any negation.
inline double madd(double a, double c, double b, bool subtract) {
    const double r = std::fma(a, c, subtract ? -b : b);
    return is_nan(r) ? nan3(a, b, c) : r;
}
// The single-precision form: frC rounded to 25 bits, a double fma, and where
// that lands exactly halfway between two singles, the fma's own rounding
// error (found with an error-free transformation) decides the direction, so
// the result is rounded to single only once, as Broadway does.
inline double madd_single(double a, double c, double b, bool subtract) {
    const double cr = round25(c), bs = subtract ? -b : b;
    double r = std::fma(a, cr, bs);
    const uint64_t rb = bits(r);
    if ((rb & 0x1FFFFFFFull) == 0x10000000ull) {
        const double a1 = bs - r;   // -(a*cr) plus the fma's rounding error, rounded
        const double b1 = r + a1;
        const double err = std::fma(a, cr, a1) + (bs - b1);
        if (err != 0.0) r = from_bits((err > 0.0) == (r > 0.0) ? rb + 1 : rb - 1);
    }
    return is_nan(r) ? nan3(a, b, c) : r;
}
inline double neg_unless_nan(double d) { return is_nan(d) ? d : -d; }

// fctiw / fctiwz: the integer in the low word, 0xFFF80000 above it (and the
// next bit up set for a negative value that converts to zero).
inline double to_int(double b, bool truncate) {
    uint32_t v;
    const double r = truncate ? std::trunc(b) : std::nearbyint(b);
    if (is_nan(b)) v = 0x80000000u;
    else if (r >= 2147483648.0) v = 0x7FFFFFFFu;
    else if (r < -2147483648.0) v = 0x80000000u;
    else v = static_cast<uint32_t>(static_cast<int32_t>(r));
    uint64_t out = 0xFFF8000000000000ull | v;
    if (v == 0 && std::signbit(b)) out |= 0x100000000ull;
    return from_bits(out);
}

inline uint32_t compare(double a, double b) {
    if (is_nan(a) || is_nan(b)) return 1;  // unordered
    return a < b ? 8u : a > b ? 4u : 2u;
}

// lfs and stfs: the PowerPC manual's conversions, bit by bit (no rounding,
// signalling NaNs kept), not the host's float/double conversions.
inline double load_single(uint32_t x) {
    const uint64_t v = x, e = (v >> 23) & 0xFF, f = v & 0x7FFFFF;
    if (e > 0 && e < 255) {
        const uint64_t y = !(e >> 7), z = y << 61 | y << 60 | y << 59;
        return from_bits(((v & 0xC0000000u) << 32) | z | ((v & 0x3FFFFFFFu) << 29));
    }
    if (e == 0 && f != 0) {
        uint64_t exp = 1023 - 126, frac = f;
        do {
            frac <<= 1;
            exp -= 1;
        } while (!(frac & 0x800000));
        return from_bits(((v & 0x80000000u) << 32) | (exp << 52) | ((frac & 0x7FFFFF) << 29));
    }
    const uint64_t y = e >> 7, z = y << 61 | y << 60 | y << 59;
    return from_bits(((v & 0xC0000000u) << 32) | z | ((v & 0x3FFFFFFFu) << 29));
}
inline uint32_t store_single(double d) {
    const uint64_t x = bits(d);
    const uint32_t e = static_cast<uint32_t>((x >> 52) & 0x7FF);
    if (e > 896 || (x & ~kSign) == 0 || e < 874)  // below 874 the manual says undefined; this is what hardware does
        return static_cast<uint32_t>(((x >> 32) & 0xC0000000u) | ((x >> 29) & 0x3FFFFFFFu));
    uint32_t t = static_cast<uint32_t>(0x80000000u | ((x & kFrac) >> 21));
    t >>= (905 - e);
    return t | static_cast<uint32_t>((x >> 32) & 0x80000000u);
}

// fres and frsqrte: Broadway's estimate tables (base, decrement), measured on
// hardware by the Dolphin project.
struct Estimate {
    int32_t base, dec;
};
inline constexpr Estimate kResTable[32] = {
    {0x7ff800, 0x3e1}, {0x783800, 0x3a7}, {0x70ea00, 0x371}, {0x6a0800, 0x340}, {0x638800, 0x313}, {0x5d6200, 0x2ea},
    {0x579000, 0x2c4}, {0x520800, 0x2a0}, {0x4cc800, 0x27f}, {0x47ca00, 0x261}, {0x430800, 0x245}, {0x3e8000, 0x22a},
    {0x3a2c00, 0x212}, {0x360800, 0x1fb}, {0x321400, 0x1e5}, {0x2e4a00, 0x1d1}, {0x2aa800, 0x1be}, {0x272c00, 0x1ac},
    {0x23d600, 0x19b}, {0x209e00, 0x18b}, {0x1d8800, 0x17c}, {0x1a9000, 0x16e}, {0x17ae00, 0x15b}, {0x14f800, 0x15b},
    {0x124400, 0x143}, {0x0fbe00, 0x143}, {0x0d3800, 0x12d}, {0x0ade00, 0x12d}, {0x088400, 0x11a}, {0x065000, 0x11a},
    {0x041c00, 0x108}, {0x020c00, 0x106},
};
inline constexpr Estimate kRsqrtTable[32] = {
    {0x1a7e800, -0x568}, {0x17cb800, -0x4f3}, {0x1552800, -0x48d}, {0x130c000, -0x435}, {0x10f2000, -0x3e7},
    {0x0eff000, -0x3a2}, {0x0d2e000, -0x365}, {0x0b7c000, -0x32e}, {0x09e5000, -0x2fc}, {0x0867000, -0x2d0},
    {0x06ff000, -0x2a8}, {0x05ab800, -0x283}, {0x046a000, -0x261}, {0x0339800, -0x243}, {0x0218800, -0x226},
    {0x0105800, -0x20b}, {0x3ffa000, -0x7a4}, {0x3c29000, -0x700}, {0x38aa000, -0x670}, {0x3572000, -0x5f2},
    {0x3279000, -0x584}, {0x2fb7000, -0x524}, {0x2d26000, -0x4cc}, {0x2ac0000, -0x47e}, {0x2881000, -0x43a},
    {0x2665000, -0x3fa}, {0x2468000, -0x3c2}, {0x2287000, -0x38e}, {0x20c1000, -0x35e}, {0x1f12000, -0x332},
    {0x1d79000, -0x30a}, {0x1bf4000, -0x2e6},
};
inline double estimate_reciprocal(double v) {
    const int64_t i = static_cast<int64_t>(bits(v));
    const int64_t m = i & ((int64_t{1} << 52) - 1), s = static_cast<int64_t>(bits(v) & kSign);
    int64_t e = i & static_cast<int64_t>(kExp);
    if (m == 0 && e == 0) return std::copysign(std::numeric_limits<double>::infinity(), v);
    if (e == static_cast<int64_t>(kExp)) return m == 0 ? std::copysign(0.0, v) : quiet(v);
    if (e < (int64_t{895} << 52)) return std::copysign(static_cast<double>(std::numeric_limits<float>::max()), v);
    if (e >= (int64_t{1149} << 52)) return std::copysign(0.0, v);
    e = (int64_t{0x7FD} << 52) - e;
    const int idx = static_cast<int>(m >> 37);
    const Estimate& t = kResTable[idx / 1024];
    return from_bits(static_cast<uint64_t>(s | e | (static_cast<int64_t>(t.base - (t.dec * (idx % 1024) + 1) / 2) << 29)));
}
inline double estimate_rsqrt(double v) {
    const int64_t i = static_cast<int64_t>(bits(v));
    int64_t m = i & ((int64_t{1} << 52) - 1);
    const int64_t s = static_cast<int64_t>(bits(v) & kSign);
    int64_t e = i & static_cast<int64_t>(kExp);
    if (m == 0 && e == 0) return std::copysign(std::numeric_limits<double>::infinity(), v);
    if (e == static_cast<int64_t>(kExp)) {
        if (m == 0) return s ? std::numeric_limits<double>::quiet_NaN() : 0.0;
        return quiet(v);
    }
    if (s) return std::numeric_limits<double>::quiet_NaN();
    if (!e) {  // normalise a subnormal
        do {
            e -= int64_t{1} << 52;
            m <<= 1;
        } while (!(m & (int64_t{1} << 52)));
        m &= (int64_t{1} << 52) - 1;
        e += int64_t{1} << 52;
    }
    const int64_t lsb = e & (int64_t{1} << 52);
    e = ((int64_t{0x3FF} << 52) - ((e - (int64_t{0x3FE} << 52)) / 2)) & static_cast<int64_t>(kExp);
    const int idx = static_cast<int>((lsb | m) >> 37);
    const Estimate& t = kRsqrtTable[idx / 2048];
    return from_bits(static_cast<uint64_t>(s | e | (static_cast<int64_t>(t.base + t.dec * (idx % 2048)) << 26)));
}

}  // namespace fp

}  // namespace qilin::rt
