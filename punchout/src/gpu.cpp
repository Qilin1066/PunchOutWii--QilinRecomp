// The GPU's front end: the hardware registers GX drives (command processor,
// pixel engine, the processor interface's FIFO) and the command stream that
// GX, and the engine's own mesh drawers, write to the write-gather pipe at
// 0xCC008000.
//
// The pipe writes each 32-byte burst to the CPU's FIFO in guest memory, as the
// hardware does. While the command processor is linked to that FIFO (normal
// drawing), it decodes the burst at once, so the CP reads as idle, its read
// pointer is always its write pointer, and the CPU never waits on it. Unlinked
// (GXBeginDisplayList), the bursts only build the display list in memory. Draw-done and token
// commands raise the pixel engine's interrupts, as the hardware does when the
// GPU reaches them. What the commands mean for drawing is the renderer's job
// (renderer.cpp): the decoder keeps the registers and hands primitives, state
// changes and EFB copies to the recorder (recorder.cpp), which reads what
// they need from guest memory as it is at that command.
//
// The command formats and register layouts are the hardware's, as documented
// by YAGCD and Dolphin (VideoCommon: OpcodeDecoding.h, CPMemory.h,
// VertexLoader_*.h, PixelEngine.h; GPL-2.0-or-later; facts only, our code).
#include "gpu.hpp"
#include "hw.hpp"
#include "priority.hpp"

#include <algorithm>
#include <atomic>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <bit>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "os.hpp"
#include "gx.hpp"
#include "recorder.hpp"
#include "video.hpp"

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
// The PI's FIFO pointers are physical addresses (MEM2 from 0x10000000); bit 26
// of the write pointer is its wrap flag (GXGetFifoWrap), never an address bit.
constexpr uint32_t kFifoAddress = 0x1FFFFFE0u & ~0x04000000u;

// ---- the decoder's state
std::array<uint32_t, 256> g_cpmem{};  // CP registers set by LOAD_CP_REG (VCD at 0x50/0x60, VAT at 0x70/0x80/0x90 + n)
std::array<uint32_t, 256> g_bp{};     // BP registers (24-bit)
std::vector<uint32_t> g_xf(0x1058);   // XF memory and registers
Stats g_stats;

gx::Registers regs() { return {g_cpmem.data(), g_xf.data(), g_bp.data()}; }
bool g_xfb_copied = false;  // set by the XFB copy just decoded (PUNCHOUT_DFF)
bool g_dff_recording = false;  // PUNCHOUT_DFF is recording a frame
uint32_t g_primitive_serial = 0;  // every primitive decoded
uint32_t g_dl_address = 0;        // the display list being decoded (physical), or 0
size_t g_dl_call_at = 0;          // PUNCHOUT_DFF: where in the commands the call to it is

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
std::vector<PeEvent> g_pe_events;  // the GPU thread adds, the guest's timer polls them
std::mutex g_pe_lock;

void poll_pe() {
    const uint64_t now = time_base();
    std::lock_guard l(g_pe_lock);
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
    std::lock_guard l(g_pe_lock);
    uint64_t t = UINT64_MAX;
    for (const auto& e : g_pe_events) t = std::min(t, e.at);
    return t;
}

void pe_finish() {
    if (g_trace) rt::log("GPU: draw done");
    {
        std::lock_guard l(g_pe_lock);
        g_pe_events.push_back({time_base() + kGpuLatency, true, 0, false});
    }
    wake_idle();  // a guest thread may be asleep waiting for it
}
void pe_token(uint16_t token, bool interrupt) {
    if (g_trace) rt::log("GPU: token %04X%s", token, interrupt ? " with interrupt" : "");
    {
        std::lock_guard l(g_pe_lock);
        g_pe_events.push_back({time_base() + kGpuLatency, false, token, interrupt});
    }
    wake_idle();
}

