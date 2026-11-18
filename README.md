# PunchOutWii-QilinComp

A static recompilation of **Punch-Out!!** (Next Level Games / Nintendo, Wii, 2009) into portable C++23 with a Vulkan 1.3 renderer, aimed at 64-bit Windows, Linux and macOS, on x86-64 or ARM64. Anything with a Vulkan 1.3 driver is in scope, including the Raspberry Pi 4 (Mesa 24.3+); every 64-bit Mac with a Metal GPU that runs macOS 10.15 (Catalina) or later, Intel or Apple Silicon, is covered through MoltenVK. Efficiency is the first priority: a Raspberry Pi 4–class board is our reference for typical low-end hardware, not a floor. The requirements and scope are the same as [GBTG-Qilincomp](https://github.com/Tecboy1066/GBTGhoulies-Qilincomp)'s, applied to the Wii as in [BeatTheBeat-QilinComp](https://github.com/Tecboy1066/BeatTheBeat-QilinComp), whose recompiler core this project started from.

> **No game data is included, and none ever will be.** You need your own copy of the game: the Europe/Australia release, disc `R7PP01`, **revision 1**. The tools read `main.dol` and the disc files from a path you give them. `.gitignore` keeps game files, and anything generated from them, out of the repo.

## Status

Roadmap step 1, **analysis**, is in progress; the recompiler core (step 2) has its first pieces.

| Piece | State |
|---|---|
| DOL loader (sections, BSS, entry point, flat image; the PAL Rev 1 build recognised by SHA-256) | ✅ |
| Broadway decoder (all 222 instructions of the 750CL with the paired singles; reserved bits and invalid forms rejected) | ✅ 0 unexplained disagreements with GNU binutils over all 799,200 words of the game's code (and ~38 million swept words in BeatTheBeat-QilinComp) |
| Function discovery + CFG (tail calls, vtables and callbacks, CodeWarrior jump tables including strength-reduced and loop-invariant ones, register save/restore chains) | ✅ 13,487 functions, all 187 jump tables proven; every word of the code accounted for (`qilin coverage`: 99.58% code, the rest padding, dead code and 101 identified words of data and debugger stub, none of it lifted) |
| CI: Linux x64 (GCC 14, Clang 19), Linux ARM64, macOS ARM64 and x64, Windows x64, warnings as errors, plus the decoder sweep | ✅ |
| What's linked in: Revolution SDK `0x4199_60831`, MSL, MetroTRK, HOME Button Menu, Audiokinetic Wwise, Next Level Games' engine | ✅ from their strings (`docs/dol-analysis.md`) |
| Every library found, its range fixed, and the lift / replace / drop boundary decided | ✅ 40 ranges from four kinds of evidence (size matches against the Rhythm Heaven Fever decomp, RTTI, SDK version banners, strings), 1,988 SDK and runtime functions named; 84% of the code lifted, 15% replaced, 1% dropped (`docs/code-analysis.md`) |
| Hardware-access profile, exception tables, timing model | ✅ lifted code touches no hardware register or uncached mirror, but writes the GX FIFO directly (803 stores in the engine's mesh drawers), so the FIFO decoder is required; no C++ exception can be thrown; one frame per retrace, gameplay driven by a time step in seconds (`docs/runtime-analysis.md`) |
| Lifter → C++, differential test | 🔨 slice 1, integer code: 10,917 of 13,487 functions (80.9%) lifted, the rest stubs that trap with the reason (2,455 wait for floating point); all of it compiles and links; 197,952 differential runs against an independent reference interpreter, 0 differences (`docs/recompiler-core.md`) |
| Runtime: OS, DVD, NAND, GX on Vulkan, AX audio, input (buttons only: the sideways Wii Remote scheme on a modern pad or keyboard, no motion controls) | later (see roadmap) |

## Versions and changes

Releases are tagged `vX.Y.Z` on GitHub, and every release is recorded in [`CHANGELOG.md`](CHANGELOG.md). Past entries are kept, never rewritten. The version lives in one place, `project(... VERSION ...)` in `CMakeLists.txt`, and `qilin --version` prints it. While the recompiler is being built, a minor bump (0.**x**) marks a milestone and a patch bump (0.x.**y**) a fix. 1.0.0 will be the first build that plays the game.

## Building

The whole project (recompiler, runtime and generated code) is **C++23**. You need CMake ≥ 3.25 and one of:

| Compiler | Minimum | Notes |
|---|---|---|
| GCC | 14 | Debian 13 / Raspberry Pi OS (Trixie) and Ubuntu 24.04 (`g++-14`) have it |
| Clang | 19 | Older Clang can't use libstdc++'s `std::expected` |
| Apple Clang | Xcode 16 | |
| MSVC | Visual Studio 2022 17.10 | |

CMake stops with a clear message if the compiler is older, or if the target is 32-bit. There are no other dependencies.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

## Using it

Extract `main.dol` from your disc first (`docs/dol-analysis.md` says how, with Dolphin or Wiimms ISO Tools).

```sh
build/qilin info    path/to/main.dol [--json info.json]   # SHA-256, supported build, entry point, sections, BSS (JSON)
build/qilin profile path/to/main.dol                      # instruction profile of the code
build/qilin insns   path/to/main.dol [-o listing.txt]     # every word of the code, decoded
build/qilin disasm  words.bin [--base 0x80004000]         # a file of big-endian words, decoded
build/qilin cfg     path/to/main.dol [--json cfg.json]    # function discovery + control flow, summary
build/qilin coverage path/to/main.dol [--list]            # every word: code, padding, dead or unknown (exit 3 if any unknown)
build/qilin suspects path/to/main.dol                     # functions with problems, and rejected starts, for review
build/qilin lift    path/to/main.dol -o generated         # every function as C++ (or a stub saying why not); keep out of git
```

### Libraries

```sh
python3 tools/analysis/identify_libraries.py --qilin build/qilin --dol path/to/main.dol [--names-out pal_names.txt]
```

Finds every library in `main.dol` and prints its range, counts and plan (`docs/code-analysis.md`). The names file is derived from the game: don't commit it.

```sh
python3 tools/analysis/hardware_access.py --qilin build/qilin --dol path/to/main.dol [--list]
```

Every load and store to a hardware register, the GX FIFO, an uncached mirror or the locked cache that the code builds the address of, by library (`docs/runtime-analysis.md`).

### Decoder check

```sh
python3 tools/check/verify_decoder.py --qilin build/qilin --dol path/to/main.dol   # every word of the game's code
python3 tools/check/verify_decoder.py --qilin build/qilin --sweep                  # the encoding space (CI runs this)
```

This compares the decoder with GNU objdump (`-M broadway,raw`): mnemonic and every operand must match, and a word must be valid for both or neither. It needs Python 3 and a PowerPC objdump: `apt install binutils-powerpc-linux-gnu`, or devkitPPC's `powerpc-eabi-objdump` with `OBJDUMP` set. Reviewed differences (binutils accepts some encodings Broadway reserves and lacks `mftb`) are counted by class; anything else fails.

## Layout

| Path | What |
|---|---|
| `recomp/src/` | Recompiler core: `dol` loader, `sha256`, `decoder` (Broadway), `cfg` (function discovery + control flow), `lift` (the lifter), `main` (CLI) |
| `recomp/tests/` | Unit tests on a synthetic in-memory DOL, the decoder and synthetic programs, so they need no game files; `difftest/`: the lifter's differential test (case generator, reference interpreter, runner) |
| `runtime/` | What the lifted code runs on: the guest CPU state and memory access (`include/qilin/rt/cpu.hpp`), the 4 GB guest address space (`memory.*`) |
| `tools/check/` | The decoder cross-check against GNU objdump |
| `tools/analysis/` | `identify_libraries.py`: the libraries in `main.dol`, their ranges and the boundary; `hardware_access.py`: every hardware, FIFO and mirror access, by library |
| `third_party/rhf/` | The Rhythm Heaven Fever decomp's symbols and splits (CC0), for the libraries both games share |
| `docs/` | Roadmap and design targets, prior art, naming, analysis and status notes |

## Design targets (short version)

- **Efficiency and reliability come first.** Every choice is judged by whether it makes the game cheaper to run on low-end hardware, and whether it fails loudly instead of misbehaving quietly.
- **Static recompilation, PowerPC → C++, compiled ahead of time.** No interpreter, no JIT, no emulator underneath. Hand-written C++ can override any lifted function.
- **Vulkan 1.3 only.** GX is translated at API level to Vulkan, with a deliberately small baseline feature set (1.3 core plus what the Pi 4's driver offers) so one render path runs on everything from low-end ARM GPUs upwards. macOS runs through MoltenVK.
- **The SDK's hardware-facing libraries are replaced natively**, not recompiled: OS, GX, AI/AX/DSP, DVD, VI, WPAD (with the Bluetooth stack under it), NAND. The C/C++ runtime, the SDK's pure-software libraries (KPAD and WBC, the Balance Board's, among them), THP, the HOME Button Menu, Audiokinetic Wwise and Next Level Games' engine are lifted with the game, for exact behaviour.
- **Ground truth is Dolphin** running the original game, reaction timing included: cue-to-attack timing and dodge windows are behaviour in Punch-Out!!.

See `docs/` for the full roadmap and the analysis notes.
