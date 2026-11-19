# The runtime (roadmap step 3)

Last updated 2026-11-18 (v0.11.0). How the lifted game runs: the guest machine, booting, and the native replacements for the libraries the plan replaces (`docs/code-analysis.md`).

## Building and running

The game is built only from the player's own `main.dol`; nothing derived from it enters the repository.

```sh
cmake -S . -B build-game -DQILIN_GAME_DOL="<extracted disc>/sys/main.dol"
cmake --build build-game --target punchout
build-game/punchout "<extracted disc>"      # the folder with sys/ and files/
                                            # --offscreen: draw without a window; --headless: don't draw
```

At build time `qilin lift <main.dol> -o build-game/generated --plan data/R7PP01/plan.tsv --files 32` writes the C++ (32 files of functions, the function table, the replaced-function table), which is compiled into `punchout` with the runtime and the replacements in `punchout/src/`. A full build takes about 7 minutes on two cores (GCC 14, `-O2`); after that, changes to the replacements rebuild in seconds. The extracted disc is what Dolphin's *Extract Entire Disc* or `wit extract` produces: `sys/main.dol`, `sys/boot.bin`, `sys/fst.bin` and `files/`.

## The library plan as data

`data/R7PP01/plan.tsv` is the plan of `code-analysis.md` as address ranges: lifted, replaced or dropped. `tools/analysis/identify_libraries.py --plan-out` writes it. Narrower ranges override the library they sit in. These are code inside a replaced library that is lifted after all, because it touches no hardware and its exact behaviour matters more than a native version:

| Range | Lifted | Why |
|---|---|---|
| `80200520`–`802008F0` | OSAlloc (heaps), OSArena (arena bounds) | The allocator decides the game's memory layout. |
| `80201C50`–`80201CDC` | `OSReport` | It formats through MSL's `vprintf` (lifted). |
| `8020C674`–`8020C728` | `__init_user`, `__init_cpp`, `exit` | The EABI's constructors and destructors. |
| `801FE598`–`801FE770` | MTX's vector functions | A boundary fix (v0.11.0): the OS range began with three paired-single vector functions. |
| `8025E6C0`–`8025E840` | CX (`CXGetUncompressedSize`, `CXUncompressLZ`) | A boundary fix (v0.11.0): pure decompression after NAND, before the HOME Button Menu. |
| `8021636C`–`8021698C` | WBC's API | A boundary fix (v0.10.0): the evidence left WBC/WPAD open by ±8 functions. These seven keep WBC's state and do its calibration maths, so WBC (lifted) runs on to WPAD's first function, `__wpadNoAlloc`. |

GX is lifted whole (v0.8.0, `code-analysis.md`): its command stream is the boundary.

One range goes the other way. `801C93D8`–`801C9494`, MetroTRK's `__TRK_write_console`, is replaced by nothing: MSL's `__write_console` passes console output to the debugger after the UART, so that one function of the dropped debugger stub is reached in normal running.

With the plan, the lifter turns every replaced or dropped function into a thunk, `c.hooks->replaced(c, address)`, into the runtime's registry. On the game, **11,535 functions are lifted, 1,759 replaced and 193 dropped, with no stubs: every instruction outside replaced and dropped code is lifted.**

## The guest machine (`runtime/`)

- **Memory:** the 4 GB guest address space is reserved in one block. MEM1 (`0x80000000`, 24 MB), MEM2 (`0x90000000`, 64 MB) and the locked cache (`0xE0000000`, 16 KB) are backed by memory. Lifted code reaches cached MEM1 and MEM2 directly, and everything from `0xC0000000` up through the hooks. There, the uncached mirrors of MEM1 and MEM2 map to the same memory, and the locked cache is plain memory. Hardware registers (`0xCC000000`–`0xCD00FFFF`) go to the game's MMIO handlers (`rt::set_mmio`); anything else stops the runtime with its address.
- **Dispatcher:** an indirect call searches the function table, which is sorted by address. A target that starts no function is fatal.
- **Registry:** the game's replacements are registered by address (`rt::provide`). Registering an address the plan lifts or drops, or one address twice, is fatal at start-up. A call to a replaced function with nothing registered stops the runtime, naming the function, its library and its caller.
- **Fatal errors** print the reason and the guest's call stack, walked through the back chain. The exit status is 70.
- **Tracing:** with `PUNCHOUT_TRACE=1`, every call into a replacement is logged with `r3`, `r4` and the caller. That's how a guest stuck waiting for something shows what it waits for.

