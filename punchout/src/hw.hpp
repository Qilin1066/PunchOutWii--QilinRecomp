// The hardware registers (0xCC000000-0xCD00FFFF): each device maps its own
// ranges; an access outside every range is fatal.
#pragma once

#include <cstdint>

#include "qilin/rt/system.hpp"

namespace punchout {

struct MmioRange {
    uint32_t begin, end;  // [begin, end)
    const char* name;
    uint32_t (*read)(qilin::rt::Cpu&, uint32_t address, unsigned size);
    void (*write)(qilin::rt::Cpu&, uint32_t address, uint32_t value, unsigned size);
};

// Map a range (before init_hardware). Overlaps are a mistake and fatal.
void map_mmio(const MmioRange& range);
// Map every device's registers and route the runtime's hardware accesses here.
void init_hardware();

}  // namespace punchout
