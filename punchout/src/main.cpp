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
#include "input.hpp"
#include "menu.hpp"
#include "os.hpp"
#include "profile.hpp"
#include "settings.hpp"
#include "qilin/host/paths.hpp"
#include "renderer.hpp"
#include "qilin/host/audio.hpp"
#include "qilin/host/display.hpp"
#include <SDL3/SDL_timer.h>
#include "threads.hpp"
#include "video.hpp"

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

int main(int argc, char** argv) {
    namespace rt = qilin::rt;
    punchout::set_command_line(argc, argv);  // for the game's reset
    punchout::start_profiler();              // PUNCHOUT_PROFILE, for development
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
                punchout::gx::recycle_frame(std::move(f));
            }
            else break;
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
