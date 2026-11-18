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
    // Show a frame of RGBA8 pixels, scaled to fit the window at `aspect`
    // (displayed width / height), with black bars around it.
    void present(uint32_t width, uint32_t height, const uint8_t* rgba, float aspect);
    // Headless: the last frame presented, as RGBA8 at the target's size.
    std::vector<uint8_t> read_back();
    const std::string& device_name() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace qilin::host
