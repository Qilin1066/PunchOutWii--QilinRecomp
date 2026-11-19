// The guest's side of drawing (recorder.cpp): gpu.cpp's decoder calls these
// as commands arrive, and frames go to the renderer.
#pragma once

#include <cstdint>

namespace punchout::gx {

// The command processor's, XF's and BP's registers as gpu.cpp keeps them.
struct Registers {
    const uint32_t* cp;  // 256
    const uint32_t* xf;  // 0x1058
    const uint32_t* bp;  // 256
};

// A primitive: opcode 0x80-0xBF (type in bits 3-5, vertex format in 0-2),
// `count` vertices of raw data at `data`.
void record_primitive(const Registers& r, uint8_t opcode, const uint8_t* data, uint32_t count);
// XF, BP or CP state changed: the next draw snapshots it again.
void state_changed();
// A BP register was written (the TEV colour registers latch by which half is written).
void bp_written(uint32_t reg, uint32_t value);
// BP 0x52: an EFB copy (to a texture, or to the external frame buffer, which
// ends the frame).
void record_copy(const Registers& r, uint32_t command);
// BP 0x65: a palette load.
void record_tlut_load(const Registers& r, uint32_t command);

struct RecorderStats {
    uint64_t frames = 0, textures_decoded = 0, texture_bytes = 0;
};
const RecorderStats& recorder_stats();

}  // namespace punchout::gx
