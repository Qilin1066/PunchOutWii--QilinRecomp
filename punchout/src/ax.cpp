// The DSP's AX microcode, Wii versions, run at a high level.
//
// The AX library (lifted) talks to the microcode by mail. Every 3 ms it
// sends 0xBABE0000 | n, then the address of a command list of n halfwords.
// The list sets up 96-sample mixing buffers, walks the linked list of voice
// parameter blocks (PBs), mixes each voice into the main and aux buses,
// hands the aux buses to the CPU for effects and takes them back, and
// writes the final stereo buffer that the audio DMA plays. The microcode
// then mails DSP_SYNC after the output and DSP_YIELD when the list ends,
// and waits for the task manager's next mail.
//
// A voice: the accelerator reads its samples (4-bit ADPCM, or 8- or 16-bit
// PCM) from memory, handling loops; a sample-rate converter steps through
// them; then a volume envelope, a low-pass filter, a biquad (newer
// versions) and per-bus volumes with ramps. Voices may also feed the Wii
// Remote speakers' 6 kHz buses.
//
// Three Wii versions differ in their command numbering and PB layout; the
// microcode's hash says which. The polyphase resampling coefficients live
// in the DSP's ROM: if the user puts a 4096-byte dump at
// <user folder>/dsp_coef.bin it's used, otherwise polyphase voices are
// resampled linearly.
//
// After Dolphin's AX.cpp, AXWii.cpp, AXVoice.h, AXStructs.h and
// DSPAccelerator.cpp: the formats, the arithmetic and the order of
// operations are facts taken from there (they decide what the game sounds
// like); the code is this project's own.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <fstream>

#include "audio.hpp"
#include "qilin/host/paths.hpp"

