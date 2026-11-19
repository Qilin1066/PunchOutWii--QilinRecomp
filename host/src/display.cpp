// The window and presentation (display.hpp).
#include "qilin/host/display.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "qilin/host/vulkan.hpp"

namespace qilin::host {

namespace {
constexpr VkFormat kFrameFormat = VK_FORMAT_R8G8B8A8_UNORM;

VkSurfaceKHR make_sdl_surface(VkInstance instance, void* window) {
    VkSurfaceKHR s = VK_NULL_HANDLE;
    if (!SDL_Vulkan_CreateSurface(static_cast<SDL_Window*>(window), instance, nullptr, &s))
        die("cannot create a Vulkan surface for the window: %s", SDL_GetError());
    return s;
}

void need_format(const Gpu& gpu, VkFormat f, VkFormatFeatureFlags want, const char* what) {
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(gpu.physical, f, &p);
    if ((p.optimalTilingFeatures & want) != want) die("%s: the GPU can't use format %d for that", what, static_cast<int>(f));
}
}  // namespace

struct Display::Impl {
    Options options;
    SDL_Window* window = nullptr;
    Gpu gpu;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swap_format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    std::vector<VkImage> swap_images;
    std::vector<VkSemaphore> rendered;  // one per swapchain image
    VkSemaphore acquired = VK_NULL_HANDLE;
    VkFence done = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    Buffer staging;
    Image frame;      // the uploaded frame
    Image target;     // headless: what's presented
    bool need_rebuild = false;
    uint32_t buttons = 0;
    SDL_Gamepad* pad = nullptr;
    bool rumbling = false;
    float stick_x = 0, stick_y = 0;
    bool moved = false;
    float mouse_x = 0, mouse_y = 0;  // window pixels
    float pic_x = 0, pic_y = 0, pic_w = 1, pic_h = 1;  // the picture's place, last present

    void show(VkImage src, uint32_t sw, uint32_t sh, float aspect);

