// punchout: the recompiled game.
//
//   punchout [--headless | --offscreen] <folder with the extracted disc: sys/main.dol, sys/boot.bin, files/>
//
// The guest runs on threads of its own (threads.cpp); this thread owns the
// window and the GPU: it handles the window's events, draws the frames the
// guest records (renderer.cpp) and shows the frame buffer VI names.
// --headless runs without a window, GPU or sound; --offscreen draws without a
// window (with PUNCHOUT_DUMP=<folder>, every 60th frame shown is saved there as
// PPM; PUNCHOUT_DUMP_EVERY=n changes the 60).
// punchout.ini in the game's data folder holds the settings (settings.hpp),
// which the settings menu (F1 or Esc, menu.hpp) changes; PUNCHOUT_SCALE
// overrides the internal resolution.
// PUNCHOUT_INPUT presses buttons on a schedule, for runs without a player:
// "A@12,Start@20.5" holds each for a fifth of a second at that many seconds
// of game time (the game's frames over 60, so a slow GPU doesn't shift them);
// PUNCHOUT_POINTER="x,y" holds the pointer there (-1 to 1, y up);
// "x,y;x,y@40;x,y@52" moves it there at those many seconds.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <thread>
#include <string>
#include <vector>

#include "boot.hpp"
#include "audio.hpp"
#include "hw.hpp"
#include "gx.hpp"
#include "gpu.hpp"
#include "input.hpp"
#include "menu.hpp"
#include "os.hpp"
#include "priority.hpp"
#include "profile.hpp"
#include "settings.hpp"
#include "save_history.hpp"
#include "speaker.hpp"
#include "build.hpp"
#include "qilin/host/paths.hpp"
#include "qilin/rt/report.hpp"
#include "renderer.hpp"
#include "recorder.hpp"
#include "qilin/host/audio.hpp"
#include "qilin/host/display.hpp"
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>
#include "threads.hpp"
#include "video.hpp"
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <timeapi.h>
#endif

// The lifted code's tables (qilin lift's table.cpp and replaced.cpp), declared
// here rather than through its functions.hpp, so this file builds without the
// lifted code (the Windows download's prebuilt part, tools/player-kit).
namespace qilin::gen {
extern const ::qilin::rt::FunctionEntry kFunctions[];
extern const unsigned kFunctionCount;
extern const ::qilin::rt::ReplacedEntry kReplaced[];
extern const unsigned kReplacedCount;
}  // namespace qilin::gen

namespace {
struct Press {
    double at;
    uint32_t button;
};
std::vector<Press> scripted_input(const char* text) {
    namespace h = qilin::host;
    static constexpr struct {
        const char* name;
        uint32_t bit;
    } kNames[] = {{"Up", h::kPadUp}, {"Down", h::kPadDown}, {"Left", h::kPadLeft}, {"Right", h::kPadRight}, {"A", h::kPadA},
                  {"B", h::kPadB},   {"X", h::kPadX},       {"Y", h::kPadY},       {"Start", h::kPadStart}, {"Back", h::kPadBack},
                  {"Guide", h::kPadGuide}};
    std::vector<Press> out;
    std::string all = text ? text : "";
    size_t at = 0;
    while (at < all.size()) {
        const size_t end = std::min(all.find(',', at), all.size());
        const std::string item = all.substr(at, end - at);
        const size_t sep = item.find('@');
        for (const auto& n : kNames)
            if (sep != std::string::npos && item.substr(0, sep) == n.name) out.push_back({std::atof(item.c_str() + sep + 1), n.bit});
        at = end + 1;
    }
    return out;
}
}  // namespace

