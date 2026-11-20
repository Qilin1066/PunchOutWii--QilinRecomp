// The renderer (renderer.hpp).
//
// The EFB is an RGBA8 colour image and a depth image at `scale` times the
// console's 640x528; both stay in the GENERAL layout, drawn to with dynamic
// rendering and read by EFB copies. Draws use one pipeline layout and the GX
// shaders (punchout/shaders/), with the frame's states in a storage buffer;
// a pipeline is made for each blend and topology combination the game uses
// (the rest is dynamic state). EFB copies become textures (sampled later by
// the id the recorder gave them) or external frame buffers, kept by address
// for VI to show.
//
// Two frames are in flight at most: each has its own command buffer, fence,
// buffers and descriptor pool, and frees what the guest released once its
// fence says the GPU is done with it.
//
// Runtime code: no std::format (macOS 10.15).
#include "renderer.hpp"
#include "recorder.hpp"
#include "priority.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#include <vector>

#include "qilin/host/paths.hpp"
#include "qilin/rt/system.hpp"
#include "shaders.inc"
#include "pipeline_seed.inc"  // kPipelineSeed: pipelines.keys as this build's runs left it

namespace punchout {
namespace host = qilin::host;
namespace rt = qilin::rt;
using host::Buffer;
using host::check;
using host::Image;

namespace {

constexpr uint32_t kEfbWidth = 640, kEfbHeight = 528;
constexpr VkFormat kColor = VK_FORMAT_R8G8B8A8_UNORM;

struct Push {
    uint32_t state, has, flags;
    float scale;
    float point_size;
};

// GX's logic ops as blending, for GPUs without logic ops (Dolphin's
// ApproximateLogicOpWithBlending): blend on or off, reverse-subtract, the
// Vulkan factors (alpha as colour), and what the shader must do to its output
// (1 output 0, 2 output 1, 3 invert). Exact for clear, copy, no-op, invert,
// copy-inverted and set; close for the rest.
struct LogicBlend {
    bool blend, subtract;
    VkBlendFactor src, dst;
    uint32_t help;
};
constexpr LogicBlend kLogicBlend[16] = {
    {false, false, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, 1},                                    // clear
    {true, false, VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ZERO, 0},                               // and
    {true, true, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, 0},                       // and reverse
    {false, false, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, 0},                                    // copy
    {true, true, VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ONE, 0},                                 // and inverted
    {true, false, VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, 0},                                     // no-op
    {true, false, VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, 0},      // xor
    {true, false, VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_ONE, 0},                      // or
    {true, false, VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, 0},      // nor
    {true, false, VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_ZERO, 0},                     // equivalent
    {true, false, VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_ZERO, 2},                     // invert
    {true, false, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA, 0},                      // or reverse
    {false, false, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, 3},                                    // copy inverted
    {true, false, VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_ONE, 0},                      // or inverted
    {true, false, VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, 0},      // nand
    {false, false, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, 2},                                    // set
};

// Once per run: something drawn differently from the console on this GPU.
void approximated(bool& said, const char* what) {
    if (said) return;
    said = true;
    rt::log("render: %s", what);
}
struct FullscreenPush {
    float src[4];
    float color[4];
    float depth;
    uint32_t format, flags;
    float gamma;
    uint32_t filter[3];   // copies: the copy filter's weights for the line above, the line, the line below
    float clamp_top, clamp_bottom;  // copies: the lines the filter may read, in the EFB's 0..1
    float line;           // copies: one line of the console's EFB, in the EFB's 0..1
};

// An image no longer in use, kept to be used again rather than destroyed:
// creating and destroying images every frame (EFB copies, textures that
// change) costs more than drawing them.
struct Spare {
    Image image;
    VkImageUsageFlags usage = 0;
    uint64_t since = 0;  // the frame it became spare
};
constexpr VkImageUsageFlags kUploaded = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
constexpr VkImageUsageFlags kCopied = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;  // read back by PUNCHOUT_PROBE
constexpr VkImageUsageFlags kXfb = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;  // sampled: the window draws it through a look
constexpr uint64_t kSpareFrames = 120;  // spares unused for two seconds are destroyed
constexpr size_t kMaxSpares = 64;

struct Slot {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    Buffer vertices, indices, states, staging;
    VkDescriptorPool descriptors = VK_NULL_HANDLE;
    std::vector<Spare> garbage;  // images this frame stopped using: spare once its fence has passed
};

struct SamplerKey {
    gx::Sampler s;
    bool operator<(const SamplerKey& o) const { return std::memcmp(&s, &o.s, sizeof s) < 0; }
};
// GX's three kinds of primitive as Vulkan lists (a pipeline's class).
constexpr VkPrimitiveTopology kTopologyLists[3] = {VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
                                                   VK_PRIMITIVE_TOPOLOGY_POINT_LIST};

// A draw's eight (view, sampler) pairs: its texture descriptor set's key.
struct TextureSetKey {
    std::array<uint64_t, 16> v;
    bool operator==(const TextureSetKey&) const = default;
};
struct TextureSetHash {
    size_t operator()(const TextureSetKey& k) const {
        uint64_t h = 1469598103934665603ull;  // FNV-1a over the words
        for (uint64_t w : k.v) h = (h ^ w) * 1099511628211ull;
        return size_t(h ^ (h >> 32));
    }
};
struct PipelineKey {
    gx::PipelineKey k;
    bool operator<(const PipelineKey& o) const { return std::memcmp(&k, &o.k, sizeof k) < 0; }
};

VkShaderModule module(VkDevice dev, const uint32_t* code, size_t bytes) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m;
    check(vkCreateShaderModule(dev, &ci, nullptr, &m), "vkCreateShaderModule");
    return m;
}

}  // namespace

struct Renderer::Impl {
    host::Gpu& gpu;
    unsigned scale;
    // PUNCHOUT_PROBE: the captured frame's texture copies, read back as they're made.
    struct CopyDump {
        Buffer buf;
        uint32_t w, h, format;
    };
    std::vector<CopyDump>* copy_dumps = nullptr;
    float anisotropy = 1.0f;  // for filtered textures (set_anisotropy)
    VkFormat depth_format = VK_FORMAT_D32_SFLOAT;
    Image efb, efb_depth, white;
    Slot slots[2];
    unsigned slot = 0;
    // GPU time per frame: two timestamps a slot, where the queue has them.
    VkQueryPool timestamps = VK_NULL_HANDLE;
    double ns_per_tick = 0;
    uint64_t tick_mask = 0;
    bool timed[2] = {};
    uint64_t frames = 0;

    VkShaderModule gx_vert, gx_frag, gx_frag_depth, fs_vert, clear_frag, copy_frag;
    VkShaderModule gx_frag_stages[16];  // specialised, by TEV stage count
    VkDescriptorSetLayout states_layout, textures_layout, copy_set_layout;
    VkPipelineLayout gx_layout, clear_layout, copy_layout;
    std::map<PipelineKey, VkPipeline> pipelines;  // the ubershader's (main thread)
    // Pipelines specialised to a draw's shape (gx.hpp, gx.frag): compiled by
    // worker threads into a pipeline cache kept on disk, the ubershader's
    // pipeline drawing until each is ready, so a new shape never stalls a
    // frame. PUNCHOUT_UBERSHADER=1 keeps to the ubershader.
    //
    // The ubershader's own pipelines (one per blend, write mask, logic op,
    // topology and vertex layout) can't wait: a draw needs one. So every
    // pipeline the game has used is remembered in pipelines.keys beside the
    // cache (and those seen while making this build are built in,
    // pipeline_seed.inc): the ubershader's are built before the first frame,
    // the specialised ones by the workers as the game starts; from the
    // driver's cache after the first run, a few milliseconds each. A draw
    // only compiles on the drawing thread when its pipeline is new to every
    // run so far, and the log says how long it took.
    enum class Specialize { kOff, kAsync, kSync } specialize = Specialize::kAsync;
    bool specialize_on = true;  // set_specialization
    struct SpecKey {
        gx::PipelineKey k;
        uint32_t shape;
        bool operator<(const SpecKey& o) const {
            const int c = std::memcmp(&k, &o.k, sizeof k);
            return c != 0 ? c < 0 : shape < o.shape;
        }
    };
    std::map<SpecKey, VkPipeline> specialized;  // ready (main thread)
    std::set<SpecKey> requested;                 // asked of the worker (main thread)
    std::mutex spec_lock;
    std::condition_variable spec_wake;
    struct Job {
        bool uber;  // the ubershader's pipeline for key.k, or key specialised
        SpecKey key;
    };
    std::deque<Job> spec_queue;                                     // to compile
    // Shapes a draw is waiting for (drawn by the ubershader meanwhile): the
    // workers take these before the rest of the queue, so what's on screen now
    // (a new effect, the first time) is specialised first, not after the
    // remembered ones queued at start. A job taken from either is done once.
    std::deque<SpecKey> spec_urgent;
    std::set<SpecKey> spec_started;                                 // taken by a worker (under spec_lock)
    std::set<SpecKey> promoted;                                     // already sent to spec_urgent (main thread)
    std::vector<std::pair<SpecKey, VkPipeline>> spec_done;          // compiled, not yet taken
    std::vector<std::pair<PipelineKey, VkPipeline>> uber_done;      // the same, the ubershader's
    std::set<PipelineKey> uber_requested;                           // queued or built (main thread)
    bool spec_quit = false;
    std::vector<std::thread> spec_workers;
    // pipelines.keys: what the game used, appended to as new ones appear.
    std::filesystem::path keys_path;
    std::vector<uint8_t> keys_new;  // records not yet written (main thread)
    // Writing the cache and the keys: a thread of its own, at most every 20 s
    // (and at the end), never the drawing thread.
    std::thread saver;
    std::condition_variable save_wake;
    bool save_wanted = false;
    std::vector<uint8_t> keys_to_write;  // under spec_lock
    std::chrono::steady_clock::time_point last_save{};
    uint64_t stalls = 0;      // pipelines compiled on the drawing thread
    double stall_ms = 0;
    VkPipelineCache pipeline_cache = VK_NULL_HANDLE;
    std::filesystem::path cache_path;
    size_t cache_saved_at = 0;  // pipelines (both kinds) when the cache was last written
    uint64_t draws_specialized = 0, draws_total = 0;
    VkDeviceSize small_at = 0, zeros_at = 0;  // this frame's compact vertices and zeros, in the vertex buffer
    double spec_ms = 0;      // time the worker spent compiling (under spec_lock)
    uint64_t spec_count = 0;
    std::map<unsigned, VkPipeline> clear_pipelines;  // colour mask | depth write << 4
    VkPipeline copy_pipeline = VK_NULL_HANDLE;
    std::map<SamplerKey, VkSampler> samplers;
    VkSampler unused_sampler = VK_NULL_HANDLE;  // for texture slots a draw doesn't use (one of samplers)
    VkSampler efb_sampler = VK_NULL_HANDLE, efb_depth_sampler = VK_NULL_HANDLE;  // linear for colour, nearest for depth

