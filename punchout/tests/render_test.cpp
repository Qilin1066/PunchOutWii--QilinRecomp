// The renderer end to end, without the game: GX register writes as a game
// makes them go through the real recorder (punchout/src/recorder.cpp) and
// renderer (renderer.cpp) on a Vulkan device, and the frame copied to the
// external frame buffer is read back and checked pixel by pixel.
//
// Covers what the game's boot screens don't reach: logic ops (with the GPU's
// own and with the blending approximation), line widths and point sizes (and
// their one-pixel fallbacks), Z textures (late and early Z), EFB copies of
// depth, and the RG8/GB8 copy formats. Exit 77 (skipped) without a Vulkan 1.3
// device.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "gx.hpp"
#include "recorder.hpp"
#include "renderer.hpp"
#include "qilin/host/vulkan.hpp"
#include "qilin/rt/system.hpp"

namespace rt = qilin::rt;
namespace host = qilin::host;
namespace gx = punchout::gx;

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++g_failures;
}

uint32_t f2u(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

constexpr uint32_t kXfb = 0x00500000u, kTexI8 = 0x00300000u, kCopyTex = 0x00400000u, kScratch = 0x00600000u;

// GX's registers, as gpu.cpp keeps them, and the writes it makes.
struct Gx {
    uint32_t cp[256] = {}, xf[0x1058] = {}, bp[256] = {};
    // Where clears copy to. The recorder's copies are global, so a second
    // renderer in the same run uses its own address (a texture one renderer
    // made, the other never did).
    uint32_t scratch = kScratch;
    // What GXInit leaves: the copy filter's default weights, 21/22/21 on the
    // line itself (64: unchanged), none on the lines above and below.
    Gx() { bp[0x53] = 0x595000, bp[0x54] = 0x15; }
    gx::Registers regs() const { return {cp, xf, bp}; }
    void BP(uint32_t reg, uint32_t v) {
        bp[reg] = v;
        gx::bp_written(reg, v, true);
    }
    void XF(uint32_t a, uint32_t v) {
        xf[a] = v;
        gx::state_changed();
    }
    void XFf(uint32_t a, float v) { XF(a, f2u(v)); }
    void CP(uint32_t reg, uint32_t v) {
        cp[reg] = v;
        gx::state_changed();
    }

    // What a 2D game sets up: a 640x528 viewport and scissor, an orthographic
    // projection with z in [-1, 0], one colour channel from the vertices, one
    // TEV stage passing the rasterised colour, depth test LEQUAL with updates.
    void setup() {
        BP(0x00, (1u << 4));                                    // genmode: 1 colour channel, 1 TEV stage, no culling
        BP(0x20, 342u | (342u << 12));                           // scissor top-left (+342)
        BP(0x21, (342u + 527u) | ((342u + 639u) << 12));         // scissor bottom-right
        BP(0x59, (342u >> 1) | ((342u >> 1) << 10));             // scissor offset
        XFf(0x101A, 320.0f), XFf(0x101B, -264.0f), XFf(0x101C, 16777215.0f);  // viewport: half sizes, z range
        XFf(0x101D, 342.0f + 320.0f), XFf(0x101E, 342.0f + 264.0f), XFf(0x101F, 16777215.0f);  // origin, far
        // Ortho over (0,0)-(640,528), near 0 and far 1 (GX's C_MTXOrtho).
        XFf(0x1020, 2.0f / 640.0f), XFf(0x1021, -1.0f), XFf(0x1022, 2.0f / -528.0f), XFf(0x1023, 1.0f);
        XFf(0x1024, -1.0f), XFf(0x1025, -1.0f), XF(0x1026, 1);
        for (uint32_t row = 0; row < 3; ++row) XFf(row * 4 + row, 1.0f);  // position matrix 0: identity
        for (uint32_t row = 0; row < 3; ++row) XFf((30 + row) * 4 + row, 1.0f);  // texture matrix 30: identity
        CP(0x30, 30u << 6);                                      // matrix indices: position 0, texture 0 -> 30
        XF(0x1009, 1), XF(0x100E, 1), XF(0x1010, 1);             // 1 colour channel, material from the vertex
        XF(0x103F, 0);                                           // no texgens
        BP(0x28, 0);                                             // stage 0: no texture, colour channel 0
        color_from(10, 5);                                       // rasterised colour and alpha
        for (uint32_t s = 0; s < 4; ++s) BP(0xF6 + 2 * s, 0 | (1u << 2)), BP(0xF7 + 2 * s, 2 | (3u << 2));  // identity swaps
        BP(0xF3, (7u << 16) | (7u << 19));                       // alpha test: always
        BP(0x40, 1u | (3u << 1) | (1u << 4));                    // Z: on, LEQUAL, updates
        BP(0x41, 1u << 3);                                       // colour updates, no blending
        BP(0x43, 0);                                             // RGB8_Z24, Z after texturing
        // Vertex format 0: position xyz f32, colour RGBA8, both direct.
        CP(0x50, (1u << 9) | (1u << 13));
        CP(0x60, 0);
        CP(0x70, 1u | (4u << 1) | (5u << 14));
        clear();
    }
    // TEV stage 0's colour and alpha outputs: input d (colour 10 rasc, 8 texc; alpha 5 rasa, 4 texa).
    void color_from(uint32_t c, uint32_t a) {
        BP(0xC0, c | (15u << 4) | (15u << 8) | (15u << 12) | (1u << 19));
        BP(0xC1, (a << 4) | (7u << 7) | (7u << 10) | (7u << 13) | (1u << 19));
    }
    // Texture map 0 sampled by stage 0 from texgen 0 (texture coordinate 0).
    void texture(uint32_t address, uint32_t w, uint32_t h, uint32_t format) {
        BP(0x80, 0), BP(0x84, 0);                                // clamp, nearest, no mips
        BP(0x88, (w - 1) | ((h - 1) << 10) | (format << 20));
        BP(0x94, address >> 5);
        BP(0x30, w - 1), BP(0x31, h - 1);                        // texture coordinate scale
        XF(0x103F, 1);
        XF(0x1040, 5u << 7);                                     // texgen 0: from texture coordinate 0, ST
        BP(0x28, 1u << 6);                                       // stage 0: texture map 0, coordinate 0, colour 0
        CP(0x50, (1u << 9) | (1u << 13));
        CP(0x60, 1u);                                            // texture coordinate 0 direct
        CP(0x70, 1u | (4u << 1) | (5u << 14) | (1u << 21) | (4u << 22));  // + tc0: s,t f32
    }
    void no_texture() {
        XF(0x103F, 0);
        BP(0x28, 0);
        CP(0x60, 0);
        CP(0x70, 1u | (4u << 1) | (5u << 14));
    }
    // A clear of the whole EFB (black, far), by a copy as games make them.
    void clear() {
        BP(0x4F, 0), BP(0x50, 0), BP(0x51, 0xFFFFFF);
        BP(0x49, 0), BP(0x4A, 639u | (527u << 10));
        BP(0x4B, scratch >> 5);
        const uint32_t cmd = (1u << 11) | (3u << 3);             // clear, RGBA8 texture copy
        BP(0x52, cmd);
        gx::record_copy(regs(), cmd);
    }
};

struct V {
    float x, y, z;
    uint8_t r, g, b, a;
    float s = 0, t = 0;
};
void put_be32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 3; i >= 0; --i) out.push_back(uint8_t(v >> (8 * i)));
}
void draw(Gx& g, uint8_t opcode, std::initializer_list<V> vs, bool tex = false) {
    std::vector<uint8_t> data;
    for (const V& v : vs) {
        put_be32(data, f2u(v.x)), put_be32(data, f2u(v.y)), put_be32(data, f2u(v.z));
        data.insert(data.end(), {v.r, v.g, v.b, v.a});
        if (tex) put_be32(data, f2u(v.s)), put_be32(data, f2u(v.t));
    }
    gx::record_primitive(g.regs(), opcode, data.data(), uint32_t(vs.size()));
}
void quad(Gx& g, float x0, float y0, float x1, float y1, float z, uint8_t r, uint8_t gr, uint8_t b, bool tex = false) {
    draw(g, 0x80, {{x0, y0, z, r, gr, b, 255, 0, 0}, {x1, y0, z, r, gr, b, 255, 1, 0}, {x1, y1, z, r, gr, b, 255, 1, 1},
                   {x0, y1, z, r, gr, b, 255, 0, 1}},
         tex);
}

