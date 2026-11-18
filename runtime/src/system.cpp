// The guest machine: memory, dispatcher, replacement registry, fatal errors.
// Runtime code: no std::format (macOS 10.15, see CMakeLists.txt).
#include "qilin/rt/system.hpp"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "qilin/rt/memory.hpp"

namespace qilin::rt {
namespace {

constexpr uint32_t kMem1 = 0x80000000u, kMem1Size = 24u << 20;
constexpr uint32_t kMem2 = 0x90000000u, kMem2Size = 64u << 20;
constexpr uint32_t kLockedCache = 0xE0000000u, kLockedCacheSize = 16u << 10;

struct Registered {
    uint32_t address;
    const char* name;
    Replacement fn;
};

struct State {
    GuestMemory memory;
    Program program;
    std::vector<Registered> registered;  // sorted by address
};
State* g = nullptr;

void vreport(Cpu* c, const char* format, va_list args);

// The uncached mirrors of MEM1 and MEM2 are the same memory, and the locked
// cache is 16 KB of memory of its own; everything else in the slow range is
// hardware, which nothing handles yet.
uint8_t* slow_ptr(Cpu& c, uint32_t a, unsigned size, bool store) {
    if ((a >= 0xC0000000u && a + size <= 0xC0000000u + kMem1Size) || (a >= 0xD0000000u && a + size <= 0xD0000000u + kMem2Size))
        return c.mem + (a - 0x40000000u);
    if (a >= kLockedCache && a + size <= kLockedCache + kLockedCacheSize) return c.mem + a;
    fatal(&c, "%s of %u bytes at %08X: hardware %s isn't handled yet", store ? "store" : "load", size, a,
          a == 0xCC008000u ? "(the GX write-gather pipe)" : "registers");
}
uint32_t on_read(Cpu& c, uint32_t a, unsigned size) {
    const uint8_t* p = slow_ptr(c, a, size, false);
    return size == 1 ? p[0] : size == 2 ? uint32_t{p[0]} << 8 | p[1] : uint32_t{p[0]} << 24 | uint32_t{p[1]} << 16 | uint32_t{p[2]} << 8 | p[3];
}
void on_write(Cpu& c, uint32_t a, uint32_t v, unsigned size) {
    uint8_t* p = slow_ptr(c, a, size, true);
    for (unsigned i = 0; i < size; ++i) p[i] = static_cast<uint8_t>(v >> (8 * (size - 1 - i)));
}
void on_call(Cpu& c, uint32_t target) { call(c, target); }
void on_trap(Cpu& c, uint32_t pc, const char* why) { fatal(&c, "guest trap at %08X: %s", pc, why); }
void on_replaced(Cpu& c, uint32_t f) {
    const auto it = std::lower_bound(g->registered.begin(), g->registered.end(), f,
                                     [](const Registered& r, uint32_t a) { return r.address < a; });
    if (it != g->registered.end() && it->address == f) [[likely]] {
        it->fn(c);
        return;
    }
    const auto& rep = g->program.replaced;
    const auto e = std::lower_bound(rep.begin(), rep.end(), f, [](const ReplacedEntry& r, uint32_t a) { return r.address < a; });
    if (e != rep.end() && e->address == f && e->dropped)
        fatal(&c, "dropped code was called: %08X (%s), called from %08X", f, e->library, c.lr);
    fatal(&c, "no replacement yet for %08X (%s), called from %08X", f,
          e != rep.end() && e->address == f ? e->library : "?", c.lr);
}

const CpuHooks kHooks{on_read, on_write, on_call, on_trap, on_replaced};

bool in_ram(uint32_t a, uint32_t n) {
    return (a >= kMem1 && a - kMem1 + n <= kMem1Size) || (a >= kMem2 && a - kMem2 + n <= kMem2Size);
}

// The guest's call stack from its back chain: each frame's first word points
// to the caller's frame, whose second word holds the saved LR.
void backtrace(const Cpu& c) {
    std::fprintf(stderr, "  r1=%08X lr=%08X (in %08X) r3=%08X r4=%08X r5=%08X\n", c.r[1], c.lr,
                 function_containing(c.lr), c.r[3], c.r[4], c.r[5]);
    uint32_t sp = c.r[1];
    for (int depth = 0; depth < 48 && in_ram(sp, 8); ++depth) {
        const uint32_t back = peek32(sp);
        if (back == 0xFFFFFFFFu || back == 0 || !in_ram(back, 8) || back <= sp) break;
        const uint32_t lr = peek32(back + 4);
        std::fprintf(stderr, "  #%-2d %08X (in %08X)\n", depth, lr, function_containing(lr));
        sp = back;
    }
}

void vreport(Cpu* c, const char* format, va_list args) {
    std::fputs("punchout: ", stderr);
    std::vfprintf(stderr, format, args);
    std::fputc('\n', stderr);
    if (c) backtrace(*c);
    std::fflush(stderr);
}

}  // namespace

void init(const Program& program) {
    if (g) fatal(nullptr, "init called twice");
    g = new State;
    g->program = program;
    if (!g->memory.reserve()) fatal(nullptr, "cannot reserve 4 GB of address space for the guest");
    if (!g->memory.commit(kMem1, kMem1Size) || !g->memory.commit(kMem2, kMem2Size) ||
        !g->memory.commit(kLockedCache, kLockedCacheSize))
        fatal(nullptr, "cannot commit the guest's MEM1, MEM2 and locked cache");
    for (size_t i = 1; i < program.functions.size(); ++i)
        if (program.functions[i - 1].address >= program.functions[i].address) fatal(nullptr, "function table isn't sorted");
}

void provide(std::span<const Provided> replacements) {
    const auto& rep = g->program.replaced;
    for (const Provided& p : replacements) {
        const auto e = std::lower_bound(rep.begin(), rep.end(), p.address,
                                        [](const ReplacedEntry& r, uint32_t a) { return r.address < a; });
        if (e == rep.end() || e->address != p.address)
            fatal(nullptr, "replacement %s at %08X: the plan lifts that function, or it isn't one", p.name, p.address);
        if (e->dropped) fatal(nullptr, "replacement %s at %08X: the plan drops that function", p.name, p.address);
        g->registered.push_back({p.address, p.name, p.fn});
    }
    std::sort(g->registered.begin(), g->registered.end(), [](const Registered& a, const Registered& b) { return a.address < b.address; });
    for (size_t i = 1; i < g->registered.size(); ++i)
        if (g->registered[i - 1].address == g->registered[i].address)
            fatal(nullptr, "%08X is replaced twice (%s, %s)", g->registered[i].address, g->registered[i - 1].name, g->registered[i].name);
}

Cpu new_cpu() {
    Cpu c;
    c.mem = g->memory.base();
    c.hooks = &kHooks;
    return c;
}

uint8_t* guest_base() { return g->memory.base(); }

void call(Cpu& c, uint32_t address) {
    const auto& f = g->program.functions;
    const auto it = std::lower_bound(f.begin(), f.end(), address, [](const FunctionEntry& e, uint32_t a) { return e.address < a; });
    if (it == f.end() || it->address != address) [[unlikely]]
        fatal(&c, "call to %08X, which isn't the start of a function (from %08X)", address, c.lr);
    it->fn(c);
}

uint32_t function_containing(uint32_t address) {
    if (!g) return 0;
    const auto& f = g->program.functions;
    const auto it = std::upper_bound(f.begin(), f.end(), address, [](uint32_t a, const FunctionEntry& e) { return a < e.address; });
    return it == f.begin() ? 0 : std::prev(it)->address;
}

void fatal(Cpu* c, const char* format, ...) {
    va_list args;
    va_start(args, format);
    vreport(c, format, args);
    va_end(args);
    std::fflush(nullptr);
    std::_Exit(70);  // EX_SOFTWARE; other guest threads may be parked: no static destructors
}

void log(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vreport(nullptr, format, args);
    va_end(args);
}

}  // namespace qilin::rt
