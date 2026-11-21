// The DSP and the audio interface, as registers.
//
// DSP (0xCC005000, 16-bit registers; a 32-bit access is two of them, high
// half first):
//   00/02 mail to the DSP (writing the low half sends it), 04/06 mail from
//   the DSP (bit 31 set while unread; reading the low half takes it),
//   0A control: 0 reset, 1 assert, 2 halt, 3/4 AI DMA flag/enable,
//   5/6 ARAM flag/enable, 7/8 DSP flag/enable, 9 ARAM DMA busy, 10 init code,
//   11 init. Writing 1 to a flag clears it. The PI's DSP line is raised while
//   any flag is set with its enable.
//   30/32 audio DMA source, 36 bit 15 enable and bits 0-14 its length in
//   32-byte blocks, 3A blocks left.
// The DSP's own program is not run: Wii titles boot one of a few AX
// microcode versions through the ROM's task loader, and ax.cpp does what
// that microcode does, from the mails it receives.
//
// The audio DMA plays 32-byte blocks (8 stereo samples, right then left,
// big-endian) at the AI's DMA rate; when the last block of a buffer has
// played it reloads from the source registers and raises the AI DMA flag.
// The AX library answers that flag by queueing the next 3 ms buffer and
// sending the DSP its command list, so this clock drives all of the sound.
//
// AI (0xCD006C00, 32-bit): 00 control (0 counter run, 1 stream rate,
// 2 interrupt enable, 3 interrupt flag, 4 interrupt valid, 5 counter reset,
// 6 DMA rate: 1 32 kHz, 0 48 kHz), 04 volume, 08 sample counter, 0C
// interrupt timing.
//
// After Dolphin's DSP.cpp, AudioInterface.cpp, DSPHLE.cpp, MailHandler.cpp,
// UCodes.cpp, ROM.cpp and INIT.cpp: the register map and mail protocol are
// facts taken from there; no code is.
#include <deque>
#include <utility>
#include <vector>

#include "audio.hpp"
#include "speaker.hpp"
#include "hw.hpp"
#include "os.hpp"