namespace punchout::ax {
namespace rt = qilin::rt;
using rt::Cpu;

namespace {

using s16 = int16_t;
using u16 = uint16_t;

// --- the parameter block (the newest, largest layout) ---

struct Volume { u16 volume, delta; };
struct Mixer {
    Volume main_l, main_r, auxa_l, auxa_r, auxb_l, auxb_r, auxc_l, auxc_r, main_s, auxa_s, auxb_s, auxc_s;
};
struct TimeDelay { u16 on, addr_hi, addr_lo, offset_l, offset_r, target_l, target_r; };
struct Dpop { s16 main_l, auxa_l, auxb_l, auxc_l, main_r, auxa_r, auxb_r, auxc_r, main_s, auxa_s, auxb_s, auxc_s; };
struct Updates { u16 count[3], data_hi, data_lo; };
struct Envelope { s16 volume, delta; };
struct Address { u16 looping, format, loop_hi, loop_lo, end_hi, end_lo, cur_hi, cur_lo; };
struct Adpcm { s16 coefs[16]; u16 gain, pred_scale; s16 yn1, yn2; };
struct Src { u16 ratio_hi, ratio_lo, frac; s16 last[4]; };
struct AdpcmLoop { u16 pred_scale; s16 yn1, yn2; };
struct LowPass { u16 on; s16 yn1; u16 a0; s16 b0; };
struct HighPass { u16 on, unknown[3]; };
struct Biquad { u16 on; s16 xn1, xn2, yn1, yn2, b0, b1, b2, a1, a2; };
struct RemoteMixer { Volume main0, aux0, main1, aux1, main2, aux2, main3, aux3; };
struct RemoteDpop { s16 main0, main1, main2, main3, aux0, aux1, aux2, aux3; };
struct RemoteSrc { u16 frac; s16 last[4]; };
union RemoteFilter { u16 on; LowPass lpf; Biquad biquad; };

struct PB {
    u16 next_hi, next_lo, this_hi, this_lo;
    u16 src_type, coef_select, mixer_control_hi, mixer_control_lo;
    u16 running, is_stream;
    Mixer mixer;
    TimeDelay itd;
    Dpop dpop;
    Updates updates;
    Envelope env;
    Address addr;
    Adpcm adpcm;
    Src src;
    AdpcmLoop loop;
    LowPass lpf;
    union { HighPass hpf; Biquad biquad; };
    u16 remote, remote_mixer_control;
    RemoteMixer remote_mixer;
    RemoteDpop remote_dpop;
    RemoteSrc remote_src;
    RemoteFilter remote_filter;
    u16 pad[2];
};
constexpr size_t kPbWords = sizeof(PB) / 2;
static_assert(sizeof(PB) == 310);
constexpr size_t kUpdatesBegin = offsetof(PB, updates) / 2, kUpdatesEnd = kUpdatesBegin + sizeof(Updates) / 2;
constexpr size_t kGapBegin = offsetof(PB, hpf) / 2 + sizeof(HighPass) / 2, kGapEnd = offsetof(PB, biquad) / 2 + sizeof(Biquad) / 2;
static_assert(kUpdatesBegin == 53 && kGapBegin == 106 && kGapEnd == 112);

// How a version's PB maps onto the struct: runs of words; the rest read as 0.
struct Run { size_t begin, end; };  // struct words [begin, end), consecutive in guest memory
enum class Layout { Updates_NoGap, NoUpdates_NoGap, NoUpdates_Biquad };
struct Version {
    uint32_t crc;
    const char* what;
    bool old_commands;
    Layout layout;
    bool biquad;          // the newer filter
    bool fixed_output;    // its output command carries no volume
};
constexpr Version kVersions[] = {
    {0xFA450138, "Wii Sports", true, Layout::Updates_NoGap, false, false},
    {0x7699AF32, "the Wii start-up menu", true, Layout::Updates_NoGap, false, false},
    {0xD9C4BF34, "Wii System Menu 1.0", false, Layout::NoUpdates_NoGap, false, true},
    {0xADBC06BD, "Elebits", false, Layout::NoUpdates_NoGap, false, false},
    {0x347112BA, "Raving Rabbids", false, Layout::NoUpdates_Biquad, true, false},
    {0x4CC52064, "Bleach: Versus Crusade", false, Layout::NoUpdates_Biquad, true, false},
};
const Version* g_version = nullptr;

std::array<Run, 3> runs(Layout l, size_t& count) {
    switch (l) {
        case Layout::Updates_NoGap: count = 2; return {{{0, kGapBegin}, {kGapEnd, kPbWords}, {}}};
        case Layout::NoUpdates_NoGap: count = 3; return {{{0, kUpdatesBegin}, {kUpdatesEnd, kGapBegin}, {kGapEnd, kPbWords}}};
        case Layout::NoUpdates_Biquad: count = 2; return {{{0, kUpdatesBegin}, {kUpdatesEnd, kPbWords}, {}}};
    }
    return {};
}
void read_pb(Cpu& c, uint32_t a, PB& pb) {
    u16 w[kPbWords] = {};
    size_t n = 0;
    const auto r = runs(g_version->layout, n);
    size_t guest = 0;
    for (size_t i = 0; i < n; ++i) guest += r[i].end - r[i].begin;
    const uint8_t* p = dsp::physical(&c, a, uint32_t(guest * 2));
    for (size_t i = 0, g = 0; i < n; ++i)
        for (size_t k = r[i].begin; k < r[i].end; ++k, ++g) w[k] = u16((p[2 * g] << 8) | p[2 * g + 1]);
    std::memcpy(&pb, w, sizeof pb);
}
void write_pb(Cpu& c, uint32_t a, const PB& pb) {
    u16 w[kPbWords];
    std::memcpy(w, &pb, sizeof pb);
    size_t n = 0;
    const auto r = runs(g_version->layout, n);
    size_t guest = 0;
    for (size_t i = 0; i < n; ++i) guest += r[i].end - r[i].begin;
    uint8_t* p = dsp::physical(&c, a, uint32_t(guest * 2));
    for (size_t i = 0, g = 0; i < n; ++i)
        for (size_t k = r[i].begin; k < r[i].end; ++k, ++g) {
            p[2 * g] = uint8_t(w[k] >> 8);
            p[2 * g + 1] = uint8_t(w[k]);
        }
}

uint32_t hilo(u16 hi, u16 lo) { return (uint32_t(hi) << 16) | lo; }
s16 clamp16(int64_t v) { return static_cast<s16>(std::clamp<int64_t>(v, -0x8000, 0x7FFF)); }

// --- the accelerator: reads a voice's samples from memory ---

struct Accelerator {
    PB* pb = nullptr;
    Cpu* cpu = nullptr;
    uint32_t start = 0, end = 0, current = 0;
    u16 format = 0, pred_scale = 0;
    s16 gain = 0, yn1 = 0, yn2 = 0;
    bool stopped = false;

