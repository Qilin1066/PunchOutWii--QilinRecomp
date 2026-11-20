// Where the runtime's messages go, and what a crash says (system.cpp).
//
// Everything rt::log and rt::fatal print goes to stderr and, once a log file
// is set, to it too: on Windows the game's console closes with it, so the
// file is what's left to read after a crash. A crash the runtime didn't
// catch itself (an access violation, an illegal instruction) is reported by
// install_crash_handler's handler: what happened, and the guest's registers
// and call stack on the thread it happened on.
#pragma once

#include <cstdio>

namespace qilin::rt {

struct Cpu;

// Mirror the messages to `file` (open for writing; nullptr: stderr only).
void set_log_file(std::FILE* file);
// The guest CPU this host thread runs (for a crash's call stack).
void set_thread_cpu(Cpu* c);
Cpu* thread_cpu();
// The log file's path, named after a fatal error or crash.
void set_log_path(const char* path);
// Report a crash: `what`, then this thread's guest registers and call stack.
void report_crash(const char* what);
// Report crashes the program didn't catch, then end it (exit code 70, as
// fatal). Also an exception nothing caught (std::terminate).
void install_crash_handler();

}  // namespace qilin::rt
