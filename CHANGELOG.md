# Changelog

All notable changes to this project are recorded here, newest first. Past entries are never rewritten.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow [Semantic Versioning](https://semver.org/spec/v2.0.0.html):

- **0.x.y** while the recompiler is being built. A **minor** bump (0.**x**) is a milestone, for example a new lifter slice or the runtime booting. A **patch** bump (0.x.**y**) is a fix.
- **1.0.0** will be the first build that plays the game end to end at original spec.

Each release is a git tag (`vX.Y.Z`) with a matching GitHub release. The version is set once, in `project(... VERSION ...)` in `CMakeLists.txt`, and `qilin --version` prints it.

## [Unreleased]

## [0.13.7] - 2026-11-19
### Fixed
- **`fopen` reached dropped code:** MSL's `fopen`, which the game calls from four places, opens files through MetroTRK's host-file open, which was dropped with the rest of the debugger stub. That function is now replaced (a new override in `identify_libraries.py`, so `plan.tsv` gains a line) by what it does without a debugger: it fails, so `fopen` returns NULL as on a retail Wii.

### Changed
- `check_replacements.py` also reports lifted code that calls dropped code. That's how this was found; there's none left.
- The one suspect in `qilin suspects` (`0x801C95E8`, MetroTRK's exception entry reaching the `lmw r0,0(r3)` invalid form) is confirmed harmless: it's dropped, and only `__start` (replaced) calls it.

## [0.13.6] - 2026-11-19
Every replaced function that lifted code calls directly now has a replacement.

### Added
- **The five that didn't:**
  - `OSTicksToCalendarTime`;
  - the game's four ways out: `OSReturnToMenu`, `OSReturnToDataManager` and `OSShutdownSystem` close the program, and `OSRestart` (Reset) starts it again with the same command line.

  Without them, the game would have stopped the first time the player left through the HOME Button Menu, or when it worked out a date.
- **`tools/analysis/check_replacements.py`** lists any replaced function the lifted code calls that has no replacement, with its callers. It decides each address by the narrowest plan range, as the lifter does.
- **Tests** for dates (leap days, weekdays, before 2000, and `OSGetTime` against the PC's clock), and for the power-off exit and the restart. These are two new `ctest` entries: the restarted program must pass the whole thread test.

### Fixed
- **Dates:** the time base counted from start-up, so every date the game worked out was 1 January 2000. It now starts at the PC's local time counted from 1 January 2000, as the console's does.

## [0.13.5] - 2026-11-19
### Fixed
- **MinGW builds:** MinGW's standard library already defines `NOMINMAX`, so defining it again before `<windows.h>` (in `runtime/src/memory.cpp` and `punchout/src/threads.cpp`) was a warning, which is an error with `QILIN_WERROR`. Both now define it only if it isn't defined.

### Added
- **`tools/check/windows_compile.sh`** compiles the Windows-only code (memory reservation, host threads) and the tests that use it for Windows with MinGW-w64, on Linux. It runs the tests under Wine where Wine works. With a game build's fetched headers, it also checks that every hand-written game and host source compiles for Windows (they all do). This is how the two `<windows.h>` problems above and in v0.13.4 were found without a Windows machine.

## [0.13.4] - 2026-11-19
### Fixed
- **The game didn't compile with MSVC:** `threads.cpp` included `<windows.h>` without `NOMINMAX`, so its `min` and `max` macros broke the file's `std::min` calls. CI now builds that file on Windows (for the thread test), which would have caught it.

## [0.13.3] - 2026-11-19
The renderer stops leaking GPU memory, and does less work a frame.

### Fixed
- **A leak of GPU images:** a texture made and released in the same frame (two EFB copies to one address, three times a frame on the boot screens) was never freed. A frame's releases are now handled after it's drawn. A release of a texture that was never made is fatal.

### Changed
- **GPU images are reused** instead of being created and destroyed every frame. EFB copy targets, uploaded textures and XFB images become spares once their frame's fence passes, and spares unused for two seconds are freed. On the boot screens, nothing is created after start-up.
- **Texture checks are about 5x cheaper:**
  - A faster hash (four lanes, about 2.8x).
  - Each texture is hashed once per frame, or after each invalidation of the GPU's texture cache (BP `0x66`), rather than at every draw.
  - On the save dialog this went from 3 ms a frame to 0.6.
- **The stats line reports drawing:**
  - recording, texture checks and decoding;
  - waits on either side;
  - GPU waits;
  - textures held, spares and images made.

## [0.13.2] - 2026-11-19
The controls can be changed, in a settings file.

### Added
- **`settings.ini`** in the per-user folder, written with the defaults and comments on the first run.
  - It sets each Wii Remote button's inputs: pad buttons, keys by name, or the mouse.
  - It sets the render scale.
  - A mistake stops the game at start-up and names the line or key.
- **`punchout_settings_test`** for the settings file, in `ctest`.

### Changed
- The keyboard is no longer fixed to pad buttons. Each key is bound to a Wii Remote button in the settings, with the same defaults as before.
- The pad's button names moved to `host/include/qilin/host/pad.hpp`.

## [0.13.1] - 2026-11-19
Thread fixes, found by a new test of the thread model that needs no game files.

### Fixed
- **A lost wakeup in `OSWaitCond`:** it released the mutex with a reschedule before sleeping on the condition. A higher-priority thread could take the mutex and signal before anyone was waiting, and the signal was lost. Now releasing and sleeping are one step, as the OS does with its scheduler off.
- **Joining a thread that wakes someone as it ends:** a finishing thread that woke a higher-priority joiner was switched away from and put back as ready, so the joiner found it not finished and waited for ever (a deadlock). A finishing thread now wakes its joiners and mutex waiters without switching.

### Added
- **A test of the thread model** (`punchout_threads_test`, in `ctest`). The real scheduler and interrupt layer run small guest programs written in C++, and the order of events is checked against the OS's rules: preemption, message queues, mutexes and conditions, yield, suspend counts, exit and join, alarms and `OSSleepTicks`. It needs no game files, so CI runs it on every platform.
- **CI checks the shaders:** `punchout/src/shaders.inc` records a hash of the shader sources. `tools/build_shaders.py --check` fails if the sources have changed without regenerating, and it also compiles them where glslang is installed. The check uses a hash, not the SPIR-V bytes, because those differ between glslang versions.

## [0.13.0] - 2026-11-19
The renderer: the game draws, on Vulkan. It shows the strap screen and the save dialog, takes the pointer and A, creates its save and goes on to the intro movie.

### Added
- **The renderer** (`punchout/src/recorder.*`, `texture.*`, `renderer.*`, `gx.hpp`, `shaders/`). The game's thread records; the main thread draws.
  - **Recording:** the decoder hands each primitive to a recorder, which decodes its vertices by the vertex format into one canonical layout and turns quads, strips and fans into index lists. It snapshots the transform, lighting, TEV and pixel state when it changes, decodes textures from guest memory (every GX format, palettes from TMEM) into a cache keyed by address, format, size and a hash of the data, and records EFB copies. A copy to the XFB ends a frame, which goes to the main thread at most two frames ahead.
  - **Drawing:** the EFB is an RGBA8 image and a 32-bit depth buffer at a whole-number scale of 640×528, drawn with dynamic rendering. Pipelines are keyed by blending and primitive type; cull and depth are dynamic state. Copies to textures stay on the GPU and are found again by address. Copies to the XFB become the images VI shows.
  - **Shaders:** a vertex shader does XF's transform, lighting and texture-coordinate generation. One fragment ubershader does TEV: 16 stages, indirect textures, the hardware's rounding, compares, konst and swap tables, the alpha test, dither and fog. A copy shader does the copy formats, halving and gamma. All are GLSL compiled to SPIR-V ahead of time by `tools/build_shaders.py` into a committed header, so nothing compiles at run time and no shader compiler ships. The arithmetic follows Dolphin's documentation; the code is ours.
  - Features the game hasn't been seen to use are logged once when it uses them: line and point widths, logic ops other than copy, EFB depth copies, Z textures, and constant alpha with source-alpha blending.
- **The Wii Remote's pointer:** the sensor bar's two lights as the remote's camera would see them, so KPAD works out the pointer as on hardware. The mouse points over the picture, and its left button is A. The right stick moves the pointer too.
- **Testing aids:**
  - `--offscreen` runs the renderer without a window.
  - `PUNCHOUT_DUMP=dir` saves every 60th frame shown as a PPM.
  - `PUNCHOUT_SCALE` sets the EFB scale.
  - `PUNCHOUT_INPUT="Y@16,Start@20"` presses buttons at given seconds.
  - `PUNCHOUT_POINTER="x,y"` holds the pointer.

### Changed
- VI shows the renderer's XFB image when it has one, and otherwise converts the XFB in guest memory on the CPU, as before.
- Sound runs only with a window.

## [0.12.0] - 2026-11-18
Sound, and the front end running: past audio start-up, the game reaches its menus at 59 frames a second, mixing sound every 3 ms.

### Added
- **Sound** (`punchout/src/dsp.cpp`, `ax.cpp`, `host/audio.*`): AI, AX and DSP are lifted. The runtime emulates the DSP (registers, mailboxes, the level-triggered interrupt, the ROM's task loader, the audio DMA) and runs the AX microcode the game boots at a high level, after Dolphin's documentation of it:
  - command lists, voices (ADPCM, PCM8, PCM16, loops, resampling, envelope, low-pass and biquad filters, ramps);
  - aux buses, the compressor, output.
  - Out through SDL3 at 32 kHz. Polyphase resampling needs the DSP ROM's table as `dsp_coef.bin` in the user folder; without it, linear.
- **The audio interface's registers:** rates, volume, the sample counter.
- **Hardware registers by device** (`hw.cpp`): each device maps its ranges; anything else is fatal.
- **Poll loops** (`recomp/src/idle.*`): loops that wait on memory call the runtime's idle hook on their back edge, where interrupts arrive and an idle wait sleeps. The lifter tells them from scans by dataflow over registers and stack slots, with unit tests.

### Changed
- The OS's interrupt mask functions set the DSP control register's interrupt enables, as the real OS does.
- The five-second stats line reports sound: DMA buffers, command lists, voices, mails.

## [0.11.0] - 2026-11-18
Saves, and the game's frame loop: it shows its boot flow and loads its front end, up to audio.

### Added
- **NAND** (`punchout/src/nand.cpp`): the Wii's flash file system as a folder under the per-user folder (`host/paths.hpp`; `PUNCHOUT_SAVE_DIR` overrides it), with the SDK's paths, current directory, errors, file handles and asynchronous calls.
- **The game's console output:** the debug serial port it writes to becomes `game: ...` log lines.
- Screen-burn dimming calls (`VIResetDimmingCount` and the switch) answer without dimming.

### Fixed
- **The write-gather pipe** now gathers 32-byte bursts, so a command reaches the decoder when the hardware would send it.
- **The pixel engine's interrupts** come after a GPU latency (50 µs), not at once: the game installs its draw-done callback after issuing the command.
- **Library boundaries:**
  - MTX's vector functions at the start of the OS range, and CX's decompression after NAND, are lifted;
  - MetroTRK's `__TRK_write_console`, which MSL calls for every console write, is replaced by nothing;
  - `OSGetConsoleType` was taken for `__write_console`; it is now named and answered as itself.

## [0.10.0] - 2026-11-18
With the extracted disc, the game loads its data, sets up its front end and brings up the Wii Remote, on the button-only controls.

### Added
- **WPAD** (`punchout/src/wpad.cpp`): one sideways Wii Remote on channel 0, buttons only, connected, sampled at 200 Hz, with rumble; the other channels empty; no pointer, no extensions, no Balance Board. Buttons come from the pad or keyboard as the design doc maps them.
- **Pad and keyboard input** in the host layer (SDL gamepads by Xbox names, the left stick as a D-pad), and rumble.
- **`PUNCHOUT_TRACE=1`** logs every call into a replacement with its caller.

### Fixed
- `OSInit` leaves interrupts on, as the real one does: before, nothing queued (a read finishing) ever reached the main thread.
- `OSYieldThread` is a safe point: the game waits for its loader thread by yielding in a loop.
- The WBC/WPAD boundary: WBC's API (`8021636C`–`8021698C`) was on the WPAD side and is now lifted with the rest of WBC.
- Five WPAD names that size matching had one off (they're branches into WUD, and their targets name them).

## [0.9.0] - 2026-11-18
The host platform: a window, Vulkan 1.3, and the guest on threads of its own.

### Added
- **`host/`:** an SDL3 window with a Vulkan 1.3 swapchain, or a headless target. The device check requires dynamic rendering and synchronization2 and names anything missing. Frames are shown letterboxed at the game's aspect.
- **Dependencies fetched by CMake at pinned versions** and built in: SDL 3.4.20, Vulkan-Headers and volk (`vulkan-sdk-1.4.363.0`; the API used stays 1.3). Building needs no Vulkan SDK.
- **`qilin_vktest`:** presents a frame headless and checks it read back; skipped without a Vulkan 1.3 device.
- **`punchout` opens a window:**
  - the main thread handles the window and shows frames, while the guest runs on host threads of its own;
  - VI hands over each retrace's frame buffer, converted from YUV for now;
  - `--headless` runs without a window.

### Changed
- `docs/roadmap-and-design-targets.md` records the GX decision of 0.8.0, the thread model and the platform layer.

## [0.8.0] - 2026-11-18
GX is lifted whole, and the runtime decodes its command stream: the game brings up its graphics and starts loading data.

### Changed
- **GX is lifted** instead of replaced. The engine writes GPU commands straight into the FIFO, so the command stream is the boundary: GX's state tracking and register packing stay exactly as shipped (11,535 functions lifted, 1,759 replaced, 193 dropped, 0 stubs; 85% of the code lifted).

### Added
- **The GPU's front end** (`punchout/src/gpu.cpp`): the CP, PE and PI registers GX uses, the write-gather pipe, and a decoder for the whole command stream (CP, XF and BP loads, indexed loads, display lists, primitives sized from the vertex descriptor and attribute tables), raising the pixel engine's draw-done and token interrupts. An unknown command stops the runtime with the bytes around it.
- **Hardware registers reach the game layer** through `rt::set_mmio`.
- **Interrupt handlers and masks** (`__OSSetInterruptHandler`, `__OSUnmaskInterrupts` …), and the write-gather pipe's address register.

## [0.7.0] - 2026-11-18
The runtime skeleton: the recompiled game builds from your own `main.dol`, boots as `__start` boots it, runs its static constructors and enters `main`, as far as its first graphics call.

### Added
- **The library plan as data** (`data/R7PP01/plan.tsv`, written by `identify_libraries.py --plan-out`), with narrower ranges that lift pure code inside a replaced library: the OS's heap and arena code, `OSReport`, and the EABI's `__init_user`, `__init_cpp` and `exit`.
- **`qilin lift --plan`:** replaced and dropped functions become thunks into the runtime's registry; everything else is lifted: 11,350 functions lifted, 1,944 replaced, 193 dropped, 0 stubs. `--files n` writes a fixed number of files for the build.
- **The guest machine** (`runtime/src/system.cpp`): memory with MEM1, MEM2, the uncached mirrors and the locked cache; the dispatcher; the registry, which refuses a replacement for lifted code and names any replaced function called without one; fatal errors with the guest's call stack.
- **The `punchout` executable** (`punchout/`), built when CMake is given `QILIN_GAME_DOL`. It checks the build, writes the boot information a retail Wii with IOS53 leaves in low memory, and starts the game natively in `__start`'s order.
- **Replacements:**
  - OS start-up, interrupts, caches, the locked cache's DMA, time, memory sizes, reset and power callbacks, error handlers;
  - threads, mutexes, condition variables, message queues and alarms, with one host thread per guest thread and the OS's scheduling rules;
  - interrupts delivered at safe points;
  - DVD from `sys/fst.bin` and `files/`;
  - VI's retrace at 60 Hz;
  - the console settings.
- **`hardware_access.py`** classifies low-memory globals: lifted code reads only the bus clock.
- `docs/runtime.md`.

## [0.6.0] - 2026-11-18
Lifter slice 3: paired singles. 13,392 of 13,487 functions (99.3%) and 99.8% of the instructions are lifted: every function in the libraries we lift. The 95 stubs left are all in code we replace or drop.

### Added
- **Paired singles in the lifter:** every `ps_*` instruction the game uses (arithmetic, multiply-adds, `ps_sum`, `ps_merge`, moves, `ps_sel`, the estimates, compares), each half by the same rules as the scalar single-precision instructions, both halves read before either is written.
- **`psq_l`/`psq_st`** in all their forms, through `psq_load`/`psq_store` in the runtime, which apply the GQR at run time: float, u8, u16, s8 and s16 with signed scales, clamping and truncation on store, `W=1` single-element forms; an invalid GQR type traps. Quantising a NaN to an integer stores 0, which isn't verified on hardware.
- **Known-answer tests** for the quantisation conversions.
- **The differential test** covers paired singles from random GQRs: 5,061 cases, 323,904 runs, 0 differences.

### Changed
- `dcbz_l` and paired-single record forms are the only paired-single encodings left out; they fail loudly with their own reasons.

## [0.5.0] - 2026-11-18
Lifter slice 2: floating point, bit for bit as Broadway computes it. 12,782 of 13,487 functions (94.8%) and 82.9% of the instructions are now lifted.

### Added
- **Floating point in the lifter:** every FP arithmetic, compare, move, conversion, load and store the game uses, with FPRs as host locals (ps0, and ps1 where single-precision results fill it). `rt::fp` reproduces Broadway: NI mode (the game's OS sets it), NaN selection and the PowerPC default NaN, rounding to single with frC at 25 bits, single multiply-adds rounded once, `fctiw`'s result pattern, the bitwise single conversions of `lfs`/`stfs`, and the `fres`/`frsqrte` tables. The rules and tables are those Dolphin's interpreter records from hardware tests; the code is our own. FPSCR access and FP record forms (only in the replaced OS, or nowhere) stay outside and fail loudly.
- **Known-answer tests** for those primitives, including Dolphin's 57 hardware-derived `frsqrte` results and the `fmadds` case where rounding twice differs from hardware.
- **The differential test** covers floating point: 1,080 more cases from random FPR states (special values, subnormals, single boundaries, singles, raw bits), comparing both halves of every FPR: 4,173 cases, 267,072 runs, 0 differences.

### Changed
- Everything builds with `-ffp-contract=off` (GCC, Clang), so the host compiler never fuses a multiply and an add on its own.
- The `divw` rule recorded in 0.4.0 is now confirmed against Dolphin's `Interpreter_Integer.cpp`.

## [0.4.0] - 2026-11-18
The lifter's first slice: integer code to C++, with every function either lifted or a stub that says why not, all of it compiling and linking, and a differential test against an independent reference interpreter.

### Added
- **`recomp/src/lift.*` and `qilin lift`:** one guest function to one C++ function over the runtime's `Cpu`. Guest registers and CR fields are host locals, synced at calls and returns; branches are `goto`s, jump tables a `switch` that traps on anything else, tail calls and fall-throughs calls; indirect calls go through the runtime's dispatcher. Slice 1 covers the integer instruction set the game uses, all branches, the CR operations, LR, CTR, XER and the GQRs. On the game: 10,917 of 13,487 functions (80.9%), 57.8% of the instructions; 2,455 of the 2,570 stubs wait for floating point.
- **`runtime/`:** `qilin/rt/cpu.hpp` (the guest state; memory access, byte-swapped, with everything from `0xC0000000` up sent to the runtime's hooks) and `qilin_rt`'s `GuestMemory` (the 4 GB guest address space, reserved, committed by range; POSIX and Windows).
- **`qilin_difftest`:** 3,093 generated cases (every slice instruction with random valid encodings, and `bc`/`bclr`/`bcctr` programs over every valid BO), 64 random states each, lifted against `recomp/tests/difftest/ref.cpp`, a reference interpreter written from the manual separately from the lifter. 197,952 runs, 0 differences; planted lifter bugs fail at once. In CI on every configuration.
- **Tests** for the lifter's control flow on the synthetic program (calls, tail calls, the switch, the get-PC idiom, indirect tail calls, falling into the next function, the reasons for a stub).

## [0.3.0] - 2026-11-18
The rest of roadmap step 1's code analysis: where the code touches the hardware, whether it can throw, and how it keeps time (`docs/runtime-analysis.md`).

### Added
- **`tools/analysis/hardware_access.py`:** traces the base of every load, store and cache-block instruction through its function and classifies the address (write-gather pipe, legacy and Hollywood registers, uncached MEM1/MEM2, locked cache), by library. Lifted code touches no register and no mirror; it writes the GX FIFO directly (the engine's mesh drawers, 803 stores in 26 functions; the HOME Button Menu; RFL), so the FIFO decoder is required.
- **Exceptions:** no `__throw`, unwinder or catch matching is linked, and only RFL, MSL and the runtime have exception tables. Nothing can throw; the tables are dead data.
- **Timing:** the engine swaps every retrace (swap mode 1, set in data, never changed), limits frames to 1/50 or 1/60 s by video mode, and drives gameplay with a float time step that pauses and scales. Whether the main loop's step is fixed or measured is left for the first boot to log; the port presents at 60 Hz either way.
- `identify_libraries.py` can be imported (`analyse`, `segment_of`) by the other analysis scripts.

### Removed
- `identify_libraries.py --segments`, which did nothing.

### Changed
- **Input is buttons only:** no motion controls and no Balance Board. The game runs its sideways Wii Remote scheme, mapped onto a modern pad or keyboard (`docs/roadmap-and-design-targets.md`).

## [0.2.0] - 2026-11-18
Every library in `main.dol` found and its range fixed, without a decomp of this game, and the lift / replace / drop boundary decided per library: 84% of the code lifted, 15% replaced, 1% dropped, with every privileged instruction and every unknown word outside the lifted code.

### Added
- **`tools/analysis/identify_libraries.py`:** labels functions from four kinds of evidence (size sequences against the Rhythm Heaven Fever decomp, C++ RTTI, the SDK's version banners, strings that name their library), then cuts the text into one contiguous run per library, placing each boundary by call evidence and reporting how far it could move. `--names-out` writes the 1,988 decomp names matched to PAL addresses (derived from the game: not committed).
- **`third_party/rhf`:** the Rhythm Heaven Fever decomp's `symbols.txt` and `splits.txt` (CC0), copied from BeatTheBeat-QilinComp.
- **`docs/code-analysis.md`:** the 40 ranges with function, instruction, floating-point, paired-single and privileged counts, and the plan for each. New against Beat the Beat: Wwise and its silence plug-in, THP and WBC lifted; `db` and ESP replaced; MetroTRK's `.init` vectors dropped. The SDK is linked in alphabetical order of library, and the seven init functions found both by banner and by size all carry the right decomp name.

## [0.1.0] - 2026-11-18
The start: GBTG-Qilincomp's requirements and scope applied to Punch-Out!! (Europe/Australia, `R7PP01` revision 1), on BeatTheBeat-QilinComp's recompiler core, and the first analysis of the PAL `main.dol`: the decoder agreeing with binutils on every word, 13,487 functions, all 187 jump tables proven, and every word of the code accounted for.

### Added
- **The recompiler core from BeatTheBeat-QilinComp v0.4.0:** the DOL loader, the Broadway decoder and its cross-check against GNU binutils, function discovery and CFG, coverage accounting, the `qilin` CLI (`info`, `insns`, `disasm`, `profile`, `cfg`, `coverage`, `suspects`), the tests and CI (Linux x64 with GCC 14 and Clang 19, Linux ARM64, macOS ARM64 and x64, Windows x64, warnings as errors, plus the decoder sweep).
- **The supported build:** `main.dol` of `R7PP01` revision 1, SHA-256 `6ae3388f…e66137`, from the user's copy.
- **Jump tables Punch-Out!! has**, each proven (`docs/recompiler-core.md`): the bound check placed after the index shift; a table base set at a loop's head after earlier uses of the register; a strength-reduced index in a loop (indexed by a register stepping by 4, checked on one stepping by 1); and for both region rules, no pointer in data into the region but the table's own.
- **Case labels of proven tables are never seeded as functions** from data pointers. A word of binary data in `.rodata` equal to a case label of a 416-case switch had split that switch into 286 false functions.
- **`qilin suspects`** lists the rejected starts' addresses.
- **Tests** for each new rule, with a negative case for each that must stay unproven.
- **Docs:** `docs/roadmap-and-design-targets.md` (the targets applied to this game: reaction timing, Wwise, Next Level Games' engine, motion and Balance Board input), `docs/prior-art.md` (no decomp or symbols for this game; where library names can come from), `docs/naming.md`, `docs/dol-analysis.md` (sections, what's linked in, instruction profile, decoder cross-check, functions, coverage, the 101 unknown words identified), `docs/recompiler-core.md`.

### Fixed
- **Soundness of the switch search:** looking back for a register's definition stepped straight through loop heads, so it could take a `lis` above a loop as the table base even when the loop rewrote the register. Crossing a branch target now requires the definition to be the only one reaching the use. No table in Punch-Out!! changed.
- **`qilin cfg` takes 1.9 s on the game instead of 44 s:** the slow part was the false functions above.