    std::unordered_map<int32_t, Spare> textures;  // by id, with the usage they were made for
    std::vector<Spare> spares;
    std::map<uint32_t, Image> xfbs;  // by guest address

    // An image of this size and use: a spare one if there is one.
    Image obtain(uint32_t w, uint32_t h, uint32_t levels, VkImageUsageFlags usage) {
        for (size_t i = 0; i < spares.size(); ++i) {
            const Image& im = spares[i].image;
            if (spares[i].usage == usage && im.width == w && im.height == h && im.levels == levels) {
                Image found = im;
                spares[i] = spares.back();
                spares.pop_back();
                return found;
            }
        }
        gx::recorder_stats().images_made.fetch_add(1, std::memory_order_relaxed);
        return Image::create(gpu, w, h, kColor, usage, levels);
    }
    // A frame's garbage, its fence passed: spare now. Old spares go.
    void recycle(Slot& s) {
        // The cap: at least a frame's worth of what comes back. A fight makes
        // ~75 EFB copies a frame, each replacing last frame's image; capping
        // below that destroyed images every frame only to make them again.
        const size_t cap = std::max(kMaxSpares, 2 * s.garbage.size());
        for (Spare& g : s.garbage) {
            g.since = frames;
            spares.push_back(g);
        }
        s.garbage.clear();
        std::erase_if(spares, [this](Spare& sp) {
            if (frames - sp.since <= kSpareFrames) return false;
            sp.image.destroy(gpu);
            return true;
        });
        while (spares.size() > cap) {  // the oldest first
            auto oldest = std::min_element(spares.begin(), spares.end(), [](const Spare& a, const Spare& b) { return a.since < b.since; });
            oldest->image.destroy(gpu);
            *oldest = spares.back();
            spares.pop_back();
        }
    }

    explicit Impl(host::Gpu& g, unsigned s) : gpu(g), scale(s) {}

    void set_anisotropy(unsigned level) {
        const float a = std::clamp(float(level), 1.0f, gpu.max_anisotropy);
        if (a == anisotropy) return;
        vkDeviceWaitIdle(gpu.device);  // the samplers may be in use
        for (auto& [k, v] : samplers) vkDestroySampler(gpu.device, v, nullptr);
        samplers.clear();
        anisotropy = a;
    }

    // ---- setup

