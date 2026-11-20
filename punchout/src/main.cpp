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
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <span>
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
#include "qilin/host/paths.hpp"
#include "renderer.hpp"
#include "qilin/host/audio.hpp"
#include "qilin/host/display.hpp"
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
// The retrace rate for the display the window is on (video.hpp's
// set_retrace_rate): its refresh, or a whole fraction of it, when that is
// within 0.3 Hz of 60; otherwise, and without vsync, a free 60 Hz (a VRR
// display follows that on its own). Logged when it changes.
void match_retrace_to_display(qilin::host::Display& display) {
    namespace rt = qilin::rt;
    static uint64_t chosen = 0;  // num << 32 | den
    uint32_t num = 60, den = 1;
    double display_hz = 0;
    if (display.picture().vsync) {
        auto* window = static_cast<SDL_Window*>(display.sdl_window());
        const SDL_DisplayMode* m = window ? SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window)) : nullptr;
        if (m) {
            uint32_t n = static_cast<uint32_t>(m->refresh_rate_numerator), d = static_cast<uint32_t>(m->refresh_rate_denominator);
            if (n == 0 || d == 0) n = static_cast<uint32_t>(std::lround(m->refresh_rate * 1000.0)), d = 1000;
            display_hz = d ? double(n) / double(d) : 0;
            for (uint32_t k = 1; k <= 4 && n && d; ++k)
                if (std::fabs(display_hz / k - 60.0) <= 0.3) {
                    num = n, den = d * k;
                    break;
                }
        }
    }
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

