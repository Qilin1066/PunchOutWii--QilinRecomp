// What the replacements share: the interrupt model and calls into guest code.
//
// The guest's interrupt handlers (a DVD read finishing, an alarm, the
// retrace) run its callbacks on whichever thread was interrupted, with
// interrupts off. Here, completions are queued and delivered at safe points:
// whenever the guest enables interrupts, and inside every replacement that
// waits or polls. Only replaced calls can see a delivery, and each one is
// entered with the guest's registers written back, which makes a delivery the
// same as an interrupt arriving at that instruction.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>

#include "qilin/rt/system.hpp"

namespace punchout {

// Queue an interrupt's work; it runs with interrupts off, at the next safe point.
void post_interrupt(std::function<void(qilin::rt::Cpu&)> work);
// A source of timed interrupts (the retrace, alarms): `poll` is called at
// every safe point to post whatever has come due by the host clock;
// `deadline` says when the next one is due (UINT64_MAX: none), so a wait with
// every thread blocked knows how long to sleep.
void add_timer_source(std::function<void()> poll, std::function<uint64_t()> deadline);
uint64_t next_timer_deadline();
// The guest's time base: 60.75 MHz (a quarter of the bus clock) since boot.
uint64_t time_base();
inline constexpr uint64_t kTimerHz = 60750000;
// A safe point: run what's queued if interrupts are on, then switch threads
// if that woke one of higher priority (threads.cpp).
void deliver_interrupts(qilin::rt::Cpu& c);
// The same with interrupts taken as on, without switching: for the wait loop
// that runs while every thread is blocked. True if anything ran.
bool deliver_interrupts_idle(qilin::rt::Cpu& c);
bool has_pending_interrupts();
bool interrupts_enabled();
void set_interrupts_enabled(bool on);  // a thread switch restores the new thread's
bool in_interrupt();                   // inside an interrupt's work

// threads.cpp: after an interrupt's work, switch if it readied a thread of
// higher priority than the one running.
void preempt(qilin::rt::Cpu& c);

// Call guest function `fn` with r3, r4 as if from an interrupt: on a fresh
// frame below the current stack, with every register restored afterwards.
void guest_callback(qilin::rt::Cpu& c, uint32_t fn, uint32_t a0, uint32_t a1);

// The extracted disc (boot.cpp sets it).
const std::filesystem::path& game_folder();
void set_game_folder(std::filesystem::path folder);

}  // namespace punchout