namespace {
bool g_display_locked = false;  // the retraces run at the display's refresh (or a whole fraction of it)

// Vblank steering (Beat the Beat's, Windows): with the retraces locked to the
// display, nudge their speed (within 0.2%) so each frame is presented a few
// ms before a vblank, every frame alike: shown as soon after it's drawn as the
// display allows, without the slow drift between the two clocks bringing
// spells where a frame waits most of a refresh. A PI loop on the time from
// the present to the next vblank (the slack), against a target of 1 ms plus
// four standard deviations of its spread, at least 3 ms.
struct VblankSteer {
    double mean = 0, var = 0, integral = 0, delta = 0;
    uint64_t last = 0, frames = 0, late = 0, slack_count = 0;
    double slack_sum = 0, target = 0;
} g_vbl;
void steer_to_vblank() {
    uint64_t vblank = 0, period = 0;
    if (!g_display_locked || !punchout::display_vblank(vblank, period)) return;
    const auto now = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    const double dt = g_vbl.last ? double(now - g_vbl.last) : 0;
    g_vbl.last = now;
    if (dt <= 0 || dt > 4.0 * 1e9 / 59.0) {  // a long gap (loading, the menu): start the averages again
        g_vbl.frames = 0;
        return;
    }
    const double p = double(period);
    double since = std::fmod(double(int64_t(now - vblank)), p);
    if (since < 0) since += p;
    const double slack = p - since;  // to the next vblank
    g_vbl.target = std::clamp(1e6 + 4.0 * std::sqrt(g_vbl.var), 3.0e6, p / 2);
    double e = slack - g_vbl.target;
    if (e > p / 2) e -= p;
    if (e < -p / 2) e += p;
    if (g_vbl.frames >= 16 && std::abs(g_vbl.mean) < 0.5e6 && slack > p - 0.5e6) ++g_vbl.late;  // just missed one
    ++g_vbl.frames;
    constexpr double a = 1.0 / 16;  // about a quarter of a second
    if (g_vbl.frames == 1) g_vbl.mean = e, g_vbl.var = 0;
    const double d = e - g_vbl.mean;
    g_vbl.mean += a * d;
    g_vbl.var = (1 - a) * (g_vbl.var + a * d * d);
    if (g_vbl.frames < 16) return;
    g_vbl.slack_sum += slack, ++g_vbl.slack_count;
    // Presenting early (slack above the target) slows the game a little. Kp 1/s, Ki 0.25/s^2.
    const double es = g_vbl.mean / 1e9, dts = dt / 1e9;
    g_vbl.integral = std::clamp(g_vbl.integral + 0.25 * es * dts, -0.001, 0.001);
    g_vbl.delta = std::clamp(-(1.0 * es + g_vbl.integral), -0.002, 0.002);
    if (g_vbl.frames % 4 == 0) punchout::set_retrace_speed(1.0 + g_vbl.delta);
}

// The retrace rate for the display the window is on (video.hpp's
// set_retrace_rate): its refresh, or a whole fraction of it, when that is
// within 0.3 Hz of 60; otherwise, and without vsync, a free 60 Hz (a VRR
// display follows that on its own). Logged when it changes.
void match_retrace_to_display(qilin::host::Display& display) {
    namespace rt = qilin::rt;
    static uint64_t chosen = 0;  // num << 32 | den
    uint32_t num = 60, den = 1;
    double display_hz = 0;
    bool matched = false;
    if (display.picture().vsync && !display.picture().variable_refresh) {  // variable refresh follows the game's own 60 Hz
        auto* window = static_cast<SDL_Window*>(display.sdl_window());
        const SDL_DisplayMode* m = window ? SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window)) : nullptr;
        if (m) {
            uint32_t n = static_cast<uint32_t>(m->refresh_rate_numerator), d = static_cast<uint32_t>(m->refresh_rate_denominator);
            if (n == 0 || d == 0) n = static_cast<uint32_t>(std::lround(m->refresh_rate * 1000.0)), d = 1000;
            display_hz = d ? double(n) / double(d) : 0;
            for (uint32_t k = 1; k <= 4 && n && d; ++k)
                if (std::fabs(display_hz / k - 60.0) <= 0.3) {
                    num = n, den = d * k;
                    matched = true;
                    break;
                }
        }
    }
    g_display_locked = matched;
    punchout::g_display_hz.store(display_hz > 0 ? display_hz : 0, std::memory_order_relaxed);
    const uint64_t key = uint64_t(num) << 32 | den;
    if (key == chosen) return;
    chosen = key;
    punchout::set_retrace_rate(num, den);
    if (num == 60 && den == 1)
        rt::log("retraces at 60 Hz%s", display_hz > 0 ? " (the display's refresh isn't near a multiple of 60 Hz)" : "");
    else
        rt::log("retraces at %.3f Hz, matching the display's %.3f Hz refresh", double(num) / double(den), display_hz);
}
}  // namespace

// Passes of the main loop, for the watchdog.
static std::atomic<uint64_t> g_main_passes{0};

// A thread that only watches: says in the log when the game draws no new
// frame for 10 s while not paused (stuck, or a long load), and when the main
// loop (input, drawing, presenting) stops for 5 s, and when each goes again.
// It changes nothing: the log is for working out what happened.
static void start_watchdog() {
    std::thread([] {
        namespace rt = qilin::rt;
        punchout::set_this_thread_priority(punchout::Priority::background);
        uint32_t frames = punchout::frame_count();
        uint64_t passes = g_main_passes.load(std::memory_order_relaxed);
        unsigned game_still = 0, main_still = 0;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            const uint32_t f = punchout::frame_count();
            if (f != frames || punchout::game_parked()) {
                if (game_still >= 10) rt::log("watchdog: the game draws again, after %u s without a new frame", game_still);
                frames = f, game_still = 0;
            } else if (++game_still == 10) {
                rt::log("watchdog: the game has drawn no new frame for 10 s (stuck, or a long load)");
            }
            const uint64_t p = g_main_passes.load(std::memory_order_relaxed);
            if (p != passes) {
                if (main_still >= 5) rt::log("watchdog: the main loop runs again, after %u s stopped", main_still);
                passes = p, main_still = 0;
            } else if (++main_still == 5) {
                rt::log("watchdog: the main loop (input, drawing, presenting) has been stopped for 5 s");
            }
        }
    }).detach();
}