void on_bp(Cpu& c, uint32_t reg, uint32_t value) {
    // A BP write is masked by the mask register (0xFE), which then resets.
    const uint32_t before = g_bp[reg];
    if (reg != 0xFE) {
        const uint32_t mask = g_bp[0xFE] ? g_bp[0xFE] : 0xFFFFFFu;
        g_bp[reg] = (g_bp[reg] & ~mask) | (value & mask);
        g_bp[0xFE] = 0;
    } else {
        g_bp[0xFE] = value;
        return;
    }
    gx::bp_written(reg, g_bp[reg], g_bp[reg] != before);
    switch (reg) {
    case 0x45: pe_finish(); break;                                  // draw done
    case 0x47: pe_token(static_cast<uint16_t>(value), false); break;  // token
    case 0x48: pe_token(static_cast<uint16_t>(value), true); break;   // token with interrupt
    case 0x52:                                                      // EFB copy
        if (value & (1u << 14)) {
            ++g_stats.xfb_copies, g_xfb_copied = true;
            if (g_dff_recording) gx::mark_captured_frame();  // this copy ends the captured frame
        }
        else ++g_stats.texture_copies;
        gx::record_copy(regs(), value);
        break;
    case 0x63:                                                      // TMEM preload
        if (value & 0x7FFF) rt::fatal(&c, "GX preloads TMEM (BP 63 = %06X): not handled", value);
        break;
    case 0x65: gx::record_tlut_load(regs(), value); break;          // palette load
    default: break;
    }
    (void)c;
}

void decode(Cpu& c, const uint8_t* data, size_t size, size_t& used, bool in_display_list);

void call_display_list(Cpu& c, uint32_t address, uint32_t size) {
    // Physical addresses: MEM1 from 0, MEM2 from 0x10000000; both cached at
    // +0x80000000. The GPU ignores the top three bits, so a virtual address
    // (GXCallDisplayList passes the caller's pointer) reaches the same memory.
    const uint32_t va = (address & 0x1FFFFFE0u) + 0x80000000u;
    size &= ~31u;
    if (!((va >= 0x80000000u && va + size <= 0x81800000u) || (va >= 0x90000000u && va + size <= 0x94000000u)))
        rt::fatal(&c, "GX display list at %08X (%u bytes) is outside MEM1 and MEM2", address, size);
    ++g_stats.display_lists;
    size_t used = 0;
    g_dl_address = va & 0x1FFFFFFFu;
    decode(c, rt::guest_ptr(va), size, used, true);
    g_dl_address = 0;
}

// The last primitive decoded, for bad_command: a wrong vertex size is the
// usual way the decoder loses its place.
struct LastPrimitive {
    uint8_t op = 0;
    uint32_t count = 0, size = 0, vcd_lo = 0, vcd_hi = 0, vat[3] = {};
} g_last_primitive;
// And the last CP register writes, oldest first, for the same report.
struct CpWrite {
    uint8_t reg;
    uint32_t value;
    bool in_display_list;
};
CpWrite g_cp_writes[8] = {};
unsigned g_cp_write_count = 0;

[[noreturn]] void bad_command(Cpu& c, const uint8_t* base, size_t used, size_t left, bool in_display_list) {
    const uint8_t* at = base + used;
    char before[3 * 48 + 1] = {}, bytes[3 * 16 + 1] = {};
    const size_t back = used < 48 ? used : 48;
    for (size_t i = 0; i < back; ++i) std::snprintf(before + 3 * i, 4, "%02X ", at[i - back]);
    for (size_t i = 0; i < left && i < 16; ++i) std::snprintf(bytes + 3 * i, 4, "%02X ", at[i]);
    const LastPrimitive& l = g_last_primitive;
    char cp[8 * 24 + 1] = {};
    for (unsigned i = 0, n = 0; i < 8 && i < g_cp_write_count; ++i) {
        const CpWrite& w = g_cp_writes[(g_cp_write_count - std::min(g_cp_write_count, 8u) + i) % 8];
        n += static_cast<unsigned>(std::snprintf(cp + n, sizeof cp - n, "%02X=%08X%s ", w.reg, w.value, w.in_display_list ? "(DL)" : ""));
    }
    rt::fatal(&c,
              "GX command stream: unknown command %02X %s(before: %s| from it: %s); the decoder has lost its place. "
              "The last primitive: %02X, %u vertices of %u bytes (VCD %08X %08X, VAT %08X %08X %08X); the last CP writes: %s",
              at[0], in_display_list ? "in a display list " : "", before, bytes, l.op, l.count, l.size, l.vcd_lo, l.vcd_hi,
              l.vat[0], l.vat[1], l.vat[2], cp);
}

uint32_t be32(const uint8_t* p) { return uint32_t{p[0]} << 24 | uint32_t{p[1]} << 16 | uint32_t{p[2]} << 8 | p[3]; }

