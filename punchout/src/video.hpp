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

// From the main thread: if VI has shown a frame newer than `*sequence` (wait
// up to `timeout_ms`), the frame buffer's guest address and size.
bool take_xfb(uint64_t* sequence, unsigned timeout_ms, uint32_t& xfb, uint32_t& width, uint32_t& height);

// From the main thread: wait up to `timeout_ms` for a frame newer than
// `*sequence`; if one came, convert it to RGBA8 and return true.
bool take_frame(uint64_t* sequence, unsigned timeout_ms, std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height);

// From any thread: the frame buffers the game has handed VI
// (VISetNextFrameBuffer), one per frame it draws. Scripted input (main.cpp) is
// timed by it, so it lands at the same point of the game however fast the
// host draws: the game waits for the GPU, so on a slow one its frames, not
// the clock, measure its progress.
uint32_t frame_count();

// From the main thread: the retrace rate, num/den Hz (60/1 until set). Set to
// the display's own refresh when that is within half a percent of 60 Hz (or
// of a multiple of it), the game's frames then land one per refresh (or every
// second one...) instead of drifting against it, which repeats or drops a
// frame every few seconds. 0 for either: back to 60 Hz.
void set_retrace_rate(uint32_t num, uint32_t den);

}  // namespace punchout
