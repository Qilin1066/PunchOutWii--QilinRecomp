// The GPU's front end: the hardware registers GX drives (command processor,
// pixel engine, the processor interface's FIFO) and the command stream that
// GX, and the engine's own mesh drawers, write to the write-gather pipe at
// 0xCC008000.
//
// The command processor here decodes as the stream arrives: there's no FIFO in
// guest memory to overflow, so the CP reads as idle, its read pointer is always
// its write pointer, and the CPU never waits on it. Draw-done and token
// commands raise the pixel engine's interrupts, as the hardware does when the
// GPU reaches them. What the commands mean for drawing is the renderer's job
// (roadmap step 4, on Vulkan); for now the decoder keeps the state it needs to
// parse (vertex descriptors and attribute tables) and counts what it sees.
//
// The command formats and register layouts are the hardware's, as documented
// by YAGCD and Dolphin (VideoCommon: OpcodeDecoding.h, CPMemory.h,
// VertexLoader_*.h, PixelEngine.h; GPL-2.0-or-later; facts only, our code).
#include "gpu.hpp"
#include "hw.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <bit>
#include <vector>

#include "os.hpp"

namespace punchout {
namespace rt = qilin::rt;
using rt::Cpu;

namespace {

// ---- registers
uint16_t g_cp[0x40] = {};  // command processor, 16-bit, at 0xCC000000
uint16_t g_pe[0x40] = {};  // pixel engine, at 0xCC001000
uint32_t g_pi[0x40] = {};  // processor interface, 32-bit, at 0xCC003000
uint16_t g_token = 0;
bool g_finish_pending = false, g_token_pending = false;

constexpr unsigned kPeCtrl = 0x0A / 2, kPeToken = 0x0E / 2;
constexpr unsigned kPiFifoBase = 0x0C / 4, kPiFifoEnd = 0x10 / 4, kPiFifoWrite = 0x14 / 4;

// ---- the decoder's state
std::array<uint32_t, 256> g_cpmem{};  // CP registers set by LOAD_CP_REG (VCD at 0x50/0x60, VAT at 0x70/0x80/0x90 + n)
std::array<uint32_t, 256> g_bp{};     // BP registers (24-bit)
std::vector<uint32_t> g_xf(0x1058);   // XF memory and registers
Stats g_stats;

std::vector<uint8_t> g_fifo;  // bytes written to the pipe, not yet a whole command
size_t g_fifo_at = 0;

unsigned component_size(unsigned fmt) { return fmt <= 1 ? 1 : fmt <= 3 ? 2 : 4; }  // u8/s8, u16/s16, float
unsigned index_size(unsigned how) { return how == 2 ? 1 : 2; }                  // 2 index8, 3 index16

// The size of one vertex in format `vat`, from the vertex descriptor and the
// attribute table (VertexLoaderBase::GetVertexSize's rules).
unsigned vertex_size(unsigned vat) {
    const uint32_t lo = g_cpmem[0x50], hi = g_cpmem[0x60];
    const uint32_t a = g_cpmem[0x70 + vat], b = g_cpmem[0x80 + vat], c = g_cpmem[0x90 + vat];
    unsigned size = std::popcount(lo & 0x1FFu);  // position and texture matrix indices
    auto part = [&](unsigned how, unsigned direct) { return how == 0 ? 0u : how == 1 ? direct : index_size(how); };
    size += part((lo >> 9) & 3, ((a & 1) ? 3 : 2) * component_size((a >> 1) & 7));  // position: xy or xyz
    {
        const unsigned how = (lo >> 11) & 3, nbt = (a >> 9) & 1, index3 = (a >> 31) & 1;
        if (how == 1) size += (nbt ? 9 : 3) * component_size((a >> 10) & 7);
        else if (how) size += index_size(how) * (nbt && index3 ? 3 : 1);
    }
    static constexpr unsigned kColor[8] = {2, 3, 4, 2, 3, 4, 0, 0};  // 565, 888, 888x, 4444, 6666, 8888
    size += part((lo >> 13) & 3, kColor[(a >> 14) & 7]);
    size += part((lo >> 15) & 3, kColor[(a >> 18) & 7]);
    // Texture coordinates: count bit and format for each of the eight.
    const unsigned tc_count[8] = {(a >> 21) & 1, b & 1, (b >> 9) & 1, (b >> 18) & 1, (b >> 27) & 1, (c >> 5) & 1, (c >> 14) & 1, (c >> 23) & 1};
    const unsigned tc_fmt[8] = {(a >> 22) & 7, (b >> 1) & 7, (b >> 10) & 7, (b >> 19) & 7, (b >> 28) & 7, (c >> 6) & 7, (c >> 15) & 7, (c >> 24) & 7};
    for (unsigned i = 0; i < 8; ++i) size += part((hi >> (2 * i)) & 3, (tc_count[i] ? 2 : 1) * component_size(tc_fmt[i]));
    return size;
}

const bool g_trace = [] {
    const char* t = std::getenv("PUNCHOUT_TRACE");
    return t && *t && *t != '0';
}();

// The GPU reaches a draw-done or token command only after the work queued
// before it, and games rely on that: GX writes draw-done, then installs the
// callback for it. So the pixel engine's interrupts come a short GPU latency
// after the command is decoded, as timed events (Dolphin likewise schedules
// them at least 500 CPU cycles out).
constexpr uint64_t kGpuLatency = kTimerHz / 20000;  // 50 us
struct PeEvent {
    uint64_t at;
    bool finish;  // or a token
    uint16_t token;
    bool interrupt;
};
std::vector<PeEvent> g_pe_events;

void poll_pe() {
    const uint64_t now = time_base();
    for (size_t i = 0; i < g_pe_events.size();) {
        const PeEvent e = g_pe_events[i];
        if (e.at > now) {
            ++i;
            continue;
        }
        g_pe_events.erase(g_pe_events.begin() + static_cast<long>(i));
        if (e.finish) {
            g_finish_pending = true;
            if (g_pe[kPeCtrl] & 2) raise_interrupt(19);  // __OS_INTERRUPT_PI_PE_FINISH
        } else {
            g_token = e.token;
            if (e.interrupt) {
                g_token_pending = true;
                if (g_pe[kPeCtrl] & 1) raise_interrupt(18);  // __OS_INTERRUPT_PI_PE_TOKEN
            }
        }
    }
}
uint64_t pe_deadline() {
    uint64_t t = UINT64_MAX;
    for (const auto& e : g_pe_events) t = std::min(t, e.at);
    return t;
}

void pe_finish() {
    if (g_trace) rt::log("GPU: draw done (PE control %04X)", g_pe[kPeCtrl]);
    g_pe_events.push_back({time_base() + kGpuLatency, true, 0, false});
}
void pe_token(uint16_t token, bool interrupt) {
    if (g_trace) rt::log("GPU: token %04X%s (PE control %04X)", token, interrupt ? " with interrupt" : "", g_pe[kPeCtrl]);
    g_pe_events.push_back({time_base() + kGpuLatency, false, token, interrupt});
}

void on_bp(Cpu& c, uint32_t reg, uint32_t value) {
    // A BP write is masked by the mask register (0xFE), which then resets.
    if (reg != 0xFE) {
        const uint32_t mask = g_bp[0xFE] ? g_bp[0xFE] : 0xFFFFFFu;
        g_bp[reg] = (g_bp[reg] & ~mask) | (value & mask);
        g_bp[0xFE] = 0;
    } else {
        g_bp[0xFE] = value;
        return;
    }
    switch (reg) {
    case 0x45: pe_finish(); break;                                  // draw done
    case 0x47: pe_token(static_cast<uint16_t>(value), false); break;  // token
    case 0x48: pe_token(static_cast<uint16_t>(value), true); break;   // token with interrupt
    case 0x52:                                                      // EFB copy
        if (value & (1u << 14)) ++g_stats.xfb_copies;
        else ++g_stats.texture_copies;
        break;
    default: break;
    }
    (void)c;
}

void decode(Cpu& c, const uint8_t* data, size_t size, size_t& used, bool in_display_list);

void call_display_list(Cpu& c, uint32_t address, uint32_t size) {
    // Physical addresses: MEM1 from 0, MEM2 from 0x10000000; both cached at +0x80000000.
    const uint32_t va = (address & ~31u) + 0x80000000u;
    size &= ~31u;
    if (!((va >= 0x80000000u && va + size <= 0x81800000u) || (va >= 0x90000000u && va + size <= 0x94000000u)))
        rt::fatal(&c, "GX display list at %08X (%u bytes) is outside MEM1 and MEM2", address, size);
    ++g_stats.display_lists;
    size_t used = 0;
    decode(c, rt::guest_ptr(va), size, used, true);
}

[[noreturn]] void bad_command(Cpu& c, const uint8_t* base, size_t used, size_t left, bool in_display_list) {
    const uint8_t* at = base + used;
    char before[3 * 48 + 1] = {}, bytes[3 * 16 + 1] = {};
    const size_t back = used < 48 ? used : 48;
    for (size_t i = 0; i < back; ++i) std::snprintf(before + 3 * i, 4, "%02X ", at[i - back]);
    for (size_t i = 0; i < left && i < 16; ++i) std::snprintf(bytes + 3 * i, 4, "%02X ", at[i]);
    rt::fatal(&c, "GX command stream: unknown command %02X %s(before: %s| from it: %s); the decoder has lost its place",
              at[0], in_display_list ? "in a display list " : "", before, bytes);
}

uint32_t be32(const uint8_t* p) { return uint32_t{p[0]} << 24 | uint32_t{p[1]} << 16 | uint32_t{p[2]} << 8 | p[3]; }

// Decode whole commands from data[used..size); stop at one that isn't complete.
void decode(Cpu& c, const uint8_t* data, size_t size, size_t& used, bool in_display_list) {
    while (used < size) {
        const uint8_t* p = data + used;
        const size_t left = size - used;
        const uint8_t op = p[0];
        size_t n;
        if (op == 0x00) {
            n = 1;
        } else if (op == 0x08) {  // LOAD_CP_REG: register, value
            if (left < 6) return;
            g_cpmem[p[1]] = be32(p + 2);
            n = 6;
        } else if (op == 0x10) {  // LOAD_XF_REG: (count - 1) << 16 | address, then the values
            if (left < 5) return;
            const uint32_t h = be32(p + 1), count = ((h >> 16) & 0xF) + 1, addr = h & 0xFFFF;
            if (left < 5 + 4 * count) return;
            for (uint32_t i = 0; i < count; ++i)
                if (addr + i < g_xf.size()) g_xf[addr + i] = be32(p + 5 + 4 * i);
            n = 5 + 4 * count;
        } else if (op == 0x20 || op == 0x28 || op == 0x30 || op == 0x38) {  // LOAD_INDX_A-D
            if (left < 5) return;
            ++g_stats.indexed_loads;
            n = 5;
        } else if (op == 0x40) {  // CALL_DL: address, size
            if (left < 9) return;
            if (in_display_list) rt::fatal(&c, "GX display list calls another display list");
            call_display_list(c, be32(p + 1), be32(p + 5));
            n = 9;
        } else if (op == 0x44 || op == 0x48) {  // metrics; invalidate the vertex cache
            n = 1;
        } else if (op == 0x61) {  // LOAD_BP_REG: register in the top byte, 24-bit value
            if (left < 5) return;
            on_bp(c, p[1], be32(p + 1) & 0xFFFFFFu);
            n = 5;
        } else if (op >= 0x80 && op <= 0xBF) {  // a primitive: count, then the vertices
            if (left < 3) return;
            const uint32_t count = uint32_t{p[1]} << 8 | p[2];
            const size_t bytes = size_t{count} * vertex_size(op & 7);
            if (g_trace && !in_display_list && used == 0)
                rt::log("GPU: primitive %02X, %u vertices of %u bytes (VCD %08X %08X, VAT %08X %08X %08X)", op, count,
                        vertex_size(op & 7), g_cpmem[0x50], g_cpmem[0x60], g_cpmem[0x70 + (op & 7)], g_cpmem[0x80 + (op & 7)],
                        g_cpmem[0x90 + (op & 7)]);
            if (left < 3 + bytes) return;
            ++g_stats.draws;
            g_stats.vertices += count;
            n = 3 + bytes;
        } else {
            bad_command(c, data, used, left, in_display_list);
        }
        used += n;
    }
}

// The write-gather pipe collects 32 bytes before it sends them on, so the GPU
// sees a command only once a burst is full. Timing depends on that: GX writes
// a draw-done command, installs the callback for it and only then flushes
// (GXFlush pads the pipe with 32 bytes of NOPs).
uint8_t g_gather[32];
unsigned g_gathered = 0;

void send_burst(Cpu& c);

void pipe_write(Cpu& c, uint32_t value, unsigned size) {
    for (unsigned i = 0; i < size; ++i) {  // a write can straddle two bursts
        g_gather[g_gathered++] = static_cast<uint8_t>(value >> (8 * (size - 1 - i)));
        if (g_gathered == 32) send_burst(c);
    }
}

void send_burst(Cpu& c) {
    g_gathered = 0;
    g_fifo.insert(g_fifo.end(), g_gather, g_gather + 32);
    g_stats.fifo_bytes += 32;
    // The PI's write pointer moves through the CPU FIFO's ring as on hardware.
    if (g_pi[kPiFifoEnd] > g_pi[kPiFifoBase]) {
        uint32_t w = (g_pi[kPiFifoWrite] & 0x03FFFFFFu) + 32;
        if (w >= (g_pi[kPiFifoEnd] & 0x03FFFFFFu)) w = (g_pi[kPiFifoBase] & 0x03FFFFFFu) + (w - (g_pi[kPiFifoEnd] & 0x03FFFFFFu));
        g_pi[kPiFifoWrite] = w;
    }
    decode(c, g_fifo.data(), g_fifo.size(), g_fifo_at, false);
    if (g_fifo_at == g_fifo.size()) {
        g_fifo.clear();
        g_fifo_at = 0;
    } else if (g_fifo_at > 65536) {
        g_fifo.erase(g_fifo.begin(), g_fifo.begin() + static_cast<long>(g_fifo_at));
        g_fifo_at = 0;
    }
}

uint32_t read_reg(Cpu& c, uint32_t a, unsigned size) {
    const uint32_t off = a & 0xFFFF;
    if (a >= 0xCC000000u && a < 0xCC000080u && size == 2) {  // command processor
        const unsigned i = (off & 0x7F) / 2;
        if (i == 0) return 0x000C;  // status: read and command idle, no overflow
        if (off >= 0x30 && off <= 0x3A) {  // RW distance 0; read and write pointers where the PI wrote
            if (off == 0x30 || off == 0x32) return 0;
            const uint32_t w = g_pi[kPiFifoWrite] & 0x03FFFFFFu;
            return (off & 2) ? w >> 16 : w & 0xFFFF;
        }
        return g_cp[i];
    }
    if (a >= 0xCC001000u && a < 0xCC001080u && size == 2) {  // pixel engine
        const unsigned i = (off & 0x7F) / 2;
        if (i == kPeToken) return g_token;
        if (i == kPeCtrl) return (g_pe[i] & 3u) | (g_token_pending ? 4u : 0u) | (g_finish_pending ? 8u : 0u);
        return g_pe[i];
    }
    if (a >= 0xCC003000u && a < 0xCC003100u && size == 4) return g_pi[(off & 0xFF) / 4];
    rt::fatal(&c, "load of %u bytes from hardware register %08X: not handled", size, a);
}

void write_reg(Cpu& c, uint32_t a, uint32_t v, unsigned size) {
    if ((a & ~31u) == 0xCC008000u) return pipe_write(c, v, size);  // the gather block: any address in it
    const uint32_t off = a & 0xFFFF;
    if (a >= 0xCC000000u && a < 0xCC000080u && size == 2) {
        g_cp[(off & 0x7F) / 2] = static_cast<uint16_t>(v);
        return;
    }
    if (a >= 0xCC001000u && a < 0xCC001080u && size == 2) {
        const unsigned i = (off & 0x7F) / 2;
        if (i == kPeCtrl) {  // bits 2 and 3 acknowledge the token and finish interrupts
            if (v & 4) g_token_pending = false;
            if (v & 8) g_finish_pending = false;
            g_pe[i] = static_cast<uint16_t>(v & 3);
            return;
        }
        g_pe[i] = static_cast<uint16_t>(v);
        return;
    }
    if (a >= 0xCC003000u && a < 0xCC003100u && size == 4) {
        g_pi[(off & 0xFF) / 4] = v;
        return;
    }
    rt::fatal(&c, "store of %u bytes (%08X) to hardware register %08X: not handled", size, v, a);
}

}  // namespace

void init_gpu() {
    map_mmio({0xCC000000u, 0xCC000080u, "the command processor", read_reg, write_reg});
    map_mmio({0xCC001000u, 0xCC001080u, "the pixel engine", read_reg, write_reg});
    map_mmio({0xCC003000u, 0xCC003100u, "the processor interface", read_reg, write_reg});
    map_mmio({0xCC008000u, 0xCC008020u, "the write-gather pipe", read_reg, write_reg});
    add_timer_source(poll_pe, pe_deadline);
}
const Stats& gpu_stats() { return g_stats; }

}  // namespace punchout