// The Vulkan side: render each frame the recorder hands over, read the XFB back.
struct Target {
    host::Gpu& gpu;
    punchout::Renderer renderer;
    Target(host::Gpu& g) : gpu(g), renderer(g, 1) {}

    std::vector<uint8_t> finish(Gx& g, bool clear_after = true) {
        g.BP(0x49, 0), g.BP(0x4A, 639u | (527u << 10));
        g.BP(0x4B, kXfb >> 5), g.BP(0x4D, 640 / 16), g.BP(0x4E, 256);
        const uint32_t cmd = 1u << 14;
        g.BP(0x52, cmd);
        gx::record_copy(g.regs(), cmd);  // ends the frame
        while (auto f = gx::next_frame(0)) {
            renderer.render(*f);
            gx::recycle_frame(std::move(f));
        }
        vkDeviceWaitIdle(gpu.device);
        const host::Image* x = renderer.xfb(kXfb);
        if (!x) rt::fatal(nullptr, "render_test: no XFB image");
        const VkImage image = x->image;
        const uint32_t w = x->width, h = x->height;
        host::Buffer buf = host::Buffer::host_visible(gpu, VkDeviceSize(w) * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.queueFamilyIndex = gpu.family;
        VkCommandPool pool;
        vkCreateCommandPool(gpu.device, &pci, nullptr, &pool);
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        VkCommandBuffer cmdbuf;
        vkAllocateCommandBuffers(gpu.device, &cai, &cmdbuf);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vkBeginCommandBuffer(cmdbuf, &bi);
        VkBufferImageCopy r{};
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        r.imageExtent = {w, h, 1};
        vkCmdCopyImageToBuffer(cmdbuf, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf.buffer, 1, &r);
        vkEndCommandBuffer(cmdbuf);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmdbuf;
        vkQueueSubmit(gpu.queue, 1, &si, VK_NULL_HANDLE);
        vkQueueWaitIdle(gpu.queue);
        std::vector<uint8_t> px(static_cast<const uint8_t*>(buf.mapped), static_cast<const uint8_t*>(buf.mapped) + size_t(w) * h * 4);
        vkDestroyCommandPool(gpu.device, pool, nullptr);
        buf.destroy(gpu);
        if (clear_after) g.clear();  // the next scenario starts black and far
        return px;
    }
};

const uint8_t* at(const std::vector<uint8_t>& px, int x, int y) { return &px[size_t(y * 640 + x) * 4]; }
std::string rgb(const uint8_t* p) {
    char s[32];
    std::snprintf(s, sizeof s, "%u,%u,%u", p[0], p[1], p[2]);
    return s;
}
bool near(const uint8_t* p, int r, int g, int b, int tolerance = 2) {
    return std::abs(p[0] - r) <= tolerance && std::abs(p[1] - g) <= tolerance && std::abs(p[2] - b) <= tolerance;
}
// How many pixels in a window are lit.
int lit(const std::vector<uint8_t>& px, int x0, int y0, int x1, int y1) {
    int n = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) n += at(px, x, y)[0] > 128;
    return n;
}

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

