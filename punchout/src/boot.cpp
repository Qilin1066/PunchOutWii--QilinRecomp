// Booting the game: main.dol into MEM1, the boot information the disc loader
// leaves in low memory, then what __start does, natively (the plan replaces
// the start-up code: docs/code-analysis.md).
#include "qilin/rt/report.hpp"
#include "boot.hpp"

#include <cstdio>
#include <vector>

#include "build.hpp"
#include "dol.hpp"
#include "os.hpp"
#include "threads.hpp"
#include "qilin/path.hpp"
#include "qilin/rt/file.hpp"
#include "qilin/rt/system.hpp"

namespace punchout {
namespace rt = qilin::rt;

namespace {

std::vector<uint8_t> read_file(const std::filesystem::path& p) {
    std::vector<uint8_t> out;
    if (FILE* f = rt::open_file(p, "rb")) {
        uint8_t buf[65536];
        for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) out.insert(out.end(), buf, buf + n);
        std::fclose(f);
    } else {
        rt::fatal(nullptr, "cannot read %s", qilin::utf8_path(p).c_str());
    }
    return out;
}

// Low memory as the Wii's boot program, IOS53 and the disc's apploader leave
// it for a retail console. The values are Dolphin's (Boot_BS2Emu.cpp,
// IOS/VersionInfo.cpp: hardware observations, GPL-2.0-or-later; facts only,
// docs/prior-art.md). The OS replacements own the OS's own state, so these
// matter for code that reads them directly: lifted code reads only the bus
// clock (docs/runtime-analysis.md).
void write_boot_info(std::span<const uint8_t> disc_header) {
    std::memcpy(rt::guest_ptr(0x80000000u), disc_header.data(), 0x20);  // disc ID, version, magic
    rt::poke32(0x80000020u, 0x0D15EA5Eu);  // boot magic
    rt::poke32(0x80000024u, 1);
    rt::poke32(0x80000028u, 0x01800000u);  // MEM1 size
    rt::poke32(0x8000002Cu, 0x00000023u);  // console type: retail Wii (RVL retail 3)
    rt::poke32(0x80000030u, 0);            // arena low: none, so OSInit uses the linker's
    rt::poke32(0x80000034u, 0x817FEC60u);  // arena high
    rt::poke32(0x800000ECu, 0x81800000u);  // debug monitor: none (top of MEM1)
    rt::poke32(0x800000F0u, 0x01800000u);  // simulated memory size
    rt::poke32(0x800000F4u, 0);            // BI2: none (no debugger, no arguments)
    rt::poke32(0x800000F8u, 0x0E7BE2C0u);  // bus clock, 243 MHz
    rt::poke32(0x800000FCu, 0x2B73A840u);  // CPU clock, 729 MHz
    rt::poke32(0x800000CCu, 1);            // video: PAL (the IPL's VI set-up)
    rt::poke32(0x800030D8u, 0xFFFFFFFFu);
    rt::poke16(0x800030E0u, 0);            // PADInit
    rt::poke16(0x800030E6u, 0x8201);
    rt::poke32(0x80003100u, 0x01800000u);  // MEM1 physical and simulated size, end, arena
    rt::poke32(0x80003104u, 0x01800000u);
    rt::poke32(0x80003108u, 0x81800000u);
    rt::poke32(0x8000310Cu, 0);
    rt::poke32(0x80003110u, 0x81800000u);
    rt::poke32(0x80003118u, 0x04000000u);  // MEM2, as IOS53 leaves it
    rt::poke32(0x8000311Cu, 0x04000000u);
    rt::poke32(0x80003120u, 0x93600000u);
    rt::poke32(0x80003124u, 0x90000800u);  // MEM2 arena
    rt::poke32(0x80003128u, 0x935E0000u);
    rt::poke32(0x80003130u, 0x935E0000u);  // IPC buffer
    rt::poke32(0x80003134u, 0x93600000u);
    rt::poke32(0x80003138u, 0x00000011u);  // Hollywood revision
    rt::poke32(0x80003140u, 0x0035161Fu);  // IOS53 version
    rt::poke32(0x80003144u, 0x00030110u);
    rt::poke32(0x80003148u, 0x93600000u);  // IOS's reserved MEM2
    rt::poke32(0x8000314Cu, 0x93620000u);
    rt::poke32(0x80003158u, 0x0000FF01u);  // RAM vendor
    rt::poke8(0x8000315Cu, 0x80);          // boot flag
    rt::poke16(0x8000315Eu, 0x0113);       // boot program version
    std::memcpy(rt::guest_ptr(0x80003180u), disc_header.data(), 4);  // game ID, kept for the OS's check
    rt::poke32(0x80003184u, 0x80000000u);
}

}  // namespace

