// The GPU's front end (gpu.cpp).
#pragma once

#include <cstdint>

namespace punchout {

struct Stats {
    uint64_t fifo_bytes = 0, draws = 0, vertices = 0, display_lists = 0, indexed_loads = 0;
    uint64_t xfb_copies = 0, texture_copies = 0;
};

// Route the hardware registers (CP, PE, PI, the write-gather pipe) here.
void init_gpu();
const Stats& gpu_stats();

}  // namespace punchout
