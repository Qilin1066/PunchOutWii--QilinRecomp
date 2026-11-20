// The video interface's replacements: the retrace, as a timed interrupt from
// the host clock, and the frame buffer the game hands over each frame, which
// the renderer will present (roadmap step 4).
//
// The rate is 60 Hz: the console's settings report EuRGB60 (sc.cpp), which
// this game supports, so it runs at the NTSC rate. VIGetTvFormat says so too.
#include <algorithm>
#include <atomic>

#include "boot.hpp"
#include "recorder.hpp"
#include "audio.hpp"
#include "gpu.hpp"
#include "video.hpp"
#include "os.hpp"
#include "threads.hpp"

namespace punchout {
namespace rt = qilin::rt;
using rt::Cpu;

namespace {

constexpr uint64_t kRetraceTicks = kTimerHz / 60;  // 60 Hz until the main thread matches it to the display
// The retrace period in time-base ticks: set by the main thread, read by the game's.
std::atomic<uint64_t> g_period{kRetraceTicks};
constexpr uint32_t kVI_EURGB60 = 5;
constexpr uint32_t kRetraceQueue = 0x00000100u;  // threads in VIWaitForRetrace (a key of our own)

uint32_t g_pre_cb = 0, g_post_cb = 0;
uint32_t g_retraces = 0;       // VIGetRetraceCount
uint64_t g_next_retrace = 0;   // time base of the next retrace
uint32_t g_next_fb = 0, g_current_fb = 0;
uint32_t g_frames = 0;  // frame buffers handed over
std::atomic<uint32_t> g_frames_seen{0};  // the same, for other threads
bool g_black = true;
uint32_t g_mode = 0;           // the GXRenderModeObj last configured

// How on time the retraces are (the host's timers and the guest's load decide):
// for the five-second report, and the frame-time overlay.
struct Lateness {
    uint64_t retraces = 0, late_1ms = 0, late_4ms = 0, resyncs = 0;
    uint64_t worst_us = 0, total_us = 0;
};
Lateness g_late;

// Every five seconds: what the GPU's front end has seen.
void report() {
    static Stats last;
    static uint32_t last_frames = 0;
    const Stats& s = gpu_stats();
    rt::log("%u retraces: %u frame buffers shown, %llu draws, %llu vertices, %llu display lists, %llu XFB and %llu texture copies, %llu KB of commands (last 5 s)",
            g_retraces, g_frames - last_frames, static_cast<unsigned long long>(s.draws - last.draws),
            static_cast<unsigned long long>(s.vertices - last.vertices), static_cast<unsigned long long>(s.display_lists - last.display_lists),
            static_cast<unsigned long long>(s.xfb_copies - last.xfb_copies), static_cast<unsigned long long>(s.texture_copies - last.texture_copies),
            static_cast<unsigned long long>((s.fifo_bytes - last.fifo_bytes) / 1024));
    last = s;
    last_frames = g_frames;
    static AudioStats a_last;
    const AudioStats& a = audio_stats();
    rt::log("  sound: %llu DMA buffers, %llu command lists, %llu voice-frames, %llu mails in and %llu out",
            static_cast<unsigned long long>(a.dma_periods - a_last.dma_periods), static_cast<unsigned long long>(a.command_lists - a_last.command_lists),
            static_cast<unsigned long long>(a.voices - a_last.voices), static_cast<unsigned long long>(a.mails_in - a_last.mails_in),
            static_cast<unsigned long long>(a.mails_out - a_last.mails_out));
    a_last = a;
    if (g_late.retraces) {
        rt::log("  retraces on time: average %.2f ms late, worst %.2f ms; %llu over 1 ms late, %llu over 4 ms; %llu a frame behind (restarted)",
                double(g_late.total_us) / 1000.0 / double(g_late.retraces), double(g_late.worst_us) / 1000.0,
                static_cast<unsigned long long>(g_late.late_1ms), static_cast<unsigned long long>(g_late.late_4ms),
                static_cast<unsigned long long>(g_late.resyncs));
        g_late = {};
    }
    // Drawing: per frame, the guest thread's recording and the main thread's drawing.
    struct Drawing {
        uint64_t frames, record, wait, lookups, hash, decoded, bytes, decode, rendered, render, gpu, made, gpu_ns, gpu_timed;
    };
    const gx::RecorderStats& g = gx::recorder_stats();
    const auto rd = [](const std::atomic<uint64_t>& v) { return v.load(std::memory_order_relaxed); };
    const Drawing now{rd(g.frames),   rd(g.record_ns), rd(g.wait_ns), rd(g.texture_lookups), rd(g.hash_ns),
                      rd(g.textures_decoded), rd(g.texture_bytes), rd(g.decode_ns), rd(g.rendered), rd(g.render_ns), rd(g.gpu_wait_ns), rd(g.images_made),
                      rd(g.gpu_ns), rd(g.gpu_timed)};
    static Drawing d_last{};
    if (now.frames != d_last.frames) {
        const double frames = double(now.frames - d_last.frames), rendered = double(std::max<uint64_t>(1, now.rendered - d_last.rendered));
        const auto ms = [&](uint64_t later, uint64_t earlier, double n) { return double(later - earlier) / 1e6 / n; };
        rt::log("  drawing, per frame: recording %.2f ms (checking %.0f textures %.2f ms, decoding %.1f %.2f ms), "
                "waiting for the main thread %.2f ms; the main thread drawing %.2f ms, of which waiting for the GPU %.2f ms; "
                "%llu KB of textures decoded; %llu textures on the GPU, %llu spare, %llu images made",
                ms(now.record, d_last.record, frames), double(now.lookups - d_last.lookups) / frames, ms(now.hash, d_last.hash, frames),
                double(now.decoded - d_last.decoded) / frames, ms(now.decode, d_last.decode, frames), ms(now.wait, d_last.wait, frames),
                ms(now.render, d_last.render, rendered), ms(now.gpu, d_last.gpu, rendered), static_cast<unsigned long long>((now.bytes - d_last.bytes) / 1024),
                static_cast<unsigned long long>(rd(g.gpu_textures)), static_cast<unsigned long long>(rd(g.gpu_spares)),
                static_cast<unsigned long long>(now.made - d_last.made));
        if (now.gpu_timed != d_last.gpu_timed)
            rt::log("  the GPU's own time per frame: %.2f ms",
                    ms(now.gpu_ns, d_last.gpu_ns, double(now.gpu_timed - d_last.gpu_timed)));
    }
    d_last = now;
}

// Post the retrace once the host clock reaches it. One at a time: after a
// stall of more than a frame the schedule starts again from now instead of
// posting the missed ones back to back, which would run several game frames
// at once and then stall (one hitch becoming a stagger). The game then runs
// a little behind the wall clock; its frames stay evenly spaced.
void poll_retrace() {
    if (g_next_retrace == 0) return;  // before VIInit
    const uint64_t now = time_base();
    const uint64_t period = g_period.load(std::memory_order_relaxed);
    if (now >= g_next_retrace) {
        const uint64_t late = now - g_next_retrace;
        const uint64_t late_us = late * 1000000 / kTimerHz;
        ++g_late.retraces, g_late.total_us += late_us;
        g_late.worst_us = std::max(g_late.worst_us, late_us);
        if (late_us >= 1000) ++g_late.late_1ms;
        if (late_us >= 4000) ++g_late.late_4ms;
        if (late >= period) {  // a whole frame behind: start again from now
            ++g_late.resyncs;
            g_next_retrace = now + period;
        } else {
            g_next_retrace += period;
        }
        post_interrupt([](Cpu& c) {
            park_if_requested();  // the settings menu's pause: here, between frames
            ++g_retraces;
            if (g_retraces % 300 == 0) report();
            if (g_pre_cb) guest_callback(c, g_pre_cb, g_retraces, 0);
            g_current_fb = g_next_fb;  // VIFlush'd settings take effect at the retrace
            // The render mode's frame buffer size: fbWidth at +4, xfbHeight at +8.
            const uint32_t w = g_mode ? rt::peek16(g_mode + 4) : 0, h = g_mode ? rt::peek16(g_mode + 8) : 0;
            show_frame(g_black ? 0 : g_current_fb, w, h);
            if (g_post_cb) guest_callback(c, g_post_cb, g_retraces, 0);
            wake_all(c, kRetraceQueue);
        });
    }
}

void VIInit(Cpu&) {
    if (g_next_retrace) return;
    g_next_retrace = time_base() + g_period.load(std::memory_order_relaxed);
    add_timer_source(poll_retrace, [] { return g_next_retrace; });
}

void VISetPreRetraceCallback(Cpu& c) {
    const uint32_t was = g_pre_cb;
    g_pre_cb = c.r[3];
    c.r[3] = was;
}
void VISetPostRetraceCallback(Cpu& c) {
    const uint32_t was = g_post_cb;
    g_post_cb = c.r[3];
    c.r[3] = was;
}

// Until the retrace count changes: the thread sleeps, others run, and the
// retrace interrupt wakes it.
void VIWaitForRetrace(Cpu& c) {
    const uint32_t start = g_retraces;
    while (g_retraces == start) wait_on(c, kRetraceQueue);
}

void VIConfigure(Cpu& c) { g_mode = c.r[3]; }
void VIFlush(Cpu&) {}
void VISetNextFrameBuffer(Cpu& c) {
    g_next_fb = c.r[3];
    ++g_frames;
    g_frames_seen.store(g_frames, std::memory_order_relaxed);
}
void VISetBlack(Cpu& c) { g_black = c.r[3] != 0; }
void VIGetRetraceCount(Cpu& c) { c.r[3] = g_retraces; }
void VIGetTvFormat(Cpu& c) { c.r[3] = kVI_EURGB60; }
void VIGetDTVStatus(Cpu& c) { c.r[3] = 0; }  // no component cable: interlaced or EuRGB60 only

// Screen-burn dimming: the Wii dims the picture after minutes without input.
// Not here (a PC monitor isn't a plasma TV), so these only answer.
void VIResetDimmingCount(Cpu& c) { c.r[3] = 1; }
void VIEnableDimming(Cpu&) {}

const rt::Provided kReplacements[] = {
    {0x80213B8Cu, "VIResetDimmingCount", VIResetDimmingCount},
    {0x80215604u, "VIEnableDimming", VIEnableDimming},
    {0x80212028u, "VIInit", VIInit},
    {0x80211C78u, "VISetPreRetraceCallback", VISetPreRetraceCallback},
    {0x80211CBCu, "VISetPostRetraceCallback", VISetPostRetraceCallback},
    {0x80212570u, "VIWaitForRetrace", VIWaitForRetrace},
    {0x80212AF0u, "VIConfigure", VIConfigure},
    {0x80213528u, "VIFlush", VIFlush},
    {0x8021363Cu, "VISetNextFrameBuffer", VISetNextFrameBuffer},
    {0x802136A8u, "VISetBlack", VISetBlack},
    {0x80213720u, "VIGetRetraceCount", VIGetRetraceCount},
    {0x802137B8u, "VIGetTvFormat", VIGetTvFormat},
    {0x80213818u, "VIGetDTVStatus", VIGetDTVStatus},
};

}  // namespace

std::span<const rt::Provided> vi_replacements() { return kReplacements; }

uint32_t frame_count() { return g_frames_seen.load(std::memory_order_relaxed); }

void set_retrace_rate(uint32_t num, uint32_t den) {
    if (num == 0 || den == 0) num = 60, den = 1;
    g_period.store((kTimerHz * den + num / 2) / num, std::memory_order_relaxed);
}

}  // namespace punchout