// The F3 readout's details (each second) and the timing log (each 5 s).
static void report_timing(qilin::host::Display* display, punchout::Menu* menu, qilin::host::AudioOut* audio) {
    namespace rt = qilin::rt;
    using clock = std::chrono::steady_clock;
    static auto next = clock::now() + std::chrono::seconds(1);
    static unsigned seconds = 0;
    const auto t = clock::now();
    if (t < next) return;
    next = t + std::chrono::seconds(1);
    // Per frame over the last second: the game's recording, the main thread's
    // drawing, the GPU's own time, and waiting to present.
    struct Totals {
        uint64_t frames, record, rendered, render, gpu_ns, gpu_timed;
    };
    const punchout::gx::RecorderStats& g = punchout::gx::recorder_stats();
    const auto rd = [](const std::atomic<uint64_t>& v) { return v.load(std::memory_order_relaxed); };
    const Totals now{rd(g.frames), rd(g.record_ns), rd(g.rendered), rd(g.render_ns), rd(g.gpu_ns), rd(g.gpu_timed)};
    static Totals last = now;
    const auto per = [](uint64_t a, uint64_t b, uint64_t n) { return n ? double(a - b) / 1e6 / double(n) : 0.0; };
    const double record = per(now.record, last.record, now.frames - last.frames);
    const double draw = per(now.render, last.render, now.rendered - last.rendered);
    const double gpu_ms = per(now.gpu_ns, last.gpu_ns, now.gpu_timed - last.gpu_timed);
    last = now;
    double present_wait = 0.0;
    if (display) {
        const auto ps = display->take_present_stats();
        present_wait = ps.presents ? ps.wait_ms / ps.presents : 0.0;
    }
    // Sound: kept over the 5 s for the log.
    static qilin::host::AudioOut::Stats sound{};
    static uint64_t sound_pushes = 0;
    static double sound_sum = 0;
    double sound_now = 0;
    if (audio) {
        const auto a = audio->take_stats();
        sound_now = a.average_ms;
        sound_sum += a.average_ms * a.pushes, sound_pushes += a.pushes;
        sound.max_ms = std::max(sound.max_ms, a.max_ms);
        sound.empty += a.empty, sound.dropped += a.dropped, sound.dropped_ms += a.dropped_ms, sound.ratio = a.ratio;
    }
    if (menu && menu->frame_times_shown()) {
        char a[96], b[96];
        std::snprintf(a, sizeof a, "game %.1f ms  draw %.1f ms  GPU %.1f ms", record, draw, gpu_ms);
        std::snprintf(b, sizeof b, "present wait %.1f ms  sound %.0f ms queued", present_wait, sound_now);
        menu->set_frame_details({a, b});
    }
    if (++seconds % 5) return;
    const auto ft = display ? display->frame_times().summary() : qilin::host::FrameTimes::Summary{};
    if (ft.frames)
        rt::log("  frame times (last %.0f s): %.1f fps (%.2f ms), 1%% low %.1f fps, 0.1%% low %.1f fps, worst %.1f ms; waiting to present %.2f ms a frame",
                ft.average_ms * double(ft.frames) / 1000.0, ft.fps, ft.average_ms, ft.low1_fps, ft.low01_fps, ft.worst_ms, present_wait);
    if (g_vbl.slack_count) {
        rt::log("  vblank steering: presented %.2f ms before the vblank on average (target %.2f), %llu just missed; game clock %+.3f%%",
                g_vbl.slack_sum / double(g_vbl.slack_count) / 1e6, g_vbl.target / 1e6, static_cast<unsigned long long>(g_vbl.late),
                g_vbl.delta * 100.0);
        g_vbl.slack_sum = 0, g_vbl.slack_count = 0, g_vbl.late = 0;
    }
    if (audio && sound_pushes) {
        rt::log("  sound: %.1f ms queued on average (aiming for %.0f), %.1f at most, played at %.4fx; ran dry %u times, dropped %.0f ms in %u times",
                sound_sum / double(sound_pushes), audio->target_ms(), sound.max_ms, sound.ratio, sound.empty, sound.dropped_ms, sound.dropped);
        sound = {}, sound_sum = 0, sound_pushes = 0;
    }
}

// --check-speaker: the remote speaker's decoder (speaker.hpp) against the encoder in the game's
// own code (the SDK's WENC, lifted: (state, 0 first or 1 going on, samples, count, bytes out)).
// The encoder keeps the signal as the remote will decode it, and its step, in its state: after
// every block of every test signal both must be exactly the decoder's, and they are looked for
// there (wherever the SDK keeps them). No game runs: only that one function.
static int check_speaker_decoder() {
    namespace rt = qilin::rt;
    constexpr uint32_t kEncode = 0x802166B4u, kState = 0x80F00000u, kPcm = 0x80F00100u, kOut = 0x80F00200u;
    rt::Cpu c = rt::new_cpu();
    rt::set_thread_cpu(&c);
    unsigned blocks = 0, exact = 0;
    double worst = 0;
    int where_signal = -1, where_step = -1;
    for (int signal = 0; signal < 5; ++signal) {
        punchout::SpeakerDecoder d;
        std::memset(rt::guest_ptr(kState), 0, 0x100);
        uint32_t noise = 12345;
        for (int block = 0; block < 400; ++block) {
            int16_t pcm[40];
            for (int i = 0; i < 40; ++i) {
                const double t = (block * 40 + i) / 6000.0;
                noise = noise * 1664525u + 1013904223u;
                const double v = signal == 0   ? 20000 * std::sin(6.283185307179586 * 440 * t)
                                 : signal == 1 ? 32767 * std::sin(6.283185307179586 * (100 + 2000 * t) * t)
                                 : signal == 2 ? (int32_t(noise >> 16) - 32768) * 0.9
                                 : signal == 3 ? ((block / 3) % 2 ? 32767.0 : -32768.0)
                                               : 0.0;
                pcm[i] = static_cast<int16_t>(std::clamp(v, -32768.0, 32767.0));
                rt::poke16(kPcm + 2 * uint32_t(i), static_cast<uint16_t>(pcm[i]));
            }
            c.r[1] = punchout::build::kStackTop - 0x100, c.r[2] = punchout::build::kSdaBase2, c.r[13] = punchout::build::kSdaBase;
            c.r[3] = kState, c.r[4] = block == 0 ? 0 : 1, c.r[5] = kPcm, c.r[6] = 40, c.r[7] = kOut;
            rt::call(c, kEncode);
            int16_t out[40];
            d.decode(rt::guest_ptr(kOut), 20, out);
            for (int i = 0; i < 40; ++i) worst = std::max(worst, std::fabs(double(out[i]) - pcm[i]));
            // The decoder's signal and step, somewhere in the encoder's state (32-bit words).
            ++blocks;
            bool has_signal = false, has_step = false;
            for (int at = 0; at < 0x40; at += 4) {
                const auto word = static_cast<int32_t>(rt::peek32(kState + uint32_t(at)));
                if (word == d.signal && (where_signal < 0 || where_signal == at) && !has_signal) {
                    if (d.signal != 0) where_signal = at;
                    has_signal = true;
                }
                if (word == d.step && (where_step < 0 || where_step == at)) where_step = at, has_step = true;
            }
            if (has_signal && has_step) ++exact;
            else if (blocks - exact <= 3)
                std::printf("  signal %d, block %d: the decoder has signal %d and step %d; the encoder's state: %08X %08X %08X %08X %08X %08X %08X %08X\n",
                            signal, block, d.signal, d.step, rt::peek32(kState), rt::peek32(kState + 4), rt::peek32(kState + 8),
                            rt::peek32(kState + 12), rt::peek32(kState + 16), rt::peek32(kState + 20), rt::peek32(kState + 24), rt::peek32(kState + 28));
        }
    }
    std::printf("the speaker's decoder against the game's encoder: %u blocks of 40 samples, %u with the encoder's signal and step exactly the "
                "decoder's (kept at +%d and +%d of its state); the codec's own largest error, on a full-scale square wave: %.0f\n",
                blocks, exact, where_signal, where_step, worst);
    std::fflush(nullptr);
    return blocks == exact ? 0 : 1;
}