// Decode whole commands from data[used..size); stop at one that isn't complete.
// ---- PUNCHOUT_DFF=<file>@<seconds>: the first whole frame after that much
// game time, as a Dolphin FIFO log (.dff, version 6), for Dolphin's FIFO
// player to render as the hardware would: a reference to compare our
// renderer's picture with. It holds the registers at the frame's start, the
// frame's commands from one XFB copy to the next, the console's RAM (MEM1 and
// MEM2) as the frame starts, and, as Dolphin's recorder does, each piece of
// memory the GPU reads (vertex arrays, textures, palettes, display lists,
// indexed loads) that changed since, at the place in the commands where it
// was read. With <file>.prims, tools/analysis/dff_cut.py cuts the frame short
// after any primitive, to see the EFB part way through.
void dff_note_memory(uint32_t physical, uint32_t size);
struct DffCapture {
    std::string path;
    double at = 0;
    int state = 0;  // 0 waiting, 1 recording the frame, 2 written
    std::vector<uint8_t> bytes;
    std::array<uint32_t, 256> bp{}, cp{};
    std::vector<uint32_t> xf;
    std::vector<uint8_t> mem1, mem2;  // MEM1 and MEM2 as the frame starts
    std::vector<uint8_t> seen1, seen2;  // as the GPU last read them (the updates applied)
    struct Update {
        uint32_t at, address;  // where in the commands; physical
        std::vector<uint8_t> bytes;
    };
    std::vector<Update> updates;
    std::string prims;  // <file>.prims: each primitive's serial and where it is, for tools/analysis/dff_cut.py
};
DffCapture* g_dff = [] () -> DffCapture* {
    const char* e = std::getenv("PUNCHOUT_DFF");
    if (!e || !*e) return nullptr;
    auto* d = new DffCapture;
    const std::string spec = e;
    const size_t atsign = spec.rfind('@');
    d->path = spec.substr(0, atsign);
    if (atsign != std::string::npos) d->at = std::atof(spec.c_str() + atsign + 1);
    return d;
}();

}  // namespace

int bench_recording(const char* dff_path, unsigned times) {
    std::vector<uint8_t> file;
    if (FILE* f = std::fopen(dff_path, "rb")) {
        uint8_t buf[1 << 16];
        for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) file.insert(file.end(), buf, buf + n);
        std::fclose(f);
    }
    const auto u32 = [&](uint64_t at) { uint32_t v = 0; if (at + 4 <= file.size()) std::memcpy(&v, file.data() + at, 4); return v; };
    const auto u64 = [&](uint64_t at) { uint64_t v = 0; if (at + 8 <= file.size()) std::memcpy(&v, file.data() + at, 8); return v; };
    if (file.size() < 128 || u32(0) != 0x0d01f1f0u) rt::fatal(nullptr, "--bench-record: %s isn't a FIFO log", dff_path);
    const uint64_t bp_at = u64(12), cp_at = u64(24), xfm_at = u64(36), xfr_at = u64(48), frame_at = u64(60);
    const uint64_t fifo_at = u64(frame_at), upd_at = u64(frame_at + 20);
    const uint32_t fifo_size = u32(frame_at + 8), updates = u32(frame_at + 28);
    if (fifo_at + fifo_size > file.size()) rt::fatal(nullptr, "--bench-record: %s is cut short", dff_path);
    // The memory as the frame starts, then the later updates (their order in
    // the frame doesn't matter for timing).
    for (uint32_t i = 0; i < updates; ++i) {
        const uint64_t e = upd_at + 24ull * i;
        const uint32_t address = u32(e + 4), size = u32(e + 16);
        const uint64_t data = u64(e + 8);
        if (data + size > file.size()) rt::fatal(nullptr, "--bench-record: %s is cut short", dff_path);
        const bool mem1 = address < 0x01800000u && address + size <= 0x01800000u;
        const bool mem2 = address >= 0x10000000u && address + size <= 0x14000000u;
        if (mem1 || mem2) std::memcpy(rt::guest_ptr(0x80000000u | address), file.data() + data, size);
    }
    for (int i = 0; i < 256; ++i) g_bp[size_t(i)] = u32(bp_at + 4ull * i), g_cpmem[size_t(i)] = u32(cp_at + 4ull * i);
    for (int i = 0; i < 4096; ++i) g_xf[size_t(i)] = u32(xfm_at + 4ull * i);
    for (int i = 0; i < 88; ++i) g_xf[0x1000 + size_t(i)] = u32(xfr_at + 4ull * i);
    const std::vector<uint8_t> commands(file.begin() + long(fifo_at), file.begin() + long(fifo_at + fifo_size));
    Cpu c = rt::new_cpu();
    gx::set_rendering(true);  // frames are queued (and taken back below), as when drawing
    std::vector<double> ms;
    uint64_t draws = 0, vertices = 0;
    for (unsigned i = 0; i < times + 5; ++i) {  // five to warm the caches up, not counted
        for (int b = 0; b < 256; ++b) g_bp[size_t(b)] = u32(bp_at + 4ull * b), g_cpmem[size_t(b)] = u32(cp_at + 4ull * b);
        gx::state_changed();
        const uint64_t t0 = gx::now_ns();
        size_t used = 0;
        decode(c, commands.data(), commands.size(), used, false);
        const uint64_t t1 = gx::now_ns();
        while (auto f = gx::next_frame(0)) {
            if (i >= 5) draws += f->draws.size(), vertices += f->vertices.size() + f->small.size();
            gx::recycle_frame(std::move(f));
        }
        if (i >= 5) ms.push_back(double(t1 - t0) / 1e6);
    }
    std::sort(ms.begin(), ms.end());
    double sum = 0;
    for (double v : ms) sum += v;
    std::printf("recording %s, %u times: median %.3f ms, fastest %.3f, slowest %.3f, mean %.3f; "
                "%llu draws and %llu vertices a frame\n",
                dff_path, times, ms[ms.size() / 2], ms.front(), ms.back(), sum / double(ms.size()),
                static_cast<unsigned long long>(draws / times), static_cast<unsigned long long>(vertices / times));
    return 0;
}

