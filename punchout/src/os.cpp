// The OS library's replacements (revolution/os, base, db): start-up, arenas,
// interrupts, caches, time. Arguments arrive in r3..., results go in r3 (and
// r4 for 64-bit values), as the EABI passes them.
#include "os.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <vector>

#include "boot.hpp"
#include "build.hpp"

namespace punchout {
namespace rt = qilin::rt;
using rt::Cpu;

namespace {

// ---- start-up
void DBInit(Cpu&) {}  // no debugger

void OSInit(Cpu&) {  // once: the game's constructors call it again
    static bool done = false;
    if (done) return;
    done = true;
    namespace b = build;
    rt::poke32(b::kArenaLoMem1, b::kArenaLoMem1Value);
    rt::poke32(b::kArenaHiMem1, rt::peek32(0x80003110u));  // MEM1 arena end, from IOS
    rt::poke32(b::kArenaLoMem2, rt::peek32(0x80003124u));
    rt::poke32(b::kArenaHiMem2, rt::peek32(0x80003128u));
    rt::log("OSInit: MEM1 arena %08X-%08X, MEM2 arena %08X-%08X", rt::peek32(b::kArenaLoMem1), rt::peek32(b::kArenaHiMem1),
            rt::peek32(b::kArenaLoMem2), rt::peek32(b::kArenaHiMem2));
}

void OSRegisterVersion(Cpu& c) { rt::log("%s", reinterpret_cast<const char*>(rt::guest_ptr(c.r[3]))); }

void OSPanic(Cpu& c) {  // OSPanic(file, line, format, ...): the format isn't expanded
    rt::fatal(&c, "OSPanic in %s:%u: %s", reinterpret_cast<const char*>(rt::guest_ptr(c.r[3])), c.r[4],
              reinterpret_cast<const char*>(rt::guest_ptr(c.r[5])));
}

// ---- the reset and power buttons: the window's close button will be POWER.
uint32_t g_reset_cb = 0, g_power_cb = 0;
void OSSetResetCallback(Cpu& c) {
    const uint32_t was = g_reset_cb;
    g_reset_cb = c.r[3];
    c.r[3] = was;
}
void OSSetPowerCallback(Cpu& c) {
    const uint32_t was = g_power_cb;
    g_power_cb = c.r[3];
    c.r[3] = was;
}

// ---- memory sizes, as IOS left them in low memory
void OSGetPhysicalMem2Size(Cpu& c) { c.r[3] = rt::peek32(0x80003118u); }
void OSGetConsoleSimulatedMem1Size(Cpu& c) { c.r[3] = rt::peek32(0x80003104u); }
void OSGetConsoleSimulatedMem2Size(Cpu& c) { c.r[3] = rt::peek32(0x8000311Cu); }

// ---- error handlers (crash screens for CPU exceptions): kept, never called;
// a fault in the guest stops the runtime with its own report.
uint32_t g_error_handlers[17] = {};
void OSSetErrorHandler(Cpu& c) {  // (error, handler) -> the previous handler
    const uint32_t e = c.r[3];
    if (e >= 17) rt::fatal(&c, "OSSetErrorHandler for error %u", e);
    const uint32_t was = g_error_handlers[e];
    g_error_handlers[e] = c.r[4];
    c.r[3] = was;
}

// ---- CPU registers (base)
uint32_t g_hid2 = 0xA0000000u;  // load/store quantised and paired singles on (__OSPSInit)
void PPCSync(Cpu&) {}
void PPCHalt(Cpu&) {  // where exit() ends
    rt::log("the game exited");
    std::fflush(nullptr);
    std::_Exit(0);  // other guest threads are parked on host threads: no static destructors
}
void PPCMfhid2(Cpu& c) { c.r[3] = g_hid2; }
void PPCMthid2(Cpu& c) { g_hid2 = c.r[3]; }

// ---- interrupts: one flag, as MSR[EE] is (os.hpp).
bool g_enabled = false;
std::deque<std::function<void(Cpu&)>> g_pending;
struct Timer {
    std::function<void()> poll;
    std::function<uint64_t()> deadline;
};
std::vector<Timer> g_timers;
bool g_in_interrupt = false;
void OSDisableInterrupts(Cpu& c) {
    c.r[3] = g_enabled;
    g_enabled = false;
}
void OSEnableInterrupts(Cpu& c) {
    const bool was = g_enabled;
    g_enabled = true;
    deliver_interrupts(c);
    c.r[3] = was;
}
void OSRestoreInterrupts(Cpu& c) {
    const bool was = g_enabled;
    g_enabled = c.r[3] != 0;
    deliver_interrupts(c);
    c.r[3] = was;
}

// ---- interrupt handlers, by the OS's numbering; a mask bit per interrupt,
// bit 31 for interrupt 0 (as __OSMaskInterrupts takes it). Masked at boot.
uint32_t g_handlers[32] = {};
uint32_t g_mask = 0xFFFFFFFFu;
void __OSSetInterruptHandler(Cpu& c) {  // (interrupt, handler) -> the previous handler
    const uint32_t n = c.r[3];
    if (n >= 32) rt::fatal(&c, "__OSSetInterruptHandler(%u)", n);
    const uint32_t was = g_handlers[n];
    g_handlers[n] = c.r[4];
    c.r[3] = was;
}
void __OSGetInterruptHandler(Cpu& c) { c.r[3] = c.r[3] < 32 ? g_handlers[c.r[3]] : 0; }
void __OSMaskInterrupts(Cpu& c) {  // (mask) -> the mask before
    const uint32_t was = g_mask;
    g_mask |= c.r[3];
    c.r[3] = was;
}
void __OSUnmaskInterrupts(Cpu& c) {
    const uint32_t was = g_mask;
    g_mask &= ~c.r[3];
    c.r[3] = was;
}
// Contexts: the guest's thread switching is native, so only bookkeeping is left.
void OSSetCurrentContext(Cpu& c) { rt::poke32(0x800000D4u, c.r[3]); rt::poke32(0x800000C0u, c.r[3] & 0x3FFFFFFFu); }
void OSClearContext(Cpu&) {}
void OSRegisterShutdownFunction(Cpu&) {}

// The write-gather pipe's address register: the FIFO is drained as written, so
// its "data not yet sent" bit (0) always reads clear.
uint32_t g_wpar = 0;
void PPCMfwpar(Cpu& c) { c.r[3] = g_wpar & ~1u; }
void PPCMtwpar(Cpu& c) { g_wpar = c.r[3]; }

// ---- caches: the host's are coherent, so only dcbz's zeroing is left to do.
void nothing(Cpu&) {}
void DCZeroRange(Cpu& c) {
    const uint32_t start = c.r[3] & ~31u, end = (c.r[3] + c.r[4] + 31) & ~31u;
    if (c.r[4]) std::memset(rt::guest_ptr(start), 0, end - start);
}

// ---- the locked cache: 16 KB at 0xE0000000 (the runtime backs it). Its DMA
// is a copy, done at once, so there's never a queue to wait for.
uint32_t blocks(uint32_t n) { return n == 0 ? 128 : n; }  // 0 means 128 blocks of 32 bytes
void LCLoadBlocks(Cpu& c) { std::memmove(rt::guest_ptr(c.r[3]), rt::guest_ptr(c.r[4] | 0x80000000u), 32 * blocks(c.r[5])); }
void LCStoreBlocks(Cpu& c) { std::memmove(rt::guest_ptr(c.r[3] | 0x80000000u), rt::guest_ptr(c.r[4]), 32 * blocks(c.r[5])); }
void LCStore(Cpu& c) {  // (dest, srcLC, bytes) -> DMA transactions issued
    const uint32_t n = (c.r[5] + 31) / 32;
    std::memmove(rt::guest_ptr(c.r[3] | 0x80000000u), rt::guest_ptr(c.r[4]), 32 * n);
    c.r[3] = (n + 127) / 128;
}

// ---- time: the time base counts at a quarter of the bus clock, from boot.
void OSGetTime(Cpu& c) {
    const uint64_t t = time_base();
    c.r[3] = static_cast<uint32_t>(t >> 32);
    c.r[4] = static_cast<uint32_t>(t);
}
void OSGetTick(Cpu& c) { c.r[3] = static_cast<uint32_t>(time_base()); }

const rt::Provided kReplacements[] = {
    {0x801E17F8u, "DBInit", DBInit},
    {0x801FF230u, "OSInit", OSInit},
    {0x801FFBD8u, "OSRegisterVersion", OSRegisterVersion},
    {0x80201CDCu, "OSPanic", OSPanic},
    {0x80201E08u, "OSSetErrorHandler", OSSetErrorHandler},
    {0x801E16C0u, "PPCSync", PPCSync},
    {0x801E16C8u, "PPCHalt", PPCHalt},
    {0x801E1754u, "PPCMfhid2", PPCMfhid2},
    {0x801E175Cu, "PPCMthid2", PPCMthid2},
    {0x802052FCu, "OSDisableInterrupts", OSDisableInterrupts},
    {0x80205310u, "OSEnableInterrupts", OSEnableInterrupts},
    {0x80205324u, "OSRestoreInterrupts", OSRestoreInterrupts},
    {0x80200DA4u, "DCEnable", nothing},
    {0x80200DB8u, "DCInvalidateRange", nothing},
    {0x80200DE4u, "DCFlushRange", nothing},
    {0x80200E14u, "DCStoreRange", nothing},
    {0x80200E44u, "DCFlushRange without a sync (a dcbf loop)", nothing},
    {0x80200E70u, "DCFlushRangeNoSync", nothing},
    {0x80200E9Cu, "DCZeroRange", DCZeroRange},
    {0x80200EC8u, "ICInvalidateRange", nothing},
    {0x80200EFCu, "ICFlashInvalidate", nothing},
    {0x80200F0Cu, "ICEnable", nothing},
    {0x80205D6Cu, "OSGetPhysicalMem2Size", OSGetPhysicalMem2Size},
    {0x80205D78u, "OSGetConsoleSimulatedMem1Size", OSGetConsoleSimulatedMem1Size},
    {0x80205D84u, "OSGetConsoleSimulatedMem2Size", OSGetConsoleSimulatedMem2Size},
    {0x802099E4u, "OSSetResetCallback", OSSetResetCallback},
    {0x80209AD0u, "OSSetPowerCallback", OSSetPowerCallback},
    {0x80200FECu, "LCEnable", nothing},
    {0x80201024u, "LCDisable", nothing},
    {0x8020104Cu, "LCLoadBlocks", LCLoadBlocks},
    {0x80201070u, "LCStoreBlocks", LCStoreBlocks},
    {0x80201094u, "LCStore", LCStore},
    {0x80201134u, "LCQueueWait", nothing},
    {0x80205348u, "__OSSetInterruptHandler", __OSSetInterruptHandler},
    {0x8020535Cu, "__OSGetInterruptHandler", __OSGetInterruptHandler},
    {0x8020568Cu, "__OSMaskInterrupts", __OSMaskInterrupts},
    {0x8020570Cu, "__OSUnmaskInterrupts", __OSUnmaskInterrupts},
    {0x8020161Cu, "OSSetCurrentContext", OSSetCurrentContext},
    {0x80201844u, "OSClearContext", OSClearContext},
    {0x80206850u, "OSRegisterShutdownFunction", OSRegisterShutdownFunction},
    {0x801E1764u, "PPCMfwpar", PPCMfwpar},
    {0x801E1770u, "PPCMtwpar", PPCMtwpar},
    {0x80209370u, "OSGetTime", OSGetTime},
    {0x80209388u, "OSGetTick", OSGetTick},
};

}  // namespace

std::span<const rt::Provided> os_replacements() { return kReplacements; }

void post_interrupt(std::function<void(Cpu&)> work) { g_pending.push_back(std::move(work)); }

void raise_interrupt(uint32_t n) {
    post_interrupt([n](Cpu& c) {
        if (g_mask & (0x80000000u >> n)) return;  // masked: lost, as nothing re-raises it yet
        const uint32_t h = g_handlers[n];
        if (!h) rt::fatal(&c, "interrupt %u has no handler", n);
        guest_callback(c, h, n, rt::peek32(0x800000D4u));
    });
}
bool interrupts_enabled() { return g_enabled; }

namespace {
const auto g_boot = std::chrono::steady_clock::now();
}
uint64_t time_base() {
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - g_boot).count();
    return static_cast<uint64_t>(ns) * 243u / 4000u;  // 60.75 MHz
}