    void create() {
        start_specialization();
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(gpu.physical, VK_FORMAT_D32_SFLOAT, &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) depth_format = VK_FORMAT_X8_D24_UNORM_PACK32;
        efb = Image::create(gpu, kEfbWidth * scale, kEfbHeight * scale, kColor,
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        efb_depth = Image::create(gpu, kEfbWidth * scale, kEfbHeight * scale, depth_format,
                                  VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        white = Image::create(gpu, 1, 1, kColor, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        {
            uint32_t qn = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(gpu.physical, &qn, nullptr);
            std::vector<VkQueueFamilyProperties> qs(qn);
            vkGetPhysicalDeviceQueueFamilyProperties(gpu.physical, &qn, qs.data());
            const uint32_t bits = gpu.family < qn ? qs[gpu.family].timestampValidBits : 0;
            if (bits && gpu.properties.limits.timestampPeriod > 0) {
                VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
                qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
                qi.queryCount = 4;
                check(vkCreateQueryPool(gpu.device, &qi, nullptr, &timestamps), "vkCreateQueryPool");
                ns_per_tick = gpu.properties.limits.timestampPeriod;
                tick_mask = bits >= 64 ? ~uint64_t{0} : (uint64_t{1} << bits) - 1;
            }
        }
        for (Slot& s : slots) {
            VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            pci.queueFamilyIndex = gpu.family;
            check(vkCreateCommandPool(gpu.device, &pci, nullptr, &s.pool), "vkCreateCommandPool");
            VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            cai.commandPool = s.pool;
            cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cai.commandBufferCount = 1;
            check(vkAllocateCommandBuffers(gpu.device, &cai, &s.cmd), "vkAllocateCommandBuffers");
            VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            check(vkCreateFence(gpu.device, &fi, nullptr, &s.fence), "vkCreateFence");
            const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4},
                                                  {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8 * 4096 + 256}};
            VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            dpi.maxSets = 4096 + 260;
            dpi.poolSizeCount = 2;
            dpi.pPoolSizes = sizes;
            check(vkCreateDescriptorPool(gpu.device, &dpi, nullptr, &s.descriptors), "vkCreateDescriptorPool");
        }
        gx_vert = module(gpu.device, kGxVert, sizeof kGxVert);
        gx_frag = module(gpu.device, kGxFrag, sizeof kGxFrag);
        gx_frag_depth = module(gpu.device, kGxFragDepth, sizeof kGxFragDepth);
        static constexpr struct {
            const uint32_t* code;
            size_t bytes;
        } kStages[16] = {{kGxFragStages1, sizeof kGxFragStages1}, {kGxFragStages2, sizeof kGxFragStages2}, {kGxFragStages3, sizeof kGxFragStages3}, {kGxFragStages4, sizeof kGxFragStages4}, {kGxFragStages5, sizeof kGxFragStages5}, {kGxFragStages6, sizeof kGxFragStages6}, {kGxFragStages7, sizeof kGxFragStages7}, {kGxFragStages8, sizeof kGxFragStages8}, {kGxFragStages9, sizeof kGxFragStages9}, {kGxFragStages10, sizeof kGxFragStages10}, {kGxFragStages11, sizeof kGxFragStages11}, {kGxFragStages12, sizeof kGxFragStages12}, {kGxFragStages13, sizeof kGxFragStages13}, {kGxFragStages14, sizeof kGxFragStages14}, {kGxFragStages15, sizeof kGxFragStages15}, {kGxFragStages16, sizeof kGxFragStages16}};
        for (unsigned i = 0; i < 16; ++i) gx_frag_stages[i] = module(gpu.device, kStages[i].code, kStages[i].bytes);
        fs_vert = module(gpu.device, kFullscreenVert, sizeof kFullscreenVert);
        clear_frag = module(gpu.device, kClearFrag, sizeof kClearFrag);
        copy_frag = module(gpu.device, kCopyFrag, sizeof kCopyFrag);
        // Layouts.
        VkDescriptorSetLayoutBinding b0{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = 1;
        li.pBindings = &b0;
        check(vkCreateDescriptorSetLayout(gpu.device, &li, nullptr, &states_layout), "vkCreateDescriptorSetLayout");
        VkDescriptorSetLayoutBinding b1{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        li.pBindings = &b1;
        check(vkCreateDescriptorSetLayout(gpu.device, &li, nullptr, &textures_layout), "vkCreateDescriptorSetLayout");
        VkDescriptorSetLayoutBinding b2{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        li.pBindings = &b2;
        check(vkCreateDescriptorSetLayout(gpu.device, &li, nullptr, &copy_set_layout), "vkCreateDescriptorSetLayout");
        const VkDescriptorSetLayout gx_sets[] = {states_layout, textures_layout};
        VkPushConstantRange pr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push)};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 2;
        pli.pSetLayouts = gx_sets;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &pr;
        check(vkCreatePipelineLayout(gpu.device, &pli, nullptr, &gx_layout), "vkCreatePipelineLayout");
        VkPushConstantRange fpr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(FullscreenPush)};
        pli.setLayoutCount = 0;
        pli.pPushConstantRanges = &fpr;
        check(vkCreatePipelineLayout(gpu.device, &pli, nullptr, &clear_layout), "vkCreatePipelineLayout");
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &copy_set_layout;
        check(vkCreatePipelineLayout(gpu.device, &pli, nullptr, &copy_layout), "vkCreatePipelineLayout");
        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        check(vkCreateSampler(gpu.device, &sci, nullptr, &efb_sampler), "vkCreateSampler");
        sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;  // depth: many GPUs can't filter D32
        check(vkCreateSampler(gpu.device, &sci, nullptr, &efb_depth_sampler), "vkCreateSampler");
        // First use: the EFB in GENERAL, cleared; the white texture filled.
        one_time([&](VkCommandBuffer cmd) {
            host::transition(cmd, efb.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
            host::transition(cmd, efb_depth.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_DEPTH_BIT);
            host::transition(cmd, white.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            const VkClearColorValue black{{0, 0, 0, 1}}, one{{1, 1, 1, 1}};
            const VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(cmd, efb.image, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &all);
            vkCmdClearColorImage(cmd, white.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &one, 1, &all);
            const VkClearDepthStencilValue far{1.0f, 0};
            const VkImageSubresourceRange d{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
            vkCmdClearDepthStencilImage(cmd, efb_depth.image, VK_IMAGE_LAYOUT_GENERAL, &far, 1, &d);
            host::transition(cmd, white.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        });
        start_warmup();
    }

    template <class F>
    void one_time(F&& record) {
        Slot& s = slots[0];
        check(vkWaitForFences(gpu.device, 1, &s.fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
        check(vkResetFences(gpu.device, 1, &s.fence), "vkResetFences");
        check(vkResetCommandBuffer(s.cmd, 0), "vkResetCommandBuffer");
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        check(vkBeginCommandBuffer(s.cmd, &bi), "vkBeginCommandBuffer");
        record(s.cmd);
        check(vkEndCommandBuffer(s.cmd), "vkEndCommandBuffer");
        submit(s);
    }

    void submit(Slot& s) {
        VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        cbi.commandBuffer = s.cmd;
        VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        si.commandBufferInfoCount = 1;
        si.pCommandBufferInfos = &cbi;
        check(vkQueueSubmit2(gpu.queue, 1, &si, s.fence), "vkQueueSubmit2");
    }

    VkPipeline make_pipeline(VkShaderModule vs, VkShaderModule fs, VkPipelineLayout layout, bool vertices, VkPrimitiveTopology topology,
                             const VkPipelineColorBlendAttachmentState& blend, bool depth, bool depth_dynamic, int logic_op = -1,
                             bool line_width_dynamic = false, const VkSpecializationInfo* fs_constants = nullptr,
                             bool small_vertices = false, bool blend_dynamic = false) {
        const VkPipelineShaderStageCreateInfo stages[2] = {
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", nullptr},
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", fs_constants}};
        VkVertexInputBindingDescription binds[2] = {{0, sizeof(gx::Vertex), VK_VERTEX_INPUT_RATE_VERTEX},
                                                    {1, 0, VK_VERTEX_INPUT_RATE_VERTEX}};  // binding 1: zeros, stride 0
        std::vector<VkVertexInputAttributeDescription> attrs = {
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(gx::Vertex, pos)},
            {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(gx::Vertex, normal)},
            {2, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(gx::Vertex, tangent)},
            {3, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(gx::Vertex, binormal)},
            {4, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(gx::Vertex, color0)},
            {5, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(gx::Vertex, color1)},
        };
        for (uint32_t i = 0; i < 8; ++i)
            attrs.push_back({6 + i, 0, VK_FORMAT_R32G32_SFLOAT, uint32_t(offsetof(gx::Vertex, uv) + 8 * i)});
        attrs.push_back({14, 0, VK_FORMAT_R32G32B32_UINT, offsetof(gx::Vertex, matrices)});
        if (small_vertices) {  // gx::SmallVertex; what it lacks reads zeros
            binds[0].stride = sizeof(gx::SmallVertex);
            attrs = {
                {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(gx::SmallVertex, pos)},
                {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(gx::SmallVertex, normal)},
                {2, 1, VK_FORMAT_R32G32B32_SFLOAT, 0},
                {3, 1, VK_FORMAT_R32G32B32_SFLOAT, 0},
                {4, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(gx::SmallVertex, color0)},
                {5, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(gx::SmallVertex, color1)},
            };
            for (uint32_t i = 0; i < gx::kSmallUvs; ++i)
                attrs.push_back({6 + i, 0, VK_FORMAT_R32G32_SFLOAT, uint32_t(offsetof(gx::SmallVertex, uv) + 8 * i)});
            for (uint32_t i = gx::kSmallUvs; i < 8; ++i) attrs.push_back({6 + i, 1, VK_FORMAT_R32G32_SFLOAT, 0});
            attrs.push_back({14, 0, VK_FORMAT_R32G32B32_UINT, offsetof(gx::SmallVertex, matrices)});
        }
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        if (vertices) {
            vi.vertexBindingDescriptionCount = small_vertices ? 2 : 1;
            vi.pVertexBindingDescriptions = binds;
            vi.vertexAttributeDescriptionCount = uint32_t(attrs.size());
            vi.pVertexAttributeDescriptions = attrs.data();
        }
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = topology;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        ds.depthTestEnable = depth;
        ds.depthWriteEnable = depth;
        ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1;
        cb.pAttachments = &blend;
        if (logic_op >= 0) {
            cb.logicOpEnable = VK_TRUE;
            cb.logicOp = VkLogicOp(logic_op);  // GX numbers its logic ops as Vulkan does
        }
        std::vector<VkDynamicState> dyn = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        if (line_width_dynamic) dyn.push_back(VK_DYNAMIC_STATE_LINE_WIDTH);
        if (blend_dynamic) {
            dyn.push_back(VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT);
            dyn.push_back(VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT);
            dyn.push_back(VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT);
            if (gpu.dynamic_logic) {
                dyn.push_back(VK_DYNAMIC_STATE_LOGIC_OP_ENABLE_EXT);
                dyn.push_back(VK_DYNAMIC_STATE_LOGIC_OP_EXT);
            }
        }
        if (depth_dynamic) {  // the GX pipelines
            dyn.insert(dyn.end(), {VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
                                   VK_DYNAMIC_STATE_DEPTH_COMPARE_OP});
            dyn.push_back(VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY);  // list or strip (Vulkan 1.3: within the class)
        }
        VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dy.dynamicStateCount = uint32_t(dyn.size());
        dy.pDynamicStates = dyn.data();
        VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        ri.colorAttachmentCount = 1;
        ri.pColorAttachmentFormats = &kColor;
        ri.depthAttachmentFormat = depth ? depth_format : VK_FORMAT_UNDEFINED;
        VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri};
        pci.stageCount = 2;
        pci.pStages = stages;
        pci.pVertexInputState = &vi;
        pci.pInputAssemblyState = &ia;
        pci.pViewportState = &vp;
        pci.pRasterizationState = &rs;
        pci.pMultisampleState = &ms;
        pci.pDepthStencilState = &ds;
        pci.pColorBlendState = &cb;
        pci.pDynamicState = &dy;
        pci.layout = layout;
        VkPipeline p;
        check(vkCreateGraphicsPipelines(gpu.device, pipeline_cache, 1, &pci, nullptr, &p), "vkCreateGraphicsPipelines");
        return p;
    }

    // The blend state for a key, and its logic op (-1: none, or approximated
    // by blending where the GPU has no logic ops).
    VkPipelineColorBlendAttachmentState blend_for(const gx::PipelineKey& k, int& logic) const {
        VkPipelineColorBlendAttachmentState b{};
        b.blendEnable = k.blend;
        b.srcColorBlendFactor = VkBlendFactor(k.src);
        b.dstColorBlendFactor = VkBlendFactor(k.dst);
        b.colorBlendOp = VkBlendOp(k.op);
        b.srcAlphaBlendFactor = VkBlendFactor(k.src_alpha);
        b.dstAlphaBlendFactor = VkBlendFactor(k.dst_alpha);
        b.alphaBlendOp = VkBlendOp(k.op_alpha);
        b.colorWriteMask = k.write_mask;
        logic = -1;
        if (k.logic) {
            const unsigned op = k.logic - 1u;
            if (gpu.logic_op) {
                logic = int(op);
                b.blendEnable = VK_FALSE;
            } else {
                static bool said = false;
                approximated(said, "logic ops are approximated with blending: this GPU has none");
                const LogicBlend& l = kLogicBlend[op];
                b.blendEnable = l.blend;
                b.srcColorBlendFactor = b.srcAlphaBlendFactor = l.src;
                b.dstColorBlendFactor = b.dstAlphaBlendFactor = l.dst;
                b.colorBlendOp = b.alphaBlendOp = l.subtract ? VK_BLEND_OP_REVERSE_SUBTRACT : VK_BLEND_OP_ADD;
            }
        }
        return b;
    }

    // With dynamic blend state (Gpu::dynamic_blend), the part of a key that
    // still needs a pipeline of its own: the blend (and the logic op, where
    // it's dynamic too or only approximated by blending) is set per draw.
    gx::PipelineKey base_key(gx::PipelineKey k) const {
        if (!gpu.dynamic_blend) return k;
        k.blend = k.src = k.dst = k.src_alpha = k.dst_alpha = k.op = k.op_alpha = 0;
        k.write_mask = 0xF;
        if (!gpu.logic_op || gpu.dynamic_logic) k.logic = 0;
        return k;
    }
    // Set a draw's blend state on the command buffer (dynamic blend only).
    void set_dynamic_blend(VkCommandBuffer cmd, const gx::PipelineKey& k) {
        int logic;
        const VkPipelineColorBlendAttachmentState b = blend_for(k, logic);
        const VkBool32 enable = b.blendEnable;
        vkCmdSetColorBlendEnableEXT(cmd, 0, 1, &enable);
        const VkColorBlendEquationEXT eq{b.srcColorBlendFactor, b.dstColorBlendFactor, b.colorBlendOp,
                                         b.srcAlphaBlendFactor, b.dstAlphaBlendFactor, b.alphaBlendOp};
        vkCmdSetColorBlendEquationEXT(cmd, 0, 1, &eq);
        const VkColorComponentFlags mask = b.colorWriteMask;
        vkCmdSetColorWriteMaskEXT(cmd, 0, 1, &mask);
        if (gpu.dynamic_logic) {
            vkCmdSetLogicOpEnableEXT(cmd, logic >= 0);
            if (logic >= 0) vkCmdSetLogicOpEXT(cmd, VkLogicOp(logic));
        }
    }

    // A GX pipeline: the ubershader's, or specialised to `shape` (thread-safe).
    VkPipeline build_gx(const gx::PipelineKey& k, const gx::ShapeWords* shape) {
        int logic;
        VkPipelineColorBlendAttachmentState b = blend_for(k, logic);
        if (gpu.dynamic_blend && gpu.dynamic_logic) logic = 0;  // logic op enable and op are dynamic: any value
        static constexpr VkPrimitiveTopology kTopology[3] = {VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
                                                              VK_PRIMITIVE_TOPOLOGY_POINT_LIST};
        // Specialisation: constants 1-70 are the shape's words (constant 0, the
        // switch, is fixed on in the specialised modules; an entry for it is ignored).
        uint32_t values[1 + gx::kShapeWords];
        VkSpecializationMapEntry entries[1 + gx::kShapeWords];
        VkSpecializationInfo info{};
        if (shape) {
            values[0] = 1;
            for (uint32_t i = 0; i < gx::kShapeWords; ++i) values[1 + i] = (*shape)[i];
            for (uint32_t i = 0; i <= gx::kShapeWords; ++i) entries[i] = {i, uint32_t(i * sizeof(uint32_t)), sizeof(uint32_t)};
            info = {1 + gx::kShapeWords, entries, sizeof values, values};
        }
        const VkShaderModule fs = shape ? gx_frag_stages[((*shape)[0] >> 10) & 15] : k.depth_out ? gx_frag_depth : gx_frag;
        return make_pipeline(gx_vert, fs, gx_layout, true, kTopology[k.topology], b, true, true, logic,
                             k.topology == 1 && gpu.wide_lines, shape ? &info : nullptr, k.small_vertices != 0, gpu.dynamic_blend);
    }

    // pipelines.keys records: a kind byte (0 the ubershader's, 1 specialised),
    // the key, and for 1 the shape's words.
    static constexpr char kKeysMagic[8] = {'P', 'O', 'P', 'K', 'E', 'Y', 'S', '1'};
    static constexpr size_t kKeysHeader = sizeof kKeysMagic + 8;
    void remember(bool uber, const gx::PipelineKey& k, uint32_t shape) {
        if (keys_path.empty()) return;
        keys_new.push_back(uber ? 0 : 1);
        const auto* kb = reinterpret_cast<const uint8_t*>(&k);
        keys_new.insert(keys_new.end(), kb, kb + sizeof k);
        if (!uber) {
            const gx::ShapeWords w = gx::shape_words(shape);
            const auto* wb = reinterpret_cast<const uint8_t*>(w.data());
            keys_new.insert(keys_new.end(), wb, wb + sizeof w);
        }
    }
    // Queue every remembered pipeline (records after the header).
    void queue_remembered(const uint8_t* p, size_t n, std::deque<Job>& uber, std::deque<Job>& spec) {
        const size_t one = 1 + sizeof(gx::PipelineKey);
        while (n >= one) {
            const uint8_t kind = p[0];
            gx::PipelineKey k;
            std::memcpy(&k, p + 1, sizeof k);
            k = base_key(k);
            if (kind == 0) {
                if (uber_requested.insert({k}).second) uber.push_back({true, {k, 0}});
                p += one, n -= one;
            } else if (kind == 1 && n >= one + sizeof(gx::ShapeWords)) {
                gx::ShapeWords w;
                std::memcpy(w.data(), p + one, sizeof w);
                const SpecKey key{k, gx::intern_shape(w)};
                if (!k.depth_out && requested.insert(key).second) spec.push_back({false, key});
                p += one + sizeof w, n -= one + sizeof w;
            } else {
                return;  // damaged: keep what was read
            }
        }
    }
    static bool keys_header_ok(const uint8_t* p, size_t n) {
        uint32_t key_size, words;
        if (n < kKeysHeader || std::memcmp(p, kKeysMagic, sizeof kKeysMagic) != 0) return false;
        std::memcpy(&key_size, p + 8, 4);
        std::memcpy(&words, p + 12, 4);
        return key_size == sizeof(gx::PipelineKey) && words == gx::kShapeWords;
    }

    // Start-up: the workers, then everything remembered queued for them (the
    // ubershader's pipelines first: draws need them; then the shapes).
    void start_warmup() {
        if (specialize == Specialize::kOff) return;
        keys_path = host::user_folder() / "pipelines.keys";
        std::deque<Job> uber, spec;
        if (keys_header_ok(kPipelineSeed, sizeof kPipelineSeed))
            queue_remembered(kPipelineSeed + kKeysHeader, sizeof kPipelineSeed - kKeysHeader, uber, spec);
        std::vector<uint8_t> file;
        if (FILE* f = std::fopen(keys_path.string().c_str(), "rb")) {
            uint8_t buf[65536];
            for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) file.insert(file.end(), buf, buf + n);
            std::fclose(f);
        }
        if (keys_header_ok(file.data(), file.size())) {
            queue_remembered(file.data() + kKeysHeader, file.size() - kKeysHeader, uber, spec);
        } else {  // none yet, or from another build: start it again
            std::error_code ec;
            std::filesystem::remove(keys_path, ec);
            keys_new.assign(kKeysMagic, kKeysMagic + sizeof kKeysMagic);
            const uint32_t key_size = sizeof(gx::PipelineKey), words = gx::kShapeWords;
            keys_new.insert(keys_new.end(), reinterpret_cast<const uint8_t*>(&key_size), reinterpret_cast<const uint8_t*>(&key_size) + 4);
            keys_new.insert(keys_new.end(), reinterpret_cast<const uint8_t*>(&words), reinterpret_cast<const uint8_t*>(&words) + 4);
        }
        if (gpu.dynamic_blend) {
            // Blend states are set per draw: the few pipelines left (each
            // topology, depth-writing or not, either vertex layout) are all
            // built now, so no draw ever waits for one.
            for (uint8_t topology = 0; topology < 3; ++topology)
                for (uint8_t depth_out = 0; depth_out < 2; ++depth_out)
                    for (uint8_t small = 0; small < 2; ++small) {
                        gx::PipelineKey k{};
                        k.topology = topology, k.depth_out = depth_out, k.small_vertices = small;
                        k = base_key(k);
                        if (uber_requested.insert({k}).second) uber.push_back({true, {k, 0}});
                    }
        }
        rt::log("pipelines: %llu of the ubershader's and %llu specialised remembered, compiling as the game starts",
                static_cast<unsigned long long>(uber.size()), static_cast<unsigned long long>(spec.size()));
        if (specialize == Specialize::kSync) {  // PUNCHOUT_SPECIALIZE_SYNC (the tests): each when first drawn, as before
            uber_requested.clear();
            requested.clear();
            return;
        }
        // The ubershader's now, before the first frame (a draw can't do without
        // one): a dozen or so, milliseconds each from the driver's cache after
        // the first run. The specialised ones go to the workers.
        const auto t0 = std::chrono::steady_clock::now();
        for (const Job& j : uber) pipelines[{j.key.k}] = build_gx(j.key.k, nullptr);
        if (!uber.empty())
            rt::log("pipelines: the ubershader's %llu built in %.0f ms before the first frame", static_cast<unsigned long long>(uber.size()),
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        {
            std::lock_guard lock(spec_lock);
            spec_queue.insert(spec_queue.end(), spec.begin(), spec.end());
        }
        spec_wake.notify_all();
    }

    // The pipeline for a draw: specialised if ready, else the ubershader's
    // while a worker compiles it.
    // The last draw's answer, reused while nothing new was made: most draws
    // share their neighbour's pipeline, and the maps compare whole keys.
    struct LastPipeline {
        gx::PipelineKey raw{};
        uint32_t shape = ~0u;
        uint64_t version = ~0ull;
        VkPipeline p = VK_NULL_HANDLE;
        bool specialized = false;
    } last_pipeline;
    uint64_t pipelines_version = 0;  // bumped whenever a pipeline is added to either map

    VkPipeline gx_pipeline(const gx::PipelineKey& raw, uint32_t shape) {
        LastPipeline& lp = last_pipeline;
        if (lp.version == pipelines_version && lp.shape == shape && std::memcmp(&lp.raw, &raw, sizeof raw) == 0 &&
            specialize_on == lp_specialize_on) {
            ++draws_total;
            if (lp.specialized) ++draws_specialized;
            return lp.p;
        }
        const uint64_t specialized_before = draws_specialized;
        VkPipeline p = gx_pipeline_lookup(raw, shape);
        lp = {raw, shape, pipelines_version, p, draws_specialized != specialized_before};
        lp_specialize_on = specialize_on;
        return p;
    }
    bool lp_specialize_on = true;

    VkPipeline gx_pipeline_lookup(const gx::PipelineKey& raw, uint32_t shape) {
        ++draws_total;
        const gx::PipelineKey k = base_key(raw);
        if (specialize != Specialize::kOff && specialize_on && !k.depth_out) {  // depth-writing draws (rare) keep the ubershader
            const SpecKey key{k, shape};
            if (auto it = specialized.find(key); it != specialized.end()) {
                ++draws_specialized;
                return it->second;
            }
            if (specialize == Specialize::kSync) {
                const gx::ShapeWords w = gx::shape_words(shape);
                ++draws_specialized;
                if (requested.insert(key).second) remember(false, raw, shape);
                ++pipelines_version;
                return specialized[key] = build_gx(k, &w);
            }
            if (requested.insert(key).second) {
                remember(false, raw, shape);
                std::lock_guard lock(spec_lock);
                spec_urgent.push_back(key);  // new and on screen now: ahead of the queue
                promoted.insert(key);
                spec_wake.notify_one();
            } else if (promoted.insert(key).second) {  // queued (remembered), not ready yet: needed now
                std::lock_guard lock(spec_lock);
                spec_urgent.push_back(key);
                spec_wake.notify_one();
            }
        }
        auto& p = pipelines[{k}];
        if (!p) {
            take_done();  // a worker may have just finished it
            if (!p) {     // new to every run so far (or not reached yet): compile it here
                const auto t0 = std::chrono::steady_clock::now();
                p = build_gx(k, nullptr);
                ++pipelines_version;
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                ++stalls, stall_ms += ms;
                if (stalls <= 64)
                    rt::log("pipelines: the ubershader's pipeline for a new blend state compiled on the drawing thread in %.0f ms", ms);
                if (uber_requested.insert({k}).second) remember(true, raw, 0);
            }
        }
        return p;
    }

    // A worker: compiles queued pipelines, in order.
    void compile_specialized() {
        set_this_thread_priority(Priority::background);  // the ubershaders cover it meanwhile
        for (;;) {
            Job job;
            {
                std::unique_lock lock(spec_lock);
                bool found = false;
                while (!found) {
                    spec_wake.wait(lock, [&] { return spec_quit || !spec_queue.empty() || !spec_urgent.empty(); });
                    if (spec_quit) return;
                    if (!spec_urgent.empty()) {
                        job = {false, spec_urgent.front()};
                        spec_urgent.pop_front();
                    } else {
                        job = spec_queue.front();
                        spec_queue.pop_front();
                    }
                    found = job.uber || spec_started.insert(job.key).second;  // the other copy of a promoted one: skip
                }
            }
            const auto t0 = std::chrono::steady_clock::now();
            VkPipeline p;
            if (job.uber) {
                p = build_gx(job.key.k, nullptr);
            } else {
                const gx::ShapeWords w = gx::shape_words(job.key.shape);
                p = build_gx(job.key.k, &w);
            }
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            std::lock_guard lock(spec_lock);
            if (job.uber) {
                uber_done.push_back({{job.key.k}, p});
            } else {
                spec_done.push_back({job.key, p});
                spec_ms += ms;
                ++spec_count;
            }
        }
    }

    // Main thread: take what the workers finished.
    void take_done() {
        std::lock_guard lock(spec_lock);
        if (!spec_done.empty() || !uber_done.empty()) ++pipelines_version;
        for (auto& [k, p] : spec_done) specialized[k] = p;
        spec_done.clear();
        for (auto& [k, p] : uber_done) {
            VkPipeline& mine = pipelines[k];
            if (mine) vkDestroyPipeline(gpu.device, p, nullptr);  // the drawing thread got there first
            else mine = p;
        }
        uber_done.clear();
    }

    // Main thread, each frame: take what the workers finished; now and then
    // have the cache and the new keys written, so the next run starts with
    // them compiled.
    void collect_specialized() {
        take_done();
        const size_t have = specialized.size() + pipelines.size();
        const auto now = std::chrono::steady_clock::now();
        if (!cache_path.empty() && (have > cache_saved_at || !keys_new.empty()) && now - last_save > std::chrono::seconds(20)) {
            last_save = now;
            cache_saved_at = have;
            std::lock_guard lock(spec_lock);
            keys_to_write.insert(keys_to_write.end(), keys_new.begin(), keys_new.end());
            keys_new.clear();
            save_wanted = true;
            save_wake.notify_all();
        }
        if (frames % 600 == 0 && draws_total)
            rt::log("pipelines: %llu specialised (%.0f ms each to compile), %llu compiling; %.0f%% of draws specialised (last 600 frames); "
                    "%llu compiled on the drawing thread (%.0f ms in all)",
                    static_cast<unsigned long long>(specialized.size()), spec_count ? spec_ms / double(spec_count) : 0.0,
                    static_cast<unsigned long long>(requested.size() - specialized.size()),
                    100.0 * double(draws_specialized) / double(draws_total), static_cast<unsigned long long>(stalls), stall_ms);
        if (frames % 600 == 0) draws_specialized = draws_total = 0;
    }

    // The saver thread: the cache (vkGetPipelineCacheData needs no lock
    // against the workers' vkCreateGraphicsPipelines) and the new keys.
    void save_loop() {
        set_this_thread_priority(Priority::background);  // never ahead of a frame
        for (;;) {
            std::vector<uint8_t> keys;
            bool quit;
            {
                std::unique_lock lock(spec_lock);
                save_wake.wait(lock, [&] { return save_wanted || spec_quit; });
                quit = spec_quit;
                keys.swap(keys_to_write);
                save_wanted = false;
            }
            save_now(keys);
            if (quit) return;
        }
    }
    void save_now(const std::vector<uint8_t>& keys) {
        if (!keys.empty() && !keys_path.empty())
            if (FILE* f = std::fopen(keys_path.string().c_str(), "ab")) {
                std::fwrite(keys.data(), 1, keys.size(), f);
                std::fclose(f);
            }
        size_t size = 0;
        if (cache_path.empty() || vkGetPipelineCacheData(gpu.device, pipeline_cache, &size, nullptr) != VK_SUCCESS || size == 0) return;
        std::vector<char> data(size);
        if (vkGetPipelineCacheData(gpu.device, pipeline_cache, &size, data.data()) != VK_SUCCESS) return;
        const auto tmp = cache_path.string() + ".tmp";
        if (FILE* f = std::fopen(tmp.c_str(), "wb")) {
            const bool ok = std::fwrite(data.data(), 1, size, f) == size;
            if (std::fclose(f) == 0 && ok) {
                std::error_code ec;
                std::filesystem::rename(tmp, cache_path, ec);
            }
        }
    }

    // The pipeline cache, from disk if there (the driver checks it's its own
    // and ignores it otherwise), the workers and the saver.
    void start_specialization() {
        if (const char* u = std::getenv("PUNCHOUT_UBERSHADER"); u && *u && *u != '0') specialize = Specialize::kOff;
        if (const char* s = std::getenv("PUNCHOUT_SPECIALIZE_SYNC"); s && *s && *s != '0') specialize = Specialize::kSync;
        std::vector<char> data;
        if (specialize != Specialize::kOff) {
            cache_path = host::user_folder() / "pipelines.bin";
            if (FILE* f = std::fopen(cache_path.string().c_str(), "rb")) {
                char buf[65536];
                for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) data.insert(data.end(), buf, buf + n);
                std::fclose(f);
            }
        }
        VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        ci.initialDataSize = data.size();
        ci.pInitialData = data.empty() ? nullptr : data.data();
        if (vkCreatePipelineCache(gpu.device, &ci, nullptr, &pipeline_cache) != VK_SUCCESS) {
            ci.initialDataSize = 0;  // a damaged file: start empty
            ci.pInitialData = nullptr;
            check(vkCreatePipelineCache(gpu.device, &ci, nullptr, &pipeline_cache), "vkCreatePipelineCache");
        }
        if (specialize == Specialize::kAsync) {
            // Leave a core each for the game and the drawing thread; at most 8.
            const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
            const unsigned n = std::clamp(cores > 2 ? cores - 2 : 1u, 1u, 8u);
            for (unsigned i = 0; i < n; ++i) spec_workers.emplace_back([this] { compile_specialized(); });
        }
        if (specialize != Specialize::kOff) saver = std::thread([this] { save_loop(); });
    }
    void stop_specialization() {
        {
            std::lock_guard lock(spec_lock);
            spec_quit = true;
            spec_queue.clear(), spec_urgent.clear();
            keys_to_write.insert(keys_to_write.end(), keys_new.begin(), keys_new.end());
            keys_new.clear();
        }
        spec_wake.notify_all();
        for (std::thread& t : spec_workers) t.join();
        take_done();
        save_wake.notify_all();
        if (saver.joinable()) saver.join();  // writes the cache and the keys a last time
        for (auto& [k, p] : specialized) vkDestroyPipeline(gpu.device, p, nullptr);
        vkDestroyPipelineCache(gpu.device, pipeline_cache, nullptr);
    }
    VkPipeline clear_pipeline(unsigned mask, bool depth) {
        auto& p = clear_pipelines[mask | (depth ? 16u : 0u)];
        if (p) return p;
        VkPipelineColorBlendAttachmentState b{};
        b.colorWriteMask = mask;
        p = make_pipeline(fs_vert, clear_frag, clear_layout, false, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, b, true, true);
        return p;
    }

    VkSampler sampler(const gx::Sampler& s) {
        auto& v = samplers[{s}];
        if (v) return v;
        static constexpr VkSamplerAddressMode kWrap[3] = {VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                                          VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT};
        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.magFilter = s.mag_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        sci.minFilter = s.min_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        sci.mipmapMode = s.mip >= 2 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = kWrap[std::min<unsigned>(s.wrap_s, 2)];
        sci.addressModeV = kWrap[std::min<unsigned>(s.wrap_t, 2)];
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        // The LOD as the hardware's (and Dolphin's) rules: with mipmaps, the
        // game's bias (1/32 steps) and range, a minimum above the maximum
        // taking the maximum; without, level 0 only.
        sci.maxLod = s.mip ? float(s.max_lod) / 16.0f : 0.0f;
        sci.minLod = s.mip ? float(std::min(s.min_lod, s.max_lod)) / 16.0f : 0.0f;
        sci.mipLodBias = s.mip ? float(s.lod_bias) / 32.0f : 0.0f;
        // Anisotropic filtering: the game's own (GX filters up to 4x, a floor
        // at a grazing angle relies on it), or more as an enhancement. Linear
        // filtering throughout while it's on, as Dolphin has it.
        const float wanted = std::max(float(1u << s.max_aniso), anisotropy);
        if (wanted > 1.0f && gpu.max_anisotropy > 1.0f) {
            sci.anisotropyEnable = VK_TRUE;
            sci.maxAnisotropy = std::min(wanted, gpu.max_anisotropy);
            sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
            if (s.mip) sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        }
        check(vkCreateSampler(gpu.device, &sci, nullptr, &v), "vkCreateSampler");
        return v;
    }

    void ensure(Buffer& b, VkDeviceSize size, VkBufferUsageFlags usage) {
        if (b.size >= size) return;
        b.destroy(gpu);
        b = Buffer::host_visible(gpu, std::max<VkDeviceSize>(size + size / 2, 65536), usage);
    }

    // ---- a frame

    void begin_efb(VkCommandBuffer cmd) {
        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = efb.view;
        color.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingAttachmentInfo depth = color;
        depth.imageView = efb_depth.view;
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, {efb.width, efb.height}};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &color;
        ri.pDepthAttachment = &depth;
        vkCmdBeginRendering(cmd, &ri);
    }

    static void barrier(VkCommandBuffer cmd) {  // everything before, before everything after
        VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        d.memoryBarrierCount = 1;
        d.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(cmd, &d);
    }

    void upload(VkCommandBuffer cmd, Slot& s, std::vector<gx::Texture>& list) {
        VkDeviceSize total = 0;
        for (auto& t : list) total += t.rgba.size();
        if (!total) return;
        ensure(s.staging, total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        VkDeviceSize at = 0;
        for (auto& t : list) {
            std::memcpy(static_cast<uint8_t*>(s.staging.mapped) + at, t.rgba.data(), t.rgba.size());
            Image im = obtain(t.width, t.height, t.levels, kUploaded);
            host::transition(cmd, im.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            std::vector<VkBufferImageCopy> regions;
            VkDeviceSize off = at;
            for (uint32_t l = 0, w = t.width, h = t.height; l < t.levels; ++l, w = std::max(1u, w / 2), h = std::max(1u, h / 2)) {
                VkBufferImageCopy r{};
                r.bufferOffset = off;
                r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1};
                r.imageExtent = {w, h, 1};
                regions.push_back(r);
                off += VkDeviceSize(w) * h * 4;
            }
            vkCmdCopyBufferToImage(cmd, s.staging.buffer, im.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(regions.size()), regions.data());
            host::transition(cmd, im.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            at += t.rgba.size();
            auto old = textures.find(t.id);
            if (old != textures.end()) s.garbage.push_back(old->second);
            textures[t.id] = {im, kUploaded, 0};
        }
    }

    VkDescriptorSet allocate(Slot& s, VkDescriptorSetLayout layout) {
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = s.descriptors;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &layout;
        VkDescriptorSet set;
        if (vkAllocateDescriptorSets(gpu.device, &ai, &set) != VK_SUCCESS) rt::fatal(nullptr, "render: out of descriptor sets in a frame");
        return set;
    }

    void copy(VkCommandBuffer cmd, Slot& s, const gx::Copy& c) {
        vkCmdEndRendering(cmd);
        barrier(cmd);
        const uint32_t w = std::max(1u, c.dest_width * scale), h = std::max(1u, c.dest_height * scale);
        Image* target;
        if (c.to_xfb) {
            Image& x = xfbs[c.dest & 0x1FFFFFFFu];
            if (x.width != w || x.height != h) {
                if (x.image) s.garbage.push_back({x, kXfb, 0});
                x = obtain(w, h, 1, kXfb);
            }
            target = &x;
        } else {
            auto old = textures.find(c.texture);
            if (old != textures.end()) s.garbage.push_back(old->second);
            textures[c.texture] = {obtain(w, h, 1, kCopied), kCopied, 0};
            target = &textures[c.texture].image;
        }
        host::transition(cmd, target->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = target->view;
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, {w, h}};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &color;
        vkCmdBeginRendering(cmd, &ri);
        if (!copy_pipeline) {
            VkPipelineColorBlendAttachmentState b{};
            b.colorWriteMask = 0xF;
            copy_pipeline = make_pipeline(fs_vert, copy_frag, copy_layout, false, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, b, false, false);
        }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, copy_pipeline);
        VkDescriptorSet set = allocate(s, copy_set_layout);
        VkDescriptorImageInfo ii = c.from_depth && !c.to_xfb ? VkDescriptorImageInfo{efb_depth_sampler, efb_depth.view, VK_IMAGE_LAYOUT_GENERAL}
                                                             : VkDescriptorImageInfo{efb_sampler, efb.view, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        wr.dstSet = set;
        wr.descriptorCount = 1;
        wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr.pImageInfo = &ii;
        vkUpdateDescriptorSets(gpu.device, 1, &wr, 0, nullptr);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, copy_layout, 0, 1, &set, 0, nullptr);
        const VkViewport vp{0, 0, float(w), float(h), 0, 1};
        const VkRect2D sc{{0, 0}, {w, h}};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);
        FullscreenPush p{};
        p.src[0] = float(c.src[0]) / kEfbWidth;
        p.src[1] = float(c.src[1]) / kEfbHeight;
        p.src[2] = float(c.src[2]) / kEfbWidth;
        p.src[3] = float(c.src[3]) / kEfbHeight;
        p.format = c.to_xfb ? 0xFFFFu : c.format;
        p.flags = (c.half_scale ? 1u : 0u) | (c.from_depth && !c.to_xfb ? 2u : 0u) | (c.efb_alpha ? 4u : 0u);
        p.gamma = c.gamma;
        for (int i = 0; i < 3; ++i) p.filter[i] = c.filter[i];
        // Without the clamp bits the hardware reads past the rectangle, into the EFB around it.
        p.line = 1.0f / float(kEfbHeight);
        p.clamp_top = (float(c.clamp_top ? c.src[1] : 0) + 0.5f) / float(kEfbHeight);
        p.clamp_bottom = (float(c.clamp_bottom ? c.src[1] + c.src[3] : int(kEfbHeight)) - 0.5f) / float(kEfbHeight);
        vkCmdPushConstants(cmd, copy_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof p, &p);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
        host::transition(cmd, target->image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         c.to_xfb ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        if (copy_dumps && !c.to_xfb) {  // PUNCHOUT_PROBE: this copy's texture, as the draws after it sample it
            CopyDump dmp{Buffer::host_visible(gpu, VkDeviceSize(w) * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT), w, h, c.format};
            host::transition(cmd, target->image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            VkBufferImageCopy r{};
            r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            r.imageExtent = {w, h, 1};
            vkCmdCopyImageToBuffer(cmd, target->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dmp.buf.buffer, 1, &r);
            host::transition(cmd, target->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            copy_dumps->push_back(std::move(dmp));
        }
        barrier(cmd);
        begin_efb(cmd);
        if (c.clear && (c.clear_color || c.clear_alpha || c.clear_depth)) {
            const unsigned mask = (c.clear_color ? 7u : 0u) | (c.clear_alpha ? 8u : 0u);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, clear_pipeline(mask, c.clear_depth));
            vkCmdSetCullMode(cmd, VK_CULL_MODE_NONE);
            vkCmdSetDepthTestEnable(cmd, c.clear_depth);
            vkCmdSetDepthWriteEnable(cmd, c.clear_depth);
            vkCmdSetDepthCompareOp(cmd, VK_COMPARE_OP_ALWAYS);
            const VkViewport cv{float(c.src[0] * int(scale)), float(c.src[1] * int(scale)), float(c.src[2] * int(scale)),
                                float(c.src[3] * int(scale)), 0, 1};
            VkRect2D cs{{c.src[0] * int(scale), c.src[1] * int(scale)}, {uint32_t(c.src[2]) * scale, uint32_t(c.src[3]) * scale}};
            clamp_rect(cs);
            vkCmdSetViewport(cmd, 0, 1, &cv);
            vkCmdSetScissor(cmd, 0, 1, &cs);
            FullscreenPush q{};
            for (int i = 0; i < 4; ++i) q.color[i] = float(c.clear_rgba[i]) / 255.0f;
            q.depth = float(c.clear_z) / 16777216.0f;
            vkCmdPushConstants(cmd, clear_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof q, &q);
            vkCmdDraw(cmd, 3, 1, 0, 0);
        }
    }

    void clamp_rect(VkRect2D& r) const {
        const int32_t W = int32_t(efb.width), H = int32_t(efb.height);
        int32_t x0 = std::clamp(r.offset.x, 0, W), y0 = std::clamp(r.offset.y, 0, H);
        int32_t x1 = std::clamp(r.offset.x + int32_t(r.extent.width), 0, W), y1 = std::clamp(r.offset.y + int32_t(r.extent.height), 0, H);
        r = {{x0, y0}, {uint32_t(std::max(0, x1 - x0)), uint32_t(std::max(0, y1 - y0))}};
    }

    // PUNCHOUT_PROBE's log: every packet that changed the pixel (slot i+1 is
    // after packet i), with the draw's blend, depth, TEV and texture state.
    void report_probe(const gx::Frame& f, const uint8_t* data, size_t count) {
        const auto at = [&](size_t i) { return data + i * 8; };
        for (size_t i = 0; i < f.copies.size(); ++i) {
            const gx::Copy& c = f.copies[i];
            rt::log("copy %llu: %s src %d,%d %dx%d -> %ux%u format %X%s gamma %.1f filter %u/%u/%u%s%s%s%s texture %d", static_cast<unsigned long long>(i),
                    c.to_xfb ? "XFB" : "texture", c.src[0], c.src[1], c.src[2], c.src[3], c.dest_width, c.dest_height, c.format,
                    c.half_scale ? " half" : "", c.gamma, c.filter[0], c.filter[1], c.filter[2], c.from_depth ? " depth" : "",
                    c.efb_alpha ? " alpha" : "", c.clamp_top ? " clamp-top" : "", c.clamp_bottom ? " clamp-bottom" : "", c.texture);
        }
        for (size_t i = 0; i + 1 <= count && i < f.packets.size(); ++i) {
            const uint8_t *a = at(i), *b = at(i + 1);
            if (std::memcmp(a, b, 8) == 0) continue;
            float za, zb;
            std::memcpy(&za, a + 4, 4), std::memcpy(&zb, b + 4, 4);
            const gx::Packet& pk = f.packets[i];
            if (pk.kind == gx::Packet::kCopy) {
                const gx::Copy& c = f.copies[pk.index];
                rt::log("probe #%llu copy%s: rgba %u,%u,%u,%u -> %u,%u,%u,%u  z %.7f -> %.7f", static_cast<unsigned long long>(i), c.clear ? " (clears)" : "",
                        a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3], za, zb);
                continue;
            }
            const gx::Draw& d = f.draws[pk.index];
            const auto& bp = f.states[d.state].bp;
            const auto reg = [&](unsigned r) { return bp[r / 4][r % 4]; };
            const unsigned stages = ((reg(0x00) >> 10) & 15) + 1;
            rt::log("probe #%llu draw %u (primitive %u): rgba %u,%u,%u,%u -> %u,%u,%u,%u  z %.7f -> %.7f", static_cast<unsigned long long>(i), pk.index, d.primitive, a[0], a[1], a[2], a[3], b[0], b[1],
                    b[2], b[3], za, zb);
            rt::log("    blend %u src %u dst %u op %u mask %X logic %u | z test %u write %u func %u | %u vertices, cull %u, has %X",
                    d.pipeline.blend, d.pipeline.src, d.pipeline.dst, d.pipeline.op, d.pipeline.write_mask, d.pipeline.logic,
                    d.depth_test, d.depth_write, d.depth_func, d.index_count, d.cull, d.has);
            rt::log("    genmode %06X zmode(40) %06X blend(41) %06X dstalpha(42) %06X pe(43) %06X alphacmp(F3) %06X fog(EE-F2) %06X %06X",
                    reg(0x00), reg(0x40), reg(0x41), reg(0x42), reg(0x43), reg(0xF3), reg(0xEE), reg(0xF2));
            const auto& st = f.states[d.state];
            for (int k = 0; k < 4; ++k)
                rt::log("    konst %d: %d,%d,%d,%d  reg %d: %d,%d,%d,%d", k, st.konst[k][0], st.konst[k][1], st.konst[k][2],
                        st.konst[k][3], k, st.tev_regs[k][0], st.tev_regs[k][1], st.tev_regs[k][2], st.tev_regs[k][3]);
            rt::log("    channels %u: color0 %06X alpha0 %06X color1 %06X alpha1 %06X  amb0 %d,%d,%d,%d mat0 %d,%d,%d,%d",
                    st.xf[0][0], st.xf[0][1], st.xf[0][3], st.xf[0][2], st.xf[1][0], st.materials[0][0], st.materials[0][1],
                    st.materials[0][2], st.materials[0][3], st.materials[2][0], st.materials[2][1], st.materials[2][2], st.materials[2][3]);
            for (int l = 0; l < 8; ++l) {
                const auto& L = st.lights[l];
                rt::log("    light %d: color %g,%g,%g,%g cosatt %g,%g,%g distatt %g,%g,%g pos %g,%g,%g dir %g,%g,%g", l, L.color[0],
                        L.color[1], L.color[2], L.color[3], L.cosatt[0], L.cosatt[1], L.cosatt[2], L.distatt[0], L.distatt[1],
                        L.distatt[2], L.pos[0], L.pos[1], L.pos[2], L.dir[0], L.dir[1], L.dir[2]);
            }
            for (unsigned t = 0; t < stages; ++t)
                rt::log("    stage %u: order %06X color %06X alpha %06X ind %06X ksel %06X", t,
                        (reg(0x28 + t / 2) >> (t % 2 ? 12 : 0)) & 0xFFF, reg(0xC0 + 2 * t), reg(0xC1 + 2 * t), reg(0x10 + t),
                        reg(0xF6 + t / 2));
            for (int m = 0; m < 8; ++m)
                if (d.textures[m] >= 0)
                    rt::log("    texmap %d: id %d, image0 %06X mode0 %06X mode1 %06X", m, d.textures[m], reg((m < 4 ? 0x88 : 0xA8) + m % 4),
                            reg((m < 4 ? 0x80 : 0xA0) + m % 4), reg((m < 4 ? 0x84 : 0xA4) + m % 4));
        }
    }

    void render(gx::Frame& f) {
        collect_specialized();
        slot = (slot + 1) % 2;
        Slot& s = slots[slot];
        const uint64_t w0 = gx::now_ns();  // the GPU finishing the frame before last
        check(vkWaitForFences(gpu.device, 1, &s.fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
        gx::recorder_stats().gpu_wait_ns.fetch_add(gx::now_ns() - w0, std::memory_order_relaxed);
        if (timed[slot]) {  // the frame this slot drew last: done (its fence passed)
            uint64_t t[2];
            if (vkGetQueryPoolResults(gpu.device, timestamps, slot * 2, 2, sizeof t, t, sizeof t[0], VK_QUERY_RESULT_64_BIT) ==
                VK_SUCCESS) {
                gx::recorder_stats().gpu_ns.fetch_add(static_cast<uint64_t>(double((t[1] - t[0]) & tick_mask) * ns_per_tick),
                                                      std::memory_order_relaxed);
                gx::recorder_stats().gpu_timed.fetch_add(1, std::memory_order_relaxed);
            }
            timed[slot] = false;
        }
        recycle(s);
        check(vkResetDescriptorPool(gpu.device, s.descriptors, 0), "vkResetDescriptorPool");
        // One buffer: the full vertices, the compact ones, then zeros for the
        // attributes the compact layout lacks.
        const size_t full_bytes = f.vertices.size() * sizeof(gx::Vertex), small_bytes = f.small.size() * sizeof(gx::SmallVertex);
        small_at = full_bytes;
        zeros_at = full_bytes + small_bytes;
        ensure(s.vertices, zeros_at + 64, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        ensure(s.indices, std::max<size_t>(1, f.indices.size()) * 4, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
        ensure(s.states, std::max<size_t>(1, f.states.size()) * sizeof(gx::State), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        std::memcpy(s.vertices.mapped, f.vertices.data(), full_bytes);
        std::memcpy(static_cast<uint8_t*>(s.vertices.mapped) + small_at, f.small.data(), small_bytes);
        std::memset(static_cast<uint8_t*>(s.vertices.mapped) + zeros_at, 0, 64);
        std::memcpy(s.indices.mapped, f.indices.data(), f.indices.size() * 4);
        std::memcpy(s.states.mapped, f.states.data(), f.states.size() * sizeof(gx::State));

        check(vkResetFences(gpu.device, 1, &s.fence), "vkResetFences");
        check(vkResetCommandBuffer(s.cmd, 0), "vkResetCommandBuffer");
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VkCommandBuffer cmd = s.cmd;
        check(vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer");
        if (timestamps) {
            vkCmdResetQueryPool(cmd, timestamps, slot * 2, 2);
            vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, timestamps, slot * 2);
        }
        upload(cmd, s, f.textures);
        barrier(cmd);

        VkDescriptorSet states_set = allocate(s, states_layout);
        VkDescriptorBufferInfo bufi{s.states.buffer, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        wr.dstSet = states_set;
        wr.descriptorCount = 1;
        wr.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        wr.pBufferInfo = &bufi;
        vkUpdateDescriptorSets(gpu.device, 1, &wr, 0, nullptr);

        begin_efb(cmd);
        VkPipeline bound = VK_NULL_HANDLE;
        gx::PipelineKey blend_key{};  // the blend state last set (dynamic blend)
        bool blend_set = false;
        unused_sampler = sampler(gx::Sampler{});  // (samplers are remade when the anisotropy changes)
        std::unordered_map<TextureSetKey, VkDescriptorSet, TextureSetHash> texture_sets;
        VkDescriptorSet bound_textures = VK_NULL_HANDLE;
        // What the last draw set, so a draw that changes nothing records
        // nothing (most draws share their neighbours' textures and state).
        // Copies bind pipelines of their own: everything is set again after one.
        bool same_valid = false;
        int32_t last_textures[8];
        gx::Sampler last_samplers[8];
        VkViewport last_viewport{};
        VkRect2D last_scissor{};
        uint8_t last_cull = 0, last_depth_test = 0, last_depth_write = 0, last_depth_func = 0;
        // The topology, dynamic within its class: strips and lists share pipelines.
        VkPrimitiveTopology last_topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        bool topology_valid = false;
        bool need_bind = true;
        uint8_t bound_small = 0;
        // PUNCHOUT_PROBE=x,y (EFB pixels, at 1x): for the frame PUNCHOUT_DFF
        // captured, read that pixel's colour and depth after every packet and
        // log the packets that change it, with their state. For finding which
        // draw makes a pixel differ from a reference render.
        static const int probe_x = [] { const char* e = std::getenv("PUNCHOUT_PROBE"); return e ? std::atoi(e) : -1; }();
        static const int probe_y = [] { const char* e = std::getenv("PUNCHOUT_PROBE"); const char* c = e ? std::strchr(e, ',') : nullptr; return c ? std::atoi(c + 1) : -1; }();
        const bool probing = f.captured && probe_x >= 0 && probe_y >= 0;
        // PUNCHOUT_PROBE_SOLO=<draw>: that draw of the captured frame unblended,
        // writing RGBA, so the probe shows what its pixel shader gives.
        static const int solo = [] { const char* e = std::getenv("PUNCHOUT_PROBE_SOLO"); return e ? std::atoi(e) : -1; }();
        if (probing && solo >= 0 && size_t(solo) < f.draws.size()) {
            f.draws[size_t(solo)].pipeline.blend = 0;
            f.draws[size_t(solo)].pipeline.write_mask = 0xF;
            f.draws[size_t(solo)].pipeline.logic = 0;
            // PUNCHOUT_PROBE_SOLO_MODE: z = depth test ALWAYS (the last triangle on the pixel wins);
            // c adds culling off.
            if (const char* m = std::getenv("PUNCHOUT_PROBE_SOLO_MODE")) {
                if (std::strchr(m, 'z')) f.draws[size_t(solo)].depth_func = VK_COMPARE_OP_ALWAYS;
                if (std::strchr(m, 'c')) f.draws[size_t(solo)].cull = VK_CULL_MODE_NONE;
            }
        }
        Buffer probe_buf;
        if (probing) probe_buf = Buffer::host_visible(gpu, VkDeviceSize(f.packets.size() + 1) * 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        size_t probe_i = 0;
        const auto probe = [&](size_t at_slot) {
            vkCmdEndRendering(cmd);
            barrier(cmd);
            VkBufferImageCopy r{};
            r.bufferOffset = VkDeviceSize(at_slot) * 8;
            r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            r.imageOffset = {probe_x * int32_t(scale), probe_y * int32_t(scale), 0};
            r.imageExtent = {1, 1, 1};
            vkCmdCopyImageToBuffer(cmd, efb.image, VK_IMAGE_LAYOUT_GENERAL, probe_buf.buffer, 1, &r);
            r.bufferOffset += 4;
            r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            vkCmdCopyImageToBuffer(cmd, efb_depth.image, VK_IMAGE_LAYOUT_GENERAL, probe_buf.buffer, 1, &r);
            barrier(cmd);
            begin_efb(cmd);
        };
        std::vector<CopyDump> dumps;
        if (probing) probe(probe_i), copy_dumps = &dumps;
        for (const gx::Packet& pk : f.packets) {
            if (probing && probe_i > 0) probe(probe_i);
            if (probing) ++probe_i;
            if (pk.kind == gx::Packet::kCopy) {
                copy(cmd, s, f.copies[pk.index]);
                bound = VK_NULL_HANDLE;
                blend_set = false;
                same_valid = false;
                topology_valid = false;
                bound_textures = VK_NULL_HANDLE;
                need_bind = true;
                continue;
            }
            const gx::Draw& d = f.draws[pk.index];
            if (d.viewport[2] <= 0.0f || d.viewport[3] <= 0.0f || d.scissor[2] <= 0 || d.scissor[3] <= 0) continue;
            VkPipeline p = gx_pipeline(d.pipeline, d.shape);
            if (p != bound) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
                bound = p;
            }
            if (gpu.dynamic_blend && (!blend_set || std::memcmp(&d.pipeline, &blend_key, sizeof blend_key) != 0)) {
                set_dynamic_blend(cmd, d.pipeline);  // kept across GX pipelines; copies and clears reset it below
                blend_key = d.pipeline, blend_set = true;
            }
            if (need_bind || d.pipeline.small_vertices != bound_small) {
                const VkBuffer buffers[2] = {s.vertices.buffer, s.vertices.buffer};
                const VkDeviceSize offsets[2] = {d.pipeline.small_vertices ? small_at : 0, zeros_at};
                vkCmdBindVertexBuffers(cmd, 0, 2, buffers, offsets);
                bound_small = d.pipeline.small_vertices;
            }
            if (need_bind) {
                vkCmdBindIndexBuffer(cmd, s.indices.buffer, 0, VK_INDEX_TYPE_UINT32);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gx_layout, 0, 1, &states_set, 0, nullptr);
                need_bind = false;
            }
            // The draw's textures, as a descriptor set (shared by draws with the
            // same ones). Unused slots get the white texture and one sampler, so
            // they don't split sets.
            if (!same_valid || std::memcmp(d.textures, last_textures, sizeof last_textures) != 0 ||
                std::memcmp(d.samplers, last_samplers, sizeof last_samplers) != 0) {
                TextureSetKey key;
                VkDescriptorImageInfo infos[8];
                for (int m = 0; m < 8; ++m) {
                    VkImageView view = white.view;
                    VkSampler smp = unused_sampler;
                    if (d.textures[m] >= 0) {
                        if (auto it = textures.find(d.textures[m]); it != textures.end()) view = it->second.image.view;
                        smp = sampler(d.samplers[m]);
                    }
                    infos[m] = {smp, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                    key.v[2 * m] = reinterpret_cast<uint64_t>(view);
                    key.v[2 * m + 1] = reinterpret_cast<uint64_t>(smp);
                }
                VkDescriptorSet& ts = texture_sets[key];
                if (!ts) {
                    ts = allocate(s, textures_layout);
                    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                    w.dstSet = ts;
                    w.descriptorCount = 8;
                    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                    w.pImageInfo = infos;
                    vkUpdateDescriptorSets(gpu.device, 1, &w, 0, nullptr);
                }
                if (ts != bound_textures) {
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gx_layout, 1, 1, &ts, 0, nullptr);
                    bound_textures = ts;
                }
                std::memcpy(last_textures, d.textures, sizeof last_textures);
                std::memcpy(last_samplers, d.samplers, sizeof last_samplers);
            }
            const float sc = float(scale);
            VkViewport vp{d.viewport[0] * sc, d.viewport[1] * sc, d.viewport[2] * sc, d.viewport[3] * sc,
                          std::clamp(d.viewport[4], 0.0f, 1.0f), std::clamp(d.viewport[5], 0.0f, 1.0f)};
            VkRect2D r{{d.scissor[0] * int(scale), d.scissor[1] * int(scale)}, {uint32_t(d.scissor[2]) * scale, uint32_t(d.scissor[3]) * scale}};
            clamp_rect(r);
            if (r.extent.width == 0 || r.extent.height == 0) continue;
            if (!same_valid || std::memcmp(&vp, &last_viewport, sizeof vp) != 0) vkCmdSetViewport(cmd, 0, 1, &vp), last_viewport = vp;
            if (!same_valid || std::memcmp(&r, &last_scissor, sizeof r) != 0) vkCmdSetScissor(cmd, 0, 1, &r), last_scissor = r;
            if (!same_valid || d.cull != last_cull) vkCmdSetCullMode(cmd, VkCullModeFlags(d.cull)), last_cull = d.cull;
            if (!same_valid || d.depth_test != last_depth_test) vkCmdSetDepthTestEnable(cmd, d.depth_test), last_depth_test = d.depth_test;
            if (!same_valid || d.depth_write != last_depth_write) vkCmdSetDepthWriteEnable(cmd, d.depth_write), last_depth_write = d.depth_write;
            if (!same_valid || d.depth_func != last_depth_func)
                vkCmdSetDepthCompareOp(cmd, VkCompareOp(d.depth_func)), last_depth_func = d.depth_func;
            // Lines and points: GX's widths are in sixths of an EFB pixel.
            float point = 1.0f;
            if (d.pipeline.topology == 1) {
                const float width = float(d.line_width) / 6.0f * sc;
                if (gpu.wide_lines) {
                    // Always: only line pipelines have it dynamic, so another
                    // pipeline in between forgets it (lines are rare).
                    vkCmdSetLineWidth(cmd, std::clamp(width, 1.0f, gpu.max_line_width));
                } else if (d.line_width != 6) {
                    static bool said = false;
                    approximated(said, "lines are one pixel wide: this GPU has no wide lines");
                }
            } else if (d.pipeline.topology == 2) {
                point = std::clamp(float(d.point_size) / 6.0f * sc, 1.0f, gpu.max_point_size);
                if (!gpu.large_points && d.point_size != 6) {
                    static bool said = false;
                    approximated(said, "points are one pixel: this GPU has no large points");
                }
            }
            Push push{d.state, d.has, 0u, sc, point};
            const auto reads_src_alpha = [](uint8_t factor) { return factor == VK_BLEND_FACTOR_SRC_ALPHA || factor == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; };
            if (d.pipeline.blend && (reads_src_alpha(d.pipeline.src) || reads_src_alpha(d.pipeline.dst))) push.flags |= 1u;
            if (d.pipeline.logic && !gpu.logic_op) push.flags |= kLogicBlend[d.pipeline.logic - 1u].help << 4;
            vkCmdPushConstants(cmd, gx_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof push, &push);
            if (d.direct) {
                // Lists and strips need no indices: the topology says which.
                static constexpr VkPrimitiveTopology kList[3] = {VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
                                                                 VK_PRIMITIVE_TOPOLOGY_POINT_LIST};
                static constexpr VkPrimitiveTopology kStrip[3] = {VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, VK_PRIMITIVE_TOPOLOGY_LINE_STRIP,
                                                                  VK_PRIMITIVE_TOPOLOGY_POINT_LIST};
                const VkPrimitiveTopology t = (d.direct == 2 ? kStrip : kList)[d.pipeline.topology];
                if (!topology_valid || t != last_topology) vkCmdSetPrimitiveTopology(cmd, t), last_topology = t, topology_valid = true;
                vkCmdDraw(cmd, d.index_count, 1, d.first_index, 0);
            } else {
                const VkPrimitiveTopology t = kTopologyLists[d.pipeline.topology];
                if (!topology_valid || t != last_topology) vkCmdSetPrimitiveTopology(cmd, t), last_topology = t, topology_valid = true;
                vkCmdDrawIndexed(cmd, d.index_count, 1, d.first_index, 0, 0);
            }
            same_valid = true;
        }
        if (probing) probe(probe_i);
        vkCmdEndRendering(cmd);
        if (timestamps) {
            vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, timestamps, slot * 2 + 1);
            timed[slot] = true;
        }
        check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");
        submit(s);
        if (probing) {
            vkDeviceWaitIdle(gpu.device);
            report_probe(f, static_cast<const uint8_t*>(probe_buf.mapped), probe_i);
            probe_buf.destroy(gpu);
            copy_dumps = nullptr;
            // The texture copies as PAM files (RGBA), numbered in order as Dolphin's DumpEFBTarget numbers its.
            const char* dir = std::getenv("PUNCHOUT_PROBE_DIR");
            for (size_t i = 0; i < dumps.size(); ++i) {
                CopyDump& dmp = dumps[i];
                if (dir) {
                    char name[512];
                    std::snprintf(name, sizeof name, "%s/copy_n%06u_%ux%u_%u.pam", dir, unsigned(i), dmp.w, dmp.h, dmp.format & 0xFF);
                    if (FILE* fp = std::fopen(name, "wb")) {
                        std::fprintf(fp, "P7\nWIDTH %u\nHEIGHT %u\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n", dmp.w, dmp.h);
                        std::fwrite(dmp.buf.mapped, 1, size_t(dmp.w) * dmp.h * 4, fp);
                        std::fclose(fp);
                    }
                }
                dmp.buf.destroy(gpu);
            }
            rt::log("probe: %llu texture copies in the frame%s", static_cast<unsigned long long>(dumps.size()), dir ? " (saved)" : "");
        }
        // What the frame released, after drawing it: a texture can be made and
        // released in the same frame (two copies to one address), and draws
        // before the release still use it. Spare once this frame's fence passes.
        for (int32_t id : f.released) {
            auto it = textures.find(id);
            if (it == textures.end()) rt::fatal(nullptr, "render: texture %d released but never made", id);
            s.garbage.push_back(it->second);
            textures.erase(it);
        }
        gx::recorder_stats().gpu_textures.store(textures.size(), std::memory_order_relaxed);
        gx::recorder_stats().gpu_spares.store(spares.size(), std::memory_order_relaxed);
        ++frames;
    }

    void destroy() {
        vkDeviceWaitIdle(gpu.device);
        for (Slot& s : slots) {
            for (Spare& g : s.garbage) g.image.destroy(gpu);
            s.vertices.destroy(gpu);
            s.indices.destroy(gpu);
            s.states.destroy(gpu);
            s.staging.destroy(gpu);
            vkDestroyDescriptorPool(gpu.device, s.descriptors, nullptr);
            vkDestroyFence(gpu.device, s.fence, nullptr);
            vkDestroyCommandPool(gpu.device, s.pool, nullptr);
        }
        for (auto& [k, t] : textures) t.image.destroy(gpu);
        if (efb_depth_sampler) vkDestroySampler(gpu.device, efb_depth_sampler, nullptr);
        for (Spare& sp : spares) sp.image.destroy(gpu);
        for (auto& [k, x] : xfbs) x.destroy(gpu);
        stop_specialization();
        for (auto& [k, p] : pipelines) vkDestroyPipeline(gpu.device, p, nullptr);
        for (auto& [k, p] : clear_pipelines) vkDestroyPipeline(gpu.device, p, nullptr);
        if (copy_pipeline) vkDestroyPipeline(gpu.device, copy_pipeline, nullptr);
        for (auto& [k, v] : samplers) vkDestroySampler(gpu.device, v, nullptr);
        vkDestroySampler(gpu.device, efb_sampler, nullptr);
        for (VkPipelineLayout l : {gx_layout, clear_layout, copy_layout}) vkDestroyPipelineLayout(gpu.device, l, nullptr);
        for (VkDescriptorSetLayout l : {states_layout, textures_layout, copy_set_layout}) vkDestroyDescriptorSetLayout(gpu.device, l, nullptr);
        for (VkShaderModule m : {gx_vert, gx_frag, gx_frag_depth, fs_vert, clear_frag, copy_frag}) vkDestroyShaderModule(gpu.device, m, nullptr);
        for (VkShaderModule m : gx_frag_stages) vkDestroyShaderModule(gpu.device, m, nullptr);
        if (timestamps) vkDestroyQueryPool(gpu.device, timestamps, nullptr);
        efb.destroy(gpu);
        efb_depth.destroy(gpu);
        white.destroy(gpu);
    }
};

void Renderer::set_specialization(bool on) { impl_->specialize_on = on; }
Renderer::Renderer(host::Gpu& gpu, unsigned scale, unsigned anisotropy) : impl_(std::make_unique<Impl>(gpu, scale)) {
    impl_->create();
    impl_->set_anisotropy(anisotropy);
}
void Renderer::set_anisotropy(unsigned level) { impl_->set_anisotropy(level); }
void Renderer::save_now() {
    std::vector<uint8_t> keys;
    {
        std::lock_guard lock(impl_->spec_lock);
        keys.swap(impl_->keys_to_write);
    }
    impl_->save_now(keys);
}
Renderer::~Renderer() { impl_->destroy(); }
void Renderer::render(gx::Frame& frame) {
    const uint64_t t0 = gx::now_ns();
    impl_->render(frame);
    gx::RecorderStats& st = gx::recorder_stats();
    st.render_ns.fetch_add(gx::now_ns() - t0, std::memory_order_relaxed);
    st.rendered.fetch_add(1, std::memory_order_relaxed);
}
const host::Image* Renderer::xfb(uint32_t address) const {
    const auto it = impl_->xfbs.find(address & 0x1FFFFFFFu);
    return it == impl_->xfbs.end() ? nullptr : &it->second;
}

}  // namespace punchout