int main(int argc, char** argv) {
    namespace rt = qilin::rt;
    punchout::set_command_line(argc, argv);  // for the game's reset
    punchout::start_profiler();              // PUNCHOUT_PROFILE, for development
#if defined(_WIN32)
    // Windows' timers tick every 15.6 ms unless a program asks for finer: then a
    // sleep to a retrace could end a whole frame late. Ended with the process.
    timeBeginPeriod(1);
#endif
    // The main loop presents frames: ahead of shader compiles and cache saves.
    punchout::set_this_thread_priority(punchout::Priority::high);
    bool headless = false, offscreen = false;
    const char* folder = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--headless") == 0) headless = true;
        else if (std::strcmp(argv[i], "--offscreen") == 0) offscreen = true;
        else if (!folder) folder = argv[i];
        else folder = nullptr, i = argc;  // too many: usage
    }
    if (!folder) {
        std::fprintf(stderr, "usage: punchout [--headless | --offscreen] <folder with the extracted disc: sys/main.dol, sys/boot.bin, files/>\n");
        return 2;
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
    punchout::load_game(folder);

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
        unsigned scale = settings.scale;
        if (const char* e = std::getenv("PUNCHOUT_SCALE")) scale = static_cast<unsigned>(std::clamp(std::atoi(e), 1, 8));
        renderer = std::make_unique<punchout::Renderer>(display->gpu(), scale, settings.anisotropy);
        punchout::gx::set_rendering(true);
        rt::log("drawing with %s, the EFB at %ux", display->device_name().c_str(), scale);
        renderer_scale = scale;
        if (!offscreen) {
            menu = std::make_unique<punchout::Menu>(*display, settings, data);
            menu->set_running_scale(scale);
        }
    }
    std::unique_ptr<qilin::host::AudioOut> audio;
    if (!headless && !offscreen) {
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
                if (b.kind == punchout::Binding::kKey && (key = display->key_code(b.key.c_str())) < 0)
                    rt::fatal(nullptr, "punchout.ini: %s = key:%s, but there's no key called '%s'",
                              punchout::control_name(static_cast<punchout::Control>(c)), b.key.c_str(), b.key.c_str());
                bindings[c].push_back({b.kind, b.pad, key});
            }
        }
    };
    if (display) look_up_controls();

    const char* dump = offscreen ? std::getenv("PUNCHOUT_DUMP") : nullptr;
    const char* every_text = std::getenv("PUNCHOUT_DUMP_EVERY");
    const uint64_t every = every_text ? std::max<uint64_t>(1, std::strtoull(every_text, nullptr, 10)) : 60;
    const std::vector<Press> presses = scripted_input(std::getenv("PUNCHOUT_INPUT"));
    const auto t0 = std::chrono::steady_clock::now();
    float cursor_x = 0, cursor_y = 0;
    struct Point {
        double at;
        float x, y;
    };
    std::vector<Point> points;  // PUNCHOUT_POINTER's schedule, in time order
    if (const char* p = std::getenv("PUNCHOUT_POINTER")) {
        for (const char* at = p; *at;) {
            Point pt{0, 0, 0};
            int used = 0;
            if (std::sscanf(at, "%f,%f%n", &pt.x, &pt.y, &used) != 2)
                rt::fatal(nullptr, "PUNCHOUT_POINTER: expected x,y at \"%s\"", at);
            at += used;
            if (*at == '@') {
                if (std::sscanf(at + 1, "%lf%n", &pt.at, &used) != 1) rt::fatal(nullptr, "PUNCHOUT_POINTER: expected seconds after @");
                at += 1 + used;
            }
            if (!points.empty() && pt.at < points.back().at) rt::fatal(nullptr, "PUNCHOUT_POINTER: times must increase");
            points.push_back(pt);
            if (*at == ';') ++at;
            else if (*at) rt::fatal(nullptr, "PUNCHOUT_POINTER: unexpected \"%s\"", at);
        }
        if (!points.empty()) cursor_x = points[0].x, cursor_y = points[0].y;
    }
    auto last = t0;
    uint64_t sequence = 0, shown = 0;
    std::vector<uint8_t> rgba;
    for (;;) {
        if (display && !display->poll()) {
            rt::log("window closed");
            std::fflush(nullptr);
            std::_Exit(0);  // guest threads are parked on host threads: no static destructors
        }
        if (menu) {
            menu->tick();
            if (menu->quit_requested()) {
                rt::log("quit from the settings menu");
                std::fflush(nullptr);
                std::_Exit(0);
            }
            if (menu->take_controls_changed()) look_up_controls();
            if (menu->is_open()) {  // the game is parked: the last picture, with the menu over it
                punchout::g_controls.store(0, std::memory_order_relaxed);
                display->rumble(false);
                display->present_again();
                if (!settings.vsync) SDL_Delay(8);  // without vsync the menu needn't spin
                last = std::chrono::steady_clock::now();
                continue;
            }
        }
        if (renderer) renderer->set_anisotropy(settings.anisotropy);  // the menu's, once it's closed (unchanged: nothing to do)
        if (display) {
            uint32_t held = display->buttons();
            const double now = punchout::frame_count() / 60.0;  // game time: see the top
            for (const Press& p : presses)
                if (now >= p.at && now < p.at + 0.2) held |= p.button;
            uint32_t controls = 0;
            for (unsigned c = 0; c < punchout::kControlCount; ++c)
                for (const Input& in : bindings[c]) {
                    const bool on = in.kind == punchout::Binding::kPad     ? (held & in.pad) != 0
                                    : in.kind == punchout::Binding::kMouse ? (held & qilin::host::kPadClick) != 0
                                                                           : display->key_down(in.key);
                    if (on) controls |= 1u << c;
                }
            punchout::g_controls.store(controls, std::memory_order_relaxed);
            // The pointer: the mouse where it moves, the right stick at a screen's width a second.
            const auto tick = std::chrono::steady_clock::now();
            const float dt = std::chrono::duration<float>(tick - last).count();
            last = tick;
            float mx, my, sx, sy;
            if (display->mouse_moved(mx, my)) cursor_x = mx, cursor_y = my;
            for (const Point& p : points)
                if (now >= p.at && now < p.at + 0.2) cursor_x = p.x, cursor_y = p.y;
            display->right_stick(sx, sy);
            cursor_x = std::clamp(cursor_x + sx * dt * 2.0f, -1.2f, 1.2f);
            cursor_y = std::clamp(cursor_y + sy * dt * 2.0f, -1.2f, 1.2f);
            punchout::g_pointer_x.store(int32_t(cursor_x * 1000), std::memory_order_relaxed);
            punchout::g_pointer_y.store(int32_t(cursor_y * 1000), std::memory_order_relaxed);
            punchout::g_pointer_on.store(std::fabs(cursor_x) <= 1.1f && std::fabs(cursor_y) <= 1.1f, std::memory_order_relaxed);
            display->rumble(settings.rumble && punchout::g_rumble.load(std::memory_order_relaxed));
        }
        if (!renderer) {
            uint32_t w, h;
            punchout::take_frame(&sequence, 10, rgba, w, h);
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
        // Every 5 s: how evenly new pictures reached the screen (last 20 s).
        if (display) {
            static auto next_report = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            if (const auto t = std::chrono::steady_clock::now(); t >= next_report) {
                next_report = t + std::chrono::seconds(5);
                const auto ft = display->frame_times().summary();
                if (ft.frames)
                    rt::log("  frame times (last %.0f s): %.1f fps (%.2f ms), 1%% low %.1f fps, 0.1%% low %.1f fps, worst %.1f ms",
                            ft.average_ms * double(ft.frames) / 1000.0, ft.fps, ft.average_ms, ft.low1_fps, ft.low01_fps, ft.worst_ms);
            }
        }
        uint32_t xfb, w, h;
        if (!punchout::take_xfb(&sequence, 0, xfb, w, h)) continue;
        if (const qilin::host::Image* x = xfb ? renderer->xfb(xfb) : nullptr) {
            display->present_image(x->image, x->view, x->width, x->height, aspect, h);
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
                rt::log("dumped %s at %.1f s of game time", name, punchout::frame_count() / 60.0);
            }
        }
    }
}
