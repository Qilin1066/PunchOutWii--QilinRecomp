// Textures from guest memory (texture.hpp).
//
// The formats, tiling and colour expansions are the hardware's, as YAGCD
// and Dolphin's TextureDecoder_Generic.cpp document them: 8x8, 8x4 or 4x4
// tiles of 32 bytes (RGBA8: 64, in two halves, AR then GB); 4-bit, 8-bit
// and 14-bit indices into a palette in TMEM; CMPR as 2x2 DXT1-like blocks
// with the console's own blends.
#include "texture.hpp"
#include "recorder.hpp"

#include <bit>
#include <cstring>
#include <map>
#include <unordered_map>
#include <tuple>

#include "qilin/rt/system.hpp"

namespace punchout::gx {
namespace rt = qilin::rt;

namespace {

uint8_t g_tmem[1024 * 1024];  // TMEM: palettes live here (offsets in 512-byte units)

constexpr uint8_t c3(unsigned v) { return static_cast<uint8_t>((v << 5) | (v << 2) | (v >> 1)); }
constexpr uint8_t c4(unsigned v) { return static_cast<uint8_t>(v * 17); }
constexpr uint8_t c5(unsigned v) { return static_cast<uint8_t>((v << 3) | (v >> 2)); }
constexpr uint8_t c6(unsigned v) { return static_cast<uint8_t>((v << 2) | (v >> 4)); }

void put(uint8_t* p, unsigned r, unsigned g, unsigned b, unsigned a) {
    p[0] = static_cast<uint8_t>(r);
    p[1] = static_cast<uint8_t>(g);
    p[2] = static_cast<uint8_t>(b);
    p[3] = static_cast<uint8_t>(a);
}
void rgb565(uint8_t* p, uint16_t v) { put(p, c5(v >> 11), c6((v >> 5) & 63), c5(v & 31), 255); }
void rgb5a3(uint8_t* p, uint16_t v) {
    if (v & 0x8000) put(p, c5((v >> 10) & 31), c5((v >> 5) & 31), c5(v & 31), 255);
    else put(p, c4((v >> 8) & 15), c4((v >> 4) & 15), c4(v & 15), c3((v >> 12) & 7));
}
void ia8(uint8_t* p, uint8_t a, uint8_t i) { put(p, i, i, i, a); }
void palette(uint8_t* p, const uint8_t* tlut, unsigned index, unsigned format) {
    const uint8_t* e = tlut + 2 * index;
    const uint16_t v = static_cast<uint16_t>((e[0] << 8) | e[1]);
    switch (format) {
        case 0: ia8(p, e[0], e[1]); break;
        case 1: rgb565(p, v); break;
        default: rgb5a3(p, v); break;
    }
}

struct Layout {
    unsigned bw, bh, block;  // tile size in pixels, bytes per tile
};
Layout layout(unsigned format) {
    switch (format) {
        case 0: case 8: case 14: return {8, 8, 32};  // I4, C4, CMPR
        case 1: case 2: case 9: return {8, 4, 32};   // I8, IA4, C8
        case 6: return {4, 4, 64};                   // RGBA8
        default: return {4, 4, 32};                  // IA8, RGB565, RGB5A3, C14X2
    }
}

}  // namespace

uint32_t texture_bytes(unsigned format, uint32_t w, uint32_t h) {
    const Layout l = layout(format);
    return ((w + l.bw - 1) / l.bw) * ((h + l.bh - 1) / l.bh) * l.block;
}

void decode_texture(unsigned format, const uint8_t* src, uint32_t w, uint32_t h, const uint8_t* tlut, unsigned tf,
                    uint8_t* out) {
    const Layout l = layout(format);
    const uint32_t tiles_x = (w + l.bw - 1) / l.bw, tiles_y = (h + l.bh - 1) / l.bh;
    const auto px = [&](uint32_t x, uint32_t y) -> uint8_t* {
        static uint8_t sink[4];
        return (x < w && y < h) ? out + (size_t(y) * w + x) * 4 : sink;
    };
    for (uint32_t ty = 0; ty < tiles_y; ++ty)
        for (uint32_t tx = 0; tx < tiles_x; ++tx, src += l.block) {
            const uint32_t x0 = tx * l.bw, y0 = ty * l.bh;
            switch (format) {
                case 0:  // I4
                    for (unsigned i = 0; i < 64; ++i) {
                        const unsigned v = (src[i / 2] >> ((i & 1) ? 0 : 4)) & 15;
                        put(px(x0 + i % 8, y0 + i / 8), c4(v), c4(v), c4(v), c4(v));
                    }
                    break;
                case 1:  // I8
                    for (unsigned i = 0; i < 32; ++i) put(px(x0 + i % 8, y0 + i / 8), src[i], src[i], src[i], src[i]);
                    break;
                case 2:  // IA4: alpha in the high nibble
                    for (unsigned i = 0; i < 32; ++i) {
                        const uint8_t a = c4(src[i] >> 4), v = c4(src[i] & 15);
                        put(px(x0 + i % 8, y0 + i / 8), v, v, v, a);
                    }
                    break;
                case 3:  // IA8: alpha, then intensity
                    for (unsigned i = 0; i < 16; ++i) ia8(px(x0 + i % 4, y0 + i / 4), src[2 * i], src[2 * i + 1]);
                    break;
                case 4:  // RGB565
                    for (unsigned i = 0; i < 16; ++i) rgb565(px(x0 + i % 4, y0 + i / 4), uint16_t((src[2 * i] << 8) | src[2 * i + 1]));
                    break;
                case 5:  // RGB5A3
                    for (unsigned i = 0; i < 16; ++i) rgb5a3(px(x0 + i % 4, y0 + i / 4), uint16_t((src[2 * i] << 8) | src[2 * i + 1]));
                    break;
                case 6:  // RGBA8: AR pairs, then GB pairs
                    for (unsigned i = 0; i < 16; ++i)
                        put(px(x0 + i % 4, y0 + i / 4), src[2 * i + 1], src[32 + 2 * i], src[32 + 2 * i + 1], src[2 * i]);
                    break;
                case 8:  // C4
                    for (unsigned i = 0; i < 64; ++i) palette(px(x0 + i % 8, y0 + i / 8), tlut, (src[i / 2] >> ((i & 1) ? 0 : 4)) & 15, tf);
                    break;
                case 9:  // C8
                    for (unsigned i = 0; i < 32; ++i) palette(px(x0 + i % 8, y0 + i / 8), tlut, src[i], tf);
                    break;
                case 10:  // C14X2
                    for (unsigned i = 0; i < 16; ++i) palette(px(x0 + i % 4, y0 + i / 4), tlut, ((src[2 * i] << 8) | src[2 * i + 1]) & 0x3FFF, tf);
                    break;
                case 14:  // CMPR: four 4x4 blocks, in reading order
                    for (unsigned b = 0; b < 4; ++b) {
                        const uint8_t* s = src + 8 * b;
                        const unsigned v1 = (s[0] << 8) | s[1], v2 = (s[2] << 8) | s[3];
                        int c[4][4];
                        const auto ex = [](unsigned v, int* o) { o[0] = c5(v >> 11); o[1] = c6((v >> 5) & 63); o[2] = c5(v & 31); o[3] = 255; };
                        ex(v1, c[0]);
                        ex(v2, c[1]);
                        for (int k = 0; k < 3; ++k) {
                            if (v1 > v2) {  // 3/8 and 5/8, as the console blends
                                c[2][k] = (c[1][k] * 3 + c[0][k] * 5) >> 3;
                                c[3][k] = (c[0][k] * 3 + c[1][k] * 5) >> 3;
                            } else {
                                c[2][k] = c[3][k] = (c[0][k] + c[1][k]) / 2;
                            }
                        }
                        c[2][3] = 255;
                        c[3][3] = v1 > v2 ? 255 : 0;  // the average, transparent: not black
                        const uint32_t bx = x0 + (b & 1) * 4, by = y0 + (b >> 1) * 4;
                        for (unsigned y = 0; y < 4; ++y)
                            for (unsigned x = 0; x < 4; ++x) {
                                const int* k = c[(s[4 + y] >> (6 - 2 * x)) & 3];
                                put(px(bx + x, by + y), k[0], k[1], k[2], k[3]);
                            }
                    }
                    break;
                default: rt::fatal(nullptr, "texture format %u: not a GX texture format", format);
            }
        }
}

namespace {

// Whether texture data changed: four independent lanes of 8 bytes, so the
// multiplies overlap (about 3x a single chain); every single-bit change
// changes the result.
uint64_t hash_bytes(const uint8_t* p, size_t n) {
    constexpr uint64_t k = 0x9E3779B97F4A7C15ull, m = 0xFF51AFD7ED558CCDull;
    uint64_t a = k ^ n, b = ~k, c = k * 3, d = n;
    size_t i = 0;
    for (; i + 32 <= n; i += 32) {
        uint64_t v[4];
        std::memcpy(v, p + i, 32);
        a = (a ^ v[0]) * m;
        a ^= a >> 32;
        b = (b ^ v[1]) * m;
        b ^= b >> 32;
        c = (c ^ v[2]) * m;
        c ^= c >> 32;
        d = (d ^ v[3]) * m;
        d ^= d >> 32;
    }
    uint64_t h = a ^ (b * 31) ^ (c * 131) ^ (d * 1031);
    for (; i < n; ++i) h = (h ^ p[i]) * 0x100000001B3ull;
    h ^= h >> 33;
    h *= m;
    h ^= h >> 33;
    return h;
}

// Texture data already hashed since the GPU's texture cache was last
// invalidated (BP 0x66) or the frame began, by address and length. On the
// Wii the GPU keeps what it fetched until told to drop it, so a game that
// changes a texture in memory invalidates before drawing with it again: until
// then the data the GPU would use is the data hashed.
std::unordered_map<uint64_t, uint64_t> g_hashed;
uint64_t hash_texture(uint32_t address, const uint8_t* src, uint32_t bytes) {
    const uint64_t key = uint64_t(address) << 32 | bytes;
    if (const auto it = g_hashed.find(key); it != g_hashed.end()) return it->second;
    const uint64_t h = hash_bytes(src, bytes);
    g_hashed.emplace(key, h);
    return h;
}

const uint8_t* guest(uint32_t phys, uint32_t size) {
    const uint32_t a = phys & 0x1FFFFFFFu;
    if ((a < 0x01800000u && a + size <= 0x01800000u) || (a >= 0x10000000u && a + size <= 0x14000000u))
        return rt::guest_ptr(0x80000000u | a);
    rt::fatal(nullptr, "a texture of %u bytes at %08X is outside MEM1 and MEM2", size, phys);
}

using Key = std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint64_t>;  // address, format, w, h, levels, palette
struct Entry {
    int32_t id;
    uint64_t hash;
    uint64_t used;
};
std::map<Key, Entry> g_cache;
struct CopyEntry {
    int32_t id;
    uint32_t width, height;
    uint64_t ram;  // a hash of the memory under it when it was made
    uint64_t used;
};
std::map<uint32_t, CopyEntry> g_copies;
int32_t g_next_id = 1;
uint64_t g_frame = 0;

uint64_t ram_hash(uint32_t address, uint32_t w, uint32_t h) {
    const uint32_t n = std::min<uint32_t>(texture_bytes(6, w, h), 4096);  // its first 4 KB says enough
    return hash_bytes(guest(address, n), n);
}

}  // namespace

void load_tlut(uint32_t source, uint32_t tmem_address, uint32_t bytes) {
    if (tmem_address + bytes > sizeof g_tmem) rt::fatal(nullptr, "a palette load runs past TMEM (%u bytes at %X)", bytes, tmem_address);
    std::memcpy(g_tmem + tmem_address, guest(source, bytes), bytes);
    if (g_dff_use) [[unlikely]] dff_use_memory(source, bytes);
}

int32_t texture_for(const uint32_t* bp, unsigned map, Frame& frame, uint32_t& w, uint32_t& h) {
    const unsigned set = map / 4, i = map % 4;
    const uint32_t mode0 = bp[(set ? 0xA0 : 0x80) + i], mode1 = bp[(set ? 0xA4 : 0x84) + i];
    const uint32_t image0 = bp[(set ? 0xA8 : 0x88) + i], image3 = bp[(set ? 0xB4 : 0x94) + i];
    const uint32_t tlut = bp[(set ? 0xB8 : 0x98) + i];
    const uint32_t address = (image3 & 0xFFFFFF) << 5;
    w = (image0 & 0x3FF) + 1;
    h = ((image0 >> 10) & 0x3FF) + 1;
    const unsigned format = (image0 >> 20) & 15;
    if (address == 0) return -1;

    if (auto c = g_copies.find(address); c != g_copies.end()) {  // an EFB copy, if memory still holds what it did
        if (c->second.width == w && c->second.height == h && c->second.ram == ram_hash(address, w, h)) {
            c->second.used = g_frame;
            return c->second.id;
        }
    }
    // Mip levels: as many as the sampler can reach.
    uint32_t levels = 1;
    if (((mode0 >> 5) & 3) != 0) {
        const uint32_t max_lod = (mode1 >> 8) & 0xFF;
        levels = std::min<uint32_t>(max_lod / 16 + 1, std::bit_width(std::max(w, h)));
    }
    uint32_t bytes = 0;
    for (uint32_t l = 0, lw = w, lh = h; l < levels; ++l, lw = std::max(1u, lw / 2), lh = std::max(1u, lh / 2))
        bytes += texture_bytes(format, lw, lh);
    const uint8_t* src = guest(address, bytes);
    if (g_dff_use) [[unlikely]] dff_use_memory(address, bytes);
    const uint8_t* palette = nullptr;
    uint64_t palette_hash = 0;
    const unsigned tf = (tlut >> 10) & 3;
    if (format >= 8 && format <= 10) {
        palette = g_tmem + ((tlut & 0x3FF) << 9);
        const uint32_t entries = format == 8 ? 16 : format == 9 ? 256 : 16384;
        if ((tlut & 0x3FF) * 512u + entries * 2 > sizeof g_tmem) rt::fatal(nullptr, "a palette runs past TMEM");
        palette_hash = hash_bytes(palette, entries * 2) ^ tf;
    }
    RecorderStats& st = recorder_stats();
    st.texture_lookups.fetch_add(1, std::memory_order_relaxed);
    const uint64_t h0 = now_ns();
    const uint64_t hash = hash_texture(address, src, bytes);
    st.hash_ns.fetch_add(now_ns() - h0, std::memory_order_relaxed);
    const Key key{address, format, w, h, levels, palette_hash};
    auto it = g_cache.find(key);
    if (it != g_cache.end() && it->second.hash == hash) {
        it->second.used = g_frame;
        return it->second.id;
    }
    Texture t;
    t.id = g_next_id++;
    t.width = w;
    t.height = h;
    t.levels = levels;
    size_t total = 0;
    for (uint32_t l = 0, lw = w, lh = h; l < levels; ++l, lw = std::max(1u, lw / 2), lh = std::max(1u, lh / 2)) total += size_t(lw) * lh * 4;
    t.rgba.resize(total);
    const uint64_t d0 = now_ns();
    size_t at = 0;
    for (uint32_t l = 0, lw = w, lh = h; l < levels; ++l, lw = std::max(1u, lw / 2), lh = std::max(1u, lh / 2)) {
        decode_texture(format, src, lw, lh, palette, tf, t.rgba.data() + at);
        src += texture_bytes(format, lw, lh);
        at += size_t(lw) * lh * 4;
    }
    st.decode_ns.fetch_add(now_ns() - d0, std::memory_order_relaxed);
    st.textures_decoded.fetch_add(1, std::memory_order_relaxed);
    st.texture_bytes.fetch_add(bytes, std::memory_order_relaxed);
    if (it != g_cache.end()) frame.released.push_back(it->second.id);
    g_cache[key] = {t.id, hash, g_frame};
    frame.textures.push_back(std::move(t));
    return g_cache[key].id;
}

int32_t texture_from_copy(uint32_t address, uint32_t w, uint32_t h, Frame& frame) {
    auto& e = g_copies[address & 0x1FFFFFFFu];
    if (e.id) frame.released.push_back(e.id);
    e = {g_next_id++, w, h, ram_hash(address, w, h), g_frame};
    return e.id;
}

void textures_invalidated() { g_hashed.clear(); }

void textures_end_frame(Frame& frame) {
    ++g_frame;
    g_hashed.clear();
    constexpr uint64_t kKeep = 600;  // ten seconds unused
    for (auto it = g_cache.begin(); it != g_cache.end();)
        if (g_frame - it->second.used > kKeep) {
            frame.released.push_back(it->second.id);
            it = g_cache.erase(it);
        } else {
            ++it;
        }
    for (auto it = g_copies.begin(); it != g_copies.end();)
        if (g_frame - it->second.used > kKeep) {
            frame.released.push_back(it->second.id);
            it = g_copies.erase(it);
        } else {
            ++it;
        }
}

}  // namespace punchout::gx