// ---- the scenarios

void sanity(Target& t, Gx& g) {
    quad(g, 0, 0, 640, 528, -0.5f, 200, 40, 90);
    const auto px = t.finish(g);
    check(near(at(px, 320, 264), 200, 40, 90), "a quad's colour reaches the frame buffer (" + rgb(at(px, 320, 264)) + ")");
}

// The copy filter: deflicker weights 16/32/16 (above, the line, below)
// blend the line where white meets black, as the hardware's integer maths:
// the last white line (16*255 + 32*255) / 64 = 191, the first black one
// 16*255 / 64 = 63; lines further away are unchanged.
void copy_filter(Target& t, Gx& g) {
    quad(g, 0, 0, 640, 264, -0.5f, 255, 255, 255);
    g.BP(0x53, 8u | (8u << 6) | (10u << 12) | (12u << 18));
    g.BP(0x54, 10u | (8u << 6) | (8u << 12));
    const auto px = t.finish(g);
    g.BP(0x53, 0x595000), g.BP(0x54, 0x15);  // GXInit's again
    const int a = at(px, 320, 262)[1], b = at(px, 320, 263)[1], c = at(px, 320, 264)[1], d = at(px, 320, 265)[1];
    check(a == 255 && std::abs(b - 191) <= 1 && std::abs(c - 63) <= 1 && d == 0,
          "the copy filter blends the lines around an edge: " + std::to_string(a) + " " + std::to_string(b) + " " +
              std::to_string(c) + " " + std::to_string(d) + " (255 191 63 0)");
}

