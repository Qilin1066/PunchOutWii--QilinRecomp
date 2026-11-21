# Changelog

All notable changes to this project are recorded here, newest first. Past entries are never rewritten.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow [Semantic Versioning](https://semver.org/spec/v2.0.0.html):

- **0.x.y** while the recompiler is being built. A **minor** bump (0.**x**) is a milestone, for example a new lifter slice or the runtime booting. A **patch** bump (0.x.**y**) is a fix.
- **1.0.0** will be the first build that plays the game end to end at original spec.

Each release is a git tag (`vX.Y.Z`) with a matching GitHub release. The version is set once, in `project(... VERSION ...)` in `CMakeLists.txt`, and `qilin --version` prints it.

## [Unreleased]
### Added
- **The Wii Remote's speaker is heard** from the PC's speakers, mixed into the game's sound (`[controls] speaker`, the Controls page's checkbox; on by default). The game encodes the remote's sound itself (4-bit ADPCM, 6,000 samples a second); `speaker.hpp` decodes it as the remote does. `punchout --check-speaker <disc>` runs the game's own encoder on test signals and checks the decoder against it: exact on every block. In the scenes played here (menus, the HOME Button Menu, fights) the game sends the remote only silence, so the speaker may be used rarely; the log says when it isn't silent.
- **A second player** (`[controls] second_player`, the Controls page's "Second player"): a controller becomes a second Wii Remote, for Head-to-Head, with the same pad buttons as the first. Either the second controller to have a button pressed (the first, with the keyboard and mouse, is player one's), or the first controller with player one on the keyboard. The second remote has buttons, rumble and a pointer of its own (its controller's right stick: Head-to-Head's menus ask player two to choose a Mii). A scripted Head-to-Head reached the split-screen fight with both players' punches landing; its frame matches Dolphin's (99.97% of pixels equal, none more than 1 off). Off by default: then every controller is player one's.
- **Mutexes lend priority** as the console's OS does: a thread that waits for a mutex lends its priority to the owner (and on through any mutex the owner waits for), so a thread in between can't hold both up. `punchout_threads_test` has the classic inversion.
- **`-DQILIN_SANITIZE=address,undefined`** builds everything with the sanitizers (GCC, Clang); the game's lifted code only with `QILIN_SANITIZE_LIFTED` (the differential test's lifted code always has them). The tests, the differential test and the render tests pass under both.
- **True colour** (`[display] true_color`, the Display page's checkbox; on in the desktop and low-end presets, off in "original"): every pixel keeps its 24 bits. The game draws parts of a fight (the practice hologram, Little Mac seen through) in the console's 18-bit mode, with dither, and copies the screen to narrower formats for its shadows; off, all of that is as the console has it, banding and dither included. As Dolphin's "Force 24-Bit Color".
- **The EFB's picture is reinterpreted when the game changes its pixel format** between RGB8 and RGBA6 (the console keeps the same 24 bits a pixel and reads them the other way). The practice fight changes it twice a frame; before, the alpha left by an earlier scene could show for a frame as a glow over the whole screen.
- **`punchout --render-dff <file.dff> <out.ppm> [loops]`** draws a frame captured with `PUNCHOUT_DFF` with our renderer and no game running, starting as Dolphin's FIFO player starts, and writes the picture at the console's size: the two renderers' pictures of the same commands then compare pixel for pixel.
- **`tools/check/picture_check.py` compares pixel for pixel**, in the console's colour and in true colour, and has the practice fight among its scenes (`--slow`): it captures headless, draws the log with both renderers at the console's size with the console's copy filter, and fails when more than 0.1% of pixels are over 8 of 255 off. Today no pixel of any scene is. It sets up Dolphin's user folder itself.
- **The log says how much of a frame the game's own code takes** (each 5 s: "the game's threads: 9.8 ms of work a retrace (59% of one), of which waiting for the GPU thread 2.1 ms"): at 100% the game can't keep 60 frames a second whatever the GPU does.
- **`PUNCHOUT_AUDIO=1`** gives a headless or offscreen run its sound (through SDL's dummy driver in tests), and **`PUNCHOUT_DEV_MISSING_FILES=<list file>`** lets a development run with part of the disc go on past a missing file (read as zeros, and listed), to find which files a scene needs.
- **Licence: GPL-3.0** (`LICENSE`; the README's Licence section). The project had none before, which left every right reserved. The Windows download has it in `LICENSES/`.
- **Project licence: MIT** (`LICENSE`), as in the other QilinRecomp projects. The README's new Licence section lists what keeps its own licence (the CC0 symbol list and the fetched libraries) and that the game is Nintendo's.
- **The Controls page's line of shortcut keys mentions F3 (frame times)**, as in Beat the Beat, Grabbed by the Ghoulies and APB.
- **Quit is at the top of the F1 menu**, beside Close, on every page (it was at the bottom of the Game page). Quitting, closing the window, the game leaving and a reset now all save the pipeline cache and the pipelines met since its last save first (they were lost if the program ended within 20 s of meeting them, so the same stutter came back next time).
- **Refresh rates as in Beat the Beat:** fullscreen on a display whose refresh isn't a whole multiple of 60 Hz (75, 144, 165 Hz) switches to the highest mode at the same resolution that is (`[display] match_refresh`, on): a 144 Hz monitor runs at 120 Hz and the game's frames are each shown twice, evenly, instead of a 2-3-2-3 judder, and the game's clock locks to it. **Variable refresh** (`variable_refresh`, off: turn it on for a G-Sync, FreeSync or VRR display with it on in the driver; no program can see that setting): no switch, no locking, the display follows the game's 60 frames a second. Both are in the Display page's new Refresh rate section, which also says what fullscreen chose; the log names the display's mode.
- **Internal resolution: auto** (`internal_scale = auto`, and the desktop preset's choice now): the smallest whole multiple whose 480 picture lines fill the screen's height (3x at 1440p, 5x at 2160p). The GPU's work grows with the square of the multiple: 8x draws 64 times 1x's pixels, 21.6 million a frame, which is most of the 9-11 ms of GPU time at 8x on a GTX 1080 Ti; the menu says so beside the slider.
- **`./punchout.sh`: one command to build and play on Linux, the Raspberry Pi 4 and macOS**, as `Play Punch-Out.bat` on Windows: `--disc <folder>` the first time, then `./punchout.sh` to play and `./punchout.sh update` to pull, rebuild and play. It checks for CMake, GCC 14+ or Clang 19+ and (Debian, Ubuntu, Raspberry Pi OS) SDL3's build packages, says what to install, builds for the machine's own CPU (the new `QILIN_NATIVE` option: `-mcpu=native` on ARM, `-march=native` on x86) and runs as many compiles at once as the memory allows (about 1.5 GB each). The README has the Pi's steps.
- **`tools/check/arm64_build.sh`** cross-builds everything for the Pi 4's Cortex-A72 (`tools/pi/aarch64-cross.cmake`, Debian's `g++-14-aarch64-linux-gnu`), the code lifted from the disc included (the lifter runs under qemu), and runs the unit tests under qemu. The whole game builds and links for ARM64 with warnings as errors.
- **`tools/check/picture_check.py`** compares our picture of a set of scenes with Dolphin's render of the same frame (`tools/check/picture_scenes.json`: a new save's notice, the title screen, the main menu; the VS screen with `--slow`), each from a fresh save or a copy of a given one with scripted input, and fails over a per-scene limit (RMSE, and pixels more than 48 off); `report.html` has them side by side with the difference. Today: 1.4%, 1.9% and 2.9% RMSE, under 0.6% of pixels off.
- **A log file, and crash reports in it:** everything the game prints also goes to `punchout.log` beside the settings (the run before is kept as `punchout.old.log`); on Windows the console closes with the game, so this is what's left to read or send after a problem. A crash the game didn't catch itself (an access violation, an illegal instruction, an exception nothing caught) is reported there with the guest thread's registers and call stack, as a fatal error already was, and every fatal error names the log.
- **A watchdog in the log:** when the game draws no new frame for 10 s while not paused, or the main loop (input, drawing, presenting) stops for 5 s, the log says so, and again when it goes on. It only watches.
- **Where a frame's time goes**, under F3's graph and in the log each 5 s: the game's recording, the main thread's drawing, the GPU's own time (timestamp queries, where the queue has them), waiting to present, and the sound queued.
- **Frame times on screen and in the log:** F3 (or `performance_overlay = true` in `[display]`, or the Display page's checkbox) shows the frame rate, the 1% and 0.1% lows (the frame rate of the slowest 1% and 0.1% of frames over the last 20 s), the worst frame and a graph of the last four seconds, top left. Every 5 s the log has the same numbers, and how late the game's retraces were posted (average, worst, how many over 1 ms and 4 ms). Pauses for the settings menu don't count. `qilin_frame_times_test` checks the maths. F3 can't be bound to a control.
- **`PUNCHOUT_DFF=<file>@<seconds>`** writes the first whole frame after that much game time as a Dolphin FIFO log (registers, the frame's commands, RAM as it starts, and each piece of memory the GPU reads that changed since, where it read it, as Dolphin's recorder does), our own picture of that frame (`<file>.ppm`) and every primitive's place in it (`<file>.prims`), so Dolphin's FIFO player can render the same frame as a reference.
- **`tools/make_pipeline_seed.py`** makes the built-in list of pipelines to compile at start (`punchout/src/pipeline_seed.inc`) from `pipelines.keys` files.
- **`tools/analysis/dff_cut.py`** cuts such a log after any primitive, optionally changing registers just before it, to see Dolphin's picture part way through the frame.
- **`PUNCHOUT_PROBE=x,y`** logs every draw and copy that changes one pixel of the captured frame, with the draw's state; `PUNCHOUT_PROBE_DIR` saves the frame's texture copies; `PUNCHOUT_PROBE_SOLO=<draw>` (with `PUNCHOUT_PROBE_SOLO_MODE`) draws one unblended. With the cuts, this found the main menu's bug below. (`docs/runtime.md`, "Comparing a frame with Dolphin".)

### Changed
- **Fights need less of the CPU again: single-precision leaf functions are lifted as single precision.** The loops that skin a fight's vertices did every single operation as a double one, rounded and widened back. They now keep their registers as floats and use the host's single arithmetic, with the double code as the path for anything unusual and for callers that pass real doubles (`docs/recompiler-core.md`). No result changes (three million operands checked against the double helpers, and the differential test runs the single versions). In the practice fight on a 2-core test machine: 275-289 frames per 300 retraces before, 293-297 now.
- **The game's thread hands commands to the GPU thread 256 bytes at a time** instead of 32 (a fight's frame is 13,000 bursts, each of which took a lock), still at once whenever the game could wait for the GPU or read its registers; and a frame's vertex buffers are no longer zeroed before they are filled.
- **EFB copies round as the console does:** whole bytes through the copy filter, the intensity weights in integers (a half rounded up), narrower formats keeping their top bits instead of rounding, the half-scale box adding its four pixels and dropping the remainder, a six-bit EFB read as eight bits with the top two repeated. Shadows and glows built from copies were a step lighter or darker in places.
- **Fights need less of the CPU: the locked cache is plain memory.** The game skins every vertex of a fight in the Wii's 16 KB locked cache (`0xE0000000`), and each load and store there went through the slow branch meant for hardware registers (a call and three range checks). Lifted code now reaches it as it reaches RAM; only `0xC0000000`-`0xDFFFFFFF` (the uncached mirrors, the write-gather pipe, the hardware) takes the slow branch. In the practice fight on a 2-core test machine, frames drawn per 300 retraces went from 253-271 to 275-289. `punchout_threads_test` checks the map (locked cache, both mirrors, each access size).
- **`PUNCHOUT_PROFILE` says which thread** each sample was on (the report lists each thread's share and its hottest functions), names functions in libraries, and with `PUNCHOUT_PROFILE_FROM=<seconds of game time>` leaves out everything before one scene. **`--headless` takes `PUNCHOUT_INPUT` and `PUNCHOUT_POINTER`** (it had no input at all), so a scripted run reaches a fight without drawing it.
- **The repository is now Qilin1066/PunchOutWii--QilinRecomp** (renamed by the owner, as were the sister projects: GBTGhoulies--QilinRecomp, ApbArcade--QilinRecomp, and Beat the Beat's, now RhythmHeavenFever--QilinRecomp). Links, the docs and the download's name (`PunchOutWii--QilinRecomp-<version>-windows-x64.zip`, unpacking to `PunchOutWii--QilinRecomp`) follow; GitHub redirects the old addresses. The settings and saves folder is unchanged.
- **F3 says what the display does with the game's frames:** the game draws 60 frames a second, as on the Wii, at every refresh rate, so the counter stays at 60; a faster display shows each frame more than once, each sooner after it's drawn. Under the counter: "display 120 Hz: each frame shown 2 times", or, for a refresh that isn't a multiple of 60 (144 Hz in a window, say), that the frames reach the screen unevenly; fullscreen with `match_refresh` switches to an even mode.
- **Frames are presented just ahead of the display's refresh, every frame alike** (Windows, retraces locked to the display, vsync on, variable refresh off): the game's clock is nudged, by at most 0.2%, so each frame goes to the screen a few ms before a refresh (1 ms plus four times the spread, at least 3 ms) instead of drifting through the refresh period, which brought spells where every frame waited most of a refresh before it was seen. As in Beat the Beat. The log says how far ahead frames went and how many just missed a refresh.
- **Windows: the main loop waits on a high-resolution timer and a wake-up event** instead of a condition variable with 1.5 ms of spin, the drawing thread registers with the system's "Games" scheduling class (MMCSS), and the process opts out of Windows 11's power throttling, which could slow it when the window was covered.
- **Clearing the screen or depth costs the GPU less:** a copy that clears all of colour and depth (or one of them alone, whole) uses the GPU's own clear instead of drawing a rectangle over every pixel, at 8x 21.6 million of them.
- **New effects the first time (a power attack, say) slow the game far less:** a draw whose specialised pipeline isn't ready yet is drawn by the ubershader, which at high internal resolutions is slow; such pipelines now go to the front of the compile queue instead of waiting behind the ones remembered at start, up to 8 compiler threads instead of 4 (a core each kept for the game and drawing), and the built-in seed holds 661 specialised pipelines instead of 335 (adding those met in a play session, fights included), so most are ready, or in the driver's cache, before they're needed.
- **The game's own code takes about a quarter less CPU** (headless, the same 95 s of boot and menus: 6.3 → 4.6 CPU seconds in the lifted code and its helpers):
  - Loads and stores are forced inline (`QILIN_INLINE`), their rare paths (the uncached mirrors, the locked cache, the write-gather pipe, hardware) out of line: in the lifted code's large functions the compiler had been leaving a call behind most of them. The executable grows (here 53 → 70 MB) and the lifted code takes a little longer to compile.
  - Paired-single loads and stores of floats (the usual GQR type) are inline; the quantised types stay out of line.
  - The single-to-double conversion is forced inline.
  - Indirect calls (virtual functions, callbacks) look up their target in a 4,096-entry cache before the binary search over every function; an entry is one 64-bit word, so no thread sees one half-written.
  - The Windows download compiles the game for the PC's own processor (`-march=native`: the Wii's fused multiply-adds become one FMA instruction instead of a call into the C library), and builds it again if the folder moves to a PC with a different processor. `./punchout.sh` already did (QILIN_NATIVE); `Update Punch-Out.bat`'s Visual Studio build now does too (/arch:AVX2 when the CPU has AVX2 and FMA, checked when configuring), with MSVC using the FMA intrinsic. The lifted code is compiled with `-ffp-contract=off` in the download too, so no multiply and add is ever fused that the Wii does separately.
  - `--headless` no longer converts every frame to RGB for nobody to see.
- **Recording a frame takes about half the CPU it did, its vertices 42% fewer bytes, and its triangles no index data** (measured with the new `--bench-record` on two captured main-menu frames: 4.3 → 2.4 ms and 4.0 → 2.0 ms here):
  - A new draw state is copied from the one before and only the parts whose registers changed are redone (the matrices, normal matrices, post-transform matrices, lights, XF registers, or the BP side), instead of 5 KB rebuilt for each of the ~370 states a frame. `PUNCHOUT_CHECK_DECODE=1` also checks each against one built whole.
  - Each attribute's loop is compiled for its way of fetching (inline, 8- or 16-bit index), with the bounds and capture checks hoisted out, and its common formats (float positions and normals, float and signed 16-bit texture coordinates) unrolled.
  - Triangle strips (the game's models are all strips: 2.7 million vertices in 11,400 of them over 30 menu frames, and nothing else but a few quads), triangle lists, lines, line strips and points are drawn as Vulkan draws of the vertices in order, with the topology set per draw (Vulkan 1.3's dynamic topology, within the pipeline's class, so no new pipelines), instead of indices generated, uploaded and fetched: about 1 MB a frame less. Quads and fans keep indices. A new render test checks that a strip's alternate triangles wind as GX's do (with culling on, the whole strip shows or none of it) and that strips and lists cover what they should.
  - The compact vertex layout holds four texture coordinates instead of two (76 bytes instead of 60): the game's models mostly have three, so nearly all of its vertices (88,600 of the main menu's 88,900 a frame) were taking the full 132-byte layout. 11.7 MB of vertices a frame to write, upload and fetch became 6.8 MB.
  - Vertices are decoded an attribute at a time across a primitive's vertices, each loop one format and one way of fetching, instead of a switch per attribute per vertex; float, and signed 16-bit texture coordinates, have their own loops. `PUNCHOUT_CHECK_DECODE=1` checks it against the old decoder byte for byte (the two frames and a run from the title to the fight intro: identical).
  - Writing a register its own value again (the game reloads matrices and BP state that hasn't changed) no longer starts a new draw state.
  - Each texture map remembers its last lookup until its registers change, the texture cache is invalidated, a palette is loaded, an EFB copy is made or the frame ends.
- **Drawing records less per draw:** a draw's textures, viewport, scissor, culling and depth state are only set when they differ from the draw before, texture descriptor sets are found by a hash of a fixed-size key instead of a map of vectors allocated per draw, unused texture slots share one sampler, and the pipeline lookup reuses the last draw's answer while nothing new has been built.
- **No pipeline is ever built mid-frame for a new blend state** where the driver has `VK_EXT_extended_dynamic_state3`'s blend enable, equation and write mask (NVIDIA, AMD, Intel, Mesa's RADV, ANV and lavapipe, among others): blending, the write mask and (with `VK_EXT_extended_dynamic_state2`'s too) the logic op are set per draw, so the ubershader needs only a dozen pipelines (each topology, depth-writing or not, either vertex layout), all built before the first frame, and the specialised ones are shared across blend states (276 to warm up instead of 335). Elsewhere it works as before. `QILIN_NO_DYNAMIC_BLEND=1` turns it off; the log's "drawing with" line says whether it's on. The render tests pass both ways, and frames drawn both ways match.
- **Sound stays at a steady 40 ms of latency:** the sound card's clock and the game's differ by up to a few hundred parts per million, so the queue used to creep up to 150 ms and then drop it all (a skip), or drain to gaps. Now playback runs up to 0.5% faster or slower to hold the queue near 40 ms (far below hearing), a queue that runs dry starts again with 40 ms of silence instead of gap after gap, and only a stall leaving more than 200 ms queued drops sound. The log says how much was queued, the speed, and how often it ran dry or dropped.
- **Frame pacing, for even frames and less latency:**
  - The game's 60 Hz follows the display's own refresh when that is within 0.3 Hz of 60 Hz or of a whole multiple of it (59.94, 120, 119.88, 240 Hz...), checked every second, so its frames land one per refresh (or one per two...) instead of drifting against it and repeating or dropping one every few seconds. Otherwise, and without vsync, it stays a free 60 Hz, which a VRR display follows. The log says which.
  - After a stall of more than a frame the retraces start again from the present instead of firing the missed ones back to back, which ran several game frames at once and then stalled: one hitch, not a stagger.
  - Waits for the next retrace end on time: the OS sleeps until shortly before it and the rest is spent yielding (at most 1.5 ms of a core on Windows, 0.25 ms elsewhere, per retrace). Windows' timer runs at 1 ms (`timeBeginPeriod`) instead of 15.6 ms, which could wake a sleep to a retrace most of a frame late.
  - Thread priorities: the game's threads, the GPU command thread and the main loop above normal, the shader compilers and the cache saver below it (on Linux the background ones are niced; raising needs `CAP_SYS_NICE`), so background work never takes the core a frame is waiting on.
  - With vsync, where the driver has `VK_KHR_present_wait`, each frame waits (50 ms at most) until the one before it is on screen before taking a swapchain image, so at most one frame waits in the queue: up to a frame less input latency than a full FIFO queue. `QILIN_NO_PRESENT_WAIT=1` turns it off.
- **Faster on the CPU, for the Raspberry Pi above all:**
  - Stores to the write-gather pipe (the GPU's command stream; a fight sends about 11 MB every 5 s, four bytes at a time) go straight to it from the store helpers (`rt::g_pipe_write`), not through the hooks, the address checks and the hardware-register table, and a whole word goes into the burst at once.
  - Loads and stores to the uncached mirrors of MEM1 and MEM2 and to the locked cache (where the game does its paired-single work) are plain memory accesses in the helpers' slow branch instead of hook calls. Ordinary RAM accesses are unchanged. On the main menu these paths went from 0.96% to 0.44% of all CPU time (llvmpipe takes most of the rest here).
  - Draw recording decodes vertices straight into the frame's vertex array, in the compact layout when it fits, instead of into a 132-byte temporary that was then copied; triangle indices are written into an array sized once instead of appended a triangle at a time. Main menu: 8.2 ms of recording per frame before, 7.3–7.6 after. The render tests pass on both vertex layouts, and the main menu's frames match apart from its animated background.
- **The ubershader's pipelines are built before the first frame**, the specialised ones by the workers, so even the boot logo's first frame no longer compiles one while drawing.
- **The project is renamed PunchOutWii-QilinRecomp** (it was PunchOutWii-QilinComp), as on GitHub: github.com/Qilin1066/PunchOutWii-QilinRecomp. The README, docs, CMake project (`PunchOutWiiQilinRecomp`), the settings menu's and settings file's text, Vulkan's application name, and the Windows download (`PunchOutWii-QilinRecomp-<version>-windows-x64.zip`, its folder and its workflow) use the new name, and mentions of the sibling projects use theirs: ApbArcade-QilinRecomp, GBTGhoulies-QilinRecomp, BeatTheBeat-QilinRecomp. Unchanged, so nothing saved is lost: the `qilin` tools, the `QILIN_*` options, and the data folder (`qilin/punchout`, with `punchout.ini`); the move from the folder used up to v0.17.0 still looks for its old name. Entries below keep the names of their time.

### Fixed
- **Every controller plays the game, not only the first one SDL lists.** A virtual pad (ViGEm) or a wheel's base listed ahead of the real controller left it dead in the game (the F1 menu already read them all). All are opened and their buttons added up; rumble goes to all of them; a controller unplugged is dropped without disturbing the others.
- **An interrupt that arrives while the OS has it masked is held** and delivered when it is unmasked, as the hardware's line stays up, instead of being dropped (none was seen dropped in play).
- A frame with no vertices of one kind passed a null pointer to `memcpy` (found by the sanitizers; harmless in practice).
- **Sound lagging behind the picture in fights.** The sound queue could grow from its 40 ms to 110-200 ms (seen in the owner's log) whenever the sound device or the game stopped for a moment, and then took minutes to play off. The queue is now our own, fed to SDL as the device asks: its length is known exactly, sound over twice the target is dropped at once down to the target (the log says how much), and after a gap nothing plays until the cushion is back. The target follows the device: 40 ms, more where the device takes its sound in large pieces.
- **`PUNCHOUT_DFF` captures:** a log held only one of each TEV colour register and the konstant that shares its address, whichever was written last, so Dolphin drew some frames in one colour (the log now begins with writes for both); and one capture in a few gave Dolphin's player no FIFO at all, which drew black or garbage (the FIFO's addresses were read while the game was building a display list, which points them at the list).
- **Every controller works the F1 menu**, not only the first one SDL lists (which can be a virtual pad or a wheel's base): Dear ImGui's SDL3 backend is set to read them all, as APB's menu now does.
- **CI was red on every platform.** Linux: install the X11/Wayland, sound and input development libraries SDL3 needs to configure (as APB's CI does). Windows: MSVC rejected `[[gnu::noinline, gnu::cold]]` as an unknown attribute (C5030, an error with warnings as errors); `QILIN_COLD_OUTLINE` gives MSVC `__declspec(noinline)` instead, and the recorder's forced-inline helpers use `__forceinline` there; its two compact-layout checks are `if constexpr` (C4127, a constant condition). macOS: `qilin_padtest` failed where there's no Vulkan loader (no MoltenVK on the runners); it now skips there, as `qilin_vktest` does.
- **Saves can't be left half-written:** the game's "safe" save (NANDPrivateSafeOpen/NANDSafeClose) now works as on the Wii: it writes to a copy in the NAND's `/tmp`, flushes it to the disk, and swaps it in with one rename, so a crash or power cut mid-save keeps the previous save whole. A copy left by such a stop is removed at the next start (and the log says so). Before, it wrote the save in place.
- **Stutter while playing, worst at first:** pipelines are no longer compiled on the drawing thread in the middle of a frame. The ubershader's pipelines (one per blend state the game uses) were made there when first needed, about 180 ms each on llvmpipe and a large fraction of a second on a desktop driver the first time; and the pipeline cache was fetched and written to disk there whenever the background compiler caught up. Now every pipeline the game uses is remembered in `pipelines.keys` beside the settings and compiled by worker threads (up to four) as the game starts, ubershader's first; the ones seen while making this build are built in, so a first run has them too; the cache and keys are written by their own thread at most every 20 s and at exit. A pipeline new to every run still compiles where it's needed, and the log says how long it took (`docs/runtime.md`).
- **The write-gather pipe redirected into memory now writes there** (`GXRedirectWriteGatherPipe`), and the main menu and the Mii faces look as on Dolphin: Little Mac lit in the gym with the punching bag behind him and the spotlight on the floor, where he was a dark shape behind dark slabs and streaks; the profile Mii has its eyes and mouth back (only the hair and beard drew). The game streams the spotlight's light cones (360 positions a frame) into a vertex buffer in MEM2 that way. The SDK points the CPU's FIFO at the buffer with an end of 0x04000000; we read that end without bit 26 (the write pointer's wrap flag, but an ordinary bit of the end), took it as 0, and wrote nothing, so the cones drew from whatever the buffer held, the menu's own sprite vertices. The pointer now also wraps only after the block at its end, as on the hardware and in Dolphin, never because it is past the end. A compile-time check covers the step, redirected into MEM1 and MEM2 included. Earlier comparisons with Dolphin had replayed the same broken buffer from our RAM, which is why both looked dark.
- **EFB copies apply the copy filter** (BP 0x53/0x54), as the hardware does for every copy, to textures and to the frame buffer: the line above, the line and the line below, weighted in 8-bit integers over 64 (Dolphin's maths), colour only; the lines are the console's own at any internal resolution, and stop at the copy's rectangle when its command's clamp bits say so. `punchout_render_test` checks an edge blended with deflicker weights (255 191 63 0, exact; a planted bug fails it). The stripes on the main menu's floor were the light cones above, not the filter.
- **A copy's alpha is 255 unless the EFB's pixel format keeps alpha** (RGBA6_Z24), as on the hardware; it passed on stale alpha. A suspect for the Mii faces drawn transparent (being checked).
- **Textures use the game's own LOD bias and anisotropic filtering** (TEXMODE0's bias in 1/32 steps; its 2x/4x anisotropy, which the console has and a floor at a grazing angle relies on; a minimum LOD above the maximum taking the maximum), as Dolphin applies them. The player's anisotropy setting can raise it, never lower it.
- **The vertex position is `invariant`**, so a surface drawn again with the depth test EQUAL lands at exactly the same depth in every pipeline.

## [0.20.0] - 2026-11-19
A Windows download that needs nothing installed, as the APB recomp has.

### Added
- **The Windows download** (`tools/player-kit`, built by `.github/workflows/player-kit.yml` on every push, and attached to a GitHub Release for a version tag): `PunchOut-QilinComp-<version>-windows-x64.zip`, 78 MB. Unpack it and double-click **Play Punch-Out.bat**: the first time it asks for the extracted disc folder (checking it's the supported build), lifts its `main.dol` and compiles it with the bundled compiler (LLVM-MinGW 20261104, the same pinned release as APB's), then starts the game; after that it starts at once. It holds everything but the game's own code, prebuilt: `qilin.exe`, the library plan, and the runtime, renderer, host layer, SDL3, Dear ImGui and volk merged into `libpunchout_kit.a`. Nothing made from the game is distributed.
  - Checked here: the download built from scratch; `play.ps1` (run under PowerShell 7 with a cross compiler) lifted the game, compiled the 35 files and linked `Punch-Out.exe` (50 MB; Windows system DLLs only; 16 MB stack) in 2 min 48 s on 8 cores; a second start is immediate; an update that doesn't change the game's code takes 3 s; a folder without `sys/main.dol`, and a `main.dol` of another build, are refused with the reason. Wine crashes in this sandbox, so the .exe files weren't run here.
- **From the source, double-click:** `Play Punch-Out.bat` (builds the first time, then starts the game) and `Update Punch-Out.bat` (pulls, rebuilds what changed, starts it), through `tools/build-punchout.ps1`, which finds Visual Studio's CMake and Git itself and remembers the disc folder.

### Changed
- `qilin lift` leaves a file whose text is unchanged alone, so a rebuild after an update recompiles only what changed.
- `main.cpp` declares the lifted code's tables itself instead of including the generated `functions.hpp`, so the runtime builds without the lifted code (the download's prebuilt part).
- Fixed for Clang: a missing `<cstdlib>` in `display.cpp` and an unused table in `settings.cpp`.

## [0.19.0] - 2026-11-19
A whole-disc audit: is every byte of the game's code found? (`docs/code-analysis.md`, "The whole-disc audit".)

### Fixed
- **`exit` would have stopped at an unanswered call.** MSL's `stdin`/`stdout`/`stderr` hold `__read_console` and `__close_console`, in the dropped debugger stub; `exit`'s `__close_all` calls the latter through a pointer. They, and the four host-file procedures `fopen` stores, are replaced by what they return with no debugger (an I/O error).
- **Four Balance Board functions were classed as VI's** and not lifted (`80215B90`–`80215FF8`: the weight maths and two WPAD callbacks). Lifted now: 11,672 functions lifted, 1,630 replaced, 185 dropped, 0 stubs.

### Added
- `check_replacements.py --dol` checks function pointers too: every function whose address lifted code builds, and every one a data word points at. 0 unanswered now.
- `qilin coverage --list` lists the dead runs (712 words in 360 runs). An independent scan confirms no branch, data word or built address points at any of them.
- The disc's file table has no other executable (no `.rel`, `.dol`, `.elf`), the DOL has no module linker, and the game never invalidates the instruction cache: `main.dol` is all the code.

## [0.18.1] - 2026-11-19
### Fixed
- **Controllers already connected when the game starts are used.** The display opened a pad only when SDL announced one being added; it now opens the first one at start-up too, and another takes over when it's unplugged. Controller input is taken whichever window has the focus (the console the game was started from often keeps it).
- **The console says what it found:** each controller SDL sees (name, vendor, product, and whether SDL knows its layout), the one being played with, the first button press from it, and disconnections. `PUNCHOUT_INPUT_LOG=1` logs every change of the buttons.
- `qilin_padtest`: a virtual Xbox controller, connected before the window and another later, must reach the game (skips without a display).

## [0.18.0] - 2026-11-19
Enhancement options, with the same settings menu, file and data folder as the other Qilin recomps (BeatTheBeat and APB).

### Added
- **The settings menu** (Dear ImGui, pinned to the commit BeatTheBeat uses): F1 or Esc, or a controller's Back and Start together. It pauses the game (the guest parks at the next retrace and its clock stops) and saves on closing. A hint says how to open it for the first 8 seconds. Pages:
  - **Controls:** set each Wii Remote button by pressing: *Add*, then a key, a pad button or a click. An input on another button moves over (and says so); click one to remove it; *Back to the defaults*. Rumble on or off.
  - **Display:** full screen and which display; the picture's size (whole multiples, fit, fill); smooth or sharp; vsync; the enhancement preset, internal resolution (from the next start) and anisotropic filtering (at once); the graphics device (from the next start). *Look at the picture* hides the menu until a key.
  - **Game:** language, widescreen or 4:3, mono or stereo, as the game reads them from the Wii's settings; about; quit.
- **Picture looks:** plain, or a CRT television (raster lines on the console's lines, adjustable scanlines and phosphor stripes; after APB's shader). The window now draws the picture with a shader pass instead of a copy, so the menu draws over it.
- **Anisotropic filtering** (1-16x, where the GPU has it) for the game's filtered textures.
- **Pad buttons** LB, RB, LT, RT, L3 and R3 can be bound.
- **F11 or Alt+Enter** switch borderless full screen.

### Changed
- **The settings file is `punchout.ini`** with `[display]`, `[controls]` and `[console]` sections and presets (original, low-end, desktop), as BeatTheBeat's. v0.17.0's `settings.ini` is carried over once and kept as `settings.ini.old`.
- **The data folder is `qilin/punchout`** in the per-user application data (Windows: `%LOCALAPPDATA%\qilin\punchout`), as the other recomps'. The old `%APPDATA%\Qilin\PunchOut-QilinComp` (saves, pipeline cache) is moved there on the first run.
- **HOME's key is H**; Escape opens the menu. Escape, F1 and F11 can't be bound.
- The window's vsync can be turned off (mailbox, or immediate where there's no mailbox).

## [0.17.0] - 2026-11-19
The GPU's command stream is decoded and recorded on a thread of its own.

### Changed
- **A GPU thread.** The guest's thread now only queues the write-gather pipe's bursts (still writing the CPU's FIFO in memory, as before); a separate thread decodes them and records the frames, as the console's GPU works beside its CPU. The game's own thread no longer pays the ~13 ms a fight frame that recording costs, so on a machine with cores to spare (the Raspberry Pi 4 has four) the two run in parallel.
  - Draw-done and token interrupts come when the GPU thread reaches those commands, as on the hardware; a guest thread asleep waiting for one is woken then (`wake_idle`), not at the next timer.
  - The queue is 16 KB, about the hardware's lag. With 1 MB the decoder fell whole draws behind and read vertex arrays and matrices the game had already rewritten: fight frames were visibly corrupt. At 16 KB a scripted run from the title to a knockdown in round 1 is clean.
  - `PUNCHOUT_GPU_THREAD=0` decodes on the guest's thread, as before.
  - In this session's 2-core sandbox the software GPU takes the cores, so the frame rate is unchanged here (34-39 frames per 5 s in a fight); the gain needs spare cores.

## [0.16.0] - 2026-11-19
The recorder's CPU time per fight frame halved: 26 ms to 13 ms (on this session's Xeon core; a Raspberry Pi 4 core is roughly 3x slower).

### Changed
- **A compact vertex layout.** Draws without tangents, binormals or more than two texture coordinates (and none sent before: emboss texgen reads the last) store 60-byte vertices instead of 132, so less to write, upload and fetch. The renderer feeds the attributes they lack from a stride-0 binding of zeros, which is what the shader sees anyway. `PUNCHOUT_FULL_VERTICES=1` forces the full layout; the render test runs both ways (a planted bug in the compact path fails it).
- **Vertex formats are compiled to steps once per primitive**, and indexed arrays resolved to a host pointer and a bound, so a vertex runs only its format's attributes (23% fewer instructions in a decode benchmark).
- **Drawn frames are reused.** A fight frame's vectors hold 10-20 MB; they were allocated fresh each frame and regrown, with page faults and copies. The renderer now hands frames back (`recycle_frame`) and the recorder clears and refills them.
- **`load_single`** keeps the rare denormal case out of line, so the common path inlines into the lifted code.

## [0.15.0] - 2026-11-19
Efficiency for weak hardware: pixel shaders specialised to each draw, and no GPU image churn in fights.

### Added
- **Specialised pixel pipelines.** The ubershader interprets the TEV registers for every pixel, which is what a weak GPU (the Raspberry Pi 4's) can least afford. Each draw's TEV shape (70 words: stage counts, indirect commands, orders, combiners, konst and swap selects, the Z-texture mode, the alpha-test functions; colours stay dynamic) now gets a pipeline with those as specialisation constants, from a build of the same shader per stage count (`kGxFragStages1`-`16`), so the driver folds every decision away and compiles only the stages the draw has.
  - Compiled on a worker thread; the ubershader draws until each is ready, so there's no stutter. The Vulkan pipeline cache goes to `pipelines.bin` in the user folder.
  - Measured on a fight under a software GPU (lavapipe, 2 cores): GPU time per frame 250-307 ms → 126-154 ms, frames per 5 s 16-20 → 31-37. On the no-Mii dialog: 66 → 37 ms.
  - A fight uses about 195 such pipelines. Per-stage-count modules took a pipeline's compile from 14 s to about 2 s on lavapipe (LLVM); GPU drivers are much faster, and they keep them in the cache.
  - `punchout_render_test` runs on both paths with the same expectations (two new ctest entries).
  - `PUNCHOUT_UBERSHADER=1`, `PUNCHOUT_SPECIALIZE_SYNC=1` and `PUNCHOUT_SPECIALIZE_FROM=<seconds>` for comparing them.
- **`PUNCHOUT_PROFILE=<file>`** (Linux): a built-in SIGPROF sampler, for machines without perf or ptrace, and `tools/analysis/profile_report.py` to name the hot functions.

### Fixed
- **GPU images were destroyed and made again every frame in fights.** A fight makes ~75 EFB copies a frame; the spare pool was capped at 64, so each frame's returned images were trimmed and remade (about 9 a frame). The cap now covers the batch being recycled; images unused for two seconds still go. Now 0 a frame.

## [0.14.0] - 2026-11-19
The first fight: from the title screen through profile creation, the main menu and Career to round 1 against Glass Joe, with punches landing. Checked from frame dumps here (software GPU); not yet on real hardware.

### Fixed
- **Display lists built at run time** (GXBeginDisplayList, used for the Mii heads among others) were decoded at once instead of written to memory, so the decoder lost its place right after the "no Mii characters" dialog. The write-gather pipe now writes each 32-byte burst to the CPU's FIFO in memory at the PI's write pointer, as the hardware does, and the decoder sees it only while the command processor is linked to that FIFO (CP control bit 4).
- **The PI's FIFO write pointer** wraps at the end with bit 26 set (GXGetFifoWrap) and keeps bit 28, so FIFOs in MEM2 work.
- **Writing WPAR** (PPCMtwpar) now resets the gather pipe, as the hardware does, so GXFlush's padding stays out of a new display list.
- **CALL_DL** takes a virtual address too: the GPU ignores the top three bits, and GXCallDisplayList passes the caller's pointer.

### Added
- A lost command decoder reports the last primitive's vertex formats and the last CP register writes.
- `PUNCHOUT_POINTER` takes a schedule (`"x,y;x,y@40"`), and `PUNCHOUT_DUMP_EVERY` sets how often frames are dumped; each dump logs its time.

### Changed
- `PUNCHOUT_INPUT` and `PUNCHOUT_POINTER` are timed in game frames (frames handed to VI, over 60), not wall-clock seconds. The game waits for the GPU, so on a slow one the clock drifted against what was on screen.

### Known
- Default Mii faces are drawn without eyes or eyebrows, and one is black. The menus and fights don't depend on them.

## [0.13.9] - 2026-11-19
The renderer's gaps: logic ops, line and point sizes, Z textures and depth copies.

### Added
- **Logic ops:** the GPU's own (GX and Vulkan number them alike), or Dolphin's blending approximation where the GPU has none.
- **Line widths and point sizes** from BP 0x22, scaled with the EFB. One pixel where the GPU can't, logged once.
- **Z textures:** the last texture weighted by type, plus a bias, replacing or adding to depth; fog uses it. Depth is written from a second build of the TEV shader, used only by draws with the Z test after texturing, so every other draw keeps the GPU's early depth test.
- **EFB copies of depth:** Z8, Z8M, Z8L and the rest, the depth's bytes through the copy formats.
- **The optional Vulkan features** `logicOp`, `wideLines` and `largePoints` are enabled where the device has them. Their absence is approximated, not fatal.
- **`punchout_render_test`:** the recorder and renderer end to end from GX register writes, with the frame read back. It has 34 checks covering the above on the GPU's own features and on the fallbacks. Planted bugs fail it.

### Fixed
- **RG8 and GB8 EFB copies** had intensity and alpha swapped (now as Dolphin's hardware-checked conversion has them).
- **Z textures were silently ignored.** Unlike the other gaps, they weren't even logged.

### Changed
- The recorder no longer logs these as "not drawn as the console draws it". The renderer logs only what it has to approximate on the GPU it runs on.
- `tools/build_shaders.py` builds shader variants with defines (`kGxFragDepth`), and the source hash covers the variant list.
- `docs/recompiler-core.md`: the differential test's figures brought up to date (193 instructions, 5,061 cases, FPRs compared).

## [0.13.8] - 2026-11-19
### Added
- **`qilin_fp_check`:** the runtime's floating-point helpers against exact rational arithmetic (`recomp/tests/fp/`). It covers every double and single arithmetic instruction, `round25`, `frsp`, `fctiw`/`fctiwz`, `lfs`/`stfs`, paired-single quantisation, and `fres`/`frsqrte` (with Dolphin's hardware-measured `frsqrte` results). That's 40,000 cases a run, including constructed double-rounding traps, on every CI platform's own FPU. The differential test already checked the floating-point wiring; this checks the arithmetic under it.

### Fixed
- **`fmadds`/`fmsubs` (and the paired-single multiply-adds) could round the wrong way** when the product was below the smallest subnormal and `b` was exactly halfway between two singles. The error-free transformation that keeps the rounding single couldn't see an error that small. Found by the new check.

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