    void build_swapchain() {
        vkDeviceWaitIdle(gpu.device);
        VkSurfaceCapabilitiesKHR caps;
        check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu.physical, surface, &caps), "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
        if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) die("the window's surface can't be drawn to by copies");
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window, &w, &h);
        extent = caps.currentExtent.width != 0xFFFFFFFFu ? caps.currentExtent
                                                          : VkExtent2D{static_cast<uint32_t>(w), static_cast<uint32_t>(h)};
        if (extent.width == 0 || extent.height == 0) return;  // minimised
        uint32_t n = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(gpu.physical, surface, &n, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(n);
        vkGetPhysicalDeviceSurfaceFormatsKHR(gpu.physical, surface, &n, formats.data());
        VkSurfaceFormatKHR chosen = formats.at(0);
        for (const auto& f : formats)
            if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) &&
                f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
                chosen = f;
        swap_format = chosen.format;
        need_format(gpu, swap_format, VK_FORMAT_FEATURE_BLIT_DST_BIT, "the window's pixel format");
        VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        sci.surface = surface;
        sci.minImageCount = std::min(caps.minImageCount + 1, caps.maxImageCount ? caps.maxImageCount : 8u);
        sci.imageFormat = chosen.format;
        sci.imageColorSpace = chosen.colorSpace;
        sci.imageExtent = extent;
        sci.imageArrayLayers = 1;
        sci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        sci.preTransform = caps.currentTransform;
        sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;  // always there; pacing comes later
        sci.clipped = VK_TRUE;
        sci.oldSwapchain = swapchain;
        VkSwapchainKHR next;
        check(vkCreateSwapchainKHR(gpu.device, &sci, nullptr, &next), "vkCreateSwapchainKHR");
        if (swapchain) vkDestroySwapchainKHR(gpu.device, swapchain, nullptr);
        swapchain = next;
        vkGetSwapchainImagesKHR(gpu.device, swapchain, &n, nullptr);
        swap_images.resize(n);
        vkGetSwapchainImagesKHR(gpu.device, swapchain, &n, swap_images.data());
        for (VkSemaphore s : rendered) vkDestroySemaphore(gpu.device, s, nullptr);
        rendered.resize(n);
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        for (auto& s : rendered) check(vkCreateSemaphore(gpu.device, &si, nullptr, &s), "vkCreateSemaphore");
        need_rebuild = false;
    }

    // Record: upload the frame (if it's ours), clear the destination, blit the frame into it letterboxed.
    void record(VkImage dst, VkExtent2D dst_size, VkImageLayout final_layout, float aspect, VkImage src, uint32_t sw, uint32_t sh) {
        if (src == VK_NULL_HANDLE) {
            transition(cmd, frame.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {frame.width, frame.height, 1};
            vkCmdCopyBufferToImage(cmd, staging.buffer, frame.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            transition(cmd, frame.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            src = frame.image;
            sw = frame.width;
            sh = frame.height;
        } else {  // drawn by the caller: wait for it
            transition(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        }
        transition(cmd, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        const VkClearColorValue black{{0.0f, 0.0f, 0.0f, 1.0f}};
        const VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
    // The largest rectangle of the frame's aspect that fits, centred.
        const float dw = static_cast<float>(dst_size.width), dh = static_cast<float>(dst_size.height);
        float w = dw, h = dw / aspect;
        if (h > dh) {
            h = dh;
            w = dh * aspect;
        }
        const int32_t x0 = static_cast<int32_t>((dw - w) / 2), y0 = static_cast<int32_t>((dh - h) / 2);
        pic_x = float(x0), pic_y = float(y0), pic_w = w, pic_h = h;
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {static_cast<int32_t>(sw), static_cast<int32_t>(sh), 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstOffsets[0] = {x0, y0, 0};
        blit.dstOffsets[1] = {x0 + static_cast<int32_t>(w), y0 + static_cast<int32_t>(h), 1};
        vkCmdBlitImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                       VK_FILTER_LINEAR);
        transition(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, final_layout);
    }
};

Display::Display(const Options& options) : impl_(std::make_unique<Impl>()) {
    Impl& d = *impl_;
    d.options = options;
    if (options.headless) {
        d.gpu = Gpu::create({}, false);
        d.target = Image::create(d.gpu, options.width, options.height, kFrameFormat,
                                 VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    } else {
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) die("SDL can't start: %s", SDL_GetError());
        d.window = SDL_CreateWindow(options.title, static_cast<int>(options.width), static_cast<int>(options.height),
                                    SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
        if (!d.window) die("can't open a window: %s", SDL_GetError());
        Uint32 n = 0;
        const char* const* ext = SDL_Vulkan_GetInstanceExtensions(&n);
        if (!ext) die("SDL has no Vulkan surface support here: %s", SDL_GetError());
        d.gpu = Gpu::create(std::span(ext, n), true, make_sdl_surface, d.window, &d.surface);
        d.build_swapchain();
    }
    need_format(d.gpu, kFrameFormat, VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT,
                "frames");
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = d.gpu.family;
    check(vkCreateCommandPool(d.gpu.device, &pci, nullptr, &d.pool), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = d.pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    check(vkAllocateCommandBuffers(d.gpu.device, &cai, &d.cmd), "vkAllocateCommandBuffers");
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    check(vkCreateSemaphore(d.gpu.device, &si, nullptr, &d.acquired), "vkCreateSemaphore");
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    check(vkCreateFence(d.gpu.device, &fi, nullptr, &d.done), "vkCreateFence");
}

Display::~Display() {
    Impl& d = *impl_;
    if (d.gpu.device) {
        vkDeviceWaitIdle(d.gpu.device);
        d.staging.destroy(d.gpu);
        d.frame.destroy(d.gpu);
        d.target.destroy(d.gpu);
        for (VkSemaphore s : d.rendered) vkDestroySemaphore(d.gpu.device, s, nullptr);
        vkDestroySemaphore(d.gpu.device, d.acquired, nullptr);
        vkDestroyFence(d.gpu.device, d.done, nullptr);
        vkDestroyCommandPool(d.gpu.device, d.pool, nullptr);
        if (d.swapchain) vkDestroySwapchainKHR(d.gpu.device, d.swapchain, nullptr);
    }
    if (d.surface) vkDestroySurfaceKHR(d.gpu.instance, d.surface, nullptr);
    d.gpu.destroy();
    if (d.window) {
        SDL_DestroyWindow(d.window);
        SDL_Quit();
    }
}

bool Display::poll() {
    Impl& d = *impl_;
    if (!d.window) return true;
    SDL_Event e;
    d.moved = false;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) return false;
        if (e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) d.need_rebuild = true;
        if (e.type == SDL_EVENT_MOUSE_MOTION) {
            const float density = SDL_GetWindowPixelDensity(d.window);
            d.mouse_x = e.motion.x * density;
            d.mouse_y = e.motion.y * density;
            d.moved = true;
        }
        if (e.type == SDL_EVENT_GAMEPAD_ADDED && !d.pad) d.pad = SDL_OpenGamepad(e.gdevice.which);
        if (e.type == SDL_EVENT_GAMEPAD_REMOVED && d.pad && SDL_GetGamepadID(d.pad) == e.gdevice.which) {
            SDL_CloseGamepad(d.pad);
            d.pad = nullptr;
        }
    }
    uint32_t b = 0;
    if (SDL_Gamepad* p = d.pad) {
        struct Map {
            SDL_GamepadButton sdl;
            uint32_t bit;
        };
        static constexpr Map kMap[] = {
            {SDL_GAMEPAD_BUTTON_DPAD_UP, kPadUp},     {SDL_GAMEPAD_BUTTON_DPAD_DOWN, kPadDown},
            {SDL_GAMEPAD_BUTTON_DPAD_LEFT, kPadLeft}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, kPadRight},
            {SDL_GAMEPAD_BUTTON_SOUTH, kPadA},        {SDL_GAMEPAD_BUTTON_EAST, kPadB},
            {SDL_GAMEPAD_BUTTON_WEST, kPadX},         {SDL_GAMEPAD_BUTTON_NORTH, kPadY},
            {SDL_GAMEPAD_BUTTON_START, kPadStart},    {SDL_GAMEPAD_BUTTON_BACK, kPadBack},
            {SDL_GAMEPAD_BUTTON_GUIDE, kPadGuide},
        };
        for (const Map& m : kMap)
            if (SDL_GetGamepadButton(p, m.sdl)) b |= m.bit;
        const int x = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTX), y = SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_LEFTY);
        if (x < -16384) b |= kPadLeft;
        if (x > 16384) b |= kPadRight;
        if (y < -16384) b |= kPadUp;
        if (y > 16384) b |= kPadDown;
        const auto axis = [](int v) {  // a dead zone of a quarter, then linear to 1
            const float f = std::clamp(v / 32767.0f, -1.0f, 1.0f);
            return std::fabs(f) < 0.25f ? 0.0f : (f - std::copysign(0.25f, f)) / 0.75f;
        };
        d.stick_x = axis(SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHTX));
        d.stick_y = -axis(SDL_GetGamepadAxis(p, SDL_GAMEPAD_AXIS_RIGHTY));
    } else {
        d.stick_x = d.stick_y = 0;
    }
    if (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_LMASK) b |= kPadClick;
    const bool* k = SDL_GetKeyboardState(nullptr);
    struct Key {
        SDL_Scancode sc;
        uint32_t bit;
    };
    static constexpr Key kKeys[] = {
        {SDL_SCANCODE_UP, kPadUp},        {SDL_SCANCODE_DOWN, kPadDown},     {SDL_SCANCODE_LEFT, kPadLeft},
        {SDL_SCANCODE_RIGHT, kPadRight},  {SDL_SCANCODE_X, kPadA},           {SDL_SCANCODE_Z, kPadB},
        {SDL_SCANCODE_C, kPadY},          {SDL_SCANCODE_SPACE, kPadY},       {SDL_SCANCODE_RETURN, kPadStart},
        {SDL_SCANCODE_BACKSPACE, kPadBack}, {SDL_SCANCODE_ESCAPE, kPadGuide},
    };
    for (const Key& key : kKeys)
        if (k[key.sc]) b |= key.bit;
    d.buttons = b;
    return true;
}

