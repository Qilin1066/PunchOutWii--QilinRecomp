// Frames from the guest to the window (video.cpp). VI hands one over at each
// retrace; the main thread shows the newest.
#pragma once

#include <cstdint>
#include <vector>

namespace punchout {

// From the guest's side (vi.cpp): the frame buffer shown from this retrace on.
// `xfb` is a guest address, `width` and `height` the frame buffer's size; 0
// shows black.
void show_frame(uint32_t xfb, uint32_t width, uint32_t height);

// From the main thread: wait up to `timeout_ms` for a frame newer than
// `*sequence`; if one came, convert it to RGBA8 and return true.
bool take_frame(uint64_t* sequence, unsigned timeout_ms, std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height);

}  // namespace punchout
