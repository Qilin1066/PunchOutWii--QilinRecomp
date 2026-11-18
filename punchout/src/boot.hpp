// Booting the game (boot.cpp).
#pragma once

#include <filesystem>
#include <span>

#include "qilin/rt/system.hpp"

namespace punchout {

// main.dol (checked against the supported build) into MEM1, and the boot
// information in low memory. `folder` holds the extracted disc: sys/, files/.
void load_game(const std::filesystem::path& folder);

// __start, natively: registers, data, DBInit, OSInit, constructors, main, exit.
[[noreturn]] void start_game();

// Each library's replacements (os.cpp, ...).
std::span<const qilin::rt::Provided> os_replacements();
std::span<const qilin::rt::Provided> dvd_replacements();
std::span<const qilin::rt::Provided> vi_replacements();
std::span<const qilin::rt::Provided> sc_replacements();
std::span<const qilin::rt::Provided> wpad_replacements();
std::span<const qilin::rt::Provided> nand_replacements();

}  // namespace punchout
