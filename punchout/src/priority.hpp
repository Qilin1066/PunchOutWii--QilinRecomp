// Host thread priorities: the threads that pace frames (the game's, the GPU
// command thread, the main loop that presents) ahead of the ones that only
// need to finish eventually (shader compiles, cache saves), so background work
// never takes the core a frame is waiting on.
//
// Best effort: Linux lets anyone lower a thread (nice) but raising one needs
// CAP_SYS_NICE or an RLIMIT_NICE allowance, so there `high` usually stays
// normal and the gap comes from the background threads alone.
#pragma once

namespace punchout {

enum class Priority { background, high };

// Sets the calling thread's priority. True if the system took it.
bool set_this_thread_priority(Priority p);

}  // namespace punchout