namespace {

void dff_write(const DffCapture& d) {
    FILE* f = std::fopen(d.path.c_str(), "wb");
    if (!f) rt::fatal(nullptr, "PUNCHOUT_DFF: can't write %s", d.path.c_str());
    auto u32le = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u64le = [&](uint64_t v) { std::fwrite(&v, 8, 1, f); };
    auto u8s = [&](uint8_t v, int n) { for (int i = 0; i < n; ++i) std::fputc(v, f); };
    constexpr uint32_t kMem1 = 0x01800000u, kMem2 = 0x04000000u, kTex = 1u << 20;
    // Layout: header (128), the frame (64), BP, CP, XF memory, XF registers,
    // TMEM, the commands, the two updates (24 each), MEM1, MEM2.
    const uint64_t frame_at = 128, bp_at = frame_at + 64, cp_at = bp_at + 1024, xfm_at = cp_at + 1024,
                   xfr_at = xfm_at + 4096 * 4, tex_at = xfr_at + 88 * 4, fifo_at = tex_at + kTex,
                   upd_at = fifo_at + d.bytes.size(), mem1_at = upd_at + 24 * (2 + d.updates.size()), mem2_at = mem1_at + kMem1;
    u32le(0x0d01f1f0), u32le(6), u32le(1);                    // id, version, loader version
    u64le(bp_at), u32le(256), u64le(cp_at), u32le(256);
    u64le(xfm_at), u32le(4096), u64le(xfr_at), u32le(88);
    u64le(frame_at), u32le(1), u32le(1);                      // one frame; flags: Wii
    u64le(tex_at), u32le(kTex), u32le(kMem1), u32le(kMem2);
    std::fwrite("R7PP01\0\0", 1, 8, f);
    u8s(0, 24);
    // The FIFO the player streams the commands through: the game's own (PI's base and end).
    const uint32_t fifo_base = g_pi[kPiFifoBase] & kFifoAddress, fifo_end = g_pi[kPiFifoEnd] & kFifoAddress;
    u64le(fifo_at), u32le(uint32_t(d.bytes.size())), u32le(fifo_base), u32le(fifo_end), u64le(upd_at),
        u32le(uint32_t(2 + d.updates.size())), u8s(0, 32);
    for (uint32_t v : d.bp) u32le(v);
    for (uint32_t v : d.cp) u32le(v);
    for (int i = 0; i < 4096; ++i) u32le(d.xf[size_t(i)]);
    for (int i = 0; i < 88; ++i) u32le(d.xf[0x1000 + size_t(i)]);
    u8s(0, int(kTex));
    std::fwrite(d.bytes.data(), 1, d.bytes.size(), f);
    u32le(0), u32le(0x00000000u), u64le(mem1_at), u32le(kMem1), std::fputc(4, f), u8s(0, 3);  // MEM1, at the first command
    u32le(0), u32le(0x10000000u), u64le(mem2_at), u32le(kMem2), std::fputc(4, f), u8s(0, 3);  // MEM2
    uint64_t data_at = mem2_at + kMem2;
    for (const auto& u : d.updates) {  // what the GPU read that had changed: type 4, "vertex array"
        u32le(u.at), u32le(u.address), u64le(data_at), u32le(uint32_t(u.bytes.size())), std::fputc(4, f), u8s(0, 3);
        data_at += u.bytes.size();
    }
    std::fwrite(d.mem1.data(), 1, kMem1, f);
    std::fwrite(d.mem2.data(), 1, kMem2, f);
    for (const auto& u : d.updates) std::fwrite(u.bytes.data(), 1, u.bytes.size(), f);
    std::fclose(f);
    size_t updated = 0;
    for (const auto& u : d.updates) updated += u.bytes.size();
    rt::log("PUNCHOUT_DFF: wrote %s (%llu bytes of commands; %llu memory updates, %llu bytes)", d.path.c_str(),
            static_cast<unsigned long long>(d.bytes.size()), static_cast<unsigned long long>(d.updates.size()),
            static_cast<unsigned long long>(updated));
    // The primitives' places, and the frame's last XFB copy (the registers a cut log ends with).
    if (FILE* pf = std::fopen((d.path + ".prims").c_str(), "w")) {
        std::fprintf(pf, "# serial kind(fifo: offset in the commands; dl: physical address) location size\n%s", d.prims.c_str());
        std::fprintf(pf, "xfb %06X %06X %06X %06X %06X %06X\n", g_bp[0x49], g_bp[0x4A], g_bp[0x4B], g_bp[0x4D], g_bp[0x4E], g_bp[0x52]);
        std::fclose(pf);
    }
}

// After each top-level command: record it while the frame is being captured;
// at an XFB copy, start or finish.
void dff_after(const uint8_t* p, size_t n) {
    DffCapture& d = *g_dff;
    if (d.state == 1) d.bytes.insert(d.bytes.end(), p, p + n);
    if (!g_xfb_copied) return;
    g_xfb_copied = false;
    if (d.state == 0 && frame_count() / 60.0 >= d.at) {
        d.bp = g_bp, d.cp = g_cpmem, d.xf = g_xf;
        const auto* m1 = static_cast<const uint8_t*>(rt::guest_ptr(0x80000000u));
        const auto* m2 = static_cast<const uint8_t*>(rt::guest_ptr(0x90000000u));
        d.mem1.assign(m1, m1 + 0x01800000u), d.mem2.assign(m2, m2 + 0x04000000u);
        d.seen1 = d.mem1, d.seen2 = d.mem2;
        d.state = 1;
        g_dff_recording = gx::g_dff_use = true;
        gx::g_dff_use_hook = dff_note_memory;
    } else if (d.state == 1) {
        dff_write(d);
        d.state = 2;
        g_dff_recording = gx::g_dff_use = false;
        gx::g_dff_use_hook = nullptr;
        d.mem1 = d.mem2 = d.seen1 = d.seen2 = {};
    }
}

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
            const uint32_t v = be32(p + 2);
            if (g_cpmem[p[1]] != v) g_cpmem[p[1]] = v, gx::state_changed();  // the same value again changes nothing
            g_cp_writes[g_cp_write_count++ % 8] = {p[1], v, in_display_list};
            n = 6;
        } else if (op == 0x10) {  // LOAD_XF_REG: (count - 1) << 16 | address, then the values
            if (left < 5) return;
            const uint32_t h = be32(p + 1), count = ((h >> 16) & 0xF) + 1, addr = h & 0xFFFF;
            if (left < 5 + 4 * count) return;
            bool changed = false;
            for (uint32_t i = 0; i < count; ++i)
                if (addr + i < g_xf.size()) {
                    const uint32_t v = be32(p + 5 + 4 * i);
                    changed |= g_xf[addr + i] != v;
                    g_xf[addr + i] = v;
                }
            if (changed) gx::state_changed();  // the same values again change nothing
            n = 5 + 4 * count;
        } else if (op == 0x20 || op == 0x28 || op == 0x30 || op == 0x38) {  // LOAD_INDX_A-D
            if (left < 5) return;
            ++g_stats.indexed_loads;
            // Matrices from CP arrays 12-15 into XF memory: index, then size - 1 and the address.
            const uint32_t v = be32(p + 1), index = v >> 16, words = ((v >> 12) & 15) + 1, addr = v & 0xFFF;
            const unsigned array = 12 + (op - 0x20) / 8;
            const uint32_t src = (g_cpmem[0xA0 + array] + index * g_cpmem[0xB0 + array]) & 0x1FFFFFFFu;
            if (!((src < 0x01800000u && src + 4 * words <= 0x01800000u) || (src >= 0x10000000u && src + 4 * words <= 0x14000000u)))
                rt::fatal(&c, "GX indexed load from %08X is outside MEM1 and MEM2", src);
            if (g_dff_recording) gx::dff_use_memory(src, 4 * words);
            const uint8_t* m = rt::guest_ptr(0x80000000u | src);
            bool changed = false;
            for (uint32_t i = 0; i < words; ++i)
                if (addr + i < g_xf.size()) {
                    const uint32_t word = be32(m + 4 * i);
                    changed |= g_xf[addr + i] != word;
                    g_xf[addr + i] = word;
                }
            if (changed) gx::state_changed();
            n = 5;
        } else if (op == 0x40) {  // CALL_DL: address, size
            if (left < 9) return;
            if (in_display_list) rt::fatal(&c, "GX display list calls another display list");
            if (g_dff_recording) {
                char line[96];
                g_dl_call_at = g_dff->bytes.size();
                std::snprintf(line, sizeof line, "call fifo %llu %08X %u\n", static_cast<unsigned long long>(g_dff->bytes.size()),
                              unsigned(be32(p + 1) & 0x1FFFFFE0u), unsigned(be32(p + 5) & ~31u));
                g_dff->prims += line;
                gx::dff_use_memory(be32(p + 1) & 0x1FFFFFE0u, be32(p + 5) & ~31u);
            }
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
            g_last_primitive = {op, count, vertex_size(op & 7), g_cpmem[0x50], g_cpmem[0x60],
                                {g_cpmem[0x70 + (op & 7)], g_cpmem[0x80 + (op & 7)], g_cpmem[0x90 + (op & 7)]}};
            ++g_primitive_serial;
            if (g_dff_recording) {
                char line[96];
                if (in_display_list)
                    std::snprintf(line, sizeof line, "%u dl %08X %llu %llu\n", g_primitive_serial, unsigned(g_dl_address + used),
                                  static_cast<unsigned long long>(3 + bytes), static_cast<unsigned long long>(g_dl_call_at));
                else
                    std::snprintf(line, sizeof line, "%u fifo %llu %llu\n", g_primitive_serial,
                                  static_cast<unsigned long long>(g_dff->bytes.size()), static_cast<unsigned long long>(3 + bytes));
                g_dff->prims += line;
            }
            gx::set_primitive_serial(g_primitive_serial);
            gx::record_primitive(regs(), op, p + 3, count);
            ++g_stats.draws;
            g_stats.vertices += count;
            n = 3 + bytes;
        } else {
            bad_command(c, data, used, left, in_display_list);
        }
        used += n;
        if (g_dff && !in_display_list && g_dff->state < 2) dff_after(p, n);
    }
}

