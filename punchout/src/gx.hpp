// What the guest's side of the GPU (gpu.cpp, recorder.cpp, texture.cpp) hands
// the renderer (renderer.cpp): a frame's worth of draws and EFB copies, in
// order, with everything they read from guest memory already read.
//
// Drawing happens on the main thread, which owns Vulkan; the guest's side
// records while guest memory is as the game left it at each command.
//
// Runtime code: no std::format (macOS 10.15).
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace punchout::gx {

// One vertex, every attribute GX can send, decoded to floats (the formats
// and fixed-point scales are the vertex attribute table's). What the vertex
// didn't have is filled as Dolphin's vertex loader fills it.
struct Vertex {
    float pos[3];
    float normal[3], tangent[3], binormal[3];
    uint8_t color0[4], color1[4];  // RGBA
    float uv[8][2];
    uint32_t matrices[3];  // position matrix row, then texture matrix rows 0-7, a byte each
};
static_assert(sizeof(Vertex) == 132);

// The compact layout for the common draw (no tangents or binormals, at most
// two texture coordinates): less than half the bytes to write, upload and
// fetch. The renderer feeds the attributes it lacks from zeros, which is
// what the shader would see anyway (renderer.cpp, kSmallVertices).
struct SmallVertex {
    float pos[3];
    float normal[3];
    uint8_t color0[4], color1[4];
    float uv[2][2];
    uint32_t matrices[3];
};
static_assert(sizeof(SmallVertex) == 60);

// Which attributes the vertex format had (the shaders' lighting and texgen
// rules depend on it).
enum Has : uint32_t {
    kHasPosMatrix = 1u << 0,
    kHasNormal = 1u << 1,
    kHasTangent = 1u << 2,
    kHasBinormal = 1u << 3,
    kHasColor0 = 1u << 4,
    kHasColor1 = 1u << 5,
    kHasUv0 = 1u << 6,         // << i for UV i
    kHasTexMatrix0 = 1u << 14,  // << i for texture matrix index i
};

// The GX state a draw reads, laid out as the shaders read it (std430, all
// 16-byte vectors). Built from the XF and BP registers when they change.
struct State {
    float posmtx[64][4];     // XF 0x000: position and texture matrices, by row
    float nrmmtx[32][4];     // XF 0x400: normal matrices, a row of 3 each
    float postmtx[64][4];    // XF 0x500: post-transform matrices
    struct Light {
        float color[4];     // 0-255
        float cosatt[4], distatt[4], pos[4], dir[4];
    } lights[8];
    float projection[4][4];  // rows
    float pixel_center[4];   // xy: half-pixel correction; z, w: depth range terms
    int32_t materials[4][4];  // ambient 0, 1, material 0, 1 (RGBA 0-255)
    uint32_t xf[8][4];       // see the shader: channel controls, texgens, post matrices
    uint32_t bp[64][4];      // the BP registers, all 256
    int32_t texdims[8][4];   // per texture map: width, height; per texgen: s and t scale
    int32_t tev_regs[4][4];  // PREV, C0, C1, C2 as the TEV starts
    int32_t konst[4][4];     // the konstant colours
    int32_t indmtx[6][4];    // indirect matrices: two rows each, scale in .w
    int32_t misc[4][4];      // [0]: alpha refs, fog; [1]: fog colour; [2]: z texture
    float fog[2][4];         // a, b magnitude, b shift; range: center, ... (see shader)
    float fog_range[3][4];
};

// What a pipeline is made from: everything Vulkan 1.3 can't change dynamically.
struct PipelineKey {
    uint8_t topology = 0;     // 0 triangles, 1 lines, 2 points
    uint8_t blend = 0;        // enable
    uint8_t src = 0, dst = 0, src_alpha = 0, dst_alpha = 0;  // Vulkan blend factors
    uint8_t op = 0, op_alpha = 0;                            // Vulkan blend ops
    uint8_t write_mask = 0xF;  // RGBA
    uint8_t logic = 0;        // logic op + 1, or 0 (approximated where the GPU has none)
    uint8_t depth_out = 0;    // the shader writes depth: Z textures with the Z test after texturing
    uint8_t small_vertices = 0;  // the draw's vertices are SmallVertex (Frame::small)
    bool operator==(const PipelineKey&) const = default;
};

// A draw's TEV "shape": the BP registers that decide what the pixel shader
// does, as opposed to the colours it uses. The renderer compiles a pipeline
// specialised to each shape (gx.frag's specialisation constants, in this
// order), so a weak GPU runs straight-line code instead of interpreting the
// registers per pixel. Masked to the bits the shader reads, so draws that
// differ only elsewhere share one.
constexpr unsigned kShapeWords = 70;
using ShapeWords = std::array<uint32_t, kShapeWords>;
// genmode (stage counts), 0x10-0x1F (indirect), 0x25-0x2F (indirect scales
// and refs, orders), 0xC0-0xDF (combiners), 0xF5 (Z texture mode),
// 0xF6-0xFD (konst and swap selects), 0xF3 bits 16-23 (alpha test functions).
inline ShapeWords shape_of(const uint32_t* bp) {
    ShapeWords w{};
    unsigned n = 0;
    w[n++] = bp[0x00] & 0x73C00u;  // TEV stages (10-13), indirect stages (16-18)
    for (unsigned i = 0x10; i < 0x20; ++i) w[n++] = bp[i];
    for (unsigned i = 0x25; i < 0x30; ++i) w[n++] = bp[i];
    for (unsigned i = 0xC0; i < 0xE0; ++i) w[n++] = bp[i];
    w[n++] = bp[0xF5] & 0xFu;
    for (unsigned i = 0xF6; i < 0xFE; ++i) w[n++] = bp[i];
    w[n++] = (bp[0xF3] >> 16) & 0xFFu;
    return w;
}
// The registry of shapes (recorder.cpp): the id for a draw's BP registers,
// and, from any thread, the words of an id handed out.
uint32_t shape_id(const uint32_t* bp);
ShapeWords shape_words(uint32_t id);

struct Sampler {
    uint8_t wrap_s, wrap_t;  // 0 clamp, 1 repeat, 2 mirror
    uint8_t mag_linear, min_linear, mip;  // mip: 0 none, 1 point, 2 linear
    int8_t lod_bias;   // in 1/32
    uint8_t min_lod, max_lod;  // in 1/16
    bool operator==(const Sampler&) const = default;
};

struct Draw {
    uint32_t state;        // index into the frame's states
    uint32_t has;          // Has bits
    uint32_t first_index, index_count;
    PipelineKey pipeline;
    uint32_t shape;        // shape id (shape_words)
    uint8_t cull;          // Vulkan cull mode
    uint8_t depth_test, depth_write, depth_func;  // Vulkan compare op
    float viewport[6];     // x, y, width, height, min, max depth (EFB pixels)
    int32_t scissor[4];    // x, y, width, height (EFB pixels)
    uint8_t line_width = 6, point_size = 6;  // lines and points: in sixths of an EFB pixel (BP 0x22)
    int32_t textures[8];   // texture ids (-1: none)
    Sampler samplers[8];
};

// An EFB copy: to a texture in guest memory, or to an external frame buffer.
struct Copy {
    int32_t src[4];        // x, y, width, height in the EFB
    uint32_t dest;         // guest physical address
    uint32_t dest_width, dest_height;
    uint32_t format;       // the copy's texture format (texture copies), see renderer.cpp
    bool to_xfb, half_scale, from_depth;
    bool efb_alpha;        // the EFB's pixel format has alpha (RGBA6_Z24); else a copy's alpha is 255
    int32_t texture;       // the id the copy becomes (texture copies)
    bool clear, clear_color, clear_alpha, clear_depth;
    uint8_t clear_rgba[4];
    uint32_t clear_z;      // 24-bit
    float gamma;
    // The copy filter (BP 0x53, 0x54): 6-bit weights for the line above, the
    // line itself and the line below (seven taps, summed into three as the
    // hardware's 2-3-2), 64 = unchanged; and whether the lines above and below
    // stop at the copy's own rectangle (else they come from the EFB around it).
    uint8_t filter[3];
    bool clamp_top, clamp_bottom;
};

// A texture decoded from guest memory, mip levels one after another.
struct Texture {
    int32_t id;
    uint32_t width, height, levels;
    std::vector<uint8_t> rgba;  // RGBA8
};

struct Packet {
    enum Kind : uint8_t { kDraw, kCopy } kind;
    uint32_t index;  // into draws or copies
};

// One frame: everything from one XFB copy to the next.
struct Frame {
    std::vector<Vertex> vertices;
    std::vector<SmallVertex> small;  // draws with PipelineKey::small_vertices index these
    std::vector<uint32_t> indices;
    std::vector<State> states;
    std::vector<Draw> draws;
    std::vector<Copy> copies;
    std::vector<Packet> packets;
    std::vector<Texture> textures;  // new this frame
    std::vector<int32_t> released;  // texture ids no longer used
    bool alpha_target = true;       // the EFB has alpha (RGBA6_Z24)
    // Empty again, keeping the vectors' capacity (recycle_frame). A new
    // member must be reset here too.
    void clear() {
        vertices.clear(), small.clear(), indices.clear(), states.clear(), draws.clear(), copies.clear(), packets.clear();
        textures.clear(), released.clear();
        alpha_target = true;
    }
};

// Hand a recorded frame to the renderer (it may wait while the renderer is
// two frames behind, as the console's CPU waits on a full FIFO).
void submit(std::unique_ptr<Frame> frame);
// The renderer's side: the next frame, or null after `timeout_ms`.
std::unique_ptr<Frame> next_frame(unsigned timeout_ms);
// Give a drawn frame back, so the recorder reuses its memory: a fight frame's
// vectors hold 10-20 MB, which allocated fresh each frame cost page faults
// and copies as they grew.
void recycle_frame(std::unique_ptr<Frame> frame);
// The renderer is there to take frames (else they're dropped: --headless).
void set_rendering(bool on);

}  // namespace punchout::gx
