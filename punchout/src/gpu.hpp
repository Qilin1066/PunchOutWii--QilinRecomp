// The GPU's front end (gpu.cpp).
#pragma once

#include <cstdint>

namespace punchout {

struct Stats {
    uint64_t fifo_bytes = 0, draws = 0, vertices = 0, display_lists = 0, indexed_loads = 0;
    uint64_t xfb_copies = 0, texture_copies = 0;
};

// Map the GPU's registers (CP, PE, PI, the write-gather pipe); init_hardware calls it.
void init_gpu();
// What the decoder has seen, as of its last batch.
Stats gpu_stats();

// punchout --bench-record <file.dff> [times]: decode and record a frame
// captured with PUNCHOUT_DFF that many times (default 200) and print how
// long recording it takes (no window, no game run; RAM and registers from the
// log). For measuring the recorder, the CPU's part of drawing. Returns the
// exit code.
int bench_recording(const char* dff_path, unsigned times);


}  // namespace punchout
