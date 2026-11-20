// The crash handler (report.hpp): signals on POSIX, the unhandled-exception
// filter on Windows, and std::terminate. What it does isn't async-signal-safe
// (formatting, stdio), but the program is ending either way; SA_RESETHAND
// means a second fault inside the handler just ends it.
#include "qilin/rt/report.hpp"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <cstring>
#endif

namespace qilin::rt {

namespace {
[[noreturn]] void on_terminate() {
    const char* what = "an error nothing caught (std::terminate)";
    std::string text;
    if (const std::exception_ptr e = std::current_exception()) {
        try {
            std::rethrow_exception(e);
        } catch (const std::exception& x) {
            text = std::string("an exception nothing caught: ") + x.what();
            what = text.c_str();
        } catch (...) {
            what = "an exception nothing caught";
        }
    }
    report_crash(what);
    std::_Exit(70);
}

#if defined(_WIN32)
LONG WINAPI on_exception(EXCEPTION_POINTERS* e) {
    const EXCEPTION_RECORD& r = *e->ExceptionRecord;
    const char* kind = r.ExceptionCode == EXCEPTION_ACCESS_VIOLATION        ? "an access violation"
                       : r.ExceptionCode == EXCEPTION_STACK_OVERFLOW        ? "a stack overflow"
                       : r.ExceptionCode == EXCEPTION_ILLEGAL_INSTRUCTION   ? "an illegal instruction"
                       : r.ExceptionCode == EXCEPTION_INT_DIVIDE_BY_ZERO    ? "an integer division by zero"
                       : r.ExceptionCode == EXCEPTION_IN_PAGE_ERROR         ? "a page that couldn't be read in"
                                                                            : "an exception";
    char what[160];
    if (r.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r.NumberParameters >= 2)
        std::snprintf(what, sizeof what, "%s (%s address %p) at %p", kind, r.ExceptionInformation[0] == 1 ? "writing" : "reading",
                      reinterpret_cast<void*>(r.ExceptionInformation[1]), r.ExceptionAddress);
    else
        std::snprintf(what, sizeof what, "%s (code %08lX) at %p", kind, static_cast<unsigned long>(r.ExceptionCode), r.ExceptionAddress);
    report_crash(what);
    std::_Exit(70);
}
#else
void on_signal(int sig, siginfo_t* info, void*) {
    const char* kind = sig == SIGSEGV ? "a segmentation fault"
                       : sig == SIGBUS ? "a bus error"
                       : sig == SIGILL ? "an illegal instruction"
                       : sig == SIGFPE ? "an arithmetic fault"
                                       : "an abort";
    char what[128];
    std::snprintf(what, sizeof what, "%s (address %p)", kind, info ? info->si_addr : nullptr);
    report_crash(what);
    std::_Exit(70);
}
#endif
}  // namespace

void install_crash_handler() {
    std::set_terminate(on_terminate);
#if defined(_WIN32)
    SetUnhandledExceptionFilter(on_exception);
#else
    struct sigaction sa;
    std::memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_signal;
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) sigaction(sig, &sa, nullptr);
#endif
}

}  // namespace qilin::rt