int main(int argc, char** argv) {
    namespace rt = qilin::rt;
    punchout::set_command_line(argc, argv);  // for the game's reset
    punchout::start_profiler();              // PUNCHOUT_PROFILE, for development
#if defined(_WIN32)
    // Windows' timers tick every 15.6 ms unless a program asks for finer: then a
    // sleep to a retrace could end a whole frame late. Ended with the process.
    timeBeginPeriod(1);
#endif
    punchout::tune_process();  // Windows 11: no throttling when the window is hidden or covered
    // The main loop presents frames: ahead of shader compiles and cache saves.
    punchout::set_this_thread_priority(punchout::Priority::high);
    bool headless = false, offscreen = false;
    const char* folder = nullptr;
    // --bench-record <file.dff> [times]: time the recorder on a captured frame (gpu.hpp).
    const char* bench_dff = nullptr;
    unsigned bench_times = 200;
    if (argc >= 3 && std::strcmp(argv[1], "--bench-record") == 0) {
        bench_dff = argv[2];
        if (argc >= 4) bench_times = unsigned(std::max(1, std::atoi(argv[3])));
        folder = argv[2];
        argc = 1;  // nothing else to read
    }
    // --render-dff <file.dff> <out.ppm> [loops]: draw a captured frame with our renderer (below).
    const char* render_dff = nullptr;
    const char* render_out = nullptr;
    unsigned render_loops = 1;
    if (argc >= 4 && std::strcmp(argv[1], "--render-dff") == 0) {
        render_dff = argv[2], render_out = argv[3];
        if (argc >= 5) render_loops = unsigned(std::max(1, std::atoi(argv[4])));
        folder = argv[2];
        argc = 1;
    }
    // --check-speaker <disc>: the Wii Remote speaker's decoder against the game's own encoder (below).
    bool check_speaker = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--check-speaker") == 0) check_speaker = headless = true;
        else if (std::strcmp(argv[i], "--headless") == 0) headless = true;
        else if (std::strcmp(argv[i], "--offscreen") == 0) offscreen = true;
        else if (!folder) folder = argv[i];
        else folder = nullptr, i = argc;  // too many: usage
    }
    if (!folder) {
        std::fprintf(stderr, "usage: punchout [--headless | --offscreen] <folder with the extracted disc: sys/main.dol, sys/boot.bin, files/>\n"
                             "       punchout --bench-record <file.dff> [times] | --render-dff <file.dff> <out.ppm> [loops]\n"
                             "       punchout --check-speaker <folder>\n");
        return 2;
    }
    // The log, beside the settings: punchout.log for this run, punchout.old.log
    // the run before. On Windows the console closes with the game, so after
    // a crash the file is what's left to read (or send).
    rt::install_crash_handler();
    if (!bench_dff && !render_dff) {  // (a benchmark or a replay leaves the game's log alone)
        const std::filesystem::path dir = qilin::host::user_folder(), log = dir / "punchout.log";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        std::filesystem::rename(log, dir / "punchout.old.log", ec);
        static const std::string log_path = log.string();
        if (std::FILE* f = std::fopen(log_path.c_str(), "w")) {
            rt::set_log_file(f);  // open until the process ends
            rt::set_log_path(log_path.c_str());
        } else {
            std::fprintf(stderr, "punchout: the log %s can't be written: only here\n", log_path.c_str());
        }
    }
    rt::init({std::span(qilin::gen::kFunctions, qilin::gen::kFunctionCount),
              std::span(qilin::gen::kReplaced, qilin::gen::kReplacedCount)});
    rt::provide(punchout::os_replacements());
    rt::provide(punchout::dvd_replacements());
    rt::provide(punchout::vi_replacements());
    rt::provide(punchout::thread_replacements());
    rt::provide(punchout::sc_replacements());
    rt::provide(punchout::wpad_replacements());
    rt::provide(punchout::nand_replacements());
    punchout::init_hardware();
    if (bench_dff) {  // then out at once: the GPU thread is waiting, and exit's destructors would wait for it
        const int code = punchout::bench_recording(bench_dff, bench_times);
        std::fflush(nullptr);
        std::_Exit(code);
    }
    if (render_dff) {
        // A frame captured with PUNCHOUT_DFF, replayed through the recorder and drawn by the
        // renderer with no game running: our picture of the very commands Dolphin's FIFO player
        // draws from the same file (tools/check/picture_check.py compares the two), starting as
        // the player starts and looping as it loops (replay_dff).
        qilin::host::Display::Options o;
        o.title = "Punch-Out!!";
        o.headless = true;
        o.picture.scaling = qilin::host::Scaling::integer;  // a pixel for a pixel
        o.picture.smooth = false;
        qilin::host::Display display(o);
        unsigned scale = 1;
        if (const char* e = std::getenv("PUNCHOUT_SCALE")) scale = static_cast<unsigned>(std::clamp(std::atoi(e), 1, 8));
        punchout::Renderer renderer(display.gpu(), scale, 1);
        renderer.set_true_color(std::getenv("PUNCHOUT_TRUE_COLOR") != nullptr);  // (the console's 18 bits unless asked)
        // Shown at its own size, a pixel for a pixel (4:3 for a 640x480 picture, in the middle of
        // the 1280x720 read back), and written so: Dolphin dumps the same pixels at its raw
        // resolution, and the two compare without either being resampled.
        uint32_t shown_w = 0, shown_h = 0;
        punchout::replay_dff(render_dff, render_loops, [&](punchout::gx::Frame& f) {
            f.captured = true;  // (PUNCHOUT_PROBE follows a pixel through a replayed frame too)
            renderer.render(f);
            for (const auto& c : f.copies)
                if (const qilin::host::Image* x = c.to_xfb ? renderer.xfb(c.dest) : nullptr) {
                    shown_w = x->width / scale, shown_h = x->height / scale;
                    display.present_image(x->image, x->view, x->width, x->height, float(shown_w) / float(shown_h), shown_h);
                }
        });
        if (!shown_w) rt::fatal(nullptr, "--render-dff: %s has no copy to the screen", render_dff);
        if (shown_w > 1280 || shown_h > 720) rt::fatal(nullptr, "--render-dff: a %ux%u picture doesn't fit the 1280x720 read back", shown_w, shown_h);
        const std::vector<uint8_t> px = display.read_back();
        FILE* fp = std::fopen(render_out, "wb");
        if (!fp) rt::fatal(nullptr, "--render-dff: %s can't be written", render_out);
        std::fprintf(fp, "P6\n%u %u\n255\n", shown_w, shown_h);
        const uint32_t x0 = (1280 - shown_w) / 2, y0 = (720 - shown_h) / 2;
        for (uint32_t y = 0; y < shown_h; ++y)
            for (uint32_t x = 0; x < shown_w; ++x) std::fwrite(&px[(size_t(y0 + y) * 1280 + x0 + x) * 4], 1, 3, fp);
        std::fclose(fp);
        std::fflush(nullptr);
        std::_Exit(0);
    }
    std::string restore_error;
    if (!punchout::apply_save_restore(qilin::host::user_folder(), restore_error))
        rt::fatal(nullptr, "save recovery: %s (the restore request is restore-save.txt in the save folder)", restore_error.c_str());
    punchout::load_game(folder);
    if (check_speaker) std::_Exit(check_speaker_decoder());

    const std::filesystem::path data = qilin::host::user_folder();
    punchout::Settings settings = punchout::load_settings(data);
    punchout::set_console(settings.language, settings.widescreen, settings.sound);
    const float aspect = settings.widescreen ? 16.0f / 9.0f : 4.0f / 3.0f;  // as the console tells the game (sc.cpp)
    std::unique_ptr<qilin::host::Display> display;
    std::unique_ptr<punchout::Renderer> renderer;
    std::unique_ptr<punchout::Menu> menu;
    unsigned renderer_scale = 1;
    if (!headless) {
        qilin::host::Gpu::prefer(settings.gpu);
        qilin::host::Display::Options o;
        o.title = "Punch-Out!!";
        o.headless = offscreen;
        o.fullscreen = settings.fullscreen;
        o.monitor = settings.monitor;
        o.picture = punchout::picture_of(settings);
        display = std::make_unique<qilin::host::Display>(o);
        if (!display->mode_note().empty()) rt::log("%s", display->mode_note().c_str());
        if (auto* w = static_cast<SDL_Window*>(display->sdl_window()))
            if (const SDL_DisplayMode* m = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(w)))
                rt::log("display: %dx%d at %.2f Hz%s", m->w, m->h, double(m->refresh_rate), settings.variable_refresh ? " (variable refresh)" : "");
        unsigned scale = settings.scale;
        if (scale == 0) {  // auto: the smallest whole multiple whose 480 picture lines fill the screen's height
            int h = 0;
            if (auto* w = static_cast<SDL_Window*>(display->sdl_window())) {
                if (const SDL_DisplayMode* m = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(w)))
                    h = int(float(m->h) * (m->pixel_density > 0 ? m->pixel_density : 1.0f));
            }
            scale = h > 0 ? unsigned(std::clamp((h + 479) / 480, 1, 8)) : 3u;
            rt::log("internal resolution auto: %ux for a screen %d pixels high", scale, h);
        }
        if (const char* e = std::getenv("PUNCHOUT_SCALE")) scale = static_cast<unsigned>(std::clamp(std::atoi(e), 1, 8));
        renderer = std::make_unique<punchout::Renderer>(display->gpu(), scale, settings.anisotropy);
        static punchout::Renderer* saving = nullptr;  // what was compiled since the last save, kept for next time
        saving = renderer.get();
        punchout::g_before_exit = [] { saving->save_now(); };
        punchout::gx::set_rendering(true);
        rt::log("drawing with %s, the EFB at %ux%s%s", display->device_name().c_str(), scale,
                display->gpu().dynamic_blend ? "; blend state set per draw (no pipeline to build for a new one)" : "",
                display->gpu().present_wait ? "; waits for each frame to reach the screen" : "");
        renderer_scale = scale;
        if (!offscreen) {
            menu = std::make_unique<punchout::Menu>(*display, settings, data);
            menu->set_running_scale(scale);
        }
    }
    std::unique_ptr<qilin::host::AudioOut> audio;
    // (PUNCHOUT_AUDIO=1: sound without a window too, to test its queue; SDL_AUDIO_DRIVER=dummy plays to nowhere.)
    if ((!headless && !offscreen) || std::getenv("PUNCHOUT_AUDIO")) {
        // (With no window nothing reads SDL's events, so SDL mustn't turn Ctrl+C or a kill into one.)
        if (headless) SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
        audio = std::make_unique<qilin::host::AudioOut>();
        punchout::set_audio_sink([](const int16_t* lr, size_t frames, unsigned rate, void* out) {
            static_cast<qilin::host::AudioOut*>(out)->push(lr, frames, rate);
        }, audio.get());
    }
    punchout::run_on_host_thread([](void*) { punchout::start_game(); }, nullptr);

    // The controls, with each key's name looked up once (again when the menu changes them).
    struct Input {
        punchout::Binding::Kind kind;
        uint32_t pad;
        int key;
    };
    std::vector<Input> bindings[punchout::kControlCount];
    const auto look_up_controls = [&] {
        for (unsigned c = 0; c < punchout::kControlCount; ++c) {
            bindings[c].clear();
            for (const punchout::Binding& b : settings.controls[c]) {
                int key = -1;
                if (b.kind == punchout::Binding::kKey && display && (key = display->key_code(b.key.c_str())) < 0)
                    rt::fatal(nullptr, "punchout.ini: %s = key:%s, but there's no key called '%s'",
                              punchout::control_name(static_cast<punchout::Control>(c)), b.key.c_str(), b.key.c_str());
                bindings[c].push_back({b.kind, b.pad, key});
            }
        }
    };
    look_up_controls();

    const char* dump = offscreen ? std::getenv("PUNCHOUT_DUMP") : nullptr;
    std::ofstream dump_index;
    if (dump) {
        std::filesystem::create_directories(dump);
        dump_index.open(std::filesystem::path(dump) / "frames.csv", std::ios::trunc);
        if (!dump_index) rt::fatal(nullptr, "cannot write capture frame index");
        dump_index << "file,vi_retrace\n";
    }
    const char* every_text = std::getenv("PUNCHOUT_DUMP_EVERY");
    const uint64_t every = every_text ? std::max<uint64_t>(1, std::strtoull(every_text, nullptr, 10)) : 60;
    const std::vector<Press> presses = scripted_input(std::getenv("PUNCHOUT_INPUT"));
    // PUNCHOUT_INPUT2: a second player's controller and its presses (the second Wii Remote is there from the start).
    const bool scripted_player2 = std::getenv("PUNCHOUT_INPUT2") != nullptr;
    const std::vector<Press> presses2 = scripted_input(std::getenv("PUNCHOUT_INPUT2"));
    const auto t0 = std::chrono::steady_clock::now();
    float cursor_x = 0, cursor_y = 0, cursor2_x = 0, cursor2_y = 0;  // the second player's: from the middle
    struct Point {
        double at;
        float x, y;
    };
    // PUNCHOUT_POINTER's schedule, in time order, and PUNCHOUT_POINTER2's for the second player.
    const auto pointer_script = [](const char* variable, float& start_x, float& start_y) {
        std::vector<Point> points;
        const char* p = std::getenv(variable);
        if (!p) return points;
        for (const char* at = p; *at;) {
            Point pt{0, 0, 0};
            int used = 0;
            if (std::sscanf(at, "%f,%f%n", &pt.x, &pt.y, &used) != 2)
                rt::fatal(nullptr, "%s: expected x,y at \"%s\"", variable, at);
            at += used;
            if (*at == '@') {
                if (std::sscanf(at + 1, "%lf%n", &pt.at, &used) != 1) rt::fatal(nullptr, "%s: expected seconds after @", variable);
                at += 1 + used;
            }
            if (!points.empty() && pt.at < points.back().at) rt::fatal(nullptr, "%s: times must increase", variable);
            points.push_back(pt);
            if (*at == ';') ++at;
            else if (*at) rt::fatal(nullptr, "%s: unexpected \"%s\"", variable, at);
        }
        if (!points.empty()) start_x = points[0].x, start_y = points[0].y;
        return points;
    };
    const std::vector<Point> points = pointer_script("PUNCHOUT_POINTER", cursor_x, cursor_y);
    const std::vector<Point> points2 = pointer_script("PUNCHOUT_POINTER2", cursor2_x, cursor2_y);
    auto last = t0;
    bool wait_control_release = false;
    uint64_t sequence = 0, shown = 0;
    std::vector<uint8_t> rgba;
    if (display) start_watchdog();
    for (;;) {
        g_main_passes.fetch_add(1, std::memory_order_relaxed);
        const unsigned player_mode = scripted_player2 ? 1 : settings.second_player;
        if (display) {
            if (player_mode == 2) display->set_player_controllers(settings.controller2, "");
            else display->set_player_controllers(settings.controller1, player_mode == 1 ? settings.controller2 : "");
            display->set_input_thresholds(settings.stick_threshold, settings.trigger_threshold, settings.pointer_deadzone);
            display->set_rumble_strength(settings.rumble_strength);
        }
        if (audio) audio->set_volume(settings.volume, settings.mute_unfocused && display && display->sdl_window() && !display->focused());
        if (display && !display->poll()) {
            rt::log("window closed");
            punchout::before_exit();
            std::fflush(nullptr);
            std::_Exit(0);  // guest threads are parked on host threads: no static destructors
        }
        if (menu) {
            menu->tick();
            if (menu->quit_requested()) {
                rt::log("quit from the settings menu");
                punchout::before_exit();
                std::fflush(nullptr);
                std::_Exit(0);
            }
            if (menu->take_controls_changed()) look_up_controls();
            if (menu->is_open()) {  // the game is parked: the last picture, with the menu over it
                wait_control_release = true;
                punchout::g_controls.store(0, std::memory_order_relaxed);
                punchout::g_controls2.store(0, std::memory_order_relaxed);
                display->rumble(false);
                display->present_again();
                if (!settings.vsync) SDL_Delay(8);  // without vsync the menu needn't spin
                last = std::chrono::steady_clock::now();
                continue;
            }
        }
        if (renderer) renderer->set_anisotropy(settings.anisotropy);  // the menu's, once it's closed (unchanged: nothing to do)
        if (renderer) renderer->set_true_color(settings.true_color);
        punchout::g_speakers_on = settings.speaker;  // (read by the guest's sound thread: a bool, set from here alone)
        {  // (without a display, --headless: the scripted presses and pointer alone)
            // Whose controller is whose (settings.hpp, second_player): with one player every
            // controller is theirs; with two, controllers go by the order each first had a
            // button pressed, and the keyboard and mouse are always player one's.
            const unsigned two = player_mode;
            const int place1 = two == 0 ? (settings.controller1.empty() ? -1 : 0) : two == 1 ? 0 : -2, place2 = two == 1 ? 1 : two == 2 ? 0 : -2;  // -1 all, -2 none
            const uint32_t all = display ? display->buttons() : 0;
            uint32_t held = (place1 == -1 ? all : place1 >= 0 && display ? display->pad_buttons(place1) : 0) | (all & qilin::host::kPadClick);
            uint32_t held2 = place2 >= 0 && display ? display->pad_buttons(place2) : 0;
            const double now = punchout::frame_count() / 60.0;  // game time: see the top
            for (const Press& p : presses)
                if (now >= p.at && now < p.at + 0.2) held |= p.button;
            for (const Press& p : presses2)
                if (now >= p.at && now < p.at + 0.2) held2 |= p.button;
            uint32_t controls = 0, controls2 = 0;
            for (unsigned c = 0; c < punchout::kControlCount; ++c)
                for (const Input& in : bindings[c]) {
                    const bool on = in.kind == punchout::Binding::kPad     ? (held & in.pad) != 0
                                    : in.kind == punchout::Binding::kMouse ? (held & qilin::host::kPadClick) != 0
                                                                           : display && display->key_down(in.key);
                    if (on) controls |= 1u << c;
                    if (in.kind == punchout::Binding::kPad && (held2 & in.pad)) controls2 |= 1u << c;  // the same pad buttons
                }
            if (wait_control_release) {
                if (!(controls | controls2)) wait_control_release = false;
                controls = controls2 = 0;
            }
            punchout::g_controls.store(controls, std::memory_order_relaxed);
            punchout::g_controls2.store(controls2, std::memory_order_relaxed);
            punchout::g_player2.store(scripted_player2 || (place2 >= 0 && display && display->pad_connected(place2)), std::memory_order_relaxed);
            // The pointer: the mouse where it moves, the right stick at a screen's width a second.
            const auto tick = std::chrono::steady_clock::now();
            const float dt = std::clamp(std::chrono::duration<float>(tick - last).count(), 0.0f, 0.05f);
            last = tick;
            float mx, my, sx = 0, sy = 0;
            if (display && display->mouse_moved(mx, my)) cursor_x = mx, cursor_y = my;
            for (const Point& p : points)
                if (now >= p.at && now < p.at + 0.2) cursor_x = p.x, cursor_y = p.y;
            // (With two players each controller's stick moves its own player's pointer; the mouse is player one's.)
            if (display && place1 == -1) display->right_stick(sx, sy);
            else if (display) display->pad_right_stick(place1, sx, sy);
            cursor_x = std::clamp(cursor_x + sx * dt * 2.0f, -1.2f, 1.2f);
            cursor_y = std::clamp(cursor_y + sy * dt * 2.0f, -1.2f, 1.2f);
            punchout::g_pointer_x.store(int32_t(cursor_x * 1000), std::memory_order_relaxed);
            punchout::g_pointer_y.store(int32_t(cursor_y * 1000), std::memory_order_relaxed);
            float sx2 = 0, sy2 = 0;
            if (display) display->pad_right_stick(place2, sx2, sy2);
            for (const Point& p : points2)
                if (now >= p.at && now < p.at + 0.2) cursor2_x = p.x, cursor2_y = p.y;
            cursor2_x = std::clamp(cursor2_x + sx2 * dt * 2.0f, -1.0f, 1.0f);  // (kept on the screen: it has no mouse to bring it back)
            cursor2_y = std::clamp(cursor2_y + sy2 * dt * 2.0f, -1.0f, 1.0f);
            punchout::g_pointer2_x.store(int32_t(cursor2_x * 1000), std::memory_order_relaxed);
            punchout::g_pointer2_y.store(int32_t(cursor2_y * 1000), std::memory_order_relaxed);
            punchout::g_pointer_on.store((!display || !display->sdl_window() || display->focused()) && std::fabs(cursor_x) <= 1.1f && std::fabs(cursor_y) <= 1.1f, std::memory_order_relaxed);
            if (display) {
                const bool r1 = settings.rumble && punchout::g_rumble.load(std::memory_order_relaxed);
                const bool r2 = settings.rumble && punchout::g_rumble2.load(std::memory_order_relaxed);
                if (place1 == -1) {
                    display->rumble(r1);
                } else {
                    display->rumble_pad(place1, r1);  // (a place nobody has: nothing)
                    display->rumble_pad(place2, r2);
                }
            }
        }
        if (!renderer) {  // headless: nothing shows the frames, so they're only waited for, not converted
            if (audio) report_timing(nullptr, nullptr, audio.get());  // (PUNCHOUT_AUDIO: the sound queue's numbers)
            uint32_t xfb, w, h;
            punchout::take_xfb(&sequence, 10, xfb, w, h);
            continue;
        }
        // PUNCHOUT_SPECIALIZE_FROM=<seconds of game time>: the ubershader until
        // then (for comparing the two on the same scene).
        static const double specialize_from = [] {
            const char* t = std::getenv("PUNCHOUT_SPECIALIZE_FROM");
            return t ? std::atof(t) : -1.0;
        }();
        if (specialize_from >= 0) renderer->set_specialization(punchout::frame_count() / 60.0 >= specialize_from);
        // Draw what the guest recorded, then show what VI shows.
        for (int i = 0; i < 2; ++i)  // what's queued (two at most), then on to showing
            if (auto f = punchout::gx::next_frame(i == 0 ? 4 : 0)) {
                renderer->render(*f);
                if (f->captured && display) {  // PUNCHOUT_DFF: our picture of the captured frame, beside it
                    for (const auto& c : f->copies)
                        if (const qilin::host::Image* x = c.to_xfb ? renderer->xfb(c.dest) : nullptr) {
                            display->present_image(x->image, x->view, x->width, x->height, aspect, x->height / renderer_scale);
                            const std::vector<uint8_t> px = display->read_back();
                            std::string name = std::getenv("PUNCHOUT_DFF");
                            name = name.substr(0, name.rfind('@')) + ".ppm";
                            if (FILE* fp = std::fopen(name.c_str(), "wb")) {
                                std::fprintf(fp, "P6\n1280 720\n255\n");
                                for (size_t k = 0; k + 3 < px.size(); k += 4) std::fwrite(&px[k], 1, 3, fp);
                                std::fclose(fp);
                                rt::log("PUNCHOUT_DFF: our picture of the frame: %s", name.c_str());
                            }
                        }
                }
                punchout::gx::recycle_frame(std::move(f));
            }
            else break;
        // Every second: the retrace rate follows the display (it may have moved, or changed mode).
        if (display) {
            static auto next_match = std::chrono::steady_clock::time_point{};
            if (const auto t = std::chrono::steady_clock::now(); t >= next_match) {
                next_match = t + std::chrono::seconds(1);
                match_retrace_to_display(*display);
            }
        }
        // Every second: where a frame's time goes, for the F3 readout; every
        // 5 s, the log has it with how evenly frames reached the screen.
        if (display) report_timing(display.get(), menu.get(), audio.get());
        uint32_t xfb, w, h;
        if (!punchout::take_xfb(&sequence, 0, xfb, w, h)) continue;
        if (const qilin::host::Image* x = xfb ? renderer->xfb(xfb) : nullptr) {
            display->present_image(x->image, x->view, x->width, x->height, aspect, h);
            steer_to_vblank();
        } else {
            uint64_t s2 = sequence - 1;
            if (punchout::take_frame(&s2, 0, rgba, w, h)) display->present(w, h, rgba.data(), aspect, h);
        }
        if (dump && ++shown % every == 0) {
            const std::vector<uint8_t> px = display->read_back();
            char name[512];
            std::snprintf(name, sizeof name, "%s/frame%05llu.ppm", dump, static_cast<unsigned long long>(shown / every));
            if (FILE* fp = std::fopen(name, "wb")) {
                std::fprintf(fp, "P6\n1280 720\n255\n");
                for (size_t i = 0; i + 3 < px.size(); i += 4) std::fwrite(&px[i], 1, 3, fp);
                std::fclose(fp);
                dump_index << std::filesystem::path(name).filename().string() << ',' << sequence << '\n';
                dump_index.flush();
                if (!dump_index) rt::fatal(nullptr, "capture frame index write failed");
                rt::log("dumped %s at %.1f s of game time", name, punchout::frame_count() / 60.0);
            }
        }
    }
}
