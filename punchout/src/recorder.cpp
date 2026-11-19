// The guest's side of drawing (recorder.hpp).
//
// As the command processor decodes a primitive, its vertices are read here
// (direct or through the CP's arrays, in the vertex attribute table's
// formats), the XF and BP state it draws with is snapshotted, and its
// textures are decoded from guest memory as they are at that command. EFB
// copies are recorded in order with the draws, and the copy to the external
// frame buffer ends a frame, which goes to the renderer.
//
// Formats, scales and the fixed-function rules are the hardware's, as
// Dolphin documents them (VideoCommon: VertexLoader*.cpp, CPMemory.h,
// XFMemory.h, BPMemory.h, VertexShaderManager.cpp, BPFunctions.cpp,
// RenderState.cpp). Facts only; the code is ours.
#include "recorder.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>

#include "gx.hpp"
#include "qilin/rt/system.hpp"
#include "texture.hpp"

namespace punchout::gx {
namespace rt = qilin::rt;

namespace {

RecorderStats g_stats;
std::unique_ptr<Frame> g_frame = std::make_unique<Frame>();
bool g_dirty = true;
uint32_t g_state = 0;
int32_t g_tev_regs[4][4] = {}, g_konst[4][4] = {};
float g_last_normal[3] = {0, 0, 1}, g_last_tangent[3] = {}, g_last_binormal[3] = {};

void warn_once(bool& said, const char* what) {
    if (said) return;
    said = true;
    rt::log("render: %s: not drawn as the console draws it yet", what);
}

// ---- vertices

float as_float(uint32_t v) { return std::bit_cast<float>(v); }
uint16_t be16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }
uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

unsigned component_size(unsigned fmt) { return fmt <= 1 ? 1 : fmt <= 3 ? 2 : 4; }
float component(const uint8_t* p, unsigned fmt, float scale, bool scale_bytes) {
    switch (fmt) {
        case 0: return scale_bytes ? p[0] * scale : p[0];
        case 1: return scale_bytes ? static_cast<int8_t>(p[0]) * scale : static_cast<int8_t>(p[0]);
        case 2: return be16(p) * scale;
        case 3: return static_cast<int16_t>(be16(p)) * scale;
        default: return as_float(be32(p));
    }
}
float normal_component(const uint8_t* p, unsigned fmt) {
    switch (fmt) {  // fixed scales: the fraction is the type's
        case 0: return p[0] / 128.0f;
        case 1: return static_cast<int8_t>(p[0]) / 64.0f;
        case 2: return be16(p) / 32768.0f;
        case 3: return static_cast<int16_t>(be16(p)) / 16384.0f;
        default: return as_float(be32(p));
    }
}

const uint8_t* array_data(const Registers& r, unsigned array, uint32_t index, uint32_t bytes) {
    const uint32_t base = r.cp[0xA0 + array], stride = r.cp[0xB0 + array];
    const uint32_t a = (base + index * stride) & 0x1FFFFFFFu;
    if ((a < 0x01800000u && a + bytes <= 0x01800000u) || (a >= 0x10000000u && a + bytes <= 0x14000000u))
        return rt::guest_ptr(0x80000000u | a);
    rt::fatal(nullptr, "vertex array %u: element %u (%u bytes at %08X) is outside MEM1 and MEM2", array, index, bytes, base + index * stride);
}

void read_color(const uint8_t* p, unsigned fmt, uint8_t* out) {
    switch (fmt) {
        case 0: {  // RGB565
            const unsigned v = be16(p);
            out[0] = uint8_t((((v >> 11) & 31) << 3) | (((v >> 11) & 31) >> 2));
            out[1] = uint8_t((((v >> 5) & 63) << 2) | (((v >> 5) & 63) >> 4));
            out[2] = uint8_t(((v & 31) << 3) | ((v & 31) >> 2));
            out[3] = 255;
            break;
        }
        case 1: case 2: out[0] = p[0], out[1] = p[1], out[2] = p[2], out[3] = 255; break;  // RGB888, RGB888x
        case 3: {  // RGBA4444
            const unsigned v = be16(p);
            for (int i = 0; i < 4; ++i) out[i] = uint8_t(((v >> (12 - 4 * i)) & 15) * 17);
            break;
        }
        case 4: {  // RGBA6666
            const uint32_t v = (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2];
            for (int i = 0; i < 4; ++i) {
                const unsigned c = (v >> (18 - 6 * i)) & 63;
                out[i] = uint8_t((c << 2) | (c >> 4));
            }
            break;
        }
        default: out[0] = p[0], out[1] = p[1], out[2] = p[2], out[3] = p[3]; break;  // RGBA8888
    }
}
unsigned color_size(unsigned fmt) { return fmt == 0 || fmt == 3 ? 2 : fmt == 1 || fmt == 4 ? 3 : 4; }

struct Format {
    uint32_t vcd_lo, vcd_hi, a, b, c;
    unsigned pos_fmt, pos_n, nrm_fmt, col_fmt[2], tc_fmt[8], tc_n[8];
    float pos_scale, tc_scale[8];
    bool nbt, nbt3, dequant;
};
Format format_of(const Registers& r, unsigned vat) {
    Format f{};
    f.vcd_lo = r.cp[0x50];
    f.vcd_hi = r.cp[0x60];
    f.a = r.cp[0x70 + vat];
    f.b = r.cp[0x80 + vat];
    f.c = r.cp[0x90 + vat];
    f.pos_n = (f.a & 1) ? 3 : 2;
    f.pos_fmt = (f.a >> 1) & 7;
    f.pos_scale = 1.0f / float(1u << ((f.a >> 4) & 31));
    f.nbt = (f.a >> 9) & 1;
    f.nrm_fmt = (f.a >> 10) & 7;
    f.col_fmt[0] = (f.a >> 14) & 7;
    f.col_fmt[1] = (f.a >> 18) & 7;
    f.dequant = (f.a >> 30) & 1;
    f.nbt3 = (f.a >> 31) & 1;
    const unsigned n[8] = {(f.a >> 21) & 1, f.b & 1, (f.b >> 9) & 1, (f.b >> 18) & 1, (f.b >> 27) & 1, (f.c >> 5) & 1, (f.c >> 14) & 1, (f.c >> 23) & 1};
    const unsigned fm[8] = {(f.a >> 22) & 7, (f.b >> 1) & 7, (f.b >> 10) & 7, (f.b >> 19) & 7, (f.b >> 28) & 7, (f.c >> 6) & 7, (f.c >> 15) & 7, (f.c >> 24) & 7};
    const unsigned fr[8] = {(f.a >> 25) & 31, (f.b >> 4) & 31, (f.b >> 13) & 31, (f.b >> 22) & 31, f.c & 31, (f.c >> 9) & 31, (f.c >> 18) & 31, (f.c >> 27) & 31};
    for (int i = 0; i < 8; ++i) {
        f.tc_n[i] = n[i] ? 2 : 1;
        f.tc_fmt[i] = fm[i];
        f.tc_scale[i] = 1.0f / float(1u << fr[i]);
    }
    return f;
}

// Read one vertex; returns the bytes it took.
size_t read_vertex(const Registers& r, const Format& f, const uint8_t* p, Vertex& v, uint32_t& has) {
    const uint8_t* const start = p;
    std::memset(&v, 0, sizeof v);
    has = 0;
    uint8_t mtx[9];
    const uint32_t ia = r.cp[0x30], ib = r.cp[0x40];
    mtx[0] = uint8_t(ia & 63);
    for (int i = 0; i < 4; ++i) mtx[1 + i] = uint8_t((ia >> (6 + 6 * i)) & 63);
    for (int i = 0; i < 4; ++i) mtx[5 + i] = uint8_t((ib >> (6 * i)) & 63);
    if (f.vcd_lo & 1) {
        mtx[0] = *p++ & 63;
        has |= kHasPosMatrix;
    }
    for (int i = 0; i < 8; ++i)
        if (f.vcd_lo & (2u << i)) {
            mtx[1 + i] = *p++ & 63;
            has |= kHasTexMatrix0 << i;
        }
    // Direct data is here; indexed data is in an array.
    const auto source = [&](unsigned how, unsigned array, uint32_t bytes) -> const uint8_t* {
        if (how == 1) {
            const uint8_t* d = p;
            p += bytes;
            return d;
        }
        uint32_t index;
        if (how == 2) index = *p++;
        else index = be16(p), p += 2;
        return array_data(r, array, index, bytes);
    };
    // Position.
    {
        const unsigned how = (f.vcd_lo >> 9) & 3;
        if (how == 0) rt::fatal(nullptr, "a vertex format without a position");
        const unsigned cs = component_size(f.pos_fmt);
        const uint8_t* d = source(how, 0, cs * f.pos_n);
        for (unsigned k = 0; k < f.pos_n; ++k) v.pos[k] = component(d + k * cs, f.pos_fmt, f.pos_scale, f.dequant);
    }
    // Normal, and with NBT the tangent and binormal.
    {
        const unsigned how = (f.vcd_lo >> 11) & 3;
        if (how) {
            const unsigned cs = component_size(f.nrm_fmt);
            float* dst[3] = {v.normal, v.tangent, v.binormal};
            const unsigned vectors = f.nbt ? 3 : 1;
            if (how != 1 && f.nbt && f.nbt3) {  // an index for each vector
                for (unsigned k = 0; k < 3; ++k) {
                    const uint8_t* d = source(how, 1, cs * 9);
                    for (unsigned j = 0; j < 3; ++j) dst[k][j] = normal_component(d + (3 * k + j) * cs, f.nrm_fmt);
                }
            } else {
                const uint8_t* d = source(how, 1, cs * 3 * vectors);
                for (unsigned k = 0; k < vectors; ++k)
                    for (unsigned j = 0; j < 3; ++j) dst[k][j] = normal_component(d + (3 * k + j) * cs, f.nrm_fmt);
            }
            has |= kHasNormal | (f.nbt ? kHasTangent | kHasBinormal : 0);
            std::memcpy(g_last_normal, v.normal, 12);
            if (f.nbt) std::memcpy(g_last_tangent, v.tangent, 12), std::memcpy(g_last_binormal, v.binormal, 12);
        }
        if (!(has & kHasNormal)) std::memcpy(v.normal, g_last_normal, 12);
        if (!(has & kHasTangent)) std::memcpy(v.tangent, g_last_tangent, 12), std::memcpy(v.binormal, g_last_binormal, 12);
    }
    // Colours: with only one, both channels get it; with none, white.
    uint8_t col[2][4] = {{255, 255, 255, 255}, {255, 255, 255, 255}};
    for (unsigned c = 0; c < 2; ++c) {
        const unsigned how = (f.vcd_lo >> (13 + 2 * c)) & 3;
        if (!how) continue;
        read_color(source(how, 2 + c, color_size(f.col_fmt[c])), f.col_fmt[c], col[c]);
        has |= kHasColor0 << c;
    }
    if ((has & kHasColor0) && !(has & kHasColor1)) std::memcpy(col[1], col[0], 4);
    if (!(has & kHasColor0) && (has & kHasColor1)) std::memcpy(col[0], col[1], 4);
    std::memcpy(v.color0, col[0], 4);
    std::memcpy(v.color1, col[1], 4);
    // Texture coordinates.
    for (unsigned t = 0; t < 8; ++t) {
        const unsigned how = (f.vcd_hi >> (2 * t)) & 3;
        if (!how) continue;
        const unsigned cs = component_size(f.tc_fmt[t]);
        const uint8_t* d = source(how, 4 + t, cs * f.tc_n[t]);
        for (unsigned k = 0; k < f.tc_n[t]; ++k) v.uv[t][k] = component(d + k * cs, f.tc_fmt[t], f.tc_scale[t], f.dequant);
        has |= kHasUv0 << t;
    }
    v.matrices[0] = mtx[0] | (uint32_t(mtx[1]) << 8) | (uint32_t(mtx[2]) << 16) | (uint32_t(mtx[3]) << 24);
    v.matrices[1] = mtx[4] | (uint32_t(mtx[5]) << 8) | (uint32_t(mtx[6]) << 16) | (uint32_t(mtx[7]) << 24);
    v.matrices[2] = mtx[8];
    return size_t(p - start);
}

// ---- state

void build_state(const Registers& r, State& s) {
    std::memset(&s, 0, sizeof s);
    const uint32_t* xf = r.xf;
    for (int i = 0; i < 256; ++i) s.posmtx[i / 4][i % 4] = as_float(xf[i]);
    for (int i = 0; i < 32; ++i)
        for (int j = 0; j < 3; ++j) s.nrmmtx[i][j] = as_float(xf[0x400 + 3 * i + j]);
    for (int i = 0; i < 256; ++i) s.postmtx[i / 4][i % 4] = as_float(xf[0x500 + i]);
    for (int l = 0; l < 8; ++l) {
        const uint32_t* L = xf + 0x600 + 16 * l;
        auto& d = s.lights[l];
        for (int k = 0; k < 4; ++k) d.color[k] = float((L[3] >> (24 - 8 * k)) & 255);
        for (int k = 0; k < 3; ++k) d.cosatt[k] = as_float(L[4 + k]);
        for (int k = 0; k < 3; ++k) d.distatt[k] = as_float(L[7 + k]);
        if (std::fabs(d.distatt[0]) < 0.00001f && std::fabs(d.distatt[1]) < 0.00001f && std::fabs(d.distatt[2]) < 0.00001f)
            d.distatt[0] = 0.00001f;
        for (int k = 0; k < 3; ++k) d.pos[k] = as_float(L[10 + k]);
        double n = 0;
        for (int k = 0; k < 3; ++k) n += double(as_float(L[13 + k])) * as_float(L[13 + k]);
        n = 1.0 / std::sqrt(n);
        for (int k = 0; k < 3; ++k) {
            const float x = static_cast<float>(as_float(L[13 + k]) * n);
            d.dir[k] = std::isfinite(x) ? x : 0.0f;
        }
    }
    // Projection.
    const float* p = reinterpret_cast<const float*>(xf + 0x1020);
    float m[16] = {};
    if (xf[0x1026] == 0) {  // perspective
        m[0] = p[0], m[2] = p[1], m[5] = p[2], m[6] = p[3], m[10] = p[4], m[11] = p[5], m[14] = -1.0f;
    } else {
        m[0] = p[0], m[3] = p[1], m[5] = p[2], m[7] = p[3], m[10] = p[4], m[11] = p[5], m[15] = 1.0f;
    }
    std::memcpy(s.projection, m, sizeof m);
    // The viewport: half-pixel correction and the depth range.
    const float wd = as_float(xf[0x101A]), ht = as_float(xf[0x101B]), z_range = as_float(xf[0x101C]), far_z = as_float(xf[0x101F]);
    constexpr float kCenter = 7.0f / 12.0f - 0.5f;
    s.pixel_center[0] = kCenter * (2.0f / (2.0f * wd));
    s.pixel_center[1] = kCenter * (2.0f / (2.0f * ht));
    s.pixel_center[2] = 1.0f;
    s.pixel_center[3] = 0.0f;
    if (z_range < 0.0f || z_range > far_z || std::fabs(z_range) > 16777215.0f || std::fabs(far_z) > 16777215.0f) {
        // Out of the depth range Vulkan can express: the shader applies it.
        s.pixel_center[2] = std::fabs(z_range) / 16777215.0f;
        s.pixel_center[3] = z_range < 0.0f ? far_z / 16777215.0f : 1.0f - far_z / 16777215.0f;
    }
    for (int c = 0; c < 2; ++c)
        for (int k = 0; k < 4; ++k) {
            s.materials[c][k] = int32_t((xf[0x100A + c] >> (24 - 8 * k)) & 255);
            s.materials[2 + c][k] = int32_t((xf[0x100C + c] >> (24 - 8 * k)) & 255);
        }
    s.xf[0][0] = xf[0x1009] & 3;
    s.xf[0][1] = xf[0x100E];
    s.xf[0][2] = xf[0x100F];
    s.xf[0][3] = xf[0x1010];
    s.xf[1][0] = xf[0x1011];
    s.xf[1][1] = xf[0x1012] & 1;
    s.xf[1][2] = xf[0x103F] & 15;
    for (int i = 0; i < 8; ++i) s.xf[2 + i / 4][i % 4] = xf[0x1040 + i];
    for (int i = 0; i < 8; ++i) s.xf[4 + i / 4][i % 4] = xf[0x1050 + i];
    // Pixel side.
    for (int i = 0; i < 256; ++i) s.bp[i / 4][i % 4] = r.bp[i];
    for (unsigned m2 = 0; m2 < 8; ++m2) {
        const uint32_t image0 = r.bp[(m2 < 4 ? 0x88 : 0xA8) + m2 % 4];
        s.texdims[m2][0] = int32_t((image0 & 0x3FF) + 1);
        s.texdims[m2][1] = int32_t(((image0 >> 10) & 0x3FF) + 1);
        s.texdims[m2][2] = int32_t((r.bp[0x30 + 2 * m2] & 0xFFFF) + 1);
        s.texdims[m2][3] = int32_t((r.bp[0x31 + 2 * m2] & 0xFFFF) + 1);
    }
    std::memcpy(s.tev_regs, g_tev_regs, sizeof g_tev_regs);
    std::memcpy(s.konst, g_konst, sizeof g_konst);
    // Indirect matrices: 11-bit signed S0.10 entries; the scale is 2^(scale - 17).
    for (int i = 0; i < 3; ++i) {
        const uint32_t A = r.bp[0x06 + 3 * i], B = r.bp[0x07 + 3 * i], C = r.bp[0x08 + 3 * i];
        const auto s11 = [](uint32_t v) { return int32_t(v << 21) >> 21; };
        const int scale = int(((A >> 22) & 3) | (((B >> 22) & 3) << 2) | (((C >> 22) & 1) << 4));
        s.indmtx[2 * i][0] = s11(A), s.indmtx[2 * i][1] = s11(B), s.indmtx[2 * i][2] = s11(C);
        s.indmtx[2 * i + 1][0] = s11(A >> 11), s.indmtx[2 * i + 1][1] = s11(B >> 11), s.indmtx[2 * i + 1][2] = s11(C >> 11);
        s.indmtx[2 * i][3] = 17 - scale;
    }
    // Fog: the shader's terms, as Dolphin's PixelShaderManager derives them.
    const uint32_t f1 = r.bp[0xEE], f2 = r.bp[0xEF], f3 = r.bp[0xF0], f4 = r.bp[0xF1];
    const auto fog_float = [](uint32_t v) {  // 1 sign, 8 exponent, 11 mantissa: a float's top bits
        return std::bit_cast<float>(((v & 0xFFFFF) << 12));
    };
    const uint32_t b_mag = f2 & 0xFFFFFF;
    const uint32_t b_shf = f3 & 31;
    const float a = fog_float(f1);
    const float c = fog_float(f4);
    s.fog[0][0] = a;  // the shader: ze = a * 2^24 / (b_mag - (z >> b_shift)), then minus c
    s.fog[0][1] = c;
    s.misc[0][0] = int32_t(b_mag);
    s.misc[0][1] = int32_t(b_shf);
    for (int k = 0; k < 3; ++k) s.misc[1][k] = int32_t((r.bp[0xF2] >> (16 - 8 * k)) & 255);
    // Fog's range adjustment: the screen's centre in -1..1, and the K table (scaled by 4 / 256).
    const uint32_t range = r.bp[0xE8];
    const float wd2 = 2.0f * as_float(xf[0x101A]);
    s.fog[0][2] = ((float(int(range & 0x3FF)) - 342.0f) / wd2) * 2.0f - 1.0f;
    s.fog[0][3] = wd2;
    for (int i = 0; i < 5; ++i) {
        const uint32_t k = r.bp[0xE9 + i];
        s.fog_range[(2 * i) / 4][(2 * i) % 4] = float(k & 0xFFF) / 64.0f;
        s.fog_range[(2 * i + 1) / 4][(2 * i + 1) % 4] = float((k >> 12) & 0xFFF) / 64.0f;
    }
    // Z texture bias.
    s.misc[2][0] = int32_t(r.bp[0xF4] & 0xFFFFFF);
}

// ---- pipeline state

constexpr uint8_t kSrcFactor[8] = {0, 1, 4, 5, 6, 7, 8, 9};  // GX source factors as Vulkan's
constexpr uint8_t kDstFactor[8] = {0, 1, 2, 3, 6, 7, 8, 9};

PipelineKey pipeline_key(const uint32_t* bp, bool& alpha_target) {
    PipelineKey k;
    const uint32_t blend = bp[0x41], pe = bp[0x43], dst_alpha = bp[0x42];
    alpha_target = (pe & 7) == 1;  // RGBA6_Z24
    const bool color_update = (blend >> 3) & 1;
    const bool alpha_update = ((blend >> 4) & 1) && alpha_target;
    const bool constant_alpha = ((dst_alpha >> 8) & 1) && alpha_update;
    uint8_t src = 1, dst = 0, src_a = 1, dst_a = 0;
    if (blend & 1) {
        k.blend = 1;
        if ((blend >> 11) & 1) {  // subtract: dst - src, factors one
            k.op = k.op_alpha = 2;  // VK_BLEND_OP_REVERSE_SUBTRACT
            src = dst = src_a = dst_a = 1;
            if (constant_alpha) k.op_alpha = 0, src_a = 1, dst_a = 0;
        } else {
            unsigned s = (blend >> 8) & 7, d = (blend >> 5) & 7;
            if (!alpha_target) {  // no destination alpha: it reads as 1
                if (s == 6) s = 1;
                else if (s == 7) s = 0;
                if (d == 6) d = 1;
                else if (d == 7) d = 0;
            }
            src = kSrcFactor[s];
            dst = kDstFactor[d];
            const unsigned sa = s == 2 ? 6 : s == 3 ? 7 : s;  // colour factors become alpha factors
            const unsigned da = d == 2 ? 4 : d == 3 ? 5 : d;
            src_a = kSrcFactor[sa];
            dst_a = kDstFactor[da];
            if (constant_alpha) src_a = 1, dst_a = 0;
        }
    } else if ((blend >> 1) & 1) {
        const unsigned op = (blend >> 12) & 15;
        if (op != 3) {  // anything but copy
            k.logic = uint8_t(op + 1);
            static bool said = false;
            warn_once(said, "logic ops other than copy");
        }
    }
    k.src = src, k.dst = dst, k.src_alpha = src_a, k.dst_alpha = dst_a;
    k.write_mask = uint8_t((color_update ? 7 : 0) | (alpha_update ? 8 : 0));
    return k;
}

void viewport_and_scissor(const Registers& r, Draw& d) {
    const float wd = as_float(r.xf[0x101A]), ht = as_float(r.xf[0x101B]), z_range = as_float(r.xf[0x101C]);
    const float x_orig = as_float(r.xf[0x101D]), y_orig = as_float(r.xf[0x101E]), far_z = as_float(r.xf[0x101F]);
    const uint32_t tl = r.bp[0x20], br = r.bp[0x21], off = r.bp[0x59];
    const int left = int((tl >> 12) & 0x7FF), top = int(tl & 0x7FF), right = int((br >> 12) & 0x7FF), bottom = int(br & 0x7FF);
    const int x_off = int(off & 0x3FF) << 1, y_off = int((off >> 10) & 0x3FF) << 1;
    // The hardware's coordinates wrap at 1024 (and carry 342 of offset): of
    // the ways the scissor can be read, take the one that covers most of the viewport.
    const float vl = std::min(x_orig - wd, x_orig + wd), vr = std::max(x_orig - wd, x_orig + wd);
    const float vt = std::min(y_orig - ht, y_orig + ht), vb = std::max(y_orig - ht, y_orig + ht);
    struct Range { int off, start, end; };
    const auto ranges = [](int start, int end, int offset, int dim) {
        std::vector<Range> out;
        for (int extra = -4096; extra <= 4096; extra += 1024) {
            const int o = offset + extra, s = std::clamp(start - o, 0, dim), e = std::clamp(end - o + 1, 0, dim);
            if (s < e) out.push_back({o, s, e});
        }
        return out;
    };
    Range bx{x_off, 0, 0}, by{y_off, 0, 0};
    long best = -1, best_area = -1;
    if (left <= right && top <= bottom)
        for (const Range& x : ranges(left, right, x_off, 640))
            for (const Range& y : ranges(top, bottom, y_off, 528)) {
                const float x0 = std::clamp(float(x.start + x.off), vl, vr), x1 = std::clamp(float(x.end + x.off), vl, vr);
                const float y0 = std::clamp(float(y.start + y.off), vt, vb), y1 = std::clamp(float(y.end + y.off), vt, vb);
                const long cover = long((x1 - x0) * (y1 - y0)), area = long(x.end - x.start) * (y.end - y.start);
                if (cover > best || (cover == best && area > best_area)) best = cover, best_area = area, bx = x, by = y;
            }
    d.scissor[0] = bx.start;
    d.scissor[1] = by.start;
    d.scissor[2] = bx.end - bx.start;
    d.scissor[3] = by.end - by.start;
    float x = (x_orig - float(bx.off)) - wd, y = (y_orig - float(by.off)) + ht;
    float w = 2.0f * wd, h = -2.0f * ht;
    if (w < 0) x += w, w = -w;
    if (h < 0) y += h, h = -h;
    float min_depth = (far_z - z_range) / 16777216.0f, max_depth = far_z / 16777216.0f;
    if (z_range < 0.0f || z_range > far_z || std::fabs(z_range) > 16777215.0f || std::fabs(far_z) > 16777215.0f) {
        if (z_range < 0.0f) min_depth = 16777215.0f / 16777216.0f, max_depth = 0.0f;
        else min_depth = 0.0f, max_depth = 16777215.0f / 16777216.0f;
    }
    d.viewport[0] = x, d.viewport[1] = y, d.viewport[2] = w, d.viewport[3] = h;
    d.viewport[4] = max_depth;  // Vulkan's minDepth: depth at the near plane (reversed range)
    d.viewport[5] = min_depth;
}

// ---- textures for a draw

void bind_textures(const Registers& r, Draw& d) {
    for (auto& t : d.textures) t = -1;
    const uint32_t gen = r.bp[0x00];
    const unsigned stages = ((gen >> 10) & 15) + 1, ind_stages = (gen >> 16) & 7;
    bool used[8] = {};
    for (unsigned s = 0; s < stages; ++s) {
        const uint32_t order = r.bp[0x28 + s / 2] >> ((s & 1) ? 12 : 0);
        if ((order >> 6) & 1) used[order & 7] = true;
    }
    for (unsigned i = 0; i < ind_stages; ++i) used[(r.bp[0x27] >> (6 * i)) & 7] = true;
    for (unsigned m = 0; m < 8; ++m) {
        if (!used[m]) continue;
        uint32_t w, h;
        d.textures[m] = texture_for(r.bp, m, *g_frame, w, h);
        const uint32_t mode0 = r.bp[(m < 4 ? 0x80 : 0xA0) + m % 4], mode1 = r.bp[(m < 4 ? 0x84 : 0xA4) + m % 4];
        Sampler& s = d.samplers[m];
        s.wrap_s = uint8_t(mode0 & 3);
        s.wrap_t = uint8_t((mode0 >> 2) & 3);
        s.mag_linear = uint8_t((mode0 >> 4) & 1);
        s.mip = uint8_t((mode0 >> 5) & 3);
        s.min_linear = uint8_t((mode0 >> 7) & 1);
        s.lod_bias = int8_t((mode0 >> 9) & 0xFF);
        s.min_lod = uint8_t(mode1 & 0xFF);
        s.max_lod = uint8_t((mode1 >> 8) & 0xFF);
        if (s.wrap_s == 3) s.wrap_s = 0;
        if (s.wrap_t == 3) s.wrap_t = 0;
    }
}

// ---- the frame queue

std::mutex g_queue_lock;
std::condition_variable g_queue_cv;
std::deque<std::unique_ptr<Frame>> g_queue;
bool g_rendering = false;

}  // namespace