    void setup(Cpu& c, PB& p) {
        cpu = &c;
        pb = &p;
        start = hilo(p.addr.loop_hi, p.addr.loop_lo) & 0x3FFFFFFF;
        end = hilo(p.addr.end_hi, p.addr.end_lo) & 0x3FFFFFFF;
        current = hilo(p.addr.cur_hi, p.addr.cur_lo) & 0xBFFFFFFF;
        format = p.addr.format;
        yn1 = p.adpcm.yn1;
        set_yn2(p.adpcm.yn2);
        gain = static_cast<s16>(p.adpcm.gain);
        pred_scale = p.adpcm.pred_scale & 0x7F;
    }
    void set_yn2(s16 v) {
        yn2 = v;
        stopped = false;
    }
    uint8_t memory(uint32_t a) const { return *dsp::physical(cpu, a & 0x13FFFFFFu, 1); }
    u16 raw() const {
        switch (format & 3) {
            case 0: {
                const u16 b = memory(current >> 1);
                return (current & 1) ? (b & 0xF) : (b >> 4);
            }
            case 1: return memory(current);
            case 2: return u16((memory(current * 2) << 8) | memory(current * 2 + 1));
            default: rt::fatal(cpu, "a voice has sample format %04X: not handled", format);
        }
    }
    void end_reached() {  // the voice's last sample was read
        if (pb->addr.looping) {
            pred_scale = pb->loop.pred_scale & 0x7F;
            if (pb->is_stream != 1) {
                yn1 = pb->loop.yn1;
                set_yn2(pb->loop.yn2);
            } else {
                set_yn2(yn2);
            }
        } else {
            pb->running = 0;
        }
    }
    s16 sample(const s16* coefs) {
        if (stopped) return 0;
        const unsigned decode = (format >> 2) & 3;
        if (decode == 1 || decode == 3) rt::fatal(cpu, "a voice reads its samples from the accelerator's input register (format %04X)", format);
        s16 in = static_cast<s16>(raw());
        const unsigned k = (pred_scale >> 4) & 7;
        const int32_t c1 = coefs[2 * k], c2 = coefs[2 * k + 1];
        s16 out;
        unsigned step;
        if (decode == 0) {  // ADPCM
            int32_t nibble = in & 0xF;
            if (nibble >= 8) nibble -= 16;
            const int32_t v = (1 << (pred_scale & 0xF)) * nibble + ((0x400 + c1 * yn1 + c2 * yn2) >> 11);
            out = clamp16(v);
            step = 2;
            yn2 = yn1;
            yn1 = out;
            current += 1;
            if ((end & 0xF) == 0 && current == end) current = start + 1;
            else if ((end & 0xF) == 1 && current == end - 1) current = start;
            else if ((current & 15) == 0) {  // a frame header: the next frame's predictor and scale
                pred_scale = memory((current & ~15u) >> 1);
                current += 2;
                step += 2;
            }
        } else {  // PCM
            unsigned shift = 0;
            switch ((format >> 4) & 3) {
                case 0: shift = 11; break;
                case 1: shift = 0; break;
                case 2: shift = 16; break;
                default: rt::fatal(cpu, "a voice has gain scale 3 (format %04X)", format);
            }
            const int32_t v = ((int32_t(gain) * in) >> shift) + ((c1 * yn1) >> shift) + ((c2 * yn2) >> shift);
            out = static_cast<s16>(v);
            yn2 = yn1;
            yn1 = out;
            step = 2;
            current += 1;
        }
        if (current == end + step - 1) {
            current = start;
            stopped = true;
            end_reached();
        }
        current &= 0xBFFFFFFF;
        return out;
    }
};

// --- resampling ---

std::array<s16, 0x800> g_coefs;
bool g_have_coefs = false;

template <class Input>
uint32_t resample(Input&& input, s16* out, unsigned count, s16* last, uint32_t pos, uint32_t ratio, u16 type, const s16* coefs) {
    if (type == 0 && coefs) {  // polyphase, 4 taps
        s16 t[4];
        unsigned i4 = 0, read = 0;
        for (int k = 0; k < 4; ++k) t[i4++ & 3] = last[k];
        for (unsigned i = 0; i < count; ++i) {
            pos += ratio;
            for (; pos >= 0x10000; pos -= 0x10000) t[i4++ & 3] = input(read++);
            const s16* c = &coefs[(((pos & 0xFFFF) >> 9) << 2)];
            int64_t s = 0;
            for (int k = 0; k < 4; ++k) s += int64_t(t[i4++ & 3]) * c[k];
            out[i] = clamp16(s >> 15);
        }
        for (int k = 3; k >= 0; --k) last[k] = t[--i4 & 3];
    } else if (type == 0 || type == 1) {  // linear
        s16 t[4];
        unsigned i4 = 0, read = 0;
        for (int k = 0; k < 4; ++k) t[i4++ & 3] = last[k];
        for (unsigned i = 0; i < count; ++i) {
            pos += ratio;
            for (; pos >= 0x10000; pos -= 0x10000) t[i4++ & 3] = input(read++);
            const u16 frac = pos & 0xFFFF;
            if (frac) {
                const int32_t s0 = t[i4++ & 3], s1 = t[i4++ & 3];
                out[i] = static_cast<s16>((s0 * u16(-frac) + s1 * frac) >> 16);
                i4 += 2;
            } else {
                out[i] = t[i4++ & 3];
                i4 += 3;
            }
        }
        for (int k = 3; k >= 0; --k) last[k] = t[--i4 & 3];
    } else {  // none: one input sample per output sample
        for (unsigned i = 0; i < count; ++i) out[i] = input(i);
        std::memcpy(last, out + count - 4, 4 * sizeof(s16));
    }
    return pos;
}

// --- mixing ---

constexpr unsigned kSamples = 96, kRemoteSamples = 18;  // 3 ms at 32 kHz and at 6 kHz

enum : uint32_t {
    MainL = 1u << 0, MainLRamp = 1u << 1, MainR = 1u << 2, MainRRamp = 1u << 3, MainS = 1u << 4, MainSRamp = 1u << 5,
    AuxAL = 1u << 6, AuxALRamp = 1u << 7, AuxAR = 1u << 8, AuxARRamp = 1u << 9, AuxAS = 1u << 10, AuxASRamp = 1u << 11,
    AuxBL = 1u << 12, AuxBLRamp = 1u << 13, AuxBR = 1u << 14, AuxBRRamp = 1u << 15, AuxBS = 1u << 16, AuxBSRamp = 1u << 17,
    AuxCL = 1u << 18, AuxCLRamp = 1u << 19, AuxCR = 1u << 20, AuxCRRamp = 1u << 21, AuxCS = 1u << 22, AuxCSRamp = 1u << 23,
};
uint32_t mixer_control(uint32_t m) {  // the PB's bits to which buses get the voice, and which ramp
    uint32_t r = 0;
    struct Bus { uint32_t l, r, both, s, s_ramp, out_l, out_r, ramp_l, ramp_r, out_s, ramp_s; };
    static constexpr Bus kBuses[] = {
        {1u << 0, 1u << 1, 1u << 2, 1u << 3, 1u << 4, MainL, MainR, MainLRamp, MainRRamp, MainS, MainSRamp},
        {1u << 16, 1u << 17, 1u << 18, 1u << 19, 1u << 20, AuxAL, AuxAR, AuxALRamp, AuxARRamp, AuxAS, AuxASRamp},
        {1u << 21, 1u << 22, 1u << 23, 1u << 24, 1u << 25, AuxBL, AuxBR, AuxBLRamp, AuxBRRamp, AuxBS, AuxBSRamp},
        {1u << 26, 1u << 27, 1u << 28, 1u << 29, 1u << 30, AuxCL, AuxCR, AuxCLRamp, AuxCRRamp, AuxCS, AuxCSRamp},
    };
    for (const Bus& b : kBuses) {
        if (m & b.l) r |= b.out_l;
        if (m & b.r) r |= b.out_r;
        if (m & b.both) r |= b.out_l | b.out_r | b.ramp_l | b.ramp_r;
        if (m & b.s) r |= b.out_s;
        if (m & b.s_ramp) r |= b.out_s | b.ramp_s;
    }
    return r;
}

struct Buses {
    int main_l[kSamples], main_r[kSamples], main_s[kSamples];
    int auxa_l[kSamples], auxa_r[kSamples], auxa_s[kSamples];
    int auxb_l[kSamples], auxb_r[kSamples], auxb_s[kSamples];
    int auxc_l[kSamples], auxc_r[kSamples], auxc_s[kSamples];
    int wm[8][kRemoteSamples];  // main0, aux0, main1, aux1, ...
};
Buses g_bus;
u16 g_last_main_volume = 0x8000;
u16 g_last_aux_volume[3] = {0x8000, 0x8000, 0x8000};
u16 g_compressor = 0;

struct BusPtrs {  // where a voice mixes this millisecond (the old versions mix 3 x 1 ms)
    int* regular[12];
    int* remote[8];
};
BusPtrs bus_ptrs() {
    Buses& b = g_bus;
    return {{b.main_l, b.main_r, b.main_s, b.auxa_l, b.auxa_r, b.auxa_s, b.auxb_l, b.auxb_r, b.auxb_s, b.auxc_l, b.auxc_r, b.auxc_s},
            {b.wm[0], b.wm[1], b.wm[2], b.wm[3], b.wm[4], b.wm[5], b.wm[6], b.wm[7]}};
}

void mix_add(int* out, const s16* in, unsigned count, Volume& v, s16& dpop, bool ramp) {
    const u16 delta = ramp ? v.delta : 0;
    for (unsigned i = 0; i < count; ++i) {
        const s16 s = clamp16((int64_t(in[i]) * v.volume) >> 15);
        out[i] += s;
        v.volume = u16(v.volume + delta);
        dpop = s;
    }
}
void low_pass(s16* s, unsigned count, LowPass& f) {
    for (unsigned i = 0; i < count; ++i) f.yn1 = s[i] = clamp16((f.a0 * int32_t(s[i]) + f.b0 * int32_t(f.yn1)) >> 15);
}
void biquad(s16* s, unsigned count, Biquad& f) {
    for (unsigned i = 0; i < count; ++i) {
        const s16 x = s[i];
        int64_t t = int64_t(f.b0) * x + int64_t(f.b1) * f.xn1 + int64_t(f.b2) * f.xn2 + int64_t(f.a1) * f.yn1 + int64_t(f.a2) * f.yn2;
        t <<= 2;
        t += (t & 0x10000) ? 0x8000 : 0x7FFF;
        t >>= 16;
        const s16 y = clamp16(t);
        f.xn2 = f.xn1;
        f.yn2 = f.yn1;
        f.xn1 = x;
        f.yn1 = y;
        s[i] = y;
    }
}

void process_voice(Cpu& c, PB& pb, const BusPtrs& out, unsigned count) {
    if (pb.running != 1) return;
    ++dsp::stats().voices;
    const s16* coefs = g_have_coefs ? &g_coefs[size_t(pb.coef_select) * 0x200] : nullptr;
    s16 s[kSamples];
    Accelerator acc;
    acc.setup(c, pb);
    const uint32_t pos = resample([&](unsigned) { return acc.sample(pb.adpcm.coefs); }, s, count, pb.src.last, pb.src.frac,
                                  hilo(pb.src.ratio_hi, pb.src.ratio_lo), pb.src_type, coefs);
    pb.src.frac = u16(pos);
    pb.addr.cur_hi = u16(acc.current >> 16);
    pb.addr.cur_lo = u16(acc.current);
    pb.adpcm.yn1 = acc.yn1;
    pb.adpcm.yn2 = acc.yn2;
    pb.adpcm.pred_scale = acc.pred_scale;

    for (unsigned i = 0; i < count; ++i) {
        s[i] = clamp16((int32_t(s[i]) * int32_t(u16(pb.env.volume))) >> 15);
        pb.env.volume = s16(pb.env.volume + pb.env.delta);
    }
    if (pb.lpf.on) low_pass(s, count, pb.lpf);
    if (g_version->biquad && pb.biquad.on) biquad(s, count, pb.biquad);

    pb.dpop = {};
    pb.remote_dpop = {};
    const uint32_t m = mixer_control(hilo(pb.mixer_control_hi, pb.mixer_control_lo));
    struct Route { uint32_t on, ramp; int* out; Volume* v; s16* dpop; };
    const Route routes[] = {
        {MainL, MainLRamp, out.regular[0], &pb.mixer.main_l, &pb.dpop.main_l},
        {MainR, MainRRamp, out.regular[1], &pb.mixer.main_r, &pb.dpop.main_r},
        {MainS, MainSRamp, out.regular[2], &pb.mixer.main_s, &pb.dpop.main_s},
        {AuxAL, AuxALRamp, out.regular[3], &pb.mixer.auxa_l, &pb.dpop.auxa_l},
        {AuxAR, AuxARRamp, out.regular[4], &pb.mixer.auxa_r, &pb.dpop.auxa_r},
        {AuxAS, AuxASRamp, out.regular[5], &pb.mixer.auxa_s, &pb.dpop.auxa_s},
        {AuxBL, AuxBLRamp, out.regular[6], &pb.mixer.auxb_l, &pb.dpop.auxb_l},
        {AuxBR, AuxBRRamp, out.regular[7], &pb.mixer.auxb_r, &pb.dpop.auxb_r},
        {AuxBS, AuxBSRamp, out.regular[8], &pb.mixer.auxb_s, &pb.dpop.auxb_s},
        {AuxCL, AuxCLRamp, out.regular[9], &pb.mixer.auxc_l, &pb.dpop.auxc_l},
        {AuxCR, AuxCRRamp, out.regular[10], &pb.mixer.auxc_r, &pb.dpop.auxc_r},
        {AuxCS, AuxCSRamp, out.regular[11], &pb.mixer.auxc_s, &pb.dpop.auxc_s},
    };
    for (const Route& r : routes)
        if (m & r.on) mix_add(r.out, s, count, *r.v, *r.dpop, (m & r.ramp) != 0);

    if (pb.remote) {  // the Wii Remote speakers: 6 kHz
        if (g_version->biquad && pb.remote_filter.on) {
            if (pb.remote_filter.on == 2) biquad(s, count, pb.remote_filter.biquad);
            else low_pass(s, count, pb.remote_filter.lpf);
        }
        const unsigned n = count == kSamples ? kRemoteSamples : 6;
        s16 wm[kRemoteSamples];
        pb.remote_src.frac = u16(resample([&](unsigned i) { return s[i]; }, wm, n, pb.remote_src.last, pb.remote_src.frac, 0x55555, 0, coefs));
        Volume* v[8] = {&pb.remote_mixer.main0, &pb.remote_mixer.aux0, &pb.remote_mixer.main1, &pb.remote_mixer.aux1,
                        &pb.remote_mixer.main2, &pb.remote_mixer.aux2, &pb.remote_mixer.main3, &pb.remote_mixer.aux3};
        s16* d[8] = {&pb.remote_dpop.main0, &pb.remote_dpop.aux0, &pb.remote_dpop.main1, &pb.remote_dpop.aux1,
                     &pb.remote_dpop.main2, &pb.remote_dpop.aux2, &pb.remote_dpop.main3, &pb.remote_dpop.aux3};
        for (unsigned k = 0; k < 8; ++k) {
            const unsigned bits = (pb.remote_mixer_control >> (2 * k)) & 3;
            if (bits) mix_add(out.remote[k], wm, n, *v[k], *d[k], (bits & 2) != 0);
        }
    }
}

void process_list(Cpu& c, uint32_t a) {
    PB pb;
    while (a) {
        read_pb(c, a, pb);
        BusPtrs out = bus_ptrs();
        if (g_version->old_commands && (pb.updates.count[0] | pb.updates.count[1] | pb.updates.count[2])) {
            // Parameter changes timed to the millisecond: 32 (offset, value) pairs in memory.
            u16 upd[64];
            const uint8_t* p = dsp::physical(&c, hilo(pb.updates.data_hi, pb.updates.data_lo), sizeof upd);
            for (unsigned i = 0; i < 64; ++i) upd[i] = u16((p[2 * i] << 8) | p[2 * i + 1]);
            const u16 counts[3] = {pb.updates.count[0], pb.updates.count[1], pb.updates.count[2]};
            unsigned first = 0;
            for (unsigned ms = 0; ms < 3; ++ms) {
                const unsigned n = counts[ms];
                if (first < 32 && n <= 32 - first)
                    for (unsigned i = first; i < first + n; ++i) {
                        const u16 off = upd[2 * i], value = upd[2 * i + 1];
                        if (off < kPbWords) {
                            u16 w[kPbWords];
                            std::memcpy(w, &pb, sizeof pb);
                            w[off] = value;
                            std::memcpy(&pb, w, sizeof pb);
                        }
                    }
                first += n;
                process_voice(c, pb, out, 32);
                for (auto& r : out.regular) r += 32;
                for (auto& r : out.remote) r += 6;
            }
        } else {
            process_voice(c, pb, out, kSamples);
        }
        write_pb(c, a, pb);
        a = hilo(pb.next_hi, pb.next_lo);
    }
}

// --- the commands ---

void read_ints(Cpu& c, int* dst, uint32_t a, unsigned n) {
    const uint8_t* p = dsp::physical(&c, a, n * 4);
    for (unsigned i = 0; i < n; ++i) dst[i] = int32_t((uint32_t(p[4 * i]) << 24) | (uint32_t(p[4 * i + 1]) << 16) | (uint32_t(p[4 * i + 2]) << 8) | p[4 * i + 3]);
}
void write_ints(Cpu& c, uint32_t a, const int* src, unsigned n) {
    uint8_t* p = dsp::physical(&c, a, n * 4);
    for (unsigned i = 0; i < n; ++i) {
        const uint32_t v = uint32_t(src[i]);
        p[4 * i] = uint8_t(v >> 24);
        p[4 * i + 1] = uint8_t(v >> 16);
        p[4 * i + 2] = uint8_t(v >> 8);
        p[4 * i + 3] = uint8_t(v);
    }
}
void volume_ramp(u16* out, u16 from, u16 to, unsigned n) {
    float v = from;
    for (unsigned i = 0; i < n; ++i) {
        v += float(int(to) - int(from)) / float(n);
        out[i] = u16(v);
    }
}

void setup(Cpu& c, uint32_t a) {  // each bus starts as value + i * delta
    int* regular[12];
    int* remote[8];
    const BusPtrs b = bus_ptrs();
    std::copy(std::begin(b.regular), std::end(b.regular), regular);
    std::copy(std::begin(b.remote), std::end(b.remote), remote);
    const uint8_t* p = dsp::physical(&c, a, 20 * 6);
    for (unsigned i = 0; i < 20; ++i) {
        const auto w = [&](unsigned k) { return u16((p[6 * i + 2 * k] << 8) | p[6 * i + 2 * k + 1]); };
        const int32_t value = int32_t((uint32_t(w(0)) << 16) | w(1));
        const s16 delta = s16(w(2));
        int* buf = i < 12 ? regular[i] : remote[i - 12];
        const unsigned n = i < 12 ? kSamples : kRemoteSamples;
        for (unsigned k = 0; k < n; ++k) buf[k] = value == 0 ? 0 : value + int(k) * delta;
    }
}
void add_to_lr(Cpu& c, uint32_t a, int l_sign, int r_sign, bool two) {
    int v[2 * kSamples];
    read_ints(c, v, a, two ? 2 * kSamples : kSamples);
    for (unsigned i = 0; i < kSamples; ++i) {
        if (two) {
            g_bus.main_l[i] += v[i];
            g_bus.main_r[i] -= v[kSamples + i];
        } else {
            g_bus.main_l[i] += l_sign * v[i];
            g_bus.main_r[i] += r_sign * v[i];
        }
    }
}
void mix_aux(Cpu& c, unsigned aux, uint32_t write, uint32_t read, u16 volume) {
    u16 ramp[kSamples];
    volume_ramp(ramp, g_last_aux_volume[aux], volume, kSamples);
    g_last_aux_volume[aux] = volume;
    int* const src[3][3] = {{g_bus.auxa_l, g_bus.auxa_r, g_bus.auxa_s}, {g_bus.auxb_l, g_bus.auxb_r, g_bus.auxb_s}, {g_bus.auxc_l, g_bus.auxc_r, g_bus.auxc_s}};
    if (write)
        for (unsigned k = 0; k < 3; ++k) write_ints(c, write + k * kSamples * 4, src[aux][k], kSamples);
    int* const main[3] = {g_bus.main_l, g_bus.main_r, g_bus.main_s};
    int back[kSamples];
    for (unsigned k = 0; k < 3; ++k) {
        read_ints(c, back, read + k * kSamples * 4, kSamples);
        for (unsigned i = 0; i < kSamples; ++i) main[k][i] += int32_t((int64_t(back[i]) * ramp[i]) >> 15);
    }
}
void upload_aux_mix_lrsc(Cpu& c, unsigned aux, const uint32_t* addr, u16 volume) {
    int* l = aux ? g_bus.auxb_l : g_bus.auxa_l;
    int* r = aux ? g_bus.auxb_r : g_bus.auxa_r;
    int* s = aux ? g_bus.auxb_s : g_bus.auxa_s;
    write_ints(c, addr[0], l, kSamples);
    write_ints(c, addr[0] + kSamples * 4, r, kSamples);
    write_ints(c, addr[0] + 2 * kSamples * 4, s, kSamples);
    write_ints(c, addr[1], aux ? g_bus.auxc_s : g_bus.auxc_r, kSamples);
    u16 ramp[kSamples];
    volume_ramp(ramp, g_last_aux_volume[aux], volume, kSamples);
    g_last_aux_volume[aux] = volume;
    int* const dest[4] = {g_bus.main_l, g_bus.main_r, g_bus.main_s, g_bus.auxc_l};
    for (unsigned k = 0; k < 4; ++k) {
        read_ints(c, l, addr[2 + k], kSamples);  // the aux bus's left buffer is the scratch, as on the DSP
        for (unsigned i = 0; i < kSamples; ++i) dest[k][i] += int32_t((int64_t(l[i]) * ramp[i]) >> 15);
    }
}
void compressor(Cpu& c, u16 threshold, u16 release_frames, uint32_t table) {
    bool over = false;
    for (unsigned i = 0; i < kSamples && !over; ++i)
        over = std::abs(g_bus.main_l[i]) > int(threshold) || std::abs(g_bus.main_r[i]) > int(threshold);
    constexpr uint32_t kFrameBytes = kSamples * 2, kAttackEntries = 11;
    uint32_t offset;
    if (over) {
        offset = g_compressor * kFrameBytes;
        g_compressor = release_frames;
    } else if (g_compressor) {
        --g_compressor;
        offset = (kAttackEntries + g_compressor) * kFrameBytes;
    } else {
        return;
    }
    const uint8_t* p = dsp::physical(&c, table + offset, kFrameBytes);
    for (unsigned i = 0; i < kSamples; ++i) {
        const u16 k = u16((p[2 * i] << 8) | p[2 * i + 1]);
        g_bus.main_l[i] = int32_t((int64_t(g_bus.main_l[i]) * k) >> 15);
        g_bus.main_r[i] = int32_t((int64_t(g_bus.main_r[i]) * k) >> 15);
    }
}
void output(Cpu& c, uint32_t lr, uint32_t surround, u16 volume, bool with_auxc) {
    u16 ramp[kSamples];
    volume_ramp(ramp, g_last_main_volume, volume, kSamples);
    g_last_main_volume = volume;
    write_ints(c, surround, g_bus.main_s, kSamples);
    if (with_auxc) write_ints(c, surround + kSamples * 4, g_bus.auxc_l, kSamples);
    uint8_t* p = dsp::physical(&c, lr, kSamples * 4);
    for (unsigned i = 0; i < kSamples; ++i) {
        const s16 l = clamp16((int64_t(g_bus.main_l[i]) * ramp[i]) >> 15);
        const s16 r = clamp16((int64_t(g_bus.main_r[i]) * ramp[i]) >> 15);
        g_bus.main_l[i] = l;
        g_bus.main_r[i] = r;
        p[4 * i] = uint8_t(u16(r) >> 8);  // right, then left
        p[4 * i + 1] = uint8_t(r);
        p[4 * i + 2] = uint8_t(u16(l) >> 8);
        p[4 * i + 3] = uint8_t(l);
    }
    dsp::push_mail(0xDCD10004, true);  // DSP_SYNC
}
void output_remotes(Cpu& c, const uint32_t* addr) {
    for (unsigned k = 0; k < 4; ++k) {
        uint8_t* p = dsp::physical(&c, addr[k], kRemoteSamples * 2);
        for (unsigned i = 0; i < kRemoteSamples; ++i) {
            const u16 s = u16(clamp16(g_bus.wm[2 * k][i]));
            p[2 * i] = uint8_t(s >> 8);
            p[2 * i + 1] = uint8_t(s);
        }
    }
}

u16 g_list[512];

void run_list(Cpu& c) {
    // The two numberings: the old one has a separate "PB address" command and
    // an output without volume; otherwise the same operations.
    enum Op { Setup, AddAdd1, SubAdd1, AddSub2, PbAddr, Process, MixA, MixB, MixC, UplA, UplB, Compress, Out, OutDpl2, OutWm, End };
    static constexpr Op kNew[] = {Setup, AddAdd1, SubAdd1, AddSub2, Process, MixA, MixB, MixC, UplA, UplB, Compress, Out, OutDpl2, OutWm, End};
    static constexpr Op kOld[] = {Setup, AddAdd1, SubAdd1, AddSub2, PbAddr, Process, MixA, MixB, MixC, UplA, UplB, Compress, Out, OutDpl2, OutWm, End};
    const bool old = g_version->old_commands;
    unsigned i = 0;
    uint32_t pb = 0;
    const auto next = [&] {
        if (i >= std::size(g_list)) rt::fatal(&c, "an AX command list runs past 512 halfwords");
        return g_list[i++];
    };
    const auto addr = [&] {
        const u16 hi = next();
        return hilo(hi, next());
    };
    for (;;) {
        const u16 cmd = next();
        const size_t n = old ? std::size(kOld) : std::size(kNew);
        if (cmd >= n) rt::fatal(&c, "AX command %u: not known to this microcode", cmd);
        switch ((old ? kOld : kNew)[cmd]) {
            case Setup: setup(c, addr()); break;
            case AddAdd1: add_to_lr(c, addr(), 1, 1, false); break;
            case SubAdd1: add_to_lr(c, addr(), -1, 1, false); break;
            case AddSub2: add_to_lr(c, addr(), 0, 0, true); break;
            case PbAddr: pb = addr(); break;
            case Process: process_list(c, old ? pb : addr()); break;
            case MixA: case MixB: case MixC: {
                const unsigned aux = cmd - (old ? 6u : 5u);
                const u16 volume = next();
                const uint32_t w = addr();
                mix_aux(c, aux, w, addr(), volume);
                break;
            }
            case UplA: case UplB: {
                const unsigned aux = cmd - (old ? 9u : 8u);
                const u16 volume = next();
                uint32_t a[6];
                for (auto& x : a) x = addr();
                upload_aux_mix_lrsc(c, aux, a, volume);
                break;
            }
            case Compress: {
                const u16 threshold = next();
                const u16 frames = next();
                compressor(c, threshold, frames, addr());
                break;
            }
            case Out: case OutDpl2: {
                const u16 volume = (old || g_version->fixed_output) ? 0x8000 : next();
                const uint32_t surround = addr();
                output(c, addr(), surround, volume, (old ? kOld : kNew)[cmd] == OutDpl2);
                break;
            }
            case OutWm: {
                uint32_t a[4];
                for (auto& x : a) x = addr();
                output_remotes(c, a);
                break;
            }
            case End: return;
        }
    }
}

// --- mail ---

enum class State { ListSize, ListAddress, NextTask, Upload };
State g_state = State::ListSize;
uint16_t g_list_size = 0;

}  // namespace

bool known(uint32_t crc) {
    for (const Version& v : kVersions)
        if (v.crc == crc) return true;
    return false;
}

void start(uint32_t crc) {
    for (const Version& v : kVersions)
        if (v.crc == crc) g_version = &v;
    rt::log("dsp: AX as in %s", g_version->what);
    std::ifstream f(qilin::host::user_folder() / "dsp_coef.bin", std::ios::binary);
    char raw[0x1000];
    if (f && f.read(raw, sizeof raw) && f.gcount() == sizeof raw) {
        for (size_t i = 0; i < g_coefs.size(); ++i) g_coefs[i] = s16((uint8_t(raw[2 * i]) << 8) | uint8_t(raw[2 * i + 1]));
        g_have_coefs = true;
        rt::log("dsp: polyphase resampling with dsp_coef.bin");
    } else {
        rt::log("dsp: no dsp_coef.bin in the user folder: polyphase voices are resampled linearly");
    }
    g_state = State::ListSize;
    dsp::push_mail(0xDCD10000, true);  // DSP_INIT
}

void mail(Cpu& c, uint32_t m) {
    switch (g_state) {
        case State::ListSize:
            if ((m & 0xFFFF0000u) != 0xBABE0000u) rt::fatal(&c, "AX expected a command list and got mail %08X", m);
            g_list_size = static_cast<uint16_t>(m);
            if (g_list_size >= std::size(g_list)) rt::fatal(&c, "an AX command list of %u halfwords: more than 512", g_list_size);
            g_state = State::ListAddress;
            return;
        case State::ListAddress: {
            const uint8_t* p = dsp::physical(&c, m, g_list_size * 2u);
            for (unsigned i = 0; i < g_list_size; ++i) g_list[i] = u16((p[2 * i] << 8) | p[2 * i + 1]);
            ++dsp::stats().command_lists;
            run_list(c);
            dsp::push_mail(0xDCD10002, true);  // DSP_YIELD: the list is done
            g_state = State::NextTask;
            return;
        }
        case State::NextTask:
            m = 0xCDD10000u | (m & 0xFFFF);  // the task manager's mails
            switch (m) {
                case 0xCDD10000: dsp::push_mail(0xDCD10001, true); g_state = State::ListSize; return;  // resume
                case 0xCDD10001: rt::fatal(&c, "a DSP task asked AX to load another microcode: not handled");
                case 0xCDD10002: dsp::reset_to_rom(); return;
                case 0xCDD10003: g_state = State::ListSize; return;  // continue
                default: rt::fatal(&c, "AX got task mail %08X: not known", m);
            }
        case State::Upload: return;
    }
}

}  // namespace punchout::ax