// dst (255,0,255) op src (255,255,0), per channel on 0/255.
uint8_t logic(unsigned op, uint8_t s, uint8_t d) {
    const unsigned S = s ? 1 : 0, D = d ? 1 : 0;
    const unsigned nS = 1 - S, nD = 1 - D;
    const unsigned table[16] = {0, S & D, S & nD, S, nS & D, D, S ^ D, S | D, 1 - (S | D), 1 - (S ^ D), nD, S | nD, nS, nS | D, 1 - (S & D), 1};
    return table[op] ? 255 : 0;
}
void logic_ops(Target& t, Gx& g, bool exact_only, const char* how) {
    static const char* names[16] = {"clear", "and", "and-reverse", "copy", "and-inverted", "no-op", "xor", "or", "nor",
                                    "equivalent", "invert", "or-reverse", "copy-inverted", "or-inverted", "nand", "set"};
    for (unsigned op = 0; op < 16; ++op) {
        // The approximation is exact for these (Dolphin's list), and for xor, and and or on 0/255.
        const bool exact = op == 0 || op == 3 || op == 5 || op == 10 || op == 12 || op == 15 || op == 1 || op == 6 || op == 7;
        if (exact_only && !exact) continue;
        quad(g, 0, 0, 640, 528, -0.5f, 255, 0, 255);
        g.BP(0x41, (1u << 1) | (1u << 3) | (op << 12));  // logic op on, colour updates
        quad(g, 0, 0, 640, 528, -0.5f, 255, 255, 0);
        g.BP(0x41, 1u << 3);
        const auto px = t.finish(g);
        const uint8_t* p = at(px, 320, 264);
        const int r = logic(op, 255, 255), gg = logic(op, 255, 0), b = logic(op, 0, 255);
        check(near(p, r, gg, b), std::string("logic op ") + names[op] + " (" + how + "): " + rgb(p));
    }
}

void lines_and_points(Target& t, Gx& g, bool wide, const char* how) {
    g.BP(0x22, 36u | (60u << 8));                        // lines 6 pixels wide, points 10 pixels
    draw(g, 0xA8, {{100.0f, 200.0f, -0.5f, 255, 255, 255, 255}, {540.0f, 200.0f, -0.5f, 255, 255, 255, 255}});
    draw(g, 0xB8, {{320.0f, 400.0f, -0.5f, 255, 255, 255, 255}});
    g.BP(0x22, 6u | (6u << 8));
    const auto px = t.finish(g);
    const int line = lit(px, 320, 180, 321, 220);   // the line's thickness, down one column
    const int point = lit(px, 300, 380, 340, 420);  // the point's area
    if (wide) {
        check(std::abs(line - 6) <= 1, "a line 6 pixels wide is " + std::to_string(line) + " (" + how + ")");
        check(std::abs(point - 100) <= 21, "a point 10 pixels across covers " + std::to_string(point) + " pixels (" + how + ")");
    } else {
        check(line == 1 && point == 1, std::string("without wide lines and large points: 1 pixel each (") + how + "), got " +
                                           std::to_string(line) + " and " + std::to_string(point));
    }
}

