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

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <unordered_map>
#include <vector>

#include "qilin/rt/system.hpp"
#include "shaders.inc"

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
};
struct FullscreenPush {
    float src[4];
    float color[4];
    float depth;
    uint32_t format, flags;
    float gamma;
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
constexpr VkImageUsageFlags kCopied = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
constexpr VkImageUsageFlags kXfb = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
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
    VkFormat depth_format = VK_FORMAT_D32_SFLOAT;
    Image efb, efb_depth, white;
    Slot slots[2];
    unsigned slot = 0;
    uint64_t frames = 0;

    VkShaderModule gx_vert, gx_frag, fs_vert, clear_frag, copy_frag;
    VkDescriptorSetLayout states_layout, textures_layout, copy_set_layout;
    VkPipelineLayout gx_layout, clear_layout, copy_layout;
    std::map<PipelineKey, VkPipeline> pipelines;
    std::map<unsigned, VkPipeline> clear_pipelines;  // colour mask | depth write << 4
    VkPipeline copy_pipeline = VK_NULL_HANDLE;
    std::map<SamplerKey, VkSampler> samplers;
    VkSampler efb_sampler = VK_NULL_HANDLE;

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
        while (spares.size() > kMaxSpares) {  // the oldest first
            auto oldest = std::min_element(spares.begin(), spares.end(), [](const Spare& a, const Spare& b) { return a.since < b.since; });
            oldest->image.destroy(gpu);
            *oldest = spares.back();
            spares.pop_back();
        }
    }

    explicit Impl(host::Gpu& g, unsigned s) : gpu(g), scale(s) {}

    // ---- setup

    void create() {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(gpu.physical, VK_FORMAT_D32_SFLOAT, &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) depth_format = VK_FORMAT_X8_D24_UNORM_PACK32;
        efb = Image::create(gpu, kEfbWidth * scale, kEfbHeight * scale, kColor,
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        efb_depth = Image::create(gpu, kEfbWidth * scale, kEfbHeight * scale, depth_format,
                                  VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        white = Image::create(gpu, 1, 1, kColor, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
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
                             const VkPipelineColorBlendAttachmentState& blend, bool depth, bool depth_dynamic) {
        const VkPipelineShaderStageCreateInfo stages[2] = {
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", nullptr},
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", nullptr}};
        VkVertexInputBindingDescription bind{0, sizeof(gx::Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
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
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        if (vertices) {
            vi.vertexBindingDescriptionCount = 1;
            vi.pVertexBindingDescriptions = &bind;
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
        std::vector<VkDynamicState> dyn = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        if (depth_dynamic) {
            dyn.insert(dyn.end(), {VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
                                   VK_DYNAMIC_STATE_DEPTH_COMPARE_OP});
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
        check(vkCreateGraphicsPipelines(gpu.device, VK_NULL_HANDLE, 1, &pci, nullptr, &p), "vkCreateGraphicsPipelines");
        return p;
    }

    VkPipeline gx_pipeline(const gx::PipelineKey& k) {
        auto& p = pipelines[{k}];
        if (p) return p;
        VkPipelineColorBlendAttachmentState b{};
        b.blendEnable = k.blend;
        b.srcColorBlendFactor = VkBlendFactor(k.src);
        b.dstColorBlendFactor = VkBlendFactor(k.dst);
        b.colorBlendOp = VkBlendOp(k.op);
        b.srcAlphaBlendFactor = VkBlendFactor(k.src_alpha);
        b.dstAlphaBlendFactor = VkBlendFactor(k.dst_alpha);
        b.alphaBlendOp = VkBlendOp(k.op_alpha);
        b.colorWriteMask = k.write_mask;
        static constexpr VkPrimitiveTopology kTopology[3] = {VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
                                                              VK_PRIMITIVE_TOPOLOGY_POINT_LIST};
        p = make_pipeline(gx_vert, gx_frag, gx_layout, true, kTopology[k.topology], b, true, true);
        return p;
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
        sci.minLod = s.mip ? float(s.min_lod) / 16.0f : 0.0f;
        sci.maxLod = s.mip ? float(s.max_lod) / 16.0f : 0.0f;
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
        VkDescriptorImageInfo ii{efb_sampler, efb.view, VK_IMAGE_LAYOUT_GENERAL};
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
        p.flags = c.half_scale ? 1u : 0u;
        p.gamma = c.gamma;
        vkCmdPushConstants(cmd, copy_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof p, &p);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
        host::transition(cmd, target->image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         c.to_xfb ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
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

    void render(gx::Frame& f) {
        slot = (slot + 1) % 2;
        Slot& s = slots[slot];
        const uint64_t w0 = gx::now_ns();  // the GPU finishing the frame before last
        check(vkWaitForFences(gpu.device, 1, &s.fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
        gx::recorder_stats().gpu_wait_ns.fetch_add(gx::now_ns() - w0, std::memory_order_relaxed);
        recycle(s);
        check(vkResetDescriptorPool(gpu.device, s.descriptors, 0), "vkResetDescriptorPool");
        ensure(s.vertices, std::max<size_t>(1, f.vertices.size()) * sizeof(gx::Vertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        ensure(s.indices, std::max<size_t>(1, f.indices.size()) * 4, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
        ensure(s.states, std::max<size_t>(1, f.states.size()) * sizeof(gx::State), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        std::memcpy(s.vertices.mapped, f.vertices.data(), f.vertices.size() * sizeof(gx::Vertex));
        std::memcpy(s.indices.mapped, f.indices.data(), f.indices.size() * 4);
        std::memcpy(s.states.mapped, f.states.data(), f.states.size() * sizeof(gx::State));

        check(vkResetFences(gpu.device, 1, &s.fence), "vkResetFences");
        check(vkResetCommandBuffer(s.cmd, 0), "vkResetCommandBuffer");
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VkCommandBuffer cmd = s.cmd;
        check(vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer");
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
        VkDeviceSize zero = 0;
        VkPipeline bound = VK_NULL_HANDLE;
        std::map<std::vector<uint64_t>, VkDescriptorSet> texture_sets;
        VkDescriptorSet bound_textures = VK_NULL_HANDLE;
        bool need_bind = true;
        for (const gx::Packet& pk : f.packets) {
            if (pk.kind == gx::Packet::kCopy) {
                copy(cmd, s, f.copies[pk.index]);
                bound = VK_NULL_HANDLE;
                bound_textures = VK_NULL_HANDLE;
                need_bind = true;
                continue;
            }
            const gx::Draw& d = f.draws[pk.index];
            if (d.viewport[2] <= 0.0f || d.viewport[3] <= 0.0f || d.scissor[2] <= 0 || d.scissor[3] <= 0) continue;
            VkPipeline p = gx_pipeline(d.pipeline);
            if (p != bound) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
                bound = p;
            }
            if (need_bind) {
                vkCmdBindVertexBuffers(cmd, 0, 1, &s.vertices.buffer, &zero);
                vkCmdBindIndexBuffer(cmd, s.indices.buffer, 0, VK_INDEX_TYPE_UINT32);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gx_layout, 0, 1, &states_set, 0, nullptr);
                need_bind = false;
            }
            // The draw's textures, as a descriptor set (shared by draws with the same ones).
            std::vector<uint64_t> key;
            VkDescriptorImageInfo infos[8];
            for (int m = 0; m < 8; ++m) {
                auto it = d.textures[m] >= 0 ? textures.find(d.textures[m]) : textures.end();
                const VkImageView view = it != textures.end() ? it->second.image.view : white.view;
                const VkSampler smp = sampler(d.samplers[m]);
                infos[m] = {smp, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                key.push_back(reinterpret_cast<uint64_t>(view));
                key.push_back(reinterpret_cast<uint64_t>(smp));
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
            const float sc = float(scale);
            VkViewport vp{d.viewport[0] * sc, d.viewport[1] * sc, d.viewport[2] * sc, d.viewport[3] * sc,
                          std::clamp(d.viewport[4], 0.0f, 1.0f), std::clamp(d.viewport[5], 0.0f, 1.0f)};
            vkCmdSetViewport(cmd, 0, 1, &vp);
            VkRect2D r{{d.scissor[0] * int(scale), d.scissor[1] * int(scale)}, {uint32_t(d.scissor[2]) * scale, uint32_t(d.scissor[3]) * scale}};
            clamp_rect(r);
            if (r.extent.width == 0 || r.extent.height == 0) continue;
            vkCmdSetScissor(cmd, 0, 1, &r);
            vkCmdSetCullMode(cmd, VkCullModeFlags(d.cull));
            vkCmdSetDepthTestEnable(cmd, d.depth_test);
            vkCmdSetDepthWriteEnable(cmd, d.depth_write);
            vkCmdSetDepthCompareOp(cmd, VkCompareOp(d.depth_func));
            Push push{d.state, d.has, 0u, sc};
            const auto reads_src_alpha = [](uint8_t factor) { return factor == VK_BLEND_FACTOR_SRC_ALPHA || factor == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; };
            if (d.pipeline.blend && (reads_src_alpha(d.pipeline.src) || reads_src_alpha(d.pipeline.dst))) push.flags |= 1u;
            vkCmdPushConstants(cmd, gx_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof push, &push);
            vkCmdDrawIndexed(cmd, d.index_count, 1, d.first_index, 0, 0);
        }
        vkCmdEndRendering(cmd);
        check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");
        submit(s);
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
        for (Spare& sp : spares) sp.image.destroy(gpu);
        for (auto& [k, x] : xfbs) x.destroy(gpu);
        for (auto& [k, p] : pipelines) vkDestroyPipeline(gpu.device, p, nullptr);
        for (auto& [k, p] : clear_pipelines) vkDestroyPipeline(gpu.device, p, nullptr);
        if (copy_pipeline) vkDestroyPipeline(gpu.device, copy_pipeline, nullptr);
        for (auto& [k, v] : samplers) vkDestroySampler(gpu.device, v, nullptr);
        vkDestroySampler(gpu.device, efb_sampler, nullptr);
        for (VkPipelineLayout l : {gx_layout, clear_layout, copy_layout}) vkDestroyPipelineLayout(gpu.device, l, nullptr);
        for (VkDescriptorSetLayout l : {states_layout, textures_layout, copy_set_layout}) vkDestroyDescriptorSetLayout(gpu.device, l, nullptr);
        for (VkShaderModule m : {gx_vert, gx_frag, fs_vert, clear_frag, copy_frag}) vkDestroyShaderModule(gpu.device, m, nullptr);
        efb.destroy(gpu);
        efb_depth.destroy(gpu);
        white.destroy(gpu);
    }
};

Renderer::Renderer(host::Gpu& gpu, unsigned scale) : impl_(std::make_unique<Impl>(gpu, scale)) { impl_->create(); }
Renderer::~Renderer() { impl_->destroy(); }
void Renderer::render(gx::Frame& frame) {
    const uint64_t t0 = gx::now_ns();
    impl_->render(frame);
    gx::RecorderStats& st = gx::recorder_stats();
    st.render_ns.fetch_add(gx::now_ns() - t0, std::memory_order_relaxed);
    st.rendered.fetch_add(1, std::memory_order_relaxed);
}
bool Renderer::xfb(uint32_t address, VkImage& image, uint32_t& width, uint32_t& height) const {
    const auto it = impl_->xfbs.find(address & 0x1FFFFFFFu);
    if (it == impl_->xfbs.end()) return false;
    image = it->second.image;
    width = it->second.width;
    height = it->second.height;
    return true;
}

}  // namespace punchout