void set_rendering(bool on) {
    std::lock_guard l(g_queue_lock);
    g_rendering = on;
}

void submit(std::unique_ptr<Frame> frame) {
    std::unique_lock l(g_queue_lock);
    if (!g_rendering) return;
    // Two frames ahead at most: the CPU waits, as it does on a full FIFO.
    g_queue_cv.wait(l, [] { return g_queue.size() < 2 || !g_rendering; });
    g_queue.push_back(std::move(frame));
    g_queue_cv.notify_all();
}

std::unique_ptr<Frame> next_frame(unsigned timeout_ms) {
    std::unique_lock l(g_queue_lock);
    if (!g_queue_cv.wait_for(l, std::chrono::milliseconds(timeout_ms), [] { return !g_queue.empty(); })) return nullptr;
    auto f = std::move(g_queue.front());
    g_queue.pop_front();
    g_queue_cv.notify_all();
    return f;
}

void state_changed() { g_dirty = true; }

void record_primitive(const Registers& r, uint8_t opcode, const uint8_t* data, uint32_t count) {
    if (count == 0) return;
    Frame& f = *g_frame;
    const Format fmt = format_of(r, opcode & 7);
    const uint32_t first = uint32_t(f.vertices.size());
    uint32_t has = 0;
    f.vertices.resize(first + count);
    for (uint32_t i = 0; i < count; ++i) data += read_vertex(r, fmt, data, f.vertices[first + i], has);
    if (g_dirty) {
        f.states.emplace_back();
        build_state(r, f.states.back());
        g_state = uint32_t(f.states.size() - 1);
        g_dirty = false;
    }
    Draw d{};
    d.state = g_state;
    d.has = has;
    d.first_index = uint32_t(f.indices.size());
    auto& ix = f.indices;
    const unsigned type = (opcode >> 3) & 7;
    switch (type) {
        case 0: case 1:  // quads (and the second quad type)
            for (uint32_t i = 0; i + 3 < count; i += 4)
                ix.insert(ix.end(), {first + i, first + i + 1, first + i + 2, first + i, first + i + 2, first + i + 3});
            break;
        case 2:  // triangles
            for (uint32_t i = 0; i + 2 < count; i += 3) ix.insert(ix.end(), {first + i, first + i + 1, first + i + 2});
            break;
        case 3:  // strip: alternate triangles keep the winding
            for (uint32_t i = 0; i + 2 < count; ++i) {
                if (i & 1) ix.insert(ix.end(), {first + i + 1, first + i, first + i + 2});
                else ix.insert(ix.end(), {first + i, first + i + 1, first + i + 2});
            }
            break;
        case 4:  // fan
            for (uint32_t i = 1; i + 1 < count; ++i) ix.insert(ix.end(), {first, first + i, first + i + 1});
            break;
        case 5:  // lines
            for (uint32_t i = 0; i + 1 < count; i += 2) ix.insert(ix.end(), {first + i, first + i + 1});
            break;
        case 6:  // line strip
            for (uint32_t i = 0; i + 1 < count; ++i) ix.insert(ix.end(), {first + i, first + i + 1});
            break;
        default:  // points
            for (uint32_t i = 0; i < count; ++i) ix.push_back(first + i);
            break;
    }
    d.index_count = uint32_t(ix.size()) - d.first_index;
    if (d.index_count == 0) return;
    bool alpha_target;
    d.pipeline = pipeline_key(r.bp, alpha_target);
    f.alpha_target = alpha_target;
    d.pipeline.topology = type <= 4 ? 0 : type <= 6 ? 1 : 2;
    if (d.pipeline.topology) {
        static bool said = false;
        warn_once(said, "line and point widths");
    }
    static constexpr uint8_t kCull[4] = {0, 2, 1, 3};  // none, back, front, all as Vulkan's
    d.cull = d.pipeline.topology == 0 ? kCull[(r.bp[0x00] >> 14) & 3] : 0;
    const uint32_t z = r.bp[0x40];
    d.depth_test = z & 1;
    d.depth_func = uint8_t((z >> 1) & 7);  // GX's and Vulkan's compare ops number alike
    d.depth_write = (z >> 4) & 1;
    if ((r.bp[0x43] & 7) >= 3) d.depth_test = 0, d.depth_write = 0;  // no Z in this pixel format
    viewport_and_scissor(r, d);
    bind_textures(r, d);
    f.packets.push_back({Packet::kDraw, uint32_t(f.draws.size())});
    f.draws.push_back(d);
}