// The write-gather pipe collects 32 bytes before it sends them on, so the GPU
// sees a command only once a burst is full. Timing depends on that: GX writes
// a draw-done command, installs the callback for it and only then flushes
// (GXFlush pads the pipe with 32 bytes of NOPs).
uint8_t g_gather[32];
unsigned g_gathered = 0;

void send_burst(Cpu& c);
void consume(Cpu& c, const uint8_t* bytes, size_t n);

// The GPU thread (v0.17.0): the guest's thread only queues the pipe's bursts;
// this one decodes and records them, as the console's GPU works beside its
// CPU. It reaches draw-done and token commands when it gets to them, and the
// guest learns of them then, as on the hardware. Memory it reads (display
// lists, vertex arrays, textures) is read when it gets there too, which games
// allow for: they sync with the GPU before reusing it. PUNCHOUT_GPU_THREAD=0
// decodes on the guest's thread.
const bool g_gpu_threaded = [] {
    const char* e = std::getenv("PUNCHOUT_GPU_THREAD");
    return !(e && *e == '0');
}();
// Bytes queued before the guest waits. Small, as the hardware's lag is: with
// 1 MB the decoder fell whole draws behind and read vertex arrays and
// matrices the game had already rewritten (corrupt fights); at 16 KB it trails
// by a few draws, which the game allows for.
constexpr size_t kMaxIncoming = 16u << 10;
std::mutex g_gpu_lock;
std::condition_variable g_gpu_cv;
std::vector<uint8_t> g_incoming;  // bursts not yet decoded
Cpu* g_guest_cpu = nullptr;       // for the decoder's error reports
std::mutex g_stats_lock;
Stats g_stats_seen;  // g_stats as of the last batch, for other threads

