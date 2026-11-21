// What the window's thread knows of the player's input, for the guest (wpad.cpp).
#pragma once

#include <atomic>
#include <cstdint>

namespace punchout {

// The Wii Remote's buttons held (a bit per punchout::Control, settings.hpp),
// written by the main thread after each poll from the player's bindings; and
// whether the guest wants the pad to rumble.
inline std::atomic<uint32_t> g_controls{0};
inline std::atomic<bool> g_rumble{false};
// A second player, when the settings give one a controller (settings.hpp,
// second_player) and that controller is there: a second Wii Remote, with
// buttons, rumble and a pointer of its own (its controller's right stick).
inline std::atomic<bool> g_player2{false};
inline std::atomic<uint32_t> g_controls2{0};
inline std::atomic<bool> g_rumble2{false};
inline std::atomic<int32_t> g_pointer2_x{0}, g_pointer2_y{0};

// Where the player points (the Wii Remote's pointer, from the mouse or the
// right stick): -1 to 1 across the picture, y up, in thousandths; and
// whether it's on the picture at all.
inline std::atomic<int32_t> g_pointer_x{0}, g_pointer_y{0};
inline std::atomic<bool> g_pointer_on{true};

}  // namespace punchout
