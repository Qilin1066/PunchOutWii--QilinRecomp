// Frames from the guest to the window (video.hpp).
//
// For now a frame is the external frame buffer in guest memory, YUV 4:2:2
// (Y0 U Y1 V per pair of pixels, BT.601 studio range), converted on the CPU.
// Once the renderer draws EFB copies on the GPU, the copy that names this
// address is shown instead (roadmap step 4).
#include "video.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>

#include "qilin/rt/system.hpp"

namespace punchout {
namespace rt = qilin::rt;

namespace {
std::mutex g_lock;
std::condition_variable g_cv;
uint64_t g_sequence = 0;
uint32_t g_xfb = 0, g_width = 0, g_height = 0;

uint8_t clamp(int v) { return static_cast<uint8_t>(std::clamp(v, 0, 255)); }
}  // namespace

void show_frame(uint32_t xfb, uint32_t width, uint32_t height) {
    {
        std::lock_guard l(g_lock);
        g_xfb = xfb;
        g_width = width;
        g_height = height;
        ++g_sequence;
    }
    g_cv.notify_one();
}

bool take_frame(uint64_t* sequence, unsigned timeout_ms, std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height) {
    uint32_t xfb;
    {
        std::unique_lock l(g_lock);
        if (!g_cv.wait_for(l, std::chrono::milliseconds(timeout_ms), [&] { return g_sequence != *sequence; })) return false;
        *sequence = g_sequence;
        xfb = g_xfb;
        width = g_width;
        height = g_height;
    }
    if (xfb == 0 || width == 0 || height == 0 || width % 2) {  // black
        width = width ? width : 640;
        height = height ? height : 480;
        rgba.assign(size_t{width} * height * 4, 0);
        for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
        return true;
    }
    const uint32_t va = (xfb & 0x3FFFFFFFu) | 0x80000000u;  // physical or uncached: the cached view
    rgba.resize(size_t{width} * height * 4);
    const uint8_t* src = rt::guest_ptr(va);
    uint8_t* out = rgba.data();
    for (size_t i = 0; i < size_t{width} * height; i += 2, src += 4, out += 8) {
        const int u = src[1] - 128, v = src[3] - 128;
        for (int k = 0; k < 2; ++k) {
            const int y = 298 * (src[2 * k] - 16);  // 1.164 in 8.8
            out[4 * k] = clamp((y + 409 * v + 128) >> 8);
            out[4 * k + 1] = clamp((y - 100 * u - 208 * v + 128) >> 8);
            out[4 * k + 2] = clamp((y + 516 * u + 128) >> 8);
            out[4 * k + 3] = 255;
        }
    }
    return true;
}

}  // namespace punchout
