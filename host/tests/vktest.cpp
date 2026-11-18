// The host Vulkan layer, headless: a frame presented into a 64x36 target at a
// 2:1 aspect must land letterboxed, with black bars above and below.
// Exit 77 (skipped) where there's no Vulkan 1.3 device.
#include <cstdio>
#include <vector>

#include "qilin/host/display.hpp"
#include "qilin/host/vulkan.hpp"

namespace {
bool vulkan_13_device() {
    if (volkInitialize() != VK_SUCCESS || volkGetInstanceVersion() < VK_API_VERSION_1_3) return false;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance inst;
    if (vkCreateInstance(&ici, nullptr, &inst) != VK_SUCCESS) return false;
    volkLoadInstance(inst);
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst, &n, nullptr);
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(inst, &n, devs.data());
    bool ok = false;
    for (VkPhysicalDevice d : devs) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(d, &p);
        ok |= p.apiVersion >= VK_API_VERSION_1_3;
    }
    vkDestroyInstance(inst, nullptr);
    return ok;
}
}  // namespace

int main() {
    if (!vulkan_13_device()) {
        std::puts("vktest: no Vulkan 1.3 device here: skipped");
        return 77;
    }
    qilin::host::Display::Options o;
    o.headless = true;
    o.width = 64;
    o.height = 36;
    qilin::host::Display d(o);
    std::vector<uint8_t> frame(8 * 4 * 4);
    for (size_t i = 0; i < frame.size(); i += 4) frame[i] = 255, frame[i + 1] = 0, frame[i + 2] = 0, frame[i + 3] = 255;  // red
    d.present(8, 4, frame.data(), 2.0f);  // 64x32 inside 64x36: bars of 2 rows
    const std::vector<uint8_t> px = d.read_back();
    auto at = [&](int x, int y) { return &px[static_cast<size_t>(y * 64 + x) * 4]; };
    int failures = 0;
    auto expect = [&](int x, int y, uint8_t r, const char* what) {
        if (at(x, y)[0] != r || at(x, y)[1] != 0 || at(x, y)[2] != 0) {
            std::printf("vktest: %s at (%d,%d) is %u,%u,%u\n", what, x, y, at(x, y)[0], at(x, y)[1], at(x, y)[2]);
            ++failures;
        }
    };
    expect(32, 0, 0, "the top bar");
    expect(32, 35, 0, "the bottom bar");
    expect(32, 18, 255, "the frame");
    expect(0, 2, 255, "the frame's corner");
    std::printf("vktest: %s on %s\n", failures ? "FAILED" : "passed", d.device_name().c_str());
    return failures ? 1 : 0;
}
