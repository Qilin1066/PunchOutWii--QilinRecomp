// The window and what's shown in it: SDL3 for the window and its events,
// Vulkan to present. Headless, it draws into an image of its own instead, for
// tests and for machines without a display.
//
// Runtime code: no std::format (macOS 10.15).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "qilin/host/pad.hpp"
#include "qilin/host/vulkan.hpp"

namespace qilin::host {

class Display {
public:
    struct Options {
        const char* title = "Punch-Out!!";
        uint32_t width = 1280, height = 720;  // the window's, or the headless target's, size
        bool headless = false;
    };
    explicit Display(const Options& options);
    ~Display();
    Display(const Display&) = delete;
    Display& operator=(const Display&) = delete;

    // Handle the window's events. False once the player has closed it.
    bool poll();
    // The pad's buttons and the mouse's left button held now (PadButton bits),
    // as of the last poll.
    uint32_t buttons() const;
    // A key by its SDL name ("X", "Space", "Left Shift"): a code for key_down,
    // or -1 if there's no such key. Whether it's held, as of the last poll
    // (never, headless).
    int key_code(const char* name) const;
    bool key_down(int code) const;
    // The right stick, -1 to 1 each way (y up), dead zone removed.
    void right_stick(float& x, float& y) const;
    // If the mouse moved over the window since the last poll: where, over the
    // picture last presented, -1 to 1 each way (y up; beyond 1 off the picture).
    bool mouse_moved(float& x, float& y) const;
    // Rumble on the first pad, on or off.
    void rumble(bool on);
    // Show a frame of RGBA8 pixels, scaled to fit the window at `aspect`
    // (displayed width / height), with black bars around it.
    void present(uint32_t width, uint32_t height, const uint8_t* rgba, float aspect);
    // Show an image drawn on this display's GPU (in TRANSFER_SRC_OPTIMAL layout,
    // its work submitted to the same queue before this call), the same way.
    void present_image(VkImage image, uint32_t width, uint32_t height, float aspect);
    Gpu& gpu();
    // Headless: the last frame presented, as RGBA8 at the target's size.
    std::vector<uint8_t> read_back();
    const std::string& device_name() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace qilin::host
