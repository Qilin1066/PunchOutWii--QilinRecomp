// A sampling profiler for development: PUNCHOUT_PROFILE=<file> (Linux only).
//
// SIGPROF fires every millisecond of CPU time the process uses, on whichever
// thread is running; the handler records the interrupted instruction address.
// Every five seconds the addresses so far are written to <file>, one hex
// address per line with its count, for tools/analysis/profile_report.py to
// resolve against the executable; the first line gives its load address. It needs no ptrace or perf events, which
// sandboxes often lack.
//
// Each sample carries its thread: "thread <n> <tid> <name>" lines name them,
// and an address line is "<n>:<hexaddress> <count>". "map <start> <end>
// <offset> <path>" lines list the executable mappings, for samples in
// libraries. PUNCHOUT_PROFILE_FROM=<seconds of game time> drops the samples
// before that (to profile one scene of a scripted run).
#include "profile.hpp"

#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
#include <link.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <ucontext.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "qilin/rt/system.hpp"
#include "video.hpp"

namespace punchout {
namespace {

constexpr size_t kRing = 1u << 20;  // samples between flushes (5 s at 1 kHz is 5,000 per thread)
uintptr_t g_ring[kRing];            // the thread's slot in the top 8 bits, the address below
std::atomic<size_t> g_head{0};
constexpr unsigned kThreads = 128;
std::atomic<int> g_tids[kThreads];

// The calling thread's slot: found or claimed without locks (a signal handler runs this).
uintptr_t thread_slot() {
    const int tid = static_cast<int>(syscall(SYS_gettid));
    for (unsigned i = 0; i < kThreads; ++i) {
        int have = g_tids[i].load(std::memory_order_relaxed);
        if (have == tid) return i;
        if (have == 0 && g_tids[i].compare_exchange_strong(have, tid, std::memory_order_relaxed)) return i;
        if (have == tid) return i;
    }
    return kThreads - 1;  // more threads than slots: the last one takes the rest
}

void on_sample(int, siginfo_t*, void* context) {
    const auto* uc = static_cast<const ucontext_t*>(context);
#if defined(__x86_64__)
    const uintptr_t pc = static_cast<uintptr_t>(uc->uc_mcontext.gregs[REG_RIP]);
#else
    const uintptr_t pc = static_cast<uintptr_t>(uc->uc_mcontext.pc);
#endif
    g_ring[g_head.fetch_add(1, std::memory_order_relaxed) % kRing] = (pc & 0x00FFFFFFFFFFFFFFull) | thread_slot() << 56;
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
    const char* from_text = std::getenv("PUNCHOUT_PROFILE_FROM");
    const double from = from_text ? std::atof(from_text) : 0.0;
    std::thread([out = std::string(path), base, from] {
        std::unordered_map<uintptr_t, uint64_t> counts;
        size_t done = 0;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(from > 0 ? 250 : 5000));
            const size_t head = g_head.load(std::memory_order_relaxed);
            if (head - done > kRing) done = head - kRing;  // overrun: keep the newest
            if (frame_count() / 60.0 < from) {  // not yet: these samples don't count
                done = head;
                continue;
            }
            for (; done < head; ++done) ++counts[g_ring[done % kRing]];
            if (FILE* f = std::fopen((out + ".tmp").c_str(), "w")) {
                std::fprintf(f, "base %zx\n", static_cast<size_t>(base));
                for (unsigned i = 0; i < kThreads; ++i)
                    if (const int tid = g_tids[i].load(std::memory_order_relaxed)) {
                        char name[64] = "?", path_comm[64];
                        std::snprintf(path_comm, sizeof path_comm, "/proc/self/task/%d/comm", tid);
                        if (FILE* c = std::fopen(path_comm, "r")) {
                            if (std::fgets(name, sizeof name, c)) name[std::strcspn(name, "\n")] = 0;
                            std::fclose(c);
                        }
                        std::fprintf(f, "thread %u %d %s\n", i, tid, name);
                    }
                if (FILE* m = std::fopen("/proc/self/maps", "r")) {
                    char line[1024];
                    while (std::fgets(line, sizeof line, m)) {
                        unsigned long long a, b, off;
                        char perms[8], file[768] = "";
                        if (std::sscanf(line, "%llx-%llx %7s %llx %*s %*s %767s", &a, &b, perms, &off, file) >= 4 && perms[2] == 'x' && file[0] == '/')
                            std::fprintf(f, "map %llx %llx %llx %s\n", a, b, off, file);
                    }
                    std::fclose(m);
                }
                for (const auto& [pc, n] : counts)
                    std::fprintf(f, "%u:%zx %llu\n", static_cast<unsigned>(pc >> 56), static_cast<size_t>(pc & 0x00FFFFFFFFFFFFFFull),
                                 static_cast<unsigned long long>(n));
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
