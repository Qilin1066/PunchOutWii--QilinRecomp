// Vulkan 1.3: the instance, the one device we draw with, and small helpers.
//
// The baseline (docs/roadmap-and-design-targets.md, "Vulkan baseline"): 1.3
// core, and of its optional features only what Mesa's v3dv driver gives a
// Raspberry Pi 4. Anything required is checked when the device is created, and
// a missing piece stops the program with its name: there are no fallbacks.
//
// Runtime code: no std::format (macOS 10.15).
#pragma once

#include <volk.h>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace qilin::host {

[[noreturn]] void die(const char* format, ...);
void check(VkResult r, const char* what);  // dies with the call's name and the error

struct Gpu {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;  // graphics, transfer and (with a surface) present
    uint32_t family = 0;
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceMemoryProperties memory{};
    std::string name;
    // Optional features, enabled where the device has them (the renderer
    // approximates without them, and says so once): GX's logic ops, its line
    // widths and its point sizes.
    bool logic_op = false, wide_lines = false, large_points = false;
    float max_line_width = 1.0f, max_point_size = 1.0f;

    // `instance_extensions`: what the window system needs (none when headless).
    // `surface`: if given, the queue must be able to present to it.
    static Gpu create(std::span<const char* const> instance_extensions, bool want_swapchain,
                      VkSurfaceKHR (*make_surface)(VkInstance, void*) = nullptr, void* surface_arg = nullptr,
                      VkSurfaceKHR* surface_out = nullptr);
    void destroy();

    uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want) const;
};

// A buffer or image with its own memory (the renderer will pool these).
struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceSize size = 0;
    static Buffer host_visible(const Gpu& gpu, VkDeviceSize size, VkBufferUsageFlags usage);
    void destroy(const Gpu& gpu);
};
struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0, levels = 1;
    // Depth formats get a depth view; others a colour view of every level.
    static Image create(const Gpu& gpu, uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage,
                        uint32_t levels = 1);
    void destroy(const Gpu& gpu);
};

// One layout transition with synchronization2, for everything the image needs
// (and an execution and memory dependency on all earlier work on the queue).
void transition(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT);
bool is_depth(VkFormat format);

}  // namespace qilin::host