// Triangle strips and lists are drawn as Vulkan strips and lists, without
// indices: a strip's alternate triangles must keep GX's winding (with culling
// on, all of the strip shows, or none), and a list's triangles all show.
void strips_and_lists(Target& t, Gx& g) {
    const uint32_t genmode = g.bp[0x00];
    int shown[3] = {};
    for (unsigned cull = 0; cull < 3; ++cull) {  // none, back, front
        g.BP(0x00, (genmode & ~(3u << 14)) | (cull << 14));
        draw(g, 0x98, {{100, 100, -0.5f, 255, 255, 255, 255}, {100, 300, -0.5f, 255, 255, 255, 255},
                       {200, 100, -0.5f, 255, 255, 255, 255}, {200, 300, -0.5f, 255, 255, 255, 255},
                       {300, 100, -0.5f, 255, 255, 255, 255}, {300, 300, -0.5f, 255, 255, 255, 255}});
        const auto px = t.finish(g);
        shown[cull] = lit(px, 100, 100, 300, 300);
    }
    g.BP(0x00, genmode);
    const int all = 200 * 200;
    check(std::abs(shown[0] - all) <= 400, "a triangle strip covers its band: " + std::to_string(shown[0]) + " of " + std::to_string(all));
    const bool one_way = (std::abs(shown[1] - all) <= 400 && shown[2] == 0) || (std::abs(shown[2] - all) <= 400 && shown[1] == 0);
    check(one_way, "culling keeps or drops a whole strip (its triangles wind alike): back " + std::to_string(shown[1]) + ", front " +
                       std::to_string(shown[2]));
    draw(g, 0x90, {{100, 100, -0.5f, 255, 255, 255, 255}, {100, 300, -0.5f, 255, 255, 255, 255}, {300, 100, -0.5f, 255, 255, 255, 255},
                   {300, 100, -0.5f, 255, 255, 255, 255}, {100, 300, -0.5f, 255, 255, 255, 255}, {300, 300, -0.5f, 255, 255, 255, 255}});
    const auto px = t.finish(g);
    const int list = lit(px, 100, 100, 300, 300);
    check(std::abs(list - all) <= 400, "a triangle list covers its square: " + std::to_string(list) + " of " + std::to_string(all));
}

// A texture of one I8 value, 8x4 (one tile), in guest memory.
void i8_texture(uint32_t phys, uint8_t value) { std::memset(rt::guest_ptr(0x80000000u | phys), value, 32); }

void z_textures(Target& t, Gx& g) {
    for (int early = 0; early < 2; ++early) {
        i8_texture(kTexI8, 0x10);
        g.texture(kTexI8, 8, 4, 1);                       // I8: alpha 0x10
        g.color_from(10, 5);                              // draw the vertex colour; the texture is sampled all the same
        g.BP(0x43, early ? (1u << 6) : 0u);
        g.BP(0xF4, 0x100000), g.BP(0xF5, 0u | (2u << 2));  // Z texture: u8, replace: z = 0x100010, near
        quad(g, 0, 0, 640, 528, -0.5f, 0, 255, 0, true);   // green, at mid depth by its vertices
        g.BP(0xF5, 0), g.BP(0x43, 0);
        g.no_texture();
        quad(g, 0, 0, 640, 528, -0.5f, 255, 0, 0);         // red at mid depth, LEQUAL: hidden only if the green wrote 0x100010
        const auto px = t.finish(g);
        const uint8_t* p = at(px, 320, 264);
        if (!early) check(near(p, 0, 255, 0), "a Z texture (replace, Z after texturing) writes depth: " + rgb(p));
        else check(near(p, 255, 0, 0), "a Z texture with the Z test before texturing leaves depth alone: " + rgb(p));
    }
}

