// The thread model (threads.cpp).
#pragma once

#include <cstdint>
#include <span>

#include "qilin/rt/system.hpp"

namespace punchout {

// The main thread: the OS's default thread, running on `main_cpu`.
void init_threads(qilin::rt::Cpu& main_cpu);
std::span<const qilin::rt::Provided> thread_replacements();

// Sleep the running thread on a queue (a guest OSThreadQueue's address, or a
// key of the runtime's own below 0x80000000), and wake every thread on one.
void wait_on(qilin::rt::Cpu& c, uint32_t queue);
void wake_all(qilin::rt::Cpu& c, uint32_t queue);

// Run fn(arg) on a new host thread with a stack big enough for lifted code
// (16 MB), detached.
void run_on_host_thread(void (*fn)(void*), void* arg);

// A guest thread's function returned or it called OSExitThread: hand the guest
// CPU on. Returns once another thread runs; its host thread then ends.
void finish_thread(qilin::rt::Cpu& c, uint32_t value);

}  // namespace punchout
