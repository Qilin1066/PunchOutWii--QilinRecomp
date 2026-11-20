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
#elif defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#else
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace punchout {

bool set_this_thread_priority(Priority p) {
#if defined(_WIN32)
    return SetThreadPriority(GetCurrentThread(),
                             p == Priority::high ? THREAD_PRIORITY_ABOVE_NORMAL : THREAD_PRIORITY_BELOW_NORMAL) != 0;
#elif defined(__APPLE__)
    return pthread_set_qos_class_self_np(p == Priority::high ? QOS_CLASS_USER_INTERACTIVE : QOS_CLASS_UTILITY, 0) == 0;
#else
    const id_t tid = static_cast<id_t>(syscall(SYS_gettid));  // per thread on Linux
    return setpriority(PRIO_PROCESS, tid, p == Priority::high ? -5 : 10) == 0;
#endif
}

}  // namespace punchout