void pipe_write(Cpu& c, uint32_t value, unsigned size) {
    if (size == 4 && g_gathered <= 28) [[likely]] {  // most stores: a word, inside the burst
        const uint32_t be = std::byteswap(value);
        std::memcpy(g_gather + g_gathered, &be, 4);
        if ((g_gathered += 4) == 32) send_burst(c);
        return;
    }
    for (unsigned i = 0; i < size; ++i) {  // a write can straddle two bursts
        g_gather[g_gathered++] = static_cast<uint8_t>(value >> (8 * (size - 1 - i)));
        if (g_gathered == 32) send_burst(c);
    }
}

// The PI's FIFO pointers are physical addresses (MEM2 from 0x10000000); bit 26
// of the write pointer is its wrap flag (GXGetFifoWrap), never an address bit.
constexpr uint32_t kFifoWrapped = 0x04000000u;  // (kFifoAddress is with the PI's registers)
constexpr unsigned kCpControl = 0x02 / 2, kCpWriteLo = 0x34 / 2, kCpWriteHi = 0x36 / 2;

// The CPU FIFO's write pointer after a burst at `pointer`: it wraps after the
// block at the end pointer, and only there (as Dolphin's GPFifo).
// GXRedirectWriteGatherPipe sets base 0, end 0x04000000 and the pointer to its
// buffer, often in MEM2, past that end: the pipe streams on into the buffer.
constexpr uint32_t next_write_pointer(uint32_t pointer, uint32_t base, uint32_t end) {
    const uint32_t w = pointer & kFifoAddress;
    return w == end ? base | kFifoWrapped : (w + 32) | (pointer & kFifoWrapped);
}
static_assert(next_write_pointer(0x00100000u, 0x00100000u, 0x00100FE0u) == 0x00100020u);
static_assert(next_write_pointer(0x00100FC0u | kFifoWrapped, 0x00100000u, 0x00100FE0u) == (0x00100FE0u | kFifoWrapped));
static_assert(next_write_pointer(0x00100FE0u, 0x00100000u, 0x00100FE0u) == (0x00100000u | kFifoWrapped));  // the end block, then wrap
static_assert(next_write_pointer(0x10184020u, 0u, 0x04000000u) == 0x10184040u);  // redirected into MEM2
static_assert(next_write_pointer(0x003FFFE0u, 0u, 0x04000000u) == 0x00400000u);   // redirected into MEM1