void add_timer_source(std::function<void()> poll, std::function<uint64_t()> deadline) {
    g_timers.push_back({std::move(poll), std::move(deadline)});
}
uint64_t next_timer_deadline() {
    uint64_t t = UINT64_MAX;
    for (const auto& timer : g_timers) t = std::min(t, timer.deadline());
    return t;
}
bool has_pending_interrupts() { return !g_pending.empty(); }
void set_interrupts_enabled(bool on) { g_enabled = on; }
bool in_interrupt() { return g_in_interrupt; }

namespace {
bool run_pending(Cpu& c) {
    for (const auto& timer : g_timers) timer.poll();
    bool ran = false;
    while (!g_pending.empty()) {
        auto work = std::move(g_pending.front());
        g_pending.pop_front();
        g_enabled = false;  // handlers run with interrupts off
        g_in_interrupt = true;
        work(c);
        g_in_interrupt = false;
        g_enabled = true;
        ran = true;
    }
    return ran;
}
}  // namespace

void deliver_interrupts(Cpu& c) {
    if (!g_enabled || g_in_interrupt) return;
    if (run_pending(c)) preempt(c);
}

bool deliver_interrupts_idle(Cpu& c) {
    const bool was = g_enabled;
    g_enabled = true;
    const bool ran = run_pending(c);
    g_enabled = was;
    return ran;
}

void guest_callback(Cpu& c, uint32_t fn, uint32_t a0, uint32_t a1) {
    const Cpu saved = c;
    // A frame of its own below the interrupted code's, chained to it for backtraces.
    c.r[1] = (saved.r[1] - 0x100u) & ~15u;
    rt::poke32(c.r[1], saved.r[1]);
    c.r[3] = a0;
    c.r[4] = a1;
    c.lr = 0;
    rt::call(c, fn);
    c = saved;
}

namespace {
std::filesystem::path g_folder;
}
const std::filesystem::path& game_folder() { return g_folder; }
void set_game_folder(std::filesystem::path folder) { g_folder = std::move(folder); }

}  // namespace punchout