uint32_t Display::buttons() const { return impl_->buttons; }
void Display::right_stick(float& x, float& y) const {
    x = impl_->stick_x;
    y = impl_->stick_y;
}
bool Display::mouse_moved(float& x, float& y) const {
    const Impl& d = *impl_;
    if (!d.moved) return false;
    x = (d.mouse_x - d.pic_x) / d.pic_w * 2.0f - 1.0f;
    y = 1.0f - (d.mouse_y - d.pic_y) / d.pic_h * 2.0f;
    return true;
}

void Display::rumble(bool on) {
    Impl& d = *impl_;
    if (!d.pad || on == d.rumbling) return;
    d.rumbling = on;
    SDL_RumbleGamepad(d.pad, on ? 0xFFFF : 0, on ? 0xFFFF : 0, on ? 60000 : 0);
}

void Display::Impl::show(VkImage src, uint32_t sw, uint32_t sh, float aspect) {
    Impl& d = *this;
    VkImage dst;
    VkExtent2D dst_size;
    uint32_t index = 0;
    if (d.window) {
        if (d.need_rebuild) d.build_swapchain();
        if (d.extent.width == 0) return;  // minimised
        const VkResult r = vkAcquireNextImageKHR(d.gpu.device, d.swapchain, UINT64_MAX, d.acquired, VK_NULL_HANDLE, &index);
        if (r == VK_ERROR_OUT_OF_DATE_KHR) {
            d.need_rebuild = true;
            return;
        }
        if (r != VK_SUBOPTIMAL_KHR) check(r, "vkAcquireNextImageKHR");
        dst = d.swap_images[index];
        dst_size = d.extent;
    } else {
        dst = d.target.image;
        dst_size = {d.target.width, d.target.height};
    }
    check(vkResetFences(d.gpu.device, 1, &d.done), "vkResetFences");
    check(vkResetCommandBuffer(d.cmd, 0), "vkResetCommandBuffer");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(d.cmd, &bi), "vkBeginCommandBuffer");
    d.record(dst, dst_size, d.window ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, aspect, src, sw, sh);
    check(vkEndCommandBuffer(d.cmd), "vkEndCommandBuffer");

    VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbi.commandBuffer = d.cmd;
    VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO}, signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    wait.semaphore = d.acquired;
    wait.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbi;
    if (d.window) {
        signal.semaphore = d.rendered[index];
        signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        si.waitSemaphoreInfoCount = 1;
        si.pWaitSemaphoreInfos = &wait;
        si.signalSemaphoreInfoCount = 1;
        si.pSignalSemaphoreInfos = &signal;
    }
    check(vkQueueSubmit2(d.gpu.queue, 1, &si, d.done), "vkQueueSubmit2");
    if (d.window) {
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &d.rendered[index];
        pi.swapchainCount = 1;
        pi.pSwapchains = &d.swapchain;
        pi.pImageIndices = &index;
        const VkResult r = vkQueuePresentKHR(d.gpu.queue, &pi);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) d.need_rebuild = true;
        else check(r, "vkQueuePresentKHR");
    }
}