namespace punchout {
namespace rt = qilin::rt;
using rt::Cpu;

namespace {

constexpr uint16_t kReset = 1u << 0, kHalt = 1u << 2, kAid = 1u << 3, kAram = 1u << 5, kDspFlag = 1u << 7;
constexpr uint16_t kInitCode = 1u << 10, kInit = 1u << 11;
constexpr uint16_t kFlags = kAid | kAram | kDspFlag;
constexpr uint16_t kMicrocodeBits = 0x0C07;  // reset, assert, halt, init code, init: the DSP's side

AudioStats g_stats;
AudioSink g_sink = nullptr;
void* g_sink_user = nullptr;

// The DSP's side.
enum class Microcode { Rom, Init, Ax };
Microcode g_ucode = Microcode::Rom;
uint16_t g_dsp_bits = kHalt | kInit;  // control as the microcode keeps it
uint64_t g_init_code_clear = 0;      // time base at which the init code bit reads clear
bool g_halted = true;
std::deque<std::pair<uint32_t, bool>> g_mail;  // to the CPU; .second: interrupt once read
uint32_t g_last_mail = 0;
uint32_t g_cpu_mail = 0;  // the CPU's mailbox

// The ROM's task loader: parameter mails, then the boot.
uint32_t g_rom_param = 0, g_rom_iram = 0, g_rom_length = 0;

// The CPU's side.
uint16_t g_control = 0;
bool g_line_posted = false;
uint16_t g_ar[16] = {};  // AR registers 0x5012..0x502A: kept, the Wii has no ARAM

// Audio DMA.
uint32_t g_dma_source = 0;
uint16_t g_dma_control = 0;  // bit 15 enable, bits 0-14 blocks
uint32_t g_dma_current = 0, g_dma_blocks = 0;
uint64_t g_dma_t0 = 0;       // time base at the start of the playing buffer
uint32_t g_dma_rate = 32000;

// AI.
uint32_t g_ai_control = 0, g_ai_volume = 0, g_ai_counter = 0, g_ai_timing = 0;
uint64_t g_ai_counter_t0 = 0;

uint64_t dma_end() { return g_dma_t0 + uint64_t(g_dma_blocks) * 8 * kTimerHz / g_dma_rate; }
unsigned ai_stream_rate() { return (g_ai_control & 2) ? 48000 : 32000; }

// --- interrupts ---

uint32_t os_interrupt_for(uint16_t flag) { return flag == kAid ? 5 : flag == kAram ? 6 : 7; }
uint16_t active() { return (g_control >> 1) & g_control & kFlags; }

void service(Cpu& c) {
    for (int round = 0; round < 64; ++round) {
        const uint16_t a = active();
        if (!a) break;
        bool ran = false;
        for (uint16_t flag : {kAid, kAram, kDspFlag})
            if ((a & flag) && call_interrupt_handler(c, os_interrupt_for(flag))) {
                ran = true;
                break;
            }
        if (!ran) break;  // masked by the OS: poll() posts it again when that changes
        if (round == 63) rt::fatal(&c, "the DSP interrupt (control %04X) is still raised after its handler ran 64 times", g_control);
    }
    g_line_posted = false;
}
void update_line() {
    if (g_line_posted || !active()) return;
    g_line_posted = true;
    post_interrupt(service);
}
void raise_flag(uint16_t flag) {
    g_control |= flag;
    update_line();
}

// --- the microcode's side ---

void set_microcode(Microcode m) {
    g_mail.clear();
    g_ucode = m;
    if (m == Microcode::Rom) {
        g_rom_param = 0;
        dsp::push_mail(0x8071FEED, false);
    } else if (m == Microcode::Init) {
        dsp::push_mail(0x80544348, false);
    }
}

uint32_t ector_hash(const uint8_t* p, uint32_t n) {
    uint32_t h = 0;
    for (uint32_t i = 0; i < n; ++i) {
        h ^= p[i];
        h = (h << 3) | (h >> 29);
    }
    return h;
}

void rom_mail(Cpu& c, uint32_t mail) {
    if (g_rom_param == 0) {
        if ((mail & 0xFFFF0000u) == 0x80F30000u) g_rom_param = mail;
        else dsp::push_mail(0xFEEE0000u | (mail & 0xFFFF), false);
        return;
    }
    const uint32_t param = std::exchange(g_rom_param, 0);
    switch (param) {
        case 0x80F3A001: g_rom_iram = mail; break;           // microcode in main memory
        case 0x80F3A002: g_rom_length = mail & 0xFFFF; break;  // its length in bytes
        case 0x80F3B002:                                      // data memory to load: AX needs none
            if (mail & 0xFFFF) rt::fatal(&c, "a DSP task asks for %u bytes of data memory: not handled", mail & 0xFFFF);
            break;
        case 0x80F3C002: break;  // where it goes in instruction memory
        case 0x80F3D001: {       // its start: boot it
            const uint32_t crc = ector_hash(dsp::physical(&c, g_rom_iram, g_rom_length), g_rom_length);
            g_stats.ucode_crc = crc;
            if (!ax::known(crc))
                rt::fatal(&c, "the DSP was given a microcode this doesn't know (hash %08X, %u bytes at %08X)", crc, g_rom_length, g_rom_iram);
            rt::log("dsp: AX microcode %08X booted", crc);
            g_mail.clear();
            g_ucode = Microcode::Ax;
            ax::start(crc);
            break;
        }
        default: rt::fatal(&c, "the DSP's boot loader got parameter %08X: not known", param);
    }
}

void mail_to_dsp(Cpu& c, uint32_t mail) {
    ++g_stats.mails_in;
    switch (g_ucode) {
        case Microcode::Rom: rom_mail(c, mail); break;
        case Microcode::Init: break;
        case Microcode::Ax: ax::mail(c, mail); break;
    }
}

uint16_t read_mail_high() {
    if (!g_halted && !g_mail.empty()) g_last_mail = g_mail.front().first;
    return static_cast<uint16_t>(g_last_mail >> 16);
}
uint16_t read_mail_low() {
    if (!g_halted && !g_mail.empty()) {
        g_last_mail = g_mail.front().first;
        const bool interrupt = g_mail.front().second;
        g_mail.pop_front();
        if (interrupt) raise_flag(kDspFlag);
    }
    g_last_mail &= ~0x80000000u;
    return static_cast<uint16_t>(g_last_mail);
}

uint16_t microcode_control_write(uint16_t v) {
    if ((g_dsp_bits ^ v) & kHalt) g_halted = (v & kHalt) != 0;
    if (v & kReset) {
        set_microcode(Microcode::Rom);
        v &= ~kReset;
    }
    if ((g_dsp_bits & kInit) && !(v & kInit)) {
        set_microcode(Microcode::Init);
        v |= kInitCode;
        g_init_code_clear = time_base() + 130;
    }
    g_dsp_bits = v;
    return v;
}
uint16_t microcode_control_read() {
    if ((g_dsp_bits & kInitCode) && time_base() >= g_init_code_clear) g_dsp_bits &= ~kInitCode;
    return g_dsp_bits;
}

// --- audio DMA ---

void play(Cpu* c, uint32_t source, uint32_t blocks) {
    ++g_stats.dma_periods;
    const uint32_t frames = blocks * 8;
    g_stats.frames += frames;
    if (!g_sink) return;
    const uint8_t* p = dsp::physical(c, source, blocks * 32);
    static thread_local std::vector<int16_t> out;
    out.resize(size_t(frames) * 2);
    for (uint32_t i = 0; i < frames; ++i) {  // right, left in the buffer
        const auto s = [&](uint32_t k) { return static_cast<int16_t>((p[k] << 8) | p[k + 1]); };
        out[2 * i] = s(4 * i + 2);
        out[2 * i + 1] = s(4 * i);
    }
    // The Wii Remotes' speakers, which the console plays from the remotes in the player's hands:
    // here from the PC's speakers, with the rest.
    const bool speakers = g_speakers_on.load(std::memory_order_relaxed);
    for (Speaker& sp : g_speakers) sp.mix(out.data(), frames, g_dma_rate, speakers);
    g_sink(out.data(), frames, g_dma_rate, g_sink_user);
}

bool dma_on() { return (g_dma_control & 0x8000) != 0; }

void poll() {
    if (active() && !g_line_posted) {  // raised while the OS had it masked
        const uint32_t mask = interrupt_mask();
        for (uint16_t flag : {kAid, kAram, kDspFlag})
            if ((active() & flag) && !(mask & (0x80000000u >> os_interrupt_for(flag)))) {
                update_line();
                break;
            }
    }
    if (!dma_on()) return;
    const uint64_t now = time_base();
    if (now >= dma_end() + kTimerHz / 10) {  // more than 100 ms behind (a stall): start again from now
        rt::log("audio: %llu ms behind, skipping ahead", static_cast<unsigned long long>((now - dma_end()) * 1000 / kTimerHz));
        g_dma_t0 = now;
        return;
    }
    if (now < dma_end()) return;
    play(nullptr, g_dma_current, g_dma_blocks);
    g_dma_t0 = dma_end();
    g_dma_current = g_dma_source;
    g_dma_blocks = g_dma_control & 0x7FFF;
    if (!g_dma_blocks) rt::fatal(nullptr, "the audio DMA reloaded a buffer of 0 blocks");
    raise_flag(kAid);
}
uint64_t deadline() {
    uint64_t t = UINT64_MAX;
    if (dma_on()) t = dma_end();
    return t;
}

// --- registers ---

uint16_t dsp_read16(Cpu& c, uint32_t off) {
    switch (off) {
        case 0x00: return static_cast<uint16_t>(g_cpu_mail >> 16);
        case 0x02: return static_cast<uint16_t>(g_cpu_mail);
        case 0x04: return read_mail_high();
        case 0x06: return read_mail_low();
        case 0x0A: return (g_control & ~kMicrocodeBits) | (microcode_control_read() & kMicrocodeBits);
        case 0x30: return static_cast<uint16_t>(g_dma_source >> 16);
        case 0x32: return static_cast<uint16_t>(g_dma_source);
        case 0x36: return g_dma_control;
        case 0x3A: {
            if (!dma_on()) return 0;
            const uint64_t now = time_base();
            const uint64_t played = now <= g_dma_t0 ? 0 : (now - g_dma_t0) * g_dma_rate / (8 * kTimerHz);
            const uint64_t left = played >= g_dma_blocks ? 0 : g_dma_blocks - played;
            return static_cast<uint16_t>(left > 0 ? left - 1 : 0);
        }
        default:
            if (off >= 0x12 && off < 0x2C) return g_ar[(off - 0x12) / 2];
            rt::fatal(&c, "load from DSP register %04X: not handled", off);
    }
}
void dsp_write16(Cpu& c, uint32_t off, uint16_t v) {
    switch (off) {
        case 0x00: g_cpu_mail = (g_cpu_mail & 0xFFFF) | (uint32_t(v) << 16); return;
        case 0x02:
            g_cpu_mail = (g_cpu_mail & 0xFFFF0000u) | v;
            mail_to_dsp(c, g_cpu_mail);
            g_cpu_mail &= 0x7FFFFFFFu;
            return;
        case 0x04: case 0x06: rt::fatal(&c, "the CPU wrote %04X to the DSP's outgoing mailbox", v);
        case 0x0A: {
            if (v & 0xF000) rt::fatal(&c, "DSP control written with %04X: the top bits must be 0", v);
            const uint16_t ucode = microcode_control_write(v) & kMicrocodeBits;
            if (v & kReset) g_dma_control = 0;
            uint16_t ctrl = (g_control & 0x0200) | ucode | (v & 0x0150);  // keep ARAM busy; the enables from the write
            ctrl |= g_control & kFlags & ~v;                               // a 1 clears a flag
            g_control = ctrl;
            update_line();
            return;
        }
        case 0x30: g_dma_source = (g_dma_source & 0xFFFF) | (uint32_t(v & 0x1FFF) << 16); return;
        case 0x32: g_dma_source = (g_dma_source & 0xFFFF0000u) | (v & 0xFFE0); return;
        case 0x36: {
            const bool was = dma_on();
            g_dma_control = v;
            if (!was && dma_on()) {  // starts with the source as it is now; asks for the next at once
                g_dma_current = g_dma_source;
                g_dma_blocks = v & 0x7FFF;
                if (!g_dma_blocks) rt::fatal(&c, "the audio DMA was started with 0 blocks");
                g_dma_t0 = time_base();
                raise_flag(kAid);
            }
            return;
        }
        default:
            if (off >= 0x12 && off < 0x2C) {
                if (off == 0x2A)
                    rt::fatal(&c, "an ARAM DMA was started (the Wii has no ARAM): not handled");
                g_ar[(off - 0x12) / 2] = v;
                return;
            }
            rt::fatal(&c, "store of %04X to DSP register %04X: not handled", v, off);
    }
}
uint32_t dsp_read(Cpu& c, uint32_t a, unsigned size) {
    const uint32_t off = a & 0x1FF;
    if (size == 2) return dsp_read16(c, off);
    if (size == 4) {
        const uint32_t hi = dsp_read16(c, off);
        return (hi << 16) | dsp_read16(c, off + 2);
    }
    rt::fatal(&c, "load of %u bytes from DSP register %04X", size, off);
}
void dsp_write(Cpu& c, uint32_t a, uint32_t v, unsigned size) {
    const uint32_t off = a & 0x1FF;
    if (size == 2) return dsp_write16(c, off, static_cast<uint16_t>(v));
    if (size == 4) {
        dsp_write16(c, off, static_cast<uint16_t>(v >> 16));
        return dsp_write16(c, off + 2, static_cast<uint16_t>(v));
    }
    rt::fatal(&c, "store of %u bytes to DSP register %04X", size, off);
}

uint32_t ai_counter() {
    uint32_t n = g_ai_counter;
    if (g_ai_control & 1) n += static_cast<uint32_t>((time_base() - g_ai_counter_t0) * ai_stream_rate() / kTimerHz);
    return n;
}
void ai_counter_restart(uint32_t value) {
    g_ai_counter = value;
    g_ai_counter_t0 = time_base();
}
uint32_t ai_read(Cpu& c, uint32_t a, unsigned size) {
    if (size != 4) rt::fatal(&c, "load of %u bytes from AI register %08X", size, a);
    switch (a & 0x1F) {
        case 0x00: return g_ai_control;
        case 0x04: return g_ai_volume;
        case 0x08: return ai_counter();
        case 0x0C: return g_ai_timing;
        default: rt::fatal(&c, "load from AI register %08X: not handled", a);
    }
}
void ai_write(Cpu& c, uint32_t a, uint32_t v, unsigned size) {
    if (size != 4) rt::fatal(&c, "store of %u bytes to AI register %08X", size, a);
    switch (a & 0x1F) {
        case 0x00: {
            if (v & 4) rt::fatal(&c, "the AI's sample-counter interrupt was enabled (%08X): not handled", v);
            const uint32_t counter = ai_counter();
            if ((v ^ g_ai_control) & 1) ai_counter_restart(counter);  // run or stop, keeping the count
            g_ai_control = (v & 0x57) & ~0x08u;
            if (v & 0x20) ai_counter_restart(0);
            const unsigned rate = (v & 0x40) ? 32000 : 48000;
            if (rate != g_dma_rate) {
                g_dma_rate = rate;
                rt::log("audio: DMA rate %u Hz", rate);
            }
            return;
        }
        case 0x04: g_ai_volume = v; return;
        case 0x08: ai_counter_restart(v); return;
        case 0x0C: g_ai_timing = v; return;
        default: rt::fatal(&c, "store of %08X to AI register %08X: not handled", v, a);
    }
}

}  // namespace

namespace dsp {
void push_mail(uint32_t mail, bool interrupt) {
    ++g_stats.mails_out;
    if (interrupt) {
        if (g_mail.empty()) raise_flag(kDspFlag);
        else g_mail.front().second = true;
    }
    g_mail.emplace_back(mail, false);
}
void reset_to_rom() { set_microcode(Microcode::Rom); }
uint8_t* physical(Cpu* c, uint32_t address, uint32_t size) {
    const uint32_t a = address & 0x1FFFFFFFu;  // the DSP's DMA ignores the cached/uncached segment bits
    const uint32_t end = a + size;
    if (a < 0x01800000u && end <= 0x01800000u) return rt::guest_ptr(0x80000000u | a);
    if (a >= 0x10000000u && end <= 0x14000000u) return rt::guest_ptr(0x80000000u | a);
    rt::fatal(c, "the DSP reached %u bytes at %08X: outside MEM1 and MEM2", size, address);
}
AudioStats& stats() { return g_stats; }
}  // namespace dsp

void init_audio() {
    map_mmio({0xCC005000u, 0xCC005200u, "the DSP", dsp_read, dsp_write});
    map_mmio({0xCD006C00u, 0xCD006C20u, "the audio interface", ai_read, ai_write});
    add_timer_source(poll, deadline);
    set_microcode(Microcode::Rom);
}
void dsp_interrupt_enables(uint32_t os_mask) {
    uint16_t enables = 0;
    if (!(os_mask & (0x80000000u >> 5))) enables |= kAid << 1;
    if (!(os_mask & (0x80000000u >> 6))) enables |= kAram << 1;
    if (!(os_mask & (0x80000000u >> 7))) enables |= kDspFlag << 1;
    g_control = static_cast<uint16_t>((g_control & ~0x0150u) | enables);
    update_line();
}
void set_audio_sink(AudioSink sink, void* user) {
    g_sink = sink;
    g_sink_user = user;
}
const AudioStats& audio_stats() { return g_stats; }

}  // namespace punchout
