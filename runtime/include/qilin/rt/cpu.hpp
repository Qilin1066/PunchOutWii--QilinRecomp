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
#if defined(_MSC_VER) && !defined(__clang__) && defined(__AVX2__)
#include <immintrin.h>  // fused(): MSVC's std::fma is a call even with /arch:AVX2
#endif

// The lifted code's hottest helpers are forced inline: in a large lifted
// function the compiler's own limits otherwise leave a call (and its register
// spills) behind every load and store. Their rare paths stay out of line.
#if defined(_MSC_VER) && !defined(__clang__)
#define QILIN_INLINE __forceinline
#define QILIN_OUTLINE __declspec(noinline) inline
#else
#define QILIN_INLINE [[gnu::always_inline]] inline
#define QILIN_OUTLINE [[gnu::noinline]] inline
#endif

namespace qilin::rt {

static_assert(std::endian::native == std::endian::little, "hosts are little-endian; the guest is big-endian");

struct Cpu;
using GuestFn = void (*)(Cpu&);

// Called for an access outside cached MEM1 and MEM2 (hardware registers, the
// write-gather pipe, uncached mirrors, the locked cache), for an indirect call,
// when guest code traps, and when a replaced or dropped function is called
// (the runtime runs its native replacement). Each must not return if it can't
// handle the case.
struct CpuHooks {
    uint32_t (*read)(Cpu&, uint32_t addr, unsigned size);
    void (*write)(Cpu&, uint32_t addr, uint32_t value, unsigned size);
    void (*call)(Cpu&, uint32_t target);  // bctrl / bctr / blrl to a computed address
    void (*trap)(Cpu&, uint32_t pc, const char* why);  // must not return
    void (*replaced)(Cpu&, uint32_t function);         // a function the plan replaces or drops was called
    void (*idle)(Cpu&);  // the back edge of a poll loop: a chance for interrupts to arrive (registers written back)
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
// memory at mem + address, kept in the guest's byte order. Accesses at
// 0xC0000000 and above take the slow branch: the uncached RAM mirrors, the
// locked cache and the write-gather pipe are handled there, the rest by the
// hooks.
constexpr uint32_t kSlowFrom = 0xC0000000u;

// The write-gather pipe (0xCC008000-0xCC00801F), if the game's runtime set
// one: stores there go straight to it instead of through the hooks and the
// hardware-register table. A game streams megabytes a second through it, four
// bytes at a time. Only the stores' slow branch looks at it.
using PipeWrite = void (*)(Cpu&, uint32_t value, unsigned size);
inline PipeWrite g_pipe_write = nullptr;
inline bool is_pipe(uint32_t a) { return (a & ~31u) == 0xCC008000u; }

// Plain memory past kSlowFrom, for an access wholly inside it: the uncached
// mirrors of MEM1 (0xC0000000) and MEM2 (0xD0000000), the same memory as
// 0x80000000 and 0x90000000, and the 16 KB locked cache (0xE0000000, its own
// memory at mem + address; the game does its paired-single work there). The
// address in mem, else 0 (the hooks take it: hardware).
inline uint32_t plain_memory(uint32_t a, unsigned size) {
    if (a - 0xC0000000u <= 0x01800000u - size) return a - 0x40000000u;
    if (a - 0xD0000000u <= 0x04000000u - size) return a - 0x40000000u;
    if (a - 0xE0000000u <= 0x00004000u - size) return a;
    return 0;
}

// The slow branch of a load or store at kSlowFrom and above: the plain memory
// there (uncached mirrors, the locked cache), the write-gather pipe, or the hooks.
QILIN_OUTLINE uint32_t read_slow(Cpu& c, uint32_t a, unsigned size) {
    const uint32_t m = plain_memory(a, size);
    if (!m) return c.hooks->read(c, a, size);
    if (size == 1) return c.mem[m];
    if (size == 2) {
        uint16_t v;
        std::memcpy(&v, c.mem + m, 2);
        return std::byteswap(v);
    }
    uint32_t v;
    std::memcpy(&v, c.mem + m, 4);
    return std::byteswap(v);
}
QILIN_OUTLINE void write_slow(Cpu& c, uint32_t a, uint32_t v, unsigned size) {
    if (is_pipe(a) && g_pipe_write) return g_pipe_write(c, v, size);
    const uint32_t m = plain_memory(a, size);
    if (!m) return c.hooks->write(c, a, v, size);
    if (size == 1) {
        c.mem[m] = static_cast<uint8_t>(v);
    } else if (size == 2) {
        const uint16_t s = std::byteswap(static_cast<uint16_t>(v));
        std::memcpy(c.mem + m, &s, 2);
    } else {
        const uint32_t s = std::byteswap(v);
        std::memcpy(c.mem + m, &s, 4);
    }
}

QILIN_INLINE uint32_t read8(Cpu& c, uint32_t a) {
    if (a >= kSlowFrom) [[unlikely]] return read_slow(c, a, 1);
    return c.mem[a];
}
QILIN_INLINE uint32_t read16(Cpu& c, uint32_t a) {
    if (a >= kSlowFrom) [[unlikely]] return read_slow(c, a, 2);
    uint16_t v;
    std::memcpy(&v, c.mem + a, 2);
    return std::byteswap(v);
}
QILIN_INLINE uint32_t read32(Cpu& c, uint32_t a) {
    if (a >= kSlowFrom) [[unlikely]] return read_slow(c, a, 4);
    uint32_t v;
    std::memcpy(&v, c.mem + a, 4);
    return std::byteswap(v);
}
QILIN_INLINE uint64_t read64(Cpu& c, uint32_t a) { return uint64_t{read32(c, a)} << 32 | read32(c, a + 4); }
QILIN_INLINE void write8(Cpu& c, uint32_t a, uint32_t v) {
    if (a >= kSlowFrom) [[unlikely]] return write_slow(c, a, v & 0xFF, 1);
    c.mem[a] = static_cast<uint8_t>(v);
}
QILIN_INLINE void write16(Cpu& c, uint32_t a, uint32_t v) {
    if (a >= kSlowFrom) [[unlikely]] return write_slow(c, a, v & 0xFFFF, 2);
    const uint16_t s = std::byteswap(static_cast<uint16_t>(v));
    std::memcpy(c.mem + a, &s, 2);
}
QILIN_INLINE void write32(Cpu& c, uint32_t a, uint32_t v) {
    if (a >= kSlowFrom) [[unlikely]] return write_slow(c, a, v, 4);
    const uint32_t s = std::byteswap(v);
    std::memcpy(c.mem + a, &s, 4);
}
QILIN_INLINE void write64(Cpu& c, uint32_t a, uint64_t v) {
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

// a * b + c with one rounding: the FMA instruction where the build has it
// (GCC and Clang inline std::fma then; MSVC needs the intrinsic).
QILIN_INLINE double fused(double a, double b, double c) {
#if defined(_MSC_VER) && !defined(__clang__) && defined(__AVX2__)
    return _mm_cvtsd_f64(_mm_fmadd_sd(_mm_set_sd(a), _mm_set_sd(b), _mm_set_sd(c)));
#else
    return std::fma(a, b, c);
#endif
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
    const double r = fused(a, c, subtract ? -b : b);
    return is_nan(r) ? nan3(a, b, c) : r;
}
// The single-precision form: frC rounded to 25 bits, a double fma, and where
// that lands exactly halfway between two singles, the fma's own rounding
// error (found with an error-free transformation) decides the direction, so
// the result is rounded to single only once, as Broadway does.
inline double madd_single(double a, double c, double b, bool subtract) {
    const double cr = round25(c), bs = subtract ? -b : b;
    double r = fused(a, cr, bs);
    const uint64_t rb = bits(r);
    if ((rb & 0x1FFFFFFFull) == 0x10000000ull) {
        const double a1 = bs - r;   // -(a*cr) plus the fma's rounding error, rounded
        const double b1 = r + a1;
        double err = fused(a, cr, a1) + (bs - b1);
        // A product below the smallest subnormal leaves r == bs with an error the
        // transformation can't see (it underflows to zero): the error is then the
        // product itself, and its sign is the product's.
        if (err == 0.0 && r == bs && a != 0.0 && cr != 0.0 && std::isfinite(a) && std::isfinite(cr))
            err = std::signbit(a) != std::signbit(cr) ? -1.0 : 1.0;
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
// A single's denormal, normalised into the double (rare: kept out of line so
// load_single inlines into the lifted code).
[[gnu::noinline, gnu::cold]] inline double load_single_denormal(uint32_t x) {
    const uint64_t v = x;
    uint64_t exp = 1023 - 126, frac = v & 0x7FFFFF;
    do {
        frac <<= 1;
        exp -= 1;
    } while (!(frac & 0x800000));
    return from_bits(((v & 0x80000000u) << 32) | (exp << 52) | ((frac & 0x7FFFFF) << 29));
}
QILIN_INLINE double load_single(uint32_t x) {
    const uint64_t v = x, e = (v >> 23) & 0xFF;
    if (e == 0 && (v & 0x7FFFFF) != 0) [[unlikely]]
        return load_single_denormal(x);
    // Normal, zero, infinity or NaN: the bits move over; the exponent's top
    // bit fills the three new ones (inverted for normals).
    const uint64_t y = (e > 0 && e < 255) ? !(e >> 7) : (e >> 7), z = y << 61 | y << 60 | y << 59;
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

// ---- paired singles: GQR quantisation (psq_l / psq_st)
// A GQR holds the store type (bits 0-2) and scale (8-13), and the load type
// (16-18) and scale (24-29). Types: 0 float, 4 u8, 5 u16, 6 s8, 7 s16; 1-3 are
// invalid. The 6-bit scale s is a power of two: loads multiply by 2^-s and
// stores by 2^s, with s read as signed (32-63 mean -32..-1).
enum class Quant : unsigned { kFloat = 0, kU8 = 4, kU16 = 5, kS8 = 6, kS16 = 7 };
inline bool quant_valid(unsigned type) { return type == 0 || type >= 4; }
inline unsigned quant_size(unsigned type) { return type == 0 ? 4 : (type == 4 || type == 6) ? 1 : 2; }
inline float pow2f(int e) { return std::ldexp(1.0f, e); }
inline int signed_scale(unsigned s) { return s < 32 ? static_cast<int>(s) : static_cast<int>(s) - 64; }

// One loaded element (raw is the big-endian value read, zero-extended) to its double.
inline double dequantize(unsigned type, unsigned scale, uint32_t raw) {
    float v;
    switch (type) {
    case 4: v = static_cast<float>(static_cast<uint8_t>(raw)); break;
    case 5: v = static_cast<float>(static_cast<uint16_t>(raw)); break;
    case 6: v = static_cast<float>(static_cast<int8_t>(static_cast<uint8_t>(raw))); break;
    case 7: v = static_cast<float>(static_cast<int16_t>(static_cast<uint16_t>(raw))); break;
    default: return load_single(raw);
    }
    return static_cast<double>(v * pow2f(-signed_scale(scale)));
}

// stfs's conversion, but values in the single-subnormal range become signed zeros.
inline uint32_t store_single_ftz(double d) {
    const uint64_t x = bits(d);
    const uint32_t e = static_cast<uint32_t>((x >> 52) & 0x7FF);
    if (e > 896 || (x & ~kSign) == 0) return static_cast<uint32_t>(((x >> 32) & 0xC0000000u) | ((x >> 29) & 0x3FFFFFFFu));
    return static_cast<uint32_t>((x >> 32) & 0x80000000u);
}

// One element to store: scaled in single precision, clamped, truncated toward
// zero. A NaN stores as 0 (Dolphin leaves this to the host; unverified).
inline uint32_t quantize(unsigned type, unsigned scale, double value) {
    if (type == 0) return store_single_ftz(value);
    const float v = static_cast<float>(value) * pow2f(signed_scale(scale));
    float lo, hi;
    switch (type) {
    case 4: lo = 0.0f, hi = 255.0f; break;
    case 5: lo = 0.0f, hi = 65535.0f; break;
    case 6: lo = -128.0f, hi = 127.0f; break;
    default: lo = -32768.0f, hi = 32767.0f; break;
    }
    if (v != v) return 0;
    const float c = v < lo ? lo : v > hi ? hi : v;
    const int32_t i = static_cast<int32_t>(c);
    return static_cast<uint32_t>(i) & (quant_size(type) == 1 ? 0xFFu : 0xFFFFu);
}

}  // namespace fp

// psq_l / psq_st over guest memory, with GQR i. w: one element (ps1 = 1.0 on load).
// The quantised types, and invalid GQRs (out of line: the float type is the common one).
QILIN_OUTLINE void psq_load_quantized(Cpu& c, uint32_t ea, unsigned i, bool w, double& ps0, double& ps1) {
    const uint32_t g = c.gqr[i];
    const unsigned type = (g >> 16) & 7, scale = (g >> 24) & 63;
    if (!fp::quant_valid(type)) c.hooks->trap(c, ea, "psq_l with an invalid GQR load type");
    const unsigned n = fp::quant_size(type);
    auto read = [&](uint32_t a) { return n == 1 ? read8(c, a) : n == 2 ? read16(c, a) : read32(c, a); };
    const double v0 = fp::dequantize(type, scale, read(ea));
    const double v1 = w ? 1.0 : fp::dequantize(type, scale, read(ea + n));
    ps0 = v0;
    ps1 = v1;
}
QILIN_OUTLINE void psq_store_quantized(Cpu& c, uint32_t ea, unsigned i, bool w, double ps0, double ps1) {
    const uint32_t g = c.gqr[i];
    const unsigned type = g & 7, scale = (g >> 8) & 63;
    if (!fp::quant_valid(type)) c.hooks->trap(c, ea, "psq_st with an invalid GQR store type");
    const unsigned n = fp::quant_size(type);
    auto write = [&](uint32_t a, uint32_t v) { n == 1 ? write8(c, a, v) : n == 2 ? write16(c, a, v) : write32(c, a, v); };
    write(ea, fp::quantize(type, scale, ps0));
    if (!w) write(ea + n, fp::quantize(type, scale, ps1));
}
QILIN_INLINE void psq_load(Cpu& c, uint32_t ea, unsigned i, bool w, double& ps0, double& ps1) {
    if (((c.gqr[i] >> 16) & 7) == 0) [[likely]] {  // floats: lfs's conversion, two at a time
        const double v0 = fp::load_single(read32(c, ea));
        const double v1 = w ? 1.0 : fp::load_single(read32(c, ea + 4));
        ps0 = v0;
        ps1 = v1;
        return;
    }
    psq_load_quantized(c, ea, i, w, ps0, ps1);
}
QILIN_INLINE void psq_store(Cpu& c, uint32_t ea, unsigned i, bool w, double ps0, double ps1) {
    if ((c.gqr[i] & 7) == 0) [[likely]] {
        write32(c, ea, fp::store_single_ftz(ps0));
        if (!w) write32(c, ea + 4, fp::store_single_ftz(ps1));
        return;
    }
    psq_store_quantized(c, ea, i, w, ps0, ps1);
}

}  // namespace qilin::rt