void load_game(const std::filesystem::path& folder) {
    set_game_folder(folder);
    const auto dol_path = folder / "sys" / "main.dol";
    auto dol = qilin::Dol::load(dol_path);
    if (!dol) rt::fatal(nullptr, "%s: %s", qilin::utf8_path(dol_path).c_str(), dol.error().c_str());
    const qilin::KnownBuild* known = qilin::known_build(dol->sha256);
    if (!known || known->disc_id != "R7PP01")
        rt::fatal(nullptr, "%s isn't the supported build (Punch-Out!! PAL Rev 1, R7PP01): SHA-256 %s",
                  qilin::utf8_path(dol_path).c_str(), dol->sha256.c_str());
    // The image covers every section and the BSS; bytes outside the sections are zero.
    std::memcpy(rt::guest_ptr(dol->base()), dol->image().data(), dol->image().size());

    const std::vector<uint8_t> boot = read_file(folder / "sys" / "boot.bin");
    if (boot.size() < 0x20 || std::memcmp(boot.data(), "R7PP01", 6) != 0)
        rt::fatal(nullptr, "%s isn't Punch-Out!!'s PAL disc header", qilin::utf8_path(folder / "sys" / "boot.bin").c_str());
    write_boot_info(std::span(boot).first(0x20));
}

[[noreturn]] void start_game() {
    namespace b = build;
    rt::Cpu c = rt::new_cpu();
    rt::set_thread_cpu(&c);  // for a crash report's call stack
    init_threads(c);
    // __init_registers: the stack and the two small-data bases.
    c.r[1] = b::kStackTop;
    c.r[2] = b::kSdaBase2;
    c.r[13] = b::kSdaBase;
    // __init_hardware (FP on, paired singles on, FPRs and caches cleared) has
    // nothing left to do: the Cpu starts zeroed, with both on.
    // The outermost frame: a back chain and LR of -1 end the stack.
    c.r[1] -= 8;
    rt::poke32(c.r[1], 0xFFFFFFFFu);
    rt::poke32(c.r[1] + 4, 0xFFFFFFFFu);
    // __init_data: copy each ROM section to its address (none moves in a DOL),
    // zero each BSS range.
    for (uint32_t e = b::kRomCopyInfo; rt::peek32(e + 8) != 0; e += 12) {
        const uint32_t rom = rt::peek32(e), to = rt::peek32(e + 4), size = rt::peek32(e + 8);
        if (rom != to) std::memmove(rt::guest_ptr(to), rt::guest_ptr(rom), size);
    }
    for (uint32_t e = b::kBssInitInfo; rt::peek32(e + 4) != 0; e += 8)
        std::memset(rt::guest_ptr(rt::peek32(e)), 0, rt::peek32(e + 4));
    rt::poke32(0x80000044u, 0);  // the debugger's exception mask
    // DBInit, OSInit: native (os.cpp). No debugger, so no MetroTRK.
    rt::call(c, 0x801E17F8u);  // DBInit
    rt::call(c, 0x801FF230u);  // OSInit
    // __check_pad3 resets when the buttons that ask for it are held at boot: none are.
    rt::call(c, b::kInitUser);  // static constructors
    c.r[3] = 0;                 // argc
    c.r[4] = 0;                 // argv
    rt::call(c, b::kMain);
    rt::call(c, b::kExit);  // exit(main's result)
    rt::fatal(&c, "exit returned");
}

}  // namespace punchout
