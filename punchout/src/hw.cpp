#include "hw.hpp"

#include <algorithm>
#include <vector>

#include "audio.hpp"
#include "gpu.hpp"
#include "os.hpp"

namespace punchout {
namespace rt = qilin::rt;
using rt::Cpu;

namespace {
std::vector<MmioRange> g_ranges;  // sorted by begin; a handful, so a linear search

const MmioRange* find(uint32_t a) {
    for (const MmioRange& r : g_ranges)
        if (a >= r.begin && a < r.end) return &r;
    return nullptr;
}
uint32_t read(Cpu& c, uint32_t a, unsigned size) {
    if (const MmioRange* r = find(a)) return r->read(c, a, size);
    rt::fatal(&c, "load of %u bytes from hardware register %08X: no device there", size, a);
}
void write(Cpu& c, uint32_t a, uint32_t v, unsigned size) {
    if (const MmioRange* r = find(a)) return r->write(c, a, v, size);
    rt::fatal(&c, "store of %u bytes (%08X) to hardware register %08X: no device there", size, v, a);
}
}  // namespace

void map_mmio(const MmioRange& range) {
    for (const MmioRange& r : g_ranges)
        if (range.begin < r.end && r.begin < range.end)
            rt::fatal(nullptr, "hardware ranges %s and %s overlap", r.name, range.name);
    g_ranges.push_back(range);
    std::sort(g_ranges.begin(), g_ranges.end(), [](const MmioRange& x, const MmioRange& y) { return x.begin < y.begin; });
}

void init_hardware() {
    init_gpu();
    init_audio();
    rt::set_mmio(read, write);
    rt::set_idle(poll_loop);
}

}  // namespace punchout
