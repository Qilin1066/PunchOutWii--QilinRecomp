// Addresses in the supported build: Punch-Out!! (Europe, Australia) Rev 1,
// disc R7PP01, main.dol SHA-256 6ae3388f... (recomp/src/dol.cpp). Everything
// that names a place in the game's code or data is here, found by reading the
// code (docs/runtime.md says how each one was found).
#pragma once

#include <cstdint>

namespace punchout::build {

// __start (80006124) and what it calls.
inline constexpr uint32_t kStackTop = 0x804292E8u;  // _stack_addr, set by __init_registers (80006304)
inline constexpr uint32_t kSdaBase2 = 0x8041E2C0u;  // r2, _SDA2_BASE_
inline constexpr uint32_t kSdaBase = 0x80419260u;   // r13, _SDA_BASE_
inline constexpr uint32_t kRomCopyInfo = 0x80006420u;  // __rom_copy_info: {rom, address, size}, size 0 ends
inline constexpr uint32_t kBssInitInfo = 0x800064A4u;  // __bss_init_info: {address, size}, size 0 ends
inline constexpr uint32_t kInitUser = 0x8020C674u;     // __init_user: the static constructors (lifted)
inline constexpr uint32_t kMain = 0x800C6D1Cu;         // main(argc, argv)
inline constexpr uint32_t kExit = 0x8020C6DCu;         // exit(status): destructors, then PPCHalt (lifted)

// OS globals (small data, r13-relative), from OSArena's accessors (80200828-80200888).
inline constexpr uint32_t kArenaLoMem1 = kSdaBase - 22528;  // __OSArenaLo
inline constexpr uint32_t kArenaLoMem2 = kSdaBase - 22524;  // s_mem2ArenaLo
inline constexpr uint32_t kArenaHiMem1 = kSdaBase - 13600;  // __OSArenaHi
inline constexpr uint32_t kArenaHiMem2 = kSdaBase - 13596;  // s_mem2ArenaHi

// MEM1's arena starts after the main stack, 32-byte aligned, when no debugger
// is attached (OSInit, 801FF390); MEM2's comes from IOS (0x80003124).
inline constexpr uint32_t kArenaLoMem1Value = (kStackTop + 31) & ~31u;

}  // namespace punchout::build
