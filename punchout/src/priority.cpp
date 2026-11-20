// Host thread priorities (priority.hpp).
#include "priority.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dwmapi.h>  // the types only: the function is looked up at run time
#include <chrono>
#elif defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#else
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace punchout {

#if defined(_WIN32)
namespace {
// Looked up when first needed, so the program needs neither avrt.dll nor
// dwmapi.dll to start (both are always there on Windows 10 and 11).
template <class F>
F from_dll(const wchar_t* dll, const char* name) {
    HMODULE m = LoadLibraryW(dll);
    return m ? reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(m, name))) : nullptr;
}
}  // namespace
#endif

bool set_this_thread_priority(Priority p) {
#if defined(_WIN32)
    if (p == Priority::high) {
        using Av = HANDLE(WINAPI*)(LPCWSTR, LPDWORD);
        static const Av av = from_dll<Av>(L"avrt.dll", "AvSetMmThreadCharacteristicsW");
        DWORD task = 0;
        if (av) av(L"Games", &task);  // without the scheduler service: no change
    }
    return SetThreadPriority(GetCurrentThread(),
                             p == Priority::high ? THREAD_PRIORITY_ABOVE_NORMAL : THREAD_PRIORITY_BELOW_NORMAL) != 0;
#elif defined(__APPLE__)
    return pthread_set_qos_class_self_np(p == Priority::high ? QOS_CLASS_USER_INTERACTIVE : QOS_CLASS_UTILITY, 0) == 0;
#else
    const id_t tid = static_cast<id_t>(syscall(SYS_gettid));  // per thread on Linux
    return setpriority(PRIO_PROCESS, tid, p == Priority::high ? -5 : 10) == 0;
#endif
}

void tune_process() {
#if defined(_WIN32) && defined(PROCESS_POWER_THROTTLING_CURRENT_VERSION)
    PROCESS_POWER_THROTTLING_STATE t{};
    t.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
#if defined(PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION)
    t.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
#else
    t.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | 0x4;  // ..._IGNORE_TIMER_RESOLUTION
#endif
    t.StateMask = 0;  // neither: full speed, the timer as asked
    // Looked up at run time: the headers declare it only for Windows 8 and up.
    using Set = BOOL(WINAPI*)(HANDLE, PROCESS_INFORMATION_CLASS, LPVOID, DWORD);
    if (auto set = from_dll<Set>(L"kernel32.dll", "SetProcessInformation"))
        set(GetCurrentProcess(), ProcessPowerThrottling, &t, sizeof t);  // before Windows 10: refused, harmless
#endif
}

bool display_vblank(uint64_t& vblank_ns, uint64_t& period_ns) {
#if defined(_WIN32)
    using Get = HRESULT(WINAPI*)(HWND, DWM_TIMING_INFO*);
    static const Get get = from_dll<Get>(L"dwmapi.dll", "DwmGetCompositionTimingInfo");
    if (!get) return false;
    DWM_TIMING_INFO ti{};
    ti.cbSize = sizeof ti;
    if (FAILED(get(nullptr, &ti)) || ti.qpcRefreshPeriod == 0) return false;
    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    const auto steady = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    const double ns_per_tick = 1e9 / double(freq.QuadPart);
    const double ago = double(int64_t(now.QuadPart) - int64_t(ti.qpcVBlank)) * ns_per_tick;  // negative: a vblank ahead
    vblank_ns = uint64_t(int64_t(steady) - int64_t(ago));
    period_ns = uint64_t(double(ti.qpcRefreshPeriod) * ns_per_tick);
    return period_ns > 0;
#else
    (void)vblank_ns, (void)period_ns;
    return false;
#endif
}

}  // namespace punchout
