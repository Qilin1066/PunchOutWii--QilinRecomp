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
#include <cstdint>
#include <cstring>

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
// Dolphin's interpreter has it since PR #6943 ("correct negative overflow
// handling for divw"). To confirm on hardware data with the differential test.
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

}  // namespace qilin::rt
