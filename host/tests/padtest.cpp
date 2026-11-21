// A controller reaches the game: a virtual Xbox-layout gamepad (SDL's own
// virtual joystick), connected before the window opens as a player's usually
// is, and one connected later. Each press must show in Display::buttons().
// Needs a window: skips (exit 77) where there's no display or no Vulkan 1.3.
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "qilin/host/display.hpp"

namespace {
int g_failures = 0;
void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

SDL_JoystickID attach_pad(const char* name) {
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.vendor_id = 0x045e;  // Microsoft
    desc.product_id = 0x0b12; // Xbox Series controller
    desc.name = name;
    const auto id = SDL_AttachVirtualJoystick(&desc);
    if (SDL_Joystick* joystick = SDL_OpenJoystick(id)) {
        SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, -32768);
        SDL_SetJoystickVirtualAxis(joystick, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, -32768);
        SDL_CloseJoystick(joystick);
    }
    return id;
}

// Poll until the buttons are `want` (events take a pass or two to arrive).
bool reaches(qilin::host::Display& d, uint32_t want) {
    for (int i = 0; i < 50; ++i) {
        d.poll();
        if ((d.buttons() & ~uint32_t(qilin::host::kPadClick)) == want) return true;
        SDL_Delay(5);
    }
    return false;
}
}  // namespace

int main() {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        std::printf("padtest: no display here (%s): skipped\n", SDL_GetError());
        return std::getenv("QILIN_REQUIRE_GRAPHICS") ? 1 : 77;
    }
    // The window is Vulkan: skip where there's no Vulkan loader (a macOS
    // runner without MoltenVK), as vktest does, rather than fail to open it.
    if (!SDL_Vulkan_LoadLibrary(nullptr)) {
        std::printf("padtest: no Vulkan here (%s): skipped\n", SDL_GetError());
        return std::getenv("QILIN_REQUIRE_GRAPHICS") ? 1 : 77;
    }
    SDL_Vulkan_UnloadLibrary();
    const SDL_JoystickID first = attach_pad("Virtual Xbox controller");
    if (!first) {
        std::printf("padtest: SDL has no virtual joysticks here: skipped\n");
        return std::getenv("QILIN_REQUIRE_GRAPHICS") ? 1 : 77;
    }
    SDL_Joystick* j = SDL_OpenJoystick(first);
    qilin::host::Display::Options o;
    o.width = 320, o.height = 180;
    qilin::host::Display d(o);  // SDL_Init again: counted, harmless

    namespace h = qilin::host;
    check(reaches(d, 0), "connected before the window: nothing held");
    SDL_SetJoystickVirtualButton(j, SDL_GAMEPAD_BUTTON_SOUTH, true);
    check(reaches(d, h::kPadA), "A (south) reaches the game: the pad was opened at start-up");
    SDL_SetJoystickVirtualButton(j, SDL_GAMEPAD_BUTTON_SOUTH, false);
    SDL_SetJoystickVirtualButton(j, SDL_GAMEPAD_BUTTON_EAST, true);
    SDL_SetJoystickVirtualButton(j, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, true);
    check(reaches(d, h::kPadB | h::kPadRB), "B and RB together");
    SDL_SetJoystickVirtualButton(j, SDL_GAMEPAD_BUTTON_EAST, false);
    SDL_SetJoystickVirtualButton(j, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, false);
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_LEFTX, -32767);
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 32767);
    check(reaches(d, h::kPadLeft | h::kPadRT), "the left stick as the D-pad, the trigger as a button");
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_LEFTX, 0);
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, -32768);
    check(reaches(d, 0), "let go");

    // Join a second controller while the first remains attached.
    const SDL_JoystickID second = attach_pad("Second virtual controller");
    SDL_Joystick* j2 = SDL_OpenJoystick(second);
    SDL_SetJoystickVirtualButton(j2, SDL_GAMEPAD_BUTTON_EAST, true);
    check(reaches(d, h::kPadB), "second controller joins");
    check(d.pad_buttons(0) == 0 && d.pad_buttons(1) == h::kPadB, "independent player inputs");
    const auto info = d.controllers();
    const auto key_for = [&](SDL_JoystickID id) { for (const auto& pad : info) if (pad.id == id) return pad.key; return std::string{}; };
    const auto key1 = key_for(first), key2 = key_for(second);
    check(!key1.empty() && !key2.empty() && key1 != key2, "distinct device identities");
    d.set_player_controllers(key2, key1);
    check(d.pad_buttons(0) == h::kPadB && d.pad_buttons(1) == 0, "explicit assignments swap players");
    d.set_player_controllers("", "");
    SDL_CloseJoystick(j); SDL_DetachVirtualJoystick(first);
    check(reaches(d, h::kPadB) && !d.pad_connected(0) && d.pad_buttons(1) == h::kPadB,
          "unplugging player one does not promote player two");
    const SDL_JoystickID returned = attach_pad("Virtual Xbox controller");
    j = SDL_OpenJoystick(returned);
    SDL_SetJoystickVirtualButton(j, SDL_GAMEPAD_BUTTON_SOUTH, true);
    check(reaches(d, h::kPadA | h::kPadB) && d.pad_buttons(0) == h::kPadA && d.pad_buttons(1) == h::kPadB,
          "reconnected controller reclaims its original slot");
    SDL_SetJoystickVirtualButton(j, SDL_GAMEPAD_BUTTON_SOUTH, false);
    SDL_SetJoystickVirtualButton(j2, SDL_GAMEPAD_BUTTON_EAST, false);
    d.set_input_thresholds(25, 25, 10);
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_LEFTX, 10000);
    check(reaches(d, h::kPadRight), "custom press threshold");
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_LEFTX, 7500);
    check(reaches(d, h::kPadRight), "hysteresis holds through jitter");
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_LEFTX, 0);
    check(reaches(d, 0), "hysteresis releases at centre");
    SDL_CloseJoystick(j); SDL_DetachVirtualJoystick(returned);
    SDL_CloseJoystick(j2); SDL_DetachVirtualJoystick(second);

    std::printf("padtest: %s\n", g_failures ? "FAILED" : "passed");
    return g_failures ? 1 : 0;
}