// CP control bit 4 links the command processor to the CPU's FIFO: then the
// GPU reads what the pipe writes. GXBeginDisplayList points the CPU's FIFO at
// the list's buffer and unlinks, so the commands it gathers go to memory only.
bool linked() { return (g_cp[kCpControl] & 0x10) != 0; }

void send_burst(Cpu& c) {
    g_gathered = 0;
    // The burst goes to memory at the PI's write pointer, which wraps at the end.
    // Bit 26 is the write pointer's wrap flag, but an ordinary bit of the end
    // pointer: GXRedirectWriteGatherPipe's end is 0x04000000.
    const uint32_t base = g_pi[kPiFifoBase] & kFifoAddress, end = g_pi[kPiFifoEnd] & 0x1FFFFFE0u;
    if (end > base) {
        const uint32_t w = g_pi[kPiFifoWrite] & kFifoAddress;
        if (!((w < 0x01800000u && w + 32 <= 0x01800000u) || (w >= 0x10000000u && w + 32 <= 0x14000000u)))
            rt::fatal(&c, "the CPU's FIFO writes to %08X, outside MEM1 and MEM2", w);
        std::memcpy(rt::guest_ptr(0x80000000u | w), g_gather, 32);
        g_pi[kPiFifoWrite] = next_write_pointer(g_pi[kPiFifoWrite], base, end);
    }
    if (!linked()) return;  // building a display list: the GPU doesn't see it
    // Linked, the GP FIFO's write pointer follows the CPU's.
    const uint32_t cp_write = g_pi[kPiFifoWrite] & kFifoAddress;
    g_cp[kCpWriteLo] = static_cast<uint16_t>(cp_write);
    g_cp[kCpWriteHi] = static_cast<uint16_t>(cp_write >> 16);
    if (!g_gpu_threaded) return consume(c, g_gather, 32);
    std::unique_lock l(g_gpu_lock);
    g_guest_cpu = &c;
    // A bounded queue, as the hardware's FIFO is: the CPU waits when it's full.
    g_gpu_cv.wait(l, [] { return g_incoming.size() < kMaxIncoming; });
    g_incoming.insert(g_incoming.end(), g_gather, g_gather + 32);
    if (g_incoming.size() == 32) g_gpu_cv.notify_all();
}