void Display::present(uint32_t width, uint32_t height, const uint8_t* rgba, float aspect) {
    Impl& d = *impl_;
    check(vkWaitForFences(d.gpu.device, 1, &d.done, VK_TRUE, UINT64_MAX), "vkWaitForFences");
    const VkDeviceSize bytes = VkDeviceSize{width} * height * 4;
    if (d.staging.size < bytes) {
        d.staging.destroy(d.gpu);
        d.staging = Buffer::host_visible(d.gpu, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    }
    if (d.frame.width != width || d.frame.height != height) {
        d.frame.destroy(d.gpu);
        d.frame = Image::create(d.gpu, width, height, kFrameFormat, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    }
    std::memcpy(d.staging.mapped, rgba, bytes);

    impl_->show(VK_NULL_HANDLE, 0, 0, aspect);
}

void Display::present_image(VkImage image, uint32_t width, uint32_t height, float aspect) {
    Impl& d = *impl_;
    check(vkWaitForFences(d.gpu.device, 1, &d.done, VK_TRUE, UINT64_MAX), "vkWaitForFences");
    d.show(image, width, height, aspect);
}

Gpu& Display::gpu() { return impl_->gpu; }

std::vector<uint8_t> Display::read_back() {
    Impl& d = *impl_;
    if (d.window) die("read_back is for headless displays");
    check(vkWaitForFences(d.gpu.device, 1, &d.done, VK_TRUE, UINT64_MAX), "vkWaitForFences");
    Buffer out = Buffer::host_visible(d.gpu, VkDeviceSize{d.target.width} * d.target.height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    check(vkResetFences(d.gpu.device, 1, &d.done), "vkResetFences");
    check(vkResetCommandBuffer(d.cmd, 0), "vkResetCommandBuffer");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    check(vkBeginCommandBuffer(d.cmd, &bi), "vkBeginCommandBuffer");
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {d.target.width, d.target.height, 1};
    vkCmdCopyImageToBuffer(d.cmd, d.target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, out.buffer, 1, &region);
    check(vkEndCommandBuffer(d.cmd), "vkEndCommandBuffer");
    VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbi.commandBuffer = d.cmd;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbi;
    check(vkQueueSubmit2(d.gpu.queue, 1, &si, d.done), "vkQueueSubmit2");
    check(vkWaitForFences(d.gpu.device, 1, &d.done, VK_TRUE, UINT64_MAX), "vkWaitForFences");
    const auto* p = static_cast<const uint8_t*>(out.mapped);
    std::vector<uint8_t> pixels(p, p + out.size);
    out.destroy(d.gpu);
    return pixels;
}

const std::string& Display::device_name() const { return impl_->gpu.name; }

}  // namespace qilin::host
