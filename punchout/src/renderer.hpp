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
    Renderer(qilin::host::Gpu& gpu, unsigned scale, unsigned anisotropy = 1);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Draw a frame's work (submitted to the GPU on return).
    void render(gx::Frame& frame);
    // The latest external frame buffer copied to `address` (in
    // TRANSFER_SRC_OPTIMAL layout, sampled usage), or null if there's none.
    const qilin::host::Image* xfb(uint32_t address) const;
    // Anisotropic filtering for the game's textures: 1 (the console's), 2, 4,
    // 8 or 16, up to what the GPU allows. Waits for the GPU.
    void set_anisotropy(unsigned level);
    // Whether draws may use pipelines specialised to their shape (on unless
    // PUNCHOUT_UBERSHADER is set); off, everything draws with the ubershader.
    void set_specialization(bool on);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace punchout
