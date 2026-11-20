// Host thread priorities: the threads that pace frames (the game's, the GPU
// command thread, the main loop that presents) ahead of the ones that only
// need to finish eventually (shader compiles, cache saves), so background work
// never takes the core a frame is waiting on.
//
// Best effort: Linux lets anyone lower a thread (nice) but raising one needs
// CAP_SYS_NICE or an RLIMIT_NICE allowance, so there `high` usually stays
// normal and the gap comes from the background threads alone.
#pragma once

#include <cstdint>

namespace punchout {

enum class Priority { background, high };

// Sets the calling thread's priority. True if the system took it. On Windows
// `high` also joins the multimedia scheduler's "Games" class (MMCSS), so the
// thread isn't held back behind background work.
bool set_this_thread_priority(Priority p);

// Once, at start: Windows 11 throttles a process whose window is hidden or
// covered (slower cores, its timer request ignored); the game's clock runs
// regardless, so opt out. Elsewhere nothing.
void tune_process();

// The display's last vblank and refresh period, on the steady clock (ns), from
// the compositor (Windows: DwmGetCompositionTimingInfo). False where there are none.
bool display_vblank(uint64_t& vblank_ns, uint64_t& period_ns);

}  // namespace punchout
