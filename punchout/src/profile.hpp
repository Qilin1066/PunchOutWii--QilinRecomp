// A sampling profiler for development (profile.cpp).
#pragma once

namespace punchout {

// With PUNCHOUT_PROFILE=<file> set, sample the instruction address every
// millisecond of CPU time and write the counts to <file> every five seconds.
// Without it, nothing.
void start_profiler();

}  // namespace punchout
