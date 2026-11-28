# PunchOutWii--QilinRecomp

A static recompilation of **Punch-Out!!** (Next Level Games / Nintendo, Wii, 2009) into portable C++23 with a Vulkan 1.3 renderer, for 64-bit Windows, Linux and macOS on x86-64 or ARM64. Any GPU with a Vulkan 1.3 driver is in scope, down to the Raspberry Pi 4 (Mesa 24.3+). Macs run it through MoltenVK (macOS 10.15 or later).

> **No game data is included, and none ever will be.** You need your own copy of the game: the Europe/Australia release, disc `R7PP01`, **revision 1**. The tools read `main.dol` and the disc files from a path you give them.

## Status

The game runs, with sound, from boot through the menus into fights at 60 frames a second. The front end, Exhibition and practice fights are played and tested; Career through every opponent, and Head-to-Head played by two people, are not yet (`docs/runtime.md`, "Where the game stands").

| Part | State |
|---|---|
| DOL loader | ✅ sections, BSS, entry point; the PAL Rev 1 build recognised by SHA-256 |
| Broadway decoder | ✅ all 222 instructions of the 750CL with paired singles; 0 unexplained disagreements with GNU binutils over all 799,200 words of the game's code |
| Function discovery and CFG | ✅ 13,487 functions, all 187 jump tables proven; every word of the code accounted for (`qilin coverage`) |
| Library boundary | ✅ 40 ranges, 1,988 SDK and runtime functions named; 84% of the code lifted, 15% replaced, 1% dropped (`docs/code-analysis.md`) |
| Hardware, exceptions, timing | ✅ no hardware register touched by lifted code except the GX FIFO; no C++ exception can be thrown; one frame per retrace (`docs/runtime-analysis.md`) |
| Lifter and differential test | ✅ 13,392 of 13,487 functions lifted (the 95 stubs are all in replaced or dropped code); 323,904 differential runs, 0 differences (`docs/recompiler-core.md`) |
| Runtime | ✅ OS, DVD, VI, NAND saves, input and sound (AI/AX/DSP lifted, AX microcode emulated, out through SDL3) (`docs/runtime.md`) |
| Renderer (GX on Vulkan) | ✅ menus, movies and fights at 1x to 8x; five scenes, a fight among them, within 8 of 255 of Dolphin on every pixel |
| Two players | 🔨 a second controller is a second Wii Remote; a scripted Head-to-Head fight runs, two people haven't played it |
| The whole game, start to credits | 🔨 front end, Exhibition and practice fights; Career and title defence not yet played through |
| CI | ✅ Linux x64 (GCC 14, Clang 19), Linux ARM64, macOS ARM64 and x64, Windows x64, warnings as errors |

## Playing

### The downloads

