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

// Run just before the program ends without destructors (the game leaving,
// a reset, the window closing): main.cpp saves the pipeline cache there.
inline void (*g_before_exit)() = nullptr;
inline void before_exit() {
    if (g_before_exit) g_before_exit();
}

// Queue an interrupt's work; it runs with interrupts off, at the next safe point.
void post_interrupt(std::function<void(qilin::rt::Cpu&)> work);
// A source of timed interrupts (the retrace, alarms): `poll` is called at
// every safe point to post whatever has come due by the host clock;
// `deadline` says when the next one is due (UINT64_MAX: none), so a wait with
// every thread blocked knows how long to sleep.
void add_timer_source(std::function<void()> poll, std::function<uint64_t()> deadline);
uint64_t next_timer_deadline();
// Sleep up to `ns` while every guest thread waits, or until another host
// thread calls wake_idle because it has something for a timer source to
// post (the GPU thread, reaching a draw-done command). `on_time`: the end is a
// timer's deadline (a retrace), so it is met to within tens of microseconds:
// the OS sleeps until shortly before it, and the rest is spent yielding.
// Otherwise the end is only an upper bound and the OS's timer slack is fine.
void idle_sleep(uint64_t ns, bool on_time = true);
void wake_idle();

// Called when the guest writes WPAR (PPCMtwpar), which resets the write-gather
// pipe; the GPU (gpu.cpp) installs it.
void on_wpar_write(void (*reset)());
// The guest's time base: 60.75 MHz (a quarter of the bus clock) since boot,
// less the time the game spent paused.
uint64_t time_base();

// Pausing the game for the settings menu. The main thread asks; the guest's
// running thread parks at the next retrace (vi.cpp), and since only one
// guest thread runs at a time the whole game stands still. The time base
// stops while it's parked, so its clock, alarms and audio go on from where
// they were.
void request_pause(bool on);
bool game_parked();
void park_if_requested();  // the guest's thread, at each retrace  // ticks since 1 January 2000, 00:00 local time, as on the console
// Days since 1970-01-01 of a Gregorian date, and back.
int64_t days_from_civil(int64_t y, unsigned m, unsigned d);
void civil_from_days(int64_t days, int64_t& y, unsigned& m, unsigned& d);

// Start the program again with its original command line (the game's reset).
// main sets it up; it doesn't return.
void set_command_line(int argc, char** argv);
[[noreturn]] void restart_program();
inline constexpr uint64_t kTimerHz = 60750000;
// A safe point: run what's queued if interrupts are on, then switch threads
// if that woke one of higher priority (threads.cpp).
void deliver_interrupts(qilin::rt::Cpu& c);
// The same with interrupts taken as on, without switching: for the wait loop
// that runs while every thread is blocked. True if anything ran.
bool deliver_interrupts_idle(qilin::rt::Cpu& c);
bool has_pending_interrupts();
// The back edge of a lifted poll loop (rt::set_idle): deliver what's due, or
// sleep until the next timed event (a millisecond at most).
void poll_loop(qilin::rt::Cpu& c);
bool interrupts_enabled();
uint32_t interrupt_mask();  // the OS's: bit 31-n set masks interrupt n
void set_interrupts_enabled(bool on);  // a thread switch restores the new thread's
bool in_interrupt();                   // inside an interrupt's work

// Raise OS interrupt `n` (__OS_INTERRUPT_*: 17 CP, 18 PE token, 19 PE
// finish ...): its registered handler runs as an interrupt, if unmasked.
void raise_interrupt(uint32_t n);
// Inside an interrupt's work: run OS interrupt `n`'s handler now. False if
// the OS has it masked (nothing ran). For level-triggered sources (the DSP).
bool call_interrupt_handler(qilin::rt::Cpu& c, uint32_t n);

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
