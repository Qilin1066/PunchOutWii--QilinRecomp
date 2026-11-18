// punchout: the recompiled game.
//
//   punchout [--headless] <folder with the extracted disc: sys/main.dol, sys/boot.bin, files/>
//
// The guest runs on threads of its own (threads.cpp); this thread owns the
// window: it handles its events and shows each frame the guest hands over.
// --headless runs without a window (for testing).
#include <cstdio>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "boot.hpp"
#include "functions.hpp"
#include "gpu.hpp"
#include "input.hpp"
#include "qilin/host/display.hpp"
#include "threads.hpp"
#include "video.hpp"

int main(int argc, char** argv) {
    namespace rt = qilin::rt;
    bool headless = false;
    const char* folder = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--headless") == 0) headless = true;
        else if (!folder) folder = argv[i];
        else folder = nullptr, i = argc;  // too many: usage
    }
    if (!folder) {
        std::fprintf(stderr, "usage: punchout [--headless] <folder with the extracted disc: sys/main.dol, sys/boot.bin, files/>\n");
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
    punchout::init_gpu();
    punchout::load_game(folder);

    std::unique_ptr<qilin::host::Display> display;
    if (!headless) {
        qilin::host::Display::Options o;
        o.title = "Punch-Out!!";
        display = std::make_unique<qilin::host::Display>(o);
        rt::log("drawing with %s", display->device_name().c_str());
    }
    punchout::run_on_host_thread([](void*) { punchout::start_game(); }, nullptr);

    uint64_t sequence = 0;
    std::vector<uint8_t> rgba;
    for (;;) {
        if (display && !display->poll()) {
            rt::log("window closed");
            std::fflush(nullptr);
            std::_Exit(0);  // guest threads are parked on host threads: no static destructors
        }
        if (display) {
            punchout::g_pad_buttons.store(display->buttons(), std::memory_order_relaxed);
            display->rumble(punchout::g_rumble.load(std::memory_order_relaxed));
        }
        uint32_t w, h;
        if (punchout::take_frame(&sequence, 10, rgba, w, h) && display)
            display->present(w, h, rgba.data(), 16.0f / 9.0f);  // the console says 16:9 (sc.cpp)
    }
}
