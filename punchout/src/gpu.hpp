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

}  // namespace punchout