// The GPU's side of the FIFO: decode what has arrived (the GPU thread, or the
// guest's own with PUNCHOUT_GPU_THREAD=0).
void consume(Cpu& c, const uint8_t* bytes, size_t n) {
    g_fifo.insert(g_fifo.end(), bytes, bytes + n);
    g_stats.fifo_bytes += n;
    decode(c, g_fifo.data(), g_fifo.size(), g_fifo_at, false);
    if (g_fifo_at == g_fifo.size()) {
        g_fifo.clear();
        g_fifo_at = 0;
    } else if (g_fifo_at > 65536) {
        g_fifo.erase(g_fifo.begin(), g_fifo.begin() + static_cast<long>(g_fifo_at));
        g_fifo_at = 0;
    }
    std::lock_guard l(g_stats_lock);
    g_stats_seen = g_stats;
}

void gpu_thread() {
    set_this_thread_priority(Priority::high);  // a frame's draws wait on it
    std::vector<uint8_t> batch;
    for (;;) {
        Cpu* c;
        {
            std::unique_lock l(g_gpu_lock);
            g_gpu_cv.wait(l, [] { return !g_incoming.empty(); });
            batch.swap(g_incoming);
            c = g_guest_cpu;
        }
        g_gpu_cv.notify_all();  // room again
        consume(*c, batch.data(), batch.size());
        batch.clear();
    }
}

uint32_t read_reg(Cpu& c, uint32_t a, unsigned size) {
    const uint32_t off = a & 0xFFFF;
    if (a >= 0xCC000000u && a < 0xCC000080u && size == 2) {  // command processor
        const unsigned i = (off & 0x7F) / 2;
        if (i == 0) return 0x000C;  // status: read and command idle, no overflow
        if (off >= 0x30 && off <= 0x3A) {  // RW distance 0; the read pointer is the write pointer
            if (off == 0x30 || off == 0x32) return 0;
            return g_cp[(off & 2) ? kCpWriteHi : kCpWriteLo];
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

namespace {
// Save the bytes of [physical, +size) that changed since the GPU last read them,
// to be written to memory before the command being decoded (a display list's
// primitive: before the call to the list).
void dff_note_memory(uint32_t physical, uint32_t size) {
    if (!g_dff || g_dff->state != 1 || size == 0) return;
    DffCapture& d = *g_dff;
    uint32_t a = physical & 0x1FFFFFFFu;
    std::vector<uint8_t>* seen;
    uint32_t base;
    if (a < 0x01800000u) seen = &d.seen1, base = 0, size = std::min(size, 0x01800000u - a);
    else if (a >= 0x10000000u && a < 0x14000000u) seen = &d.seen2, base = 0x10000000u, size = std::min(size, 0x14000000u - a);
    else return;
    const uint8_t* now = rt::guest_ptr(0x80000000u | a);
    uint8_t* old = seen->data() + (a - base);
    if (std::memcmp(now, old, size) == 0) return;
    uint32_t lo = 0, hi = size;
    while (now[lo] == old[lo]) ++lo;
    while (now[hi - 1] == old[hi - 1]) --hi;
    std::memcpy(old + lo, now + lo, hi - lo);
    const uint32_t at = uint32_t(g_dl_address != 0 ? g_dl_call_at : d.bytes.size());
    d.updates.push_back({at, a + lo, std::vector<uint8_t>(now + lo, now + hi)});
}
}  // namespace

void init_gpu() {
    // Writing WPAR resets the pipe: bytes not yet sent as a burst are dropped.
    // GX does it after switching the CPU's FIFO (GXBeginDisplayList), so
    // GXFlush's padding stays out of the new FIFO.
    on_wpar_write([] { g_gathered = 0; });
    map_mmio({0xCC000000u, 0xCC000080u, "the command processor", read_reg, write_reg});
    map_mmio({0xCC001000u, 0xCC001080u, "the pixel engine", read_reg, write_reg});
    map_mmio({0xCC003000u, 0xCC003100u, "the processor interface", read_reg, write_reg});
    map_mmio({0xCC008000u, 0xCC008020u, "the write-gather pipe", read_reg, write_reg});
    rt::g_pipe_write = pipe_write;  // the stores' direct route (cpu.hpp)
    add_timer_source(poll_pe, pe_deadline);
    if (g_gpu_threaded) std::thread(gpu_thread).detach();  // runs until the process exits
}
Stats gpu_stats() {
    std::lock_guard l(g_stats_lock);
    return g_gpu_threaded ? g_stats_seen : g_stats;
}

}  // namespace punchout
