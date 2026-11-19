// A sampling profiler for development: PUNCHOUT_PROFILE=<file> (Linux only).
//
// SIGPROF fires every millisecond of CPU time the process uses, on whichever
// thread is running; the handler records the interrupted instruction address.
// Every five seconds the addresses so far are written to <file>, one hex
// address per line with its count, for tools/analysis/profile_report.py to
// resolve against the executable; the first line gives its load address. It needs no ptrace or perf events, which
// sandboxes often lack.
#include "profile.hpp"

#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
#include <link.h>
#include <signal.h>
#include <sys/time.h>
#include <ucontext.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "qilin/rt/system.hpp"

namespace punchout {
namespace {

constexpr size_t kRing = 1u << 20;  // samples between flushes (5 s at 1 kHz is 5,000 per thread)
uintptr_t g_ring[kRing];
std::atomic<size_t> g_head{0};

void on_sample(int, siginfo_t*, void* context) {
    const auto* uc = static_cast<const ucontext_t*>(context);
#if defined(__x86_64__)
    const uintptr_t pc = static_cast<uintptr_t>(uc->uc_mcontext.gregs[REG_RIP]);
#else
    const uintptr_t pc = static_cast<uintptr_t>(uc->uc_mcontext.pc);
#endif
    g_ring[g_head.fetch_add(1, std::memory_order_relaxed) % kRing] = pc;
}

}  // namespace

void start_profiler() {
    const char* path = std::getenv("PUNCHOUT_PROFILE");
    if (!path || !*path) return;
    struct sigaction sa = {};
    sa.sa_sigaction = on_sample;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGPROF, &sa, nullptr) != 0) qilin::rt::fatal(nullptr, "PUNCHOUT_PROFILE: can't install the SIGPROF handler");
    const itimerval every_ms = {{0, 1000}, {0, 1000}};
    if (setitimer(ITIMER_PROF, &every_ms, nullptr) != 0) qilin::rt::fatal(nullptr, "PUNCHOUT_PROFILE: can't start the profiling timer");
    // The writer: counts accumulate here and the whole table is rewritten, so
    // the file is complete whenever the game stops.
    // The executable's load address (the first object dl_iterate_phdr lists),
    // so the report can resolve addresses in a position-independent build.
    uintptr_t base = 0;
    dl_iterate_phdr([](dl_phdr_info* info, size_t, void* out) {
        *static_cast<uintptr_t*>(out) = info->dlpi_addr;
        return 1;
    }, &base);
    std::thread([out = std::string(path), base] {
        std::unordered_map<uintptr_t, uint64_t> counts;
        size_t done = 0;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            const size_t head = g_head.load(std::memory_order_relaxed);
            if (head - done > kRing) done = head - kRing;  // overrun: keep the newest
            for (; done < head; ++done) ++counts[g_ring[done % kRing]];
            if (FILE* f = std::fopen((out + ".tmp").c_str(), "w")) {
                std::fprintf(f, "base %zx\n", static_cast<size_t>(base));
                for (const auto& [pc, n] : counts) std::fprintf(f, "%zx %llu\n", static_cast<size_t>(pc), static_cast<unsigned long long>(n));
                std::fclose(f);
                std::rename((out + ".tmp").c_str(), out.c_str());
            }
        }
    }).detach();
    qilin::rt::log("profiling to %s (SIGPROF, 1 kHz of CPU time)", path);
}

}  // namespace punchout

#else

#include <cstdlib>

#include "qilin/rt/system.hpp"

namespace punchout {
void start_profiler() {
    if (const char* path = std::getenv("PUNCHOUT_PROFILE"); path && *path)
        qilin::rt::fatal(nullptr, "PUNCHOUT_PROFILE works on Linux (x86-64, ARM64) only");
}
}  // namespace punchout

#endif
