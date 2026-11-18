// The guest machine the lifted program runs on: its memory, the function
// table the dispatcher searches, the registry of native replacements, and the
// one way the runtime stops when something isn't handled: fatal(), which names
// what was reached and prints the guest call stack.
//
// One guest per process, so this is a set of free functions over one state.
//
// Runtime code: no std::format here (macOS 10.15, see CMakeLists.txt).
#pragma once

#include <cstdint>
#include <span>

#include "qilin/rt/cpu.hpp"

#if defined(__GNUC__) || defined(__clang__)
#define QILIN_PRINTF(f, a) __attribute__((format(printf, f, a)))
#else
#define QILIN_PRINTF(f, a)
#endif

namespace qilin::rt {

// What qilin lift generates (functions.hpp, table.cpp, replaced.cpp).
struct FunctionEntry {
    uint32_t address;
    GuestFn fn;
};
struct ReplacedEntry {
    uint32_t address;
    const char* library;
    bool dropped;  // dropped code never runs; replaced code runs natively
};
struct Program {
    std::span<const FunctionEntry> functions;  // sorted by address
    std::span<const ReplacedEntry> replaced;   // sorted by address
};

// A native replacement for a guest function: arguments in r3-r10 and f1-f8,
// results in r3 (r4) or f1, as the PowerPC EABI passes them.
using Replacement = void (*)(Cpu&);
struct Provided {
    uint32_t address;
    const char* name;
    Replacement fn;
};

// Reserve the 4 GB guest address space and back MEM1 (0x80000000, 24 MB),
// MEM2 (0x90000000, 64 MB) and the locked cache (0xE0000000, 16 KB, reached
// through the hooks like the uncached mirrors). Fails loudly.
void init(const Program& program);

// Register replacements. Each address must be one the program replaces or
// drops: registering a lifted function is a mistake and stops the runtime.
void provide(std::span<const Provided> replacements);

// Hardware registers (0xCC000000-0xCD00FFFF, the write-gather pipe among
// them): accesses there go to these. Without them, any access is fatal.
using MmioRead = uint32_t (*)(Cpu&, uint32_t address, unsigned size);
using MmioWrite = void (*)(Cpu&, uint32_t address, uint32_t value, unsigned size);
void set_mmio(MmioRead read, MmioWrite write);

// Lifted poll loops (a branch back over only loads and compares) call this on
// every pass, with the registers written back: where interrupts can arrive.
// Without one, they spin.
using IdleHook = void (*)(Cpu&);
void set_idle(IdleHook hook);

// A Cpu for a new guest thread: zeroed registers, guest memory and the hooks.
Cpu new_cpu();
uint8_t* guest_base();

// Call the guest function at `address` (lifted, or replaced): fatal if there's none.
void call(Cpu& c, uint32_t address);
// The start of the function holding `address`, or 0.
uint32_t function_containing(uint32_t address);

[[noreturn]] void fatal(Cpu* c, const char* format, ...) QILIN_PRINTF(2, 3);
void log(const char* format, ...) QILIN_PRINTF(1, 2);

// Guest memory from native code, in the guest's byte order.
inline uint8_t* guest_ptr(uint32_t a) { return guest_base() + a; }
inline uint32_t peek32(uint32_t a) {
    uint32_t v;
    std::memcpy(&v, guest_ptr(a), 4);
    return std::byteswap(v);
}
inline uint16_t peek16(uint32_t a) {
    uint16_t v;
    std::memcpy(&v, guest_ptr(a), 2);
    return std::byteswap(v);
}
inline void poke32(uint32_t a, uint32_t v) {
    v = std::byteswap(v);
    std::memcpy(guest_ptr(a), &v, 4);
}
inline void poke16(uint32_t a, uint16_t v) {
    v = std::byteswap(v);
    std::memcpy(guest_ptr(a), &v, 2);
}
inline void poke8(uint32_t a, uint8_t v) { *guest_ptr(a) = v; }

}  // namespace qilin::rt