void bp_written(uint32_t reg, uint32_t value) {
    if (reg >= 0xE0 && reg <= 0xE7) {  // TEV colour registers and konstants: 11-bit signed, the type bit says which
        const unsigned i = (reg - 0xE0) / 2;
        const bool konst = (value >> 23) & 1;
        int32_t(&dst)[4] = konst ? g_konst[i] : g_tev_regs[i];
        const auto s11 = [](uint32_t v) { return int32_t(v << 21) >> 21; };
        if (((reg - 0xE0) & 1) == 0) dst[0] = s11(value), dst[3] = s11(value >> 12);  // red, alpha
        else dst[2] = s11(value), dst[1] = s11(value >> 12);                       // blue, green
    }
    g_dirty = true;
}

void record_tlut_load(const Registers& r, uint32_t command) {
    const uint32_t source = (r.bp[0x64] & 0xFFFFFF) << 5;
    const uint32_t tmem = (command & 0x3FF) << 9, bytes = ((command >> 10) & 0x7FF) * 32;
    load_tlut(source, tmem, bytes);
}

void record_copy(const Registers& r, uint32_t command) {
    Frame& f = *g_frame;
    Copy c{};
    const uint32_t src = r.bp[0x49], size = r.bp[0x4A];
    c.src[0] = int32_t(src & 0x3FF);
    c.src[1] = int32_t((src >> 10) & 0x3FF);
    c.src[2] = int32_t((size & 0x3FF) + 1);
    c.src[3] = int32_t(((size >> 10) & 0x3FF) + 1);
    c.dest = (r.bp[0x4B] & 0xFFFFFF) << 5;
    c.to_xfb = (command >> 14) & 1;
    c.half_scale = (command >> 9) & 1;
    c.clear = (command >> 11) & 1;
    c.from_depth = (r.bp[0x43] & 7) == 3;  // Z24: the EFB copy reads depth
    static constexpr float kGamma[4] = {1.0f, 1.7f, 2.2f, 2.2f};
    c.gamma = kGamma[(command >> 7) & 3];
    const uint32_t blend = r.bp[0x41];
    const bool alpha_target = (r.bp[0x43] & 7) == 1;
    c.clear_color = (blend >> 3) & 1;
    c.clear_alpha = ((blend >> 4) & 1) && alpha_target;
    c.clear_depth = (r.bp[0x40] >> 4) & 1;
    const uint32_t ar = r.bp[0x4F], gb = r.bp[0x50];
    c.clear_rgba[0] = uint8_t(ar & 255), c.clear_rgba[3] = uint8_t((ar >> 8) & 255);
    c.clear_rgba[1] = uint8_t((gb >> 8) & 255), c.clear_rgba[2] = uint8_t(gb & 255);
    if (!alpha_target) c.clear_rgba[3] = 255;
    c.clear_z = r.bp[0x51] & 0xFFFFFF;
    if (c.to_xfb) {
        // The frame buffer's height after vertical scaling (BP 0x4E: 256 / scale).
        const uint32_t yscale = r.bp[0x4E] & 0x1FF;
        const float scale = ((command >> 10) & 1) && yscale ? 256.0f / float(yscale) : 1.0f;
        c.dest_width = uint32_t(c.src[2]);
        c.dest_height = uint32_t(std::lround(float(c.src[3]) * scale));
        c.texture = -1;
    } else {
        c.format = ((command >> 3) & 15) / 2 + (((command >> 3) & 1) * 8) + (((command >> 15) & 1) ? 0x100 : 0);  // + intensity flag
        c.dest_width = uint32_t(c.half_scale ? c.src[2] / 2 : c.src[2]);
        c.dest_height = uint32_t(c.half_scale ? c.src[3] / 2 : c.src[3]);
        if (c.from_depth) {
            static bool said = false;
            warn_once(said, "EFB copies of depth");
        }
        c.texture = texture_from_copy(c.dest, c.dest_width, c.dest_height, f);
    }
    f.packets.push_back({Packet::kCopy, uint32_t(f.copies.size())});
    f.copies.push_back(c);
    if (c.to_xfb) {
        textures_end_frame(f);
        ++g_stats.frames;
        submit(std::move(g_frame));
        g_frame = std::make_unique<Frame>();
        g_dirty = true;
    }
}

const RecorderStats& recorder_stats() { return g_stats; }

}  // namespace punchout::gx
