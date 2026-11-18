// The video interface's replacements: the retrace, as a timed interrupt from
// the host clock, and the frame buffer the game hands over each frame, which
// the renderer will present (roadmap step 4).
//
// The rate is 60 Hz: the console's settings report EuRGB60 (sc.cpp), which
// this game supports, so it runs at the NTSC rate. VIGetTvFormat says so too.
#include "boot.hpp"
#include "gpu.hpp"
#include "video.hpp"
#include "os.hpp"
#include "threads.hpp"

namespace punchout {
namespace rt = qilin::rt;
using rt::Cpu;

namespace {

constexpr uint64_t kRetraceTicks = kTimerHz / 60;  // 60 Hz (59.94 on hardware: close enough until pacing is tuned)
constexpr uint32_t kVI_EURGB60 = 5;
constexpr uint32_t kRetraceQueue = 0x00000100u;  // threads in VIWaitForRetrace (a key of our own)

uint32_t g_pre_cb = 0, g_post_cb = 0;
uint32_t g_retraces = 0;       // VIGetRetraceCount
uint64_t g_next_retrace = 0;   // time base of the next retrace
uint32_t g_next_fb = 0, g_current_fb = 0;
uint32_t g_frames = 0;  // frame buffers handed over
bool g_black = true;
uint32_t g_mode = 0;           // the GXRenderModeObj last configured

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
}

// Post a retrace for each one the host clock has passed.
void poll_retrace() {
    if (g_next_retrace == 0) return;  // before VIInit
    const uint64_t now = time_base();
    while (now >= g_next_retrace) {
        g_next_retrace += kRetraceTicks;
        post_interrupt([](Cpu& c) {
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
    g_next_retrace = time_base() + kRetraceTicks;
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
}
void VISetBlack(Cpu& c) { g_black = c.r[3] != 0; }
void VIGetRetraceCount(Cpu& c) { c.r[3] = g_retraces; }
void VIGetTvFormat(Cpu& c) { c.r[3] = kVI_EURGB60; }
void VIGetDTVStatus(Cpu& c) { c.r[3] = 0; }  // no component cable: interlaced or EuRGB60 only

const rt::Provided kReplacements[] = {
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

}  // namespace punchout
