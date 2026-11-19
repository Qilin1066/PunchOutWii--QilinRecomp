// The renderer (renderer.cpp): the guest's frames (gx.hpp) drawn with Vulkan
// on the main thread.
#pragma once

#include <cstdint>
#include <memory>

#include "gx.hpp"
#include "qilin/host/vulkan.hpp"

namespace punchout {

class Renderer {
public:
    // `scale`: the EFB's resolution as a multiple of the console's 640x528.
    Renderer(qilin::host::Gpu& gpu, unsigned scale);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Draw a frame's work (submitted to the GPU on return).
    void render(gx::Frame& frame);
    // The latest external frame buffer copied to `address` (in
    // TRANSFER_SRC_OPTIMAL layout), if there is one.
    bool xfb(uint32_t address, VkImage& image, uint32_t& width, uint32_t& height) const;
    // Whether draws may use pipelines specialised to their shape (on unless
    // PUNCHOUT_UBERSHADER is set); off, everything draws with the ubershader.
    void set_specialization(bool on);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace punchout
