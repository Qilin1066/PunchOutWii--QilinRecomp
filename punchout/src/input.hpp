// What the window's thread knows of the player's input, for the guest (wpad.cpp).
#pragma once

#include <atomic>
#include <cstdint>

namespace punchout {

// The pad's buttons (qilin::host::PadButton bits), written by the main thread
// after each poll; and whether the guest wants the pad to rumble.
inline std::atomic<uint32_t> g_pad_buttons{0};
inline std::atomic<bool> g_rumble{false};

// Where the player points (the Wii Remote's pointer, from the mouse or the
// right stick): -1 to 1 across the picture, y up, in thousandths; and
// whether it's on the picture at all.
inline std::atomic<int32_t> g_pointer_x{0}, g_pointer_y{0};
inline std::atomic<bool> g_pointer_on{true};

}  // namespace punchout
