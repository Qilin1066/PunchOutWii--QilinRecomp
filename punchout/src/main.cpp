// punchout: the recompiled game. Usage: punchout <folder with the extracted disc>
// (sys/main.dol, sys/boot.bin, files/), from Dolphin's "Extract Entire Disc"
// or wit's extract.
#include <cstdio>
#include <span>

#include "boot.hpp"
#include "functions.hpp"
#include "gpu.hpp"
#include "threads.hpp"

int main(int argc, char** argv) {
    namespace rt = qilin::rt;
    if (argc != 2) {
        std::fprintf(stderr, "usage: punchout <folder with the extracted disc: sys/main.dol, sys/boot.bin, files/>\n");
        return 2;
    }
    rt::init({std::span(qilin::gen::kFunctions, qilin::gen::kFunctionCount),
              std::span(qilin::gen::kReplaced, qilin::gen::kReplacedCount)});
    rt::provide(punchout::os_replacements());
    rt::provide(punchout::dvd_replacements());
    rt::provide(punchout::vi_replacements());
    rt::provide(punchout::thread_replacements());
    rt::provide(punchout::sc_replacements());
    punchout::init_gpu();
    punchout::load_game(argv[1]);
    punchout::start_game();
}
