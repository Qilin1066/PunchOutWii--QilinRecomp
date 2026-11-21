# PunchOutWii--QilinRecomp

A static recompilation of **Punch-Out!!** (Next Level Games / Nintendo, Wii, 2009) into portable C++23 with a Vulkan 1.3 renderer, aimed at 64-bit Windows, Linux and macOS, on x86-64 or ARM64. Anything with a Vulkan 1.3 driver is in scope, including the Raspberry Pi 4 (Mesa 24.3+); every 64-bit Mac with a Metal GPU that runs macOS 10.15 (Catalina) or later, Intel or Apple Silicon, is covered through MoltenVK. Efficiency is the first priority: a Raspberry Pi 4–class board is our reference for typical low-end hardware, not a floor. The requirements and scope are the same as [GBTGhoulies--QilinRecomp](https://github.com/Qilin1066/GBTGhoulies--QilinRecomp)'s, applied to the Wii as in [RhythmHeavenFever--QilinRecomp](https://github.com/Qilin1066/RhythmHeavenFever--QilinRecomp), whose recompiler core this project started from.

> **No game data is included, and none ever will be.** You need your own copy of the game: the Europe/Australia release, disc `R7PP01`, **revision 1**. The tools read `main.dol` and the disc files from a path you give them. `.gitignore` keeps game files, and anything generated from them, out of the repo.

## Status

Analysis (roadmap step 1) and the recompiler core (step 2) are done; the runtime (step 3) runs the game, with sound, from boot through the menus into fights, and the renderer (step 4) draws it on Vulkan. Still to do: a play-through of every opponent and mode, and Head-to-Head played by two people (a second controller is a second Wii Remote; a scripted two-player fight runs) (`docs/runtime.md`, "Where the game stands").

| Piece | State |
|---|---|
| DOL loader (sections, BSS, entry point, flat image; the PAL Rev 1 build recognised by SHA-256) | ✅ |
| Broadway decoder (all 222 instructions of the 750CL with the paired singles; reserved bits and invalid forms rejected) | ✅ 0 unexplained disagreements with GNU binutils over all 799,200 words of the game's code (and ~38 million swept words in RhythmHeavenFever--QilinRecomp) |
| Function discovery + CFG (tail calls, vtables and callbacks, CodeWarrior jump tables including strength-reduced and loop-invariant ones, register save/restore chains) | ✅ 13,487 functions, all 187 jump tables proven; every word of the code accounted for (`qilin coverage`: 99.58% code, the rest padding, dead code and 101 identified words of data and debugger stub, none of it lifted) |
| CI: Linux x64 (GCC 14, Clang 19), Linux ARM64, macOS ARM64 and x64, Windows x64, warnings as errors, plus the decoder sweep | ✅ |
| What's linked in: Revolution SDK `0x4199_60831`, MSL, MetroTRK, HOME Button Menu, Audiokinetic Wwise, Next Level Games' engine | ✅ from their strings (`docs/dol-analysis.md`) |
| Every library found, its range fixed, and the lift / replace / drop boundary decided | ✅ 40 ranges from four kinds of evidence (size matches against the Rhythm Heaven Fever decomp, RTTI, SDK version banners, strings), 1,988 SDK and runtime functions named; 84% of the code lifted, 15% replaced, 1% dropped (`docs/code-analysis.md`) |
| Hardware-access profile, exception tables, timing model | ✅ lifted code touches no hardware register or uncached mirror, but writes the GX FIFO directly (803 stores in the engine's mesh drawers), so the FIFO decoder is required; no C++ exception can be thrown; one frame per retrace, gameplay driven by a time step in seconds (`docs/runtime-analysis.md`) |
| Lifter → C++, differential test | ✅ integer code, floating point and paired singles with GQR quantisation (bit-exact with Broadway: NI mode, NaN rules, single rounding, the estimate tables): 13,392 of 13,487 functions (99.3%) lifted, every one in the libraries we lift; the 95 stubs, all in code we replace or drop, trap with the reason; all of it compiles and links; 323,904 differential runs against an independent reference interpreter and known-answer tests from hardware data, 0 differences (`docs/recompiler-core.md`) |
| Runtime skeleton: the game built from your own `main.dol`, booted as `__start` boots it, a replacement registry that names anything missing; OS (threads, mutexes, message queues, alarms, interrupts, time, locked cache), DVD from the extracted disc, VI's retrace, console settings | ✅ runs the front end at 59 frames a second from the extracted disc: GX lifted with its command stream decoded, NAND saves, input, and sound (AI/AX/DSP lifted, the DSP's AX microcode emulated, out through SDL3) (`docs/runtime.md`) |
| GX on Vulkan (the renderer) | ✅ menus, movies and fights: vertices, XF lighting, TEV as a precompiled ubershader with specialised pipelines, every texture format, EFB copies and the EFB's pixel formats on the GPU, 1x to 8x internal resolution. Checked pixel for pixel against Dolphin on five scenes, a fight among them, in the console's 18-bit colour and in true colour: no pixel more than 8 of 255 off (`docs/runtime.md`, "Comparing a frame with Dolphin") |
| The whole game, start to credits | 🚧 the front end, Exhibition and practice fights are played and tested; Career through every opponent and a Head-to-Head fight are not yet |

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

CMake stops with a clear message if the compiler is older, or if the target is 32-bit. CMake fetches the rest at pinned versions and builds it in: SDL3 (window, input, audio), the Vulkan headers and volk. To run, you need a Vulkan 1.3 driver.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

## Playing

**On Windows, the download (nothing to install).** Each release has `PunchOutWii--QilinRecomp-<version>-windows-x64.zip` (about 80 MB; GitHub builds it, `.github/workflows/player-kit.yml`), as the APB recomp has. Unpack it and double-click **`Play Punch-Out.bat`**: the first time it asks for your extracted disc folder and builds the game from it (a few minutes: 3 on an 8-core PC), with the compiler inside the download (LLVM-MinGW: Clang, free and open source); after that it starts the game at once. The download has everything but the game's own code, prebuilt: the recompiler, the runtime, the renderer, SDL3, the menu. The code made from the disc is made on the player's PC, from their own copy, and never distributed. See `tools/player-kit/` (`package.sh` builds the download on Linux; `play.ps1` is what Play runs).

**On Windows, from the source, double-click.** With Visual Studio 2022 (17.10 or newer, the free Community edition, with "Desktop development with C++") and Git installed, in the project's folder:

- **`Play Punch-Out.bat`** starts the game. The first time it builds it (10 to 40 minutes), asking for your extracted disc folder with a folder picker.
- **`Update Punch-Out.bat`** gets the latest code, rebuilds only what changed, and starts the game.

Both run `tools/build-punchout.ps1`, which finds Visual Studio's CMake and Git itself.

**On Linux, the Raspberry Pi 4 and macOS, one command.** In the project's folder:

```sh
./punchout.sh --disc "<extracted disc>"   # the first time: builds it, then starts it
./punchout.sh                             # after that: starts it (building first if needed)
./punchout.sh update                      # gets the latest code, rebuilds what changed, starts it
```

It checks for CMake, a compiler (GCC 14+ or Clang 19+) and, on Debian, Ubuntu and Raspberry Pi OS, the packages SDL3 needs to build, and says what to install if one is missing. The build is optimised for the machine's own CPU and runs as many compiles at once as the memory allows (about 1.5 GB each).

**The Raspberry Pi 4** (4 GB or more; 64-bit Raspberry Pi OS based on Debian 13 "Trixie", whose Mesa has the Pi's Vulkan 1.3 driver):

```sh
sudo apt install git cmake g++-14 libx11-dev libxext-dev libwayland-dev libxkbcommon-dev libegl-dev \
                 libasound2-dev libpulse-dev libudev-dev libdbus-1-dev
git clone https://github.com/Qilin1066/PunchOutWii--QilinRecomp && cd PunchOutWii--QilinRecomp
./punchout.sh --disc "<extracted disc>"
```

The first build takes about an hour on a Pi 4 (two compiles at once on 4 GB). Use the `low-end` preset (the default, `original`, is the same: the console's own 640x528 picture), keep vsync on, and play full screen (F11) so the desktop's compositor doesn't copy every frame. F3 shows how evenly frames arrive. Our ARM64 build is checked by cross-compiling everything, the code lifted from the disc included, for the Pi 4's Cortex-A72 (`tools/check/arm64_build.sh`) and by CI on Linux ARM64.

**By hand:**

The game is built from your own disc's `main.dol`; nothing of the game is in this repository. Extract the whole disc first (Dolphin: right-click the game, *Extract Entire Disc*; or `wit extract`). That gives a folder with `sys/` and `files/`. Then:

```sh
cmake -S . -B build-game -DCMAKE_BUILD_TYPE=Release -DQILIN_GAME_DOL="<extracted disc>/sys/main.dol"
cmake --build build-game --parallel --target punchout
build-game/punchout "<extracted disc>"
```

On Windows, with Visual Studio's Developer PowerShell, for example:

```powershell
cmake -S . -B build-game -DQILIN_GAME_DOL="D:\Games\PunchOut disc\sys\main.dol"
cmake --build build-game --config Release --parallel --target punchout
build-game\Release\punchout.exe "D:\Games\PunchOut disc"
```

The first build lifts the whole game to C++ and compiles it, which takes several minutes.

**Controls:** a gamepad (Xbox names; every one connected works) or the keyboard, standing in for a Wii Remote held sideways. Buttons only: there are no motion controls.

| Wii Remote | Pad | Keyboard |
|---|---|---|
| Cross pad (dodge, duck, block) | D-pad or left stick | Arrows |
| 1 (left punch) | B | Z |
| 2 (right punch) | A | X |
| A (Star Punch, menus' OK) | X or Y | C or Space |
| + / − / HOME | Start / Back / Guide | Enter / Backspace / H |
| Pointer (menus) | Right stick | Mouse (left button is A) |

**The settings menu:** F1 or Esc (a controller: Back and Start together) opens it over the game, which pauses; closing it saves. It's the same menu as the other Qilin recomps (Dear ImGui), with three pages:

- **Controls:** each Wii Remote button's inputs. Press *Add* on a row, then the key, the pad's button (shoulders and triggers too) or a click; an input already on another button moves over. Click an input to remove it. Rumble on or off, the Wii Remote speaker's sounds on or off. A second player for Head-to-Head: a controller as a second Wii Remote (the second controller to press a button, or the first with player one on the keyboard).
- **Display:** full screen (also F11 or Alt+Enter) and which display; the picture's size (whole multiples, the sharpest; fit; or fill the window), smooth or sharp, and its look: plain, or a **CRT television** (raster lines on the console's own 480 lines, with phosphor stripes; both adjustable). *Look at the picture* hides the menu to see it. Vsync. The enhancement preset (original, low-end, desktop), the internal resolution (auto or 1-8x, from the next start), anisotropic filtering and true colour (at once; off is the console's 18-bit colour with its dither). The graphics device (from the next start). Frame times: F3 shows the frame rate, 1% and 0.1% lows and a frame-time graph in a corner.
- **Game:** the Wii's own settings the game reads: language (English, German, French, Spanish, Italian), widescreen or 4:3, mono or stereo (from the next start).

Close and Quit are at the top of every page.

Everything is kept in `punchout.ini` in the game's data folder, `qilin/punchout` in your per-user application data (Windows: `%LOCALAPPDATA%\qilin\punchout`), with the saves (`nand/`), the pipeline cache and the log (`punchout.log`, with `punchout.old.log` from the run before: send it with any bug report), as the other Qilin recomps keep theirs. The file has `[display]`, `[controls]` and `[console]` sections and comments, and can be edited by hand. Up to v0.17.0 the folder was `%APPDATA%\Qilin\PunchOut-QilinComp` and the file `settings.ini`: the first run of v0.18.0 moves the folder and carries the file's controls and scale over (Escape, now the menu's, becomes H for HOME).

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

## Licence

PunchOutWii--QilinRecomp's own code and documentation are free software under the [GNU General Public License, version 3](LICENSE) (GPL-3.0), as in the other QilinRecomp projects: you may use, share, change and sell it, but anything you distribute that is built from it, changed or not, must come with its full source under the same licence. Some things in or around it are not ours:

- `third_party/rhf/`, the Rhythm Heaven Fever decomp's symbols and splits, keeps its own licence, CC0 1.0 (`third_party/rhf/LICENSE`).
- The dependencies CMake fetches at build time keep theirs, all compatible: SDL3 (zlib), Khronos' Vulkan headers (Apache-2.0 or MIT), volk (MIT) and Dear ImGui (MIT). The Windows download carries each under `LICENSES/`.

Punch-Out!! itself, its code and data, belong to Nintendo and are not covered by anything here. None of it is in this repository: the game's code is translated from your own disc when you build, and nothing built from it may be shared.

## Design targets (short version)

- **Efficiency and reliability come first.** Every choice is judged by whether it makes the game cheaper to run on low-end hardware, and whether it fails loudly instead of misbehaving quietly.
- **Static recompilation, PowerPC → C++, compiled ahead of time.** No interpreter, no JIT, no emulator underneath. Hand-written C++ can override any lifted function.
- **Vulkan 1.3 only.** GX is translated at API level to Vulkan, with a deliberately small baseline feature set (1.3 core plus what the Pi 4's driver offers) so one render path runs on everything from low-end ARM GPUs upwards. macOS runs through MoltenVK.
- **The SDK's hardware-facing libraries are replaced natively**, not recompiled: OS, DVD, VI, WPAD (with the Bluetooth stack under it), NAND. GX, AI, AX and DSP are lifted, with the GPU's command stream and the DSP's AX microcode emulated under them. The C/C++ runtime, the SDK's pure-software libraries (KPAD and WBC, the Balance Board's, among them), THP, the HOME Button Menu, Audiokinetic Wwise and Next Level Games' engine are lifted with the game, for exact behaviour.
- **Ground truth is Dolphin** running the original game, reaction timing included: cue-to-attack timing and dodge windows are behaviour in Punch-Out!!.

See `docs/` for the full roadmap and the analysis notes.