Each [release](https://github.com/Qilin1066/PunchOutWii--QilinRecomp/releases) has a download for each system:

| System | Download | Then |
|---|---|---|
| Windows 10 or 11, 64-bit | `PunchOutWii--QilinRecomp-<version>-windows-x64.zip` | unzip it, double-click **`Play Punch-Out.exe`** |
| Linux on x86-64 (glibc 2.35 or later) | `PunchOutWii--QilinRecomp-<version>-linux-x86_64.tar.gz` | unpack it, double-click **`Play Punch-Out`** |
| Linux on ARM64: Raspberry Pi 4 or 5 (64-bit Raspberry Pi OS, Trixie or later) | `PunchOutWii--QilinRecomp-<version>-linux-arm64.tar.gz` | unpack it, double-click **`Play Punch-Out`** |
| macOS 14.5 or newer, Apple silicon or Intel | `PunchOutWii--QilinRecomp-<version>-macos.dmg` | drag **`Punch-Out.app`** onto Applications and open it (the first time: System Settings, Privacy & Security, *Open Anyway*) |

First extract the whole disc with Dolphin (right-click the game, *Properties*, the *Filesystem* tab, right-click the disc, *Extract Entire Disc*). In the launcher, choose that folder, the one holding `sys` and `files` (or the `DATA` folder inside it, if Dolphin made one), or drop it onto the window: it says at once whether it is the right game, region and revision. The first start then builds the game from it with the compiler the download carries (LLVM-MinGW on Windows, zig on Linux; on a Mac, Apple's command line tools, which the launcher offers to install), which takes a few minutes on a PC and longer on a Raspberry Pi; after that, Play starts the game at once. The launcher remembers the folder, checks GitHub for updates when asked, and rebuilds only what an update changed. Each download's `README.txt` has the details.

The downloads hold everything prebuilt except the game's own code, which is made on your computer from your copy and never distributed. `tools/player-kit/` builds them (`package.sh`, `package-linux.sh`, `package-macos.sh`, with `games.sh`), and `tools/player-kit/launcher/` is the launcher, the same files as in the other QilinRecomp projects (Toy Commander's copy is the master: `ADOPTING.md` there).

### From the source on Windows

With Visual Studio 2022 (17.10 or newer, "Desktop development with C++") and Git installed, in the project's folder:

- **`Play Punch-Out.bat`** starts the game. The first time it builds it (10 to 40 minutes), asking for your extracted disc folder.
- **`Update Punch-Out.bat`** gets the latest code, rebuilds what changed and starts the game.

Both run `tools/build-punchout.ps1`, which finds Visual Studio's CMake and Git itself.

### From the source on Linux and macOS

```sh
./punchout.sh --disc "<extracted disc>"   # the first time: builds it, then starts it
./punchout.sh                             # starts it (building first if needed)
./punchout.sh update                      # gets the latest code, rebuilds what changed, starts it
./punchout.sh build                       # builds without starting
```

It checks for CMake, GCC 14+ or Clang 19+ and (on Debian, Ubuntu and Raspberry Pi OS) the packages SDL3 needs, and says what is missing. It builds for the machine's own CPU, one compile per 1.5 GB of memory.

**Raspberry Pi 4** (4 GB or more, 64-bit Raspberry Pi OS based on Debian 13 "Trixie"):

```sh
sudo apt install git cmake g++-14 libx11-dev libxext-dev libwayland-dev libxkbcommon-dev libegl-dev \
                 libasound2-dev libpulse-dev libudev-dev libdbus-1-dev
git clone https://github.com/Qilin1066/PunchOutWii--QilinRecomp && cd PunchOutWii--QilinRecomp
./punchout.sh --disc "<extracted disc>"
```

The first build takes about an hour. Use the `low-end` or `original` preset (both draw the console's own 640x528 picture), keep vsync on, and play full screen (F11). `tools/check/arm64_build.sh` cross-builds everything, the lifted code included, for its Cortex-A72.

### By hand

Extract the whole disc first (Dolphin: right-click the game, *Extract Entire Disc*; or `wit extract`), giving a folder with `sys/` and `files/`. Then:

```sh
cmake -S . -B build-game -DCMAKE_BUILD_TYPE=Release -DQILIN_GAME_DOL="<extracted disc>/sys/main.dol"
cmake --build build-game --parallel --target punchout
build-game/punchout "<extracted disc>"
```

With Visual Studio's Developer PowerShell:

```powershell
cmake -S . -B build-game -DQILIN_GAME_DOL="<extracted disc>\sys\main.dol"
cmake --build build-game --config Release --parallel --target punchout
build-game\Release\punchout.exe "<extracted disc>"
```

### Controls

A gamepad (Xbox names; every one connected works) or the keyboard stands in for a Wii Remote held sideways. There are no motion controls.

| Wii Remote | Pad | Keyboard |
|---|---|---|
| Cross pad (dodge, duck, block) | D-pad or left stick | Arrows |
| 1 (left punch) | B | Z |
| 2 (right punch) | A | X |
| A (Star Punch, menus' OK) | X or Y | C or Space |
| + / − / HOME | Start / Back / Guide | Enter / Backspace / H |
| Pointer (menus) | Right stick | Mouse (left button is A) |

### Settings

F1 or Esc (on a controller, Back and Start together) opens the settings menu over the paused game; closing it saves. Close and Quit are at the top of every page.

- **Controls:** each Wii Remote button's inputs (press *Add*, then the key, button or click); remembered controller assignments per player; stick and trigger thresholds, pointer dead zone, rumble strength, volume and background muting; the Wii Remote speaker's sounds; a second player for Head-to-Head (a second controller, or the first controller with player one on the keyboard).
- **Display:** full screen (also F11 or Alt+Enter) and which display; picture size (whole multiples, fit or fill), smooth or sharp, plain or a CRT television look; vsync; the preset (original, low-end, desktop), internal resolution (auto or 1-8x), anisotropic filtering, true colour (off is the console's 18-bit colour with its dither); the graphics device. F3 shows the frame rate, the 1% and 0.1% lows and a frame-time graph.
- **Game:** the Wii settings the game reads: language (English, German, French, Spanish, Italian), widescreen or 4:3, mono or stereo. Previous saves lists up to five earlier copies of each changed save file; *Restore and restart* backs up the current version first ([recovery and controller details](docs/reliability-and-recovery.md)).

The settings (`punchout.ini`, editable by hand), the saves (`nand/`), the pipeline cache and the log live in `qilin/punchout` in your per-user application data (Windows: `%LOCALAPPDATA%\qilin\punchout`). Send `punchout.log` (and `punchout.old.log`, from the run before) with any bug report.

## Building

The whole project (recompiler, runtime and generated code) is C++23. You need CMake 3.25 or newer and one of:

| Compiler | Minimum | Notes |
|---|---|---|
| GCC | 14 | Debian 13 / Raspberry Pi OS (Trixie) and Ubuntu 24.04 (`g++-14`) have it |
| Clang | 19 | Older Clang can't use libstdc++'s `std::expected` |
| Apple Clang | Xcode 16 | |
| MSVC | Visual Studio 2022 17.10 | |

CMake stops with a message if the compiler is older or the target is 32-bit. It fetches the rest at pinned versions and builds it in: SDL3, the Vulkan headers, volk and Dear ImGui. Running needs a Vulkan 1.3 driver.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Without game data the build links every runtime source into `punchout_runtime_check`, a build check rather than a playable program; the tests need no game files. `-DQILIN_SANITIZE=address,undefined` builds with the sanitizers (GCC, Clang). CI builds and tests on every platform in the Status table (graphics tests under Xvfb and Mesa on Linux), runs the scheduler, save and audio tests under ASan/UBSan and the decoder sweep, and checks the committed SPIR-V shaders against their sources.

## Using it

Extract `main.dol` from your disc first (`docs/dol-analysis.md` says how).

```sh
build/qilin info     path/to/main.dol [--json info.json]   # SHA-256, supported build, entry point, sections, BSS
build/qilin profile  path/to/main.dol                      # instruction profile of the code
build/qilin insns    path/to/main.dol [-o listing.txt]     # every word of the code, decoded
build/qilin disasm   words.bin [--base 0x80004000]         # a file of big-endian words, decoded
build/qilin cfg      path/to/main.dol [--json cfg.json]    # function discovery and control flow, summary
build/qilin coverage path/to/main.dol [--list]             # every word: code, padding, dead or unknown (exit 3 if any unknown)
build/qilin suspects path/to/main.dol                      # functions with problems, and rejected starts, for review
build/qilin lift     path/to/main.dol -o generated         # every function as C++ (or a stub saying why not); keep out of git
build/qilin --version
```

### Libraries and hardware access

```sh
python3 tools/analysis/identify_libraries.py --qilin build/qilin --dol path/to/main.dol [--names-out pal_names.txt]
python3 tools/analysis/hardware_access.py   --qilin build/qilin --dol path/to/main.dol [--list]
```

The first finds every library in `main.dol` and prints its range, counts and plan (`docs/code-analysis.md`); the names file is derived from the game, so don't commit it. The second lists every load and store to a hardware register, the GX FIFO, an uncached mirror or the locked cache, by library (`docs/runtime-analysis.md`).

### Decoder check

```sh
python3 tools/check/verify_decoder.py --qilin build/qilin --dol path/to/main.dol   # every word of the game's code
python3 tools/check/verify_decoder.py --qilin build/qilin --sweep                  # the encoding space (CI runs this)
```

Compares the decoder with GNU objdump (`-M broadway,raw`): the mnemonic and every operand must match, and a word must be valid for both or neither. Reviewed differences (encodings binutils accepts that Broadway reserves, and `mftb`) are counted by class; anything else fails. Needs a PowerPC objdump: `apt install binutils-powerpc-linux-gnu`, or devkitPPC's `powerpc-eabi-objdump` with `OBJDUMP` set.

### Picture check

```sh
python3 tools/check/picture_check.py --game build-game/punchout --disc "<extracted disc>" \
    --dolphin <dolphin-emu-nogui> --dolphin-user <dir> [--save <user folder>] [--slow] [--colour console|true|both]
```

Captures the scenes in `tools/check/picture_scenes.json` headless, draws each frame with both our renderer (`punchout --render-dff`) and Dolphin's FIFO player at the console's size, and fails when more than 0.1% of pixels are over 8 of 255 off. `--slow` adds the practice fight. Needs Pillow (`python3 -m pip install Pillow`). See `docs/runtime.md`, "Comparing a frame with Dolphin".

### Fight timing check

```sh
python3 tools/check/timing_check.py --reference dolphin/capture.json \
  --candidate recomp/capture.json --checks intervals.json --out timing-report.json
```

Compares cue-to-attack intervals and dodge windows between matched Dolphin and recomp captures, within explicit frame tolerances; missing evidence fails, and offscreen dumps write `frames.csv` so missing retraces are caught. The synthetic tests pass; actual opponent timings are not yet checked (`docs/timing-validation.md`).

## Layout

| Path | What |
|---|---|
| `recomp/src/` | Recompiler: `dol` loader, `sha256`, `decoder` (Broadway), `cfg` (function discovery and control flow), `lift` (the lifter), `main` (the `qilin` CLI) |
| `recomp/tests/` | Unit tests on synthetic inputs; `difftest/`: the lifter's differential test (case generator, reference interpreter, runner) |
| `runtime/` | The guest CPU state, the 4 GB guest address space, system and error reporting for lifted code |
| `host/` | The platform layer: window, Vulkan, audio, paths, shaders |
| `punchout/` | The game's runtime: boot, OS, DVD, VI, NAND, WPAD, GX front end and renderer, sound, settings and the menu |
| `data/R7PP01/` | `plan.tsv`: the lift / replace / drop plan for each library range |
| `tools/check/` | Decoder, picture and timing checks; the ARM64 cross-build |
| `tools/analysis/` | Library identification, hardware access, replacement checks, profiling and FIFO-log tools |
| `tools/player-kit/` | The Windows, Linux and macOS downloads: packaging, checks, README templates; `launcher/` is the players' launcher |
| `tools/pi/`, `tools/tests/` | The Pi 4 cross toolchain file; launcher and timing-tool tests |
| `third_party/rhf/` | The Rhythm Heaven Fever decomp's symbols and splits (CC0), for the libraries both games share |
| `docs/` | Roadmap and design targets, prior art, naming, analysis and status notes |

## Versions and changes

Releases are tagged `vX.Y.Z` on GitHub, and every release is recorded in [`CHANGELOG.md`](CHANGELOG.md). The version lives in one place, `project(... VERSION ...)` in `CMakeLists.txt`, and `qilin --version` prints it. A minor bump (0.**x**) marks a milestone and a patch bump (0.x.**y**) a fix. 1.0.0 will be the first build that plays the whole game.

## Licence

This project's own code and documentation are under the [GNU General Public License, version 3](LICENSE) (GPL-3.0): you may use, share, change and sell it, but anything you distribute that is built from it must come with its full source under the same licence. `third_party/rhf/` keeps its own licence, CC0 1.0 (`third_party/rhf/LICENSE`). The dependencies CMake fetches keep theirs: SDL3 (zlib), the Vulkan headers (Apache-2.0 or MIT), volk (MIT) and Dear ImGui (MIT); each download carries them under `LICENSES/`, with its compiler's (llvm-mingw, zig) and, on macOS, MoltenVK's.

Punch-Out!! itself, its code and data, belong to Nintendo and are not covered by anything here: you need your own copy, and nothing built from it may be shared.

## Design targets

- **Efficiency and reliability come first.** Every choice is judged by whether it makes the game cheaper to run on low-end hardware, with a Raspberry Pi 4–class board as the reference, and whether it fails loudly instead of misbehaving quietly.
- **Static recompilation, PowerPC → C++, compiled ahead of time.** No interpreter and no JIT. Hand-written C++ can override any lifted function.
- **Vulkan 1.3 only**, with a small baseline feature set (1.3 core plus what the Pi 4's driver offers), so one render path runs everywhere. GX is translated at API level.
- **The SDK's hardware-facing libraries are replaced natively:** OS, DVD, VI, WPAD (with its Bluetooth stack) and NAND. GX, AI, AX and DSP are lifted, with the GPU's command stream and the DSP's AX microcode emulated under them. The C/C++ runtime, the SDK's software libraries, THP, the HOME Button Menu, Audiokinetic Wwise and Next Level Games' engine are lifted with the game.
- **Ground truth is Dolphin** running the original game, reaction timing included: cue-to-attack timing and dodge windows are behaviour in Punch-Out!!.

See [`docs/`](docs/) for the roadmap and the analysis notes.