void depth_copy(Target& t, Gx& g) {
    // Depth 0.3 of the way: 0x4CCCCC, so Z8 (the high byte) is 0x4C and Z8M (the
    // middle byte) 0xCC. Z8M is what shows the bytes are split: reading the
    // depth image as a colour gets the high byte about right by itself.
    const uint32_t z = uint32_t(0.3 * 16777215.0);
    struct Case { uint32_t fmt4; const char* name; int want; };
    const Case cases[] = {{1, "Z8 (the high byte)", int(z >> 16)}, {3, "Z8M (the middle byte)", int((z >> 8) & 255)}};
    for (const Case& k : cases) {
        quad(g, 0, 0, 640, 528, -0.3f, 0, 0, 0);
        g.BP(0x43, 3);                                    // Z24: the copy reads depth
        g.BP(0x49, 0), g.BP(0x4A, 63u | (63u << 10));
        g.BP(0x4B, kCopyTex >> 5), g.BP(0x4D, 64 / 32);
        const uint32_t cmd = k.fmt4 << 3;                 // texture copy: R8 (8) or G8 (9)
        g.BP(0x52, cmd);
        gx::record_copy(g.regs(), cmd);
        g.BP(0x43, 0);
        g.texture(kCopyTex, 64, 64, 1);                   // sampled as I8
        g.color_from(8, 4);                               // texture colour
        quad(g, 0, 0, 640, 528, -0.1f, 255, 255, 255, true);
        g.no_texture();
        g.color_from(10, 5);
        const auto px = t.finish(g);
        const uint8_t* p = at(px, 320, 264);
        char want[8];
        std::snprintf(want, sizeof want, "0x%02X", k.want);
        check(near(p, k.want, k.want, k.want, 2), std::string("an EFB copy of depth as ") + k.name + " is " + want + ": " + rgb(p));
    }
}

void rg8_gb8(Target& t, Gx& g) {
    for (int gb = 0; gb < 2; ++gb) {
        quad(g, 0, 0, 640, 528, -0.5f, 0x40, 0x80, 0xC0);
        g.BP(0x49, 0), g.BP(0x4A, 63u | (63u << 10));
        g.BP(0x4B, kCopyTex >> 5), g.BP(0x4D, 64 / 16);
        const uint32_t fmt4 = gb ? 9u : 7u;               // RG8 = 11, GB8 = 12: fmt/2 + (fmt & 1) * 8
        const uint32_t cmd = fmt4 << 3;
        g.BP(0x52, cmd);
        gx::record_copy(g.regs(), cmd);
        g.texture(kCopyTex, 64, 64, 3);                   // sampled as IA8
        g.color_from(8, 4);
        quad(g, 0, 0, 640, 528, -0.25f, 255, 255, 255, true);
        g.no_texture();
        g.color_from(10, 5);
        const auto px = t.finish(g);
        const uint8_t* p = at(px, 320, 264);
        const int want = gb ? 0x80 : 0x40;
        check(near(p, want, want, want), std::string(gb ? "GB8" : "RG8") + " copies sample with intensity from " +
                                             (gb ? "green" : "red") + ": " + rgb(p));
    }
}

const rt::FunctionEntry kNoFunctions[] = {{0x80004000u, [](rt::Cpu&) {}}};

}  // namespace

int main() {
    if (!vulkan_13_device()) {
        std::puts("render_test: no Vulkan 1.3 device here: skipped");
        return std::getenv("QILIN_REQUIRE_GRAPHICS") ? 1 : 77;
    }
    rt::init({kNoFunctions, {}});  // guest memory, for textures
    host::Gpu gpu = host::Gpu::create({}, false);
    std::printf("device: %s (logic ops %s, wide lines %s, large points %s)\n", gpu.name.c_str(), gpu.logic_op ? "yes" : "no",
                gpu.wide_lines ? "yes" : "no", gpu.large_points ? "yes" : "no");
    gx::set_rendering(true);
    {
        Target t(gpu);
        Gx g;
        g.setup();
        sanity(t, g);
        copy_filter(t, g);
        if (gpu.logic_op) logic_ops(t, g, false, "the GPU's");
        lines_and_points(t, g, gpu.wide_lines && gpu.large_points, "the GPU's");
        strips_and_lists(t, g);
        z_textures(t, g);
        depth_copy(t, g);
        rg8_gb8(t, g);
        t.finish(g, false);  // everything this renderer's textures are in, drawn by it
    }
    {
        // As on a GPU without the optional features (Apple's, through MoltenVK).
        host::Gpu without = gpu;
        without.logic_op = without.wide_lines = without.large_points = false;
        without.max_line_width = without.max_point_size = 1.0f;
        Target t(without);
        Gx g;
        g.scratch = 0x00700000u;
        g.setup();
        logic_ops(t, g, true, "approximated");
        lines_and_points(t, g, false, "fallback");
        t.finish(g, false);
    }
    gpu.destroy();
    std::printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
