# The runtime (roadmap step 3)

Last updated 2026-11-18 (v0.8.0). How the lifted game runs: the guest machine, booting, and the native replacements for the libraries the plan replaces (`docs/code-analysis.md`).

## Building and running

The game is built only from the player's own `main.dol`; nothing derived from it enters the repository.

```sh
cmake -S . -B build-game -DQILIN_GAME_DOL="<extracted disc>/sys/main.dol"
cmake --build build-game --target punchout
build-game/punchout "<extracted disc>"      # the folder with sys/ and files/
```

At build time `qilin lift <main.dol> -o build-game/generated --plan data/R7PP01/plan.tsv --files 32` writes the C++ (32 files of functions, the function table, the replaced-function table), which is compiled into `punchout` with the runtime and the replacements in `punchout/src/`. A full build takes about 7 minutes on two cores (GCC 14, `-O2`); after that, changes to the replacements rebuild in seconds. The extracted disc is what Dolphin's *Extract Entire Disc* or `wit extract` produces: `sys/main.dol`, `sys/boot.bin`, `sys/fst.bin` and `files/`.

## The library plan as data

`data/R7PP01/plan.tsv` is the plan of `code-analysis.md` as address ranges: lifted, replaced or dropped. `tools/analysis/identify_libraries.py --plan-out` writes it. Narrower ranges override the library they sit in. These are code inside a replaced library that is lifted after all, because it touches no hardware and its exact behaviour matters more than a native version:

| Range | Lifted | Why |
|---|---|---|
| `80200520`–`802008F0` | OSAlloc (heaps), OSArena (arena bounds) | The allocator decides the game's memory layout. |
| `80201C50`–`80201CDC` | `OSReport` | It formats through MSL's `vprintf` (lifted). |
| `8020C674`–`8020C728` | `__init_user`, `__init_cpp`, `exit` | The EABI's constructors and destructors. |

GX is lifted whole (v0.8.0, `code-analysis.md`): its command stream is the boundary.

With the plan, the lifter turns every replaced or dropped function into a thunk, `c.hooks->replaced(c, address)`, into the runtime's registry. On the game, **11,535 functions are lifted, 1,759 replaced and 193 dropped, with no stubs: every instruction outside replaced and dropped code is lifted.**

## The guest machine (`runtime/`)