## The host platform (`host/`, v0.9.0)

- **Threads:** the main thread owns the window: it handles its events and shows the newest frame the guest has handed over. The guest runs on threads of its own; its main thread starts on a host thread with a 16 MB stack, like every other guest thread. Closing the window ends the program (it will become the console's power button).
- **Window:** an SDL3 window, resizable and high-DPI.
- **Input:** the first SDL gamepad (any, by an Xbox pad's button names) and the keyboard. Arrows are the D-pad, X and Z are A and B, C or Space is Y, Enter is Start, Backspace is View, Escape is Guide. The left stick counts as the D-pad past half way. Rumble goes to the pad.
- **Vulkan:** `Gpu` makes a Vulkan 1.3 device, preferring a discrete GPU. It requires dynamic rendering and synchronization2 (both in v3dv's set) and names whatever a device lacks. `Display` presents through a FIFO swapchain, rebuilt when the window changes. Headless, it draws into an image of its own, which `qilin_vktest` reads back to check the letterboxing (skipped where there's no Vulkan 1.3 device; it passes on Mesa's llvmpipe).
- **Frames:** at each retrace VI hands over the frame buffer the game set (`video.cpp`), and the main thread shows it at 16:9, letterboxed. For now that's the XFB in guest memory, YUV 4:2:2, converted on the CPU. The renderer's EFB copies will take its place.

## Booting (`punchout/src/boot.cpp`)

1. **Load and check.** `main.dol` must be the supported build (SHA-256); its image is copied into MEM1.
2. **Boot information.** Low memory gets what the boot program, IOS53 (the game's IOS) and the apploader leave on a retail console. That covers the disc header, MEM1 and MEM2 sizes and arenas, the IPC buffer, the clocks, the console type and the video mode. The values are Dolphin's (hardware observations; facts only, `prior-art.md`). `hardware_access.py` now classifies low memory too: lifted code reads only the bus clock (`0x800000F8`), from 24 instructions that turn ticks into milliseconds.
3. **`__start`, natively.** The runtime does what `__start` does, in the same order:
   - set `r1`, `r2` and `r13`;
   - write the outermost frame (back chain and LR of −1);
   - run `__init_data`'s copy and zero tables (read from guest memory);
   - call `DBInit` and `OSInit`, then `__init_user` (the static constructors);
   - call `main(0, NULL)`, then `exit`.

   There's no debugger, no BI2 and no arguments.

## Replacements so far

Replacements take arguments in `r3`–`r10` and return in `r3` (with `r4` for 64-bit results), as the EABI passes them.

- **OS start-up:**
  - `OSInit` runs once and sets the four arena bounds. MEM1's arena starts after the main stack (`80429300`), as with no debugger attached; the others come from what IOS left (`81800000`, `90000800`–`935E0000`).
  - `DBInit` does nothing.
  - `OSPanic` and `OSSetErrorHandler`: a panic is fatal, and error handlers are kept but never called.
- **CPU registers:** `PPCSync`; `PPCHalt` ends the process; `PPCMfhid2`/`PPCMthid2`.
- **Interrupts:** `OSDisableInterrupts`, `OSEnableInterrupts` and `OSRestoreInterrupts` keep one flag, as MSR[EE] does, and each thread keeps its own across switches. `OSInit` ends with interrupts on, as the real one does; without that, nothing queued for the main thread ever arrives.
- **Caches:** every flush and invalidate is a no-op, because the host is coherent. `DCZeroRange` zeroes memory.
- **Locked cache:** its DMA (`LCLoadBlocks`, `LCStoreBlocks`, `LCStore`) is a copy done at once, so `LCQueueWait` has nothing to wait for.
- **Time:** the time base counts at 60.75 MHz, by the host's steady clock (`OSGetTime`, `OSGetTick`).
  - It starts where the console's would: at the PC's local date and time, counted from 1 January 2000, 00:00. So `OSGetTime` is the date, as on a Wii (v0.13.6; before, it counted from start-up and every date was 1 January 2000).
  - `OSTicksToCalendarTime` turns it into a date: Gregorian, exact for any date, before 2000 included.
- **Leaving:** the game's exit code (`800E249C`, behind the HOME Button Menu and the power and reset buttons) blanks the screen, calls one of these, then spins, because on a Wii they never return.
  - `OSReturnToMenu`, `OSReturnToDataManager` and `OSShutdownSystem` close the program, logging which the game chose.
  - `OSRestart` (Reset) starts it again with the same command line (`restart.cpp`). On Windows it uses the command line exactly as given, quoting and all; elsewhere it uses `execvp`. Saves are already on disk, as NAND writes are synchronous.
- **MetroTRK's host files:** MSL's `fopen`, which the game calls from four places, opens files on a debugger's host through MetroTRK. With no debugger, that open fails (returns 1), so the replacement does just that and `fopen` returns NULL, as on a retail Wii (v0.13.7; before, the call reached dropped code and stopped the game).
- **Every replaced function that lifted code calls directly has a replacement** (v0.13.6), and no lifted code calls dropped code (v0.13.7). `tools/analysis/check_replacements.py` checks both from `qilin cfg --json`, deciding each address by the narrowest plan range as the lifter does. It fails if anything is missing, listing it with its callers. Calls through function pointers aren't in the call graph, so a missing one still stops the game, by name, when called.
- **Memory sizes and buttons:** the memory-size queries, `OSSetResetCallback` and `OSSetPowerCallback`.
- **Threads** (`threads.cpp`):
  - **How they run:** each guest thread runs on a host thread of its own (16 MB host stack) with its own registers, and exactly one runs at a time.
  - **When they switch:** the scheduler hands over where the OS would, with the OS's rules: priorities 0–31, first come first served within one, suspend counts, threads created suspended. A switch happens when a thread blocks, yields or wakes one of higher priority.
  - **Yield is a safe point:** the game waits for its loader thread in a loop around `OSYieldThread`, polling for an asynchronous read, so a yield delivers interrupts, as one would arrive mid-loop on the Wii.
  - **The idle loop:** with every thread blocked, the blocked thread acts as the OS's idle loop. It sleeps to the next timer deadline and delivers interrupts. If nothing can ever wake a thread, the runtime stops and reports a deadlock.
  - **Guest-visible state:** guest structures stay current where the game can read them: an `OSThread`'s state, suspend count, priority and exit value, the current thread at `0x800000E4`, a mutex's owner and count, a message queue's ring.
  - **Covered:**
    - threads: create, resume, suspend, yield, exit, cancel, join, sleep, wake;
    - mutexes, condition variables, message queues (send, jam, receive);
    - alarms (one-shot and periodic, with user data) and `OSSleepTicks`.
  - **Atomic steps:** two steps can't be interrupted by a switch, as on the OS.
    - `OSWaitCond` releases the mutex and goes to sleep on the condition in one step. The OS wakes the mutex's waiters with the scheduler off.
    - A finishing thread wakes its joiners, and the waiters on its mutexes, without switching, then hands the CPU on once.
  - **Tested without the game** (`punchout/tests/threads_test.cpp`, v0.13.1). The real scheduler and interrupt layer run small guest programs written in C++, entered through a function table as lifted code is. The order of events is checked against the OS's rules:
    - preemption on resume;
    - a blocking receive and the send that preempts;
    - queue order, a full queue and jam;
    - a mutex with a condition, recursion included;
    - yield within a priority;
    - suspend counts;
    - exit values, detached threads, mutexes released on exit;
    - an alarm firing while every thread waits;
    - `OSSleepTicks`.
  - **Not yet:** priority inheritance on mutexes.
- **Interrupts and callbacks** (`os.hpp`):
  - **Safe points:** completions are queued and delivered at safe points, which are whenever the guest enables interrupts and inside every replacement that waits or polls. Each is entered with the guest's registers written back, so a delivery is the same as an interrupt arriving at that instruction. If a delivery wakes a thread of higher priority, the switch happens right after it.
  - **Callbacks:** run on a fresh frame below the interrupted thread's stack, with every register restored afterwards.
- **DVD** (`dvd.cpp`):
  - **Lookups:** the disc's file table comes from `sys/fst.bin`. Paths resolve as the SDK does: relative to the current directory, `.` and `..`, without case.
  - **Reads:** data is read from `files/`. Synchronous reads return at once. Asynchronous reads complete as an interrupt (command block busy until then, then the callback). A read from a file that isn't there stops the runtime, naming it.
- **VI** (`vi.cpp`):
  - **The retrace:** a timed interrupt at 60 Hz (EuRGB60) from the host clock. It runs the pre- and post-retrace callbacks and wakes the threads in `VIWaitForRetrace`.
  - **Frame buffers:** `VISetNextFrameBuffer` is recorded for the renderer.
- **Console output:** `OSGetConsoleType` reports a retail Wii. The C library's console writes go to the debug serial port (`InitializeUART`, `WriteUARTN`), which a retail console doesn't have; here it becomes the game's own log, as `game: ...` lines.
- **NAND** (`nand.cpp`, v0.11.0): the Wii's flash file system as a folder, `<user folder>/nand/...`. The user folder is the platform's per-user folder from SDL (Windows: `%APPDATA%\Qilin\PunchOut-QilinComp`), or `PUNCHOUT_SAVE_DIR`.
  - **Paths:** absolute or relative to the current directory (the game's home, `/title/00010000/52375050/data`, to start with).
  - **Coverage:** create, delete, open, read, write, seek, length, move, type, change directory, the free-space check, and the asynchronous forms, whose callbacks run as interrupts. Errors are NAND's codes.
  - **Not yet:** the "safe" open and close open the file itself, so a write cut short isn't rolled back.
- **Console settings** (`sc.cpp`): 16:9, EuRGB60, English, stereo, a European console. These will become settings.
- **WPAD** (`wpad.cpp`, v0.10.0): one sideways Wii Remote on channel 0, buttons only (the port's decision: no motion, no Nunchuk, no Balance Board).
  - **What the game sees:** a bare core controller, resting flat, with nothing in its pointer's view; channels 1–3 are empty.
  - **Connecting:** it connects when the game sets a connect callback. That callback and the extension callback ("core") then run as interrupts, and the sampling callback runs at the remote's 200 Hz.
  - **Buttons:** they come from the window's pad or keyboard (`host/`), mapped as the design doc says. The cross pad is turned a quarter for the sideways grip; A is the 2 button (right punch), B the 1 button (left punch), X or Y the A button (Star Punch); Start, View and Guide are +, − and HOME.
  - **Rumble** reaches the pad.
  - **Speaker:** its stream is accepted and dropped for now.
  - **Names:** five one-instruction WPAD functions are branches into WUD, and their targets name them. The size-matched names had them one off.
- **Interrupt handlers:** `__OSSetInterruptHandler`, `__OSGetInterruptHandler`, `__OSMaskInterrupts` and `__OSUnmaskInterrupts` keep the OS's table and mask. `raise_interrupt(n)` runs a handler as an interrupt; a masked one is dropped for now. Context and shutdown bookkeeping are no-ops, as is the write-gather pipe's address register (its "not yet sent" bit always reads clear).

## The GPU's front end (`gpu.cpp`, v0.8.0)

- **Registers:** the command processor, pixel engine and processor-interface FIFO registers GX touches, and the write-gather pipe (any address in its 32-byte block at `0xCC008000`).
- **Decoding:** the command processor decodes as the stream arrives. There's no FIFO in guest memory to overflow, so it reads as idle, its read pointer is its write pointer, and the PI's write pointer moves through the CPU FIFO's ring as on hardware.
- **Commands:** the decoder knows every command (NOP, CP, XF, BP and indexed loads, display-list calls, primitives). It keeps the vertex descriptors and attribute tables it needs to size vertex data, by the hardware's rules as Dolphin documents them. An unknown byte stops the runtime with the bytes around it, because it means the decoder has lost its place.
- **The write-gather pipe** collects 32 bytes before the decoder sees them, as on hardware (a write can straddle two bursts).
- **Interrupts:** draw-done (BP `0x45`) and token commands (`0x47`, `0x48`) raise the pixel engine's finish and token interrupts, which reach GX's own handlers; the token register reads back the last token.
- **GPU latency:** the game relies on the GPU being behind. It writes draw-done, installs the callback for it, then waits. So these interrupts come 50 µs after the command is decoded, as timed events. Dolphin likewise schedules them at least 500 CPU cycles out.
- **Counting:** every five seconds it logs the draws, vertices, display lists, frame-buffer copies and command bytes it has seen. Drawing them is the renderer's job.

## The renderer (`recorder.cpp`, `texture.cpp`, `renderer.cpp`, v0.13.0)

Two threads share the work. The game's thread records what the command stream asks for; the main thread owns Vulkan and draws it.
- **Recording** (`recorder.cpp`): each primitive's vertices are decoded by the current vertex format into one canonical vertex (position, normal, binormal and tangent, two colours, eight texture coordinates, matrix indices, and bits saying which are present). Quads, strips and fans become index lists. The XF, BP and TEV state the shaders read is mirrored in a `gx::State` (std430). A new one is snapshotted only when a register it depends on changes. Viewport and scissor follow the hardware's rules as Dolphin documents them.
- **Textures** (`texture.cpp`): TMEM is modelled for palettes and preloads. Every format (I4, I8, IA4, IA8, RGB565, RGB5A3, RGBA8, C4, C8, C14X2, CMPR) is decoded on the CPU, with mip levels, into a cache keyed by address, format, size, levels and palette, and checked by a hash of the data. A texture that an EFB copy wrote is used straight from the GPU while guest memory still holds what the copy left there. Entries unused for 600 frames go.
  - Checking for changes hashes a texture's data (four independent lanes, about 11 GB/s on a desktop core). It does so once between invalidations of the GPU's texture cache (BP `0x66`) and frame ends, however many draws use the texture. On the Wii, the GPU keeps what it fetched until the game invalidates, so until then the data it would use is the data hashed.
- **Frames:** a copy to the XFB ends a frame. The main thread takes it, at most two frames behind (otherwise the game waits).
- **Drawing** (`renderer.cpp`): the EFB is RGBA8 plus a 32-bit depth buffer at a whole-number scale of 640×528 (`PUNCHOUT_SCALE`). Draws use dynamic rendering. Pipelines are keyed by blending and primitive type; cull mode and depth test are dynamic state. GX's reversed depth range goes into the viewport's min and max depth.
- **Shaders** (`punchout/shaders/`, built by `tools/build_shaders.py`):
  - The vertex shader does XF's transform, lighting (the channel controls, attenuation, spot and specular) and texture-coordinate generation.
  - One fragment ubershader reads the TEV configuration from the state and does all 16 stages: indirect textures, the colour and alpha combiners with the hardware's 8-bit rounding and compares, konst and swap tables, the alpha test, dither and fog.
  - **Specialised pipelines (v0.15.0):** a weak GPU pays dearly for interpreting the TEV registers per pixel, so each draw's *shape* (gx.hpp `shape_of`: 70 words of stage counts, indirect commands, orders, combiners, konst and swap selects, the Z-texture mode and the alpha-test functions; colours stay dynamic) also gets a pipeline of its own. The same shader source is built once per stage count (`kGxFragStages1`…`16`, stages unrolled) and the shape goes in as specialisation constants, so the driver folds every decision away and compiles only the stages the draw has.
    - A worker thread compiles them; until one is ready its draws use the ubershader, so a new shape never stalls a frame. The Vulkan pipeline cache is written to `pipelines.bin` in the user folder whenever the worker catches up, so later runs start with them compiled (drivers that keep nothing there, like lavapipe, still have Mesa's own disk cache).
    - Draws that write depth (Z textures with late Z) keep the ubershader variant.
    - `PUNCHOUT_UBERSHADER=1` draws everything with the ubershader; `PUNCHOUT_SPECIALIZE_SYNC=1` compiles each specialised pipeline when first needed (the tests); `PUNCHOUT_SPECIALIZE_FROM=<seconds>` switches specialisation on at that game time, to compare the two on one scene.
    - `punchout_render_test` runs on both paths (`_ubershader`, `_specialized`) with the same expectations; a planted shape bug fails 32 of its checks.
  - A copy shader handles the EFB copy formats, halving and gamma.
  - They're compiled to SPIR-V when the shaders change, not at build time, into `punchout/src/shaders.inc`, which is committed. `--check` fails if the header is stale.
- **GPU images are reused:** a texture or copy target that's done with becomes spare once its frame's fence has passed. The next image of the same size and use takes it, and spares unused for two seconds are freed. On the boot screens, no image is created after start-up. A frame's releases are handled after it's drawn: one frame can make and release the same texture (two copies to one address), and handling them first leaked it.
- **Copies:** a copy to a texture stays a GPU image, found again by its address. A copy to the XFB becomes the image VI presents (`Display::present_image`). When there isn't one, VI converts the XFB in guest memory on the CPU, as before v0.13.0.
- **Logic ops** (v0.13.9) use Vulkan's where the GPU has them, since GX numbers its sixteen as Vulkan does. Where it doesn't (Apple's GPUs through MoltenVK), they're approximated with blending, following Dolphin's table, with the shader forcing or inverting its output for four of them. That's exact for clear, copy, no-op, invert, copy-inverted and set, and for and, or and xor on 0/255 values; close for the rest. It's logged once when it happens.
- **Line widths and point sizes** (BP 0x22, in sixths of a pixel) use dynamic line width and `gl_PointSize`, scaled with the EFB, within the device's range. Without `wideLines` or `largePoints` they're one pixel, logged once. The texture-coordinate offsets GX applies across lines and points aren't done.
- **Z textures** (BP 0xF4/0xF5): the last texture sampled, unswizzled, weighted by its type (u8, u16, u24), plus the bias, replacing or adding to the depth, as Dolphin computes it.
  - Fog uses the result whenever it's on.
  - It reaches the depth buffer when Z is compared after texturing (BP 0x43 bit 6 clear). Those draws use a second build of the TEV shader that writes depth, because writing depth turns off the GPU's early depth test and every other draw keeps it.
- **EFB copies of depth** (pixel format Z24 at the copy): the 24-bit depth is split into bytes, (z >> 16, z >> 8, z, 255), and these go through the colour copy's format conversion. So Z8 is the high byte, Z8M the middle and Z8L the low, as in Dolphin. The depth image is read with a nearest sampler, and the half-scale filter averages the bytes.
- **Copy formats RG8 and GB8** sample (as IA8) with intensity from red and from green respectively. Before v0.13.9 the two channels were the wrong way round.
- **Still approximate:** constant alpha together with blending that reads source alpha needs dual-source blending, which the Pi 4's driver doesn't have. There, the constant isn't written.
- **Tested without the game** (`punchout_render_test`, in `ctest`, skipped without a Vulkan 1.3 device). GX register writes go through the real recorder and renderer, and the frame copied to the XFB is read back and checked. It covers:
  - all sixteen logic ops, with the GPU's own and approximated;
  - line and point sizes, and their fallbacks;
  - Z textures with late and early Z;
  - Z8 and Z8M depth copies;
  - RG8 and GB8.

  Planted bugs fail it: the old RG8 order, Z textures not writing depth, and depth copies reading colour.
- **The stats line** (every five seconds) adds, per frame:
  - the game's thread recording, of which checking and decoding textures;
  - its waits for the main thread;
  - the main thread's drawing, of which waiting for the GPU;
  - the textures held on the GPU, the spares, and the images made.

  It shows which side holds the game back on a given machine.
- **Testing without a screen:**
  - `--offscreen` draws with no window, for example on llvmpipe.
  - `PUNCHOUT_DUMP=dir` writes every 60th frame shown as a PPM at 1280×720 and logs the time of each; `PUNCHOUT_DUMP_EVERY=n` changes the 60.
  - `PUNCHOUT_INPUT="Y@16,Start@20.5"` presses pad buttons at those seconds of game time, for a fifth of a second each. Game time is the frames the game has handed VI, over 60: the game waits for the GPU, so on a slow one this keeps a script in step with the screen where the clock would drift.
  - `PUNCHOUT_POINTER="x,y"` holds the pointer at a point (−1 to 1, y up); `"x,y;x,y@40;x,y@52"` moves it at those times.
  - The save lives in the per-user folder's `nand/`; copying it aside and back makes a scripted run repeatable.
  - `PUNCHOUT_PROFILE=<file>` (Linux): a SIGPROF sampler, 1 kHz of CPU time, needing no perf or ptrace; `tools/analysis/profile_report.py <exe> <file>` names the hot functions.

## Input (`wpad.cpp`)

WPAD is replaced, and KPAD is lifted on top of it. The remote is held sideways and buttons only, with no motion controls, per the design targets. Which inputs press which of its buttons is the player's choice, in the settings file (below). By default the first pad maps as:
- D-pad or left stick: the cross;
- A: 2;
- B: 1;
- X or Y: A;
- Start: +;
- Back: −;
- Guide: HOME.

The keyboard's defaults are the arrows, Z and X for 1 and 2, C or Space for A, Enter for +, Backspace for − and Escape for HOME. The pointer is real input, since the menus need it. The runtime places the sensor bar's two lights where the remote's camera would see them, after Dolphin's camera geometry. KPAD then works out the pointer from them as on hardware. The mouse over the picture moves the pointer, and its left button is A. The right stick also moves it, a screen's width a second. The angles are calibrated so the pointer lands where the mouse is, with the bar below the TV, the console's default setting.

## Settings (`settings.cpp`, v0.13.2)

`settings.ini` sits in the per-user folder, next to the saves (Windows: `%APPDATA%\Qilin\PunchOut-QilinComp`). The first run writes it with the defaults and comments explaining each line.
- `[video] scale`: the EFB's resolution, 1 to 8 times 640×528. The default is 1, which suits the Pi 4. `PUNCHOUT_SCALE` overrides it.
- `[controls]`: each of the remote's buttons, in the player's terms (`up down left right one two a plus minus home`), takes a comma-separated list of inputs:
  - `pad:<Xbox name>`;
  - `key:<SDL key name>`;
  - `mouse`, its left button.

  An empty list leaves the button unused. Controls the file doesn't mention keep their defaults.

The main thread turns the pad, the keys and the mouse into the remote's buttons after each poll; WPAD turns those into its own bits. Mistakes stop the program at start-up, naming the line or the key, rather than leaving a button dead without saying why:
- an unknown name, section, button or key;
- a scale out of range;
- a control set twice.

`punchout_settings_test` checks that the defaults read back as written, that edits are read, and that mistakes are reported.

## Hardware registers (`hw.cpp`, v0.12.0)

Each device maps its own ranges of `0xCC000000`-`0xCD00FFFF` (the GPU's CP, PE, PI and write-gather pipe; the DSP; the audio interface). An access outside every range is fatal and names the address.

## Sound (`dsp.cpp`, `ax.cpp`, v0.12.0)

AI, AX and DSP are lifted; the hardware under them is emulated.
- **The DSP's registers:** the mailboxes both ways, the control register (reset, halt, init, the three interrupt flags with their enables, written 1 to clear), the audio DMA (source, length in 32-byte blocks, blocks left). The interrupt is a level, as on hardware: while a flag is set with its enable, the OS's handler for it runs (5 AI DMA, 6 ARAM, 7 DSP), again until it's acknowledged. The OS's own mask functions set those enables, as the real OS does.
- **The microcode:** the DSP starts in its ROM's task loader, which takes the uploaded program's address, length and start by mail. The program is identified by its hash; three Wii AX versions are known (they differ in command numbering and parameter-block layout), and an unknown one is fatal. Punch-Out!! boots `4CC52064`.
- **AX at a high level** (`ax.cpp`): the mail protocol (`0xBABE` + length, then the list's address; `DSP_SYNC` after the output, `DSP_YIELD` at the end, then the task manager's resume or continue), and every command: buffer setup, the voice list, aux send and return, the compressor, the output (96 stereo samples, every 3 ms), the Wii Remote speakers' buses. For each voice: the accelerator reads ADPCM, PCM8 or PCM16 from memory with its loop rules; a sample-rate converter (polyphase if `dsp_coef.bin`, a dump of the DSP ROM's table, is in the user folder, else linear); the volume envelope, low-pass and biquad filters; per-bus volumes with ramps. Formats, arithmetic and order are Dolphin's facts; the code is ours.
- **The audio DMA** is the clock: 8 samples a block at the AI's rate (32 kHz). When a buffer has played it reloads and raises the AI DMA flag; AX answers by queueing the next buffer and sending the next command list, so the DMA paces all of the sound. Its samples go to SDL3 (`host/audio.hpp`); more than 150 ms queued is dropped, and if the guest falls 100 ms behind the clock skips ahead.
- **The AI's registers:** the control (rates, the sample counter running), volume, the sample counter. Its sample-counter interrupt is fatal if enabled; nothing does yet.

## Poll loops (v0.12.0)

Code that waits for an interrupt by spinning, as in `while (flag == 0) {}` or `while (RFLGetAsyncStatus() == BUSY) {}`, would never see it, because interrupts arrive at safe points. The lifter finds such loops (`recomp/src/idle.*`): a loop is a poll when no pass can tell itself from the last. It may only load, compute into registers, store into its own stack frame, and call pure getters. Nothing may be carried from one pass to the next, in registers or stack slots, on any path. Their back edges call the runtime's idle hook, which delivers what's due or sleeps until the next timed event (at most 1 ms). In this game: the DSP mailbox waits, AX's start-up and the Mii library's status loops (49 back edges, 11 functions). A scan such as `while (*p++)` isn't one, and the unit tests say so.

## Where boot stands

The game boots, runs its static constructors (which bring up the OS, DVD, VI and the heaps through the lifted OSAlloc), enters `main`, enables the locked cache, sets its error handlers and reads the console settings. `GXInit` runs, lifted, and its command stream decodes cleanly. It loads its scripts and global data, sets up the front end, brings up WPAD and KPAD, opens the save system, runs its frame loop and shows its boot flow. Then:
- Wwise starts AX; the DSP boots the AX microcode and mixes every 3 ms;
- the HOME Button Menu loads its archives and sounds;
- the Mii library reads its database;
- the front end runs: 59 frames a second, about 900 draws and 15,000 vertices a second, with sound playing;
- the renderer draws the strap screen and the save dialog; pointing at OK and pressing A creates the save (`banner.bin`, `save_games`), and the game goes on to its intro movie.

From there it needs more of the disc: its Wwise sound bank's files (`files/art/wwiseaudio`, about 1 GB) and the movies.

## Next

1. **The renderer's gaps** as the game reaches them, and checking pictures against hardware behaviour on the player's PC.
2. **The rest of the game's paths:** what the menus, fights and movies reach that the front end doesn't.
