// What the window's thread knows of the player's input, for the guest (wpad.cpp).
#pragma once

#include <atomic>
#include <cstdint>

namespace punchout {

// The pad's buttons (qilin::host::PadButton bits), written by the main thread
// after each poll; and whether the guest wants the pad to rumble.
inline std::atomic<uint32_t> g_pad_buttons{0};
inline std::atomic<bool> g_rumble{false};

}  // namespace punchout