- **Memory:** the 4 GB guest address space is reserved in one block. MEM1 (`0x80000000`, 24 MB), MEM2 (`0x90000000`, 64 MB) and the locked cache (`0xE0000000`, 16 KB) are backed by memory. Lifted code reaches cached MEM1 and MEM2 directly, and everything from `0xC0000000` up through the hooks. There, the uncached mirrors of MEM1 and MEM2 map to the same memory, and the locked cache is plain memory. Hardware registers (`0xCC000000`–`0xCD00FFFF`) go to the game's MMIO handlers (`rt::set_mmio`); anything else stops the runtime with its address.
- **Dispatcher:** an indirect call searches the function table, which is sorted by address. A target that starts no function is fatal.
- **Registry:** the game's replacements are registered by address (`rt::provide`). Registering an address the plan lifts or drops, or one address twice, is fatal at start-up. A call to a replaced function with nothing registered stops the runtime, naming the function, its library and its caller.
- **Fatal errors** print the reason and the guest's call stack, walked through the back chain. The exit status is 70.

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
- **Interrupts:** `OSDisableInterrupts`, `OSEnableInterrupts` and `OSRestoreInterrupts` keep one flag, as MSR[EE] does, and each thread keeps its own across switches.
- **Caches:** every flush and invalidate is a no-op, because the host is coherent. `DCZeroRange` zeroes memory.
- **Locked cache:** its DMA (`LCLoadBlocks`, `LCStoreBlocks`, `LCStore`) is a copy done at once, so `LCQueueWait` has nothing to wait for.
- **Time:** the time base counts at 60.75 MHz from boot, by the host's steady clock (`OSGetTime`, `OSGetTick`).
- **Memory sizes and buttons:** the memory-size queries, `OSSetResetCallback` and `OSSetPowerCallback`.
- **Threads** (`threads.cpp`):
  - **How they run:** each guest thread runs on a host thread of its own (16 MB host stack) with its own registers, and exactly one runs at a time.
  - **When they switch:** the scheduler hands over where the OS would, with the OS's rules: priorities 0–31, first come first served within one, suspend counts, threads created suspended. A switch happens when a thread blocks, yields or wakes one of higher priority.
  - **The idle loop:** with every thread blocked, the blocked thread acts as the OS's idle loop. It sleeps to the next timer deadline and delivers interrupts. If nothing can ever wake a thread, the runtime stops and reports a deadlock.
  - **Guest-visible state:** guest structures stay current where the game can read them: an `OSThread`'s state, suspend count, priority and exit value, the current thread at `0x800000E4`, a mutex's owner and count, a message queue's ring.
  - **Covered:**
    - threads: create, resume, suspend, yield, exit, cancel, join, sleep, wake;
    - mutexes, condition variables, message queues (send, jam, receive);
    - alarms (one-shot and periodic, with user data) and `OSSleepTicks`.
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
- **Console settings** (`sc.cpp`): 16:9, EuRGB60, English, stereo, a European console. These will become settings.
- **Interrupt handlers:** `__OSSetInterruptHandler`, `__OSGetInterruptHandler`, `__OSMaskInterrupts` and `__OSUnmaskInterrupts` keep the OS's table and mask. `raise_interrupt(n)` runs a handler as an interrupt; a masked one is dropped for now. Context and shutdown bookkeeping are no-ops, as is the write-gather pipe's address register (its "not yet sent" bit always reads clear).

## The GPU's front end (`gpu.cpp`, v0.8.0)

- **Registers:** the command processor, pixel engine and processor-interface FIFO registers GX touches, and the write-gather pipe (any address in its 32-byte block at `0xCC008000`).
- **Decoding:** the command processor decodes as the stream arrives. There's no FIFO in guest memory to overflow, so it reads as idle, its read pointer is its write pointer, and the PI's write pointer moves through the CPU FIFO's ring as on hardware.
- **Commands:** the decoder knows every command (NOP, CP, XF, BP and indexed loads, display-list calls, primitives). It keeps the vertex descriptors and attribute tables it needs to size vertex data, by the hardware's rules as Dolphin documents them. An unknown byte stops the runtime with the bytes around it, because it means the decoder has lost its place.
- **Interrupts:** draw-done (BP `0x45`) and token commands (`0x47`, `0x48`) raise the pixel engine's finish and token interrupts, which reach GX's own handlers; the token register reads back the last token.
- **Counting:** every five seconds it logs the draws, vertices, display lists, frame-buffer copies and command bytes it has seen. Drawing them is the renderer's job (next).

## Where boot stands

The game boots, runs its static constructors (which bring up the OS, DVD, VI and the heaps through the lifted OSAlloc), enters `main`, enables the locked cache, sets its error handlers and reads the console settings. Then `GXInit` runs, lifted, and its command stream decodes cleanly. The game starts loading its data with `files/art/scripts/flow.dict`, the first file it reads, and that is where boot needs the extracted disc's `files/`.

## Next

1. **The renderer (roadmap step 4):** the decoded stream on Vulkan: vertex loading, XF transform and lighting, TEV as generated shaders, textures, EFB copies to the XFB and to textures, presenting the XFB that `VISetNextFrameBuffer` names.
2. **The rest of the boot path:** AI/AX/DSP (audio, which Wwise drives), WPAD with the button-only input, NAND (saves), the HOME Button Menu's dependencies.
3. **Tests:** a test of the thread model without the game, using small guest programs built from encoded instructions, like the differential test's.
