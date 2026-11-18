# Runtime analysis: hardware access, exceptions, timing (roadmap step 1)

Last updated 2026-11-18 (v0.3.0). What the runtime has to provide beyond the replaced libraries' APIs, found in the code before any of it runs.

## Hardware access

`tools/analysis/hardware_access.py` traces the base register of every load, store and cache-block instruction back through its function (`lis`, `addi`, `ori`, register copies, and loads of a pointer variable whose initial value is in the DOL) and classifies the addresses it can resolve:

```sh
python3 tools/analysis/hardware_access.py --qilin build/qilin --dol path/to/main.dol [--list]
```

| Class | Addresses |
|---|---|
| wgpipe | `0xCC008000`: the GX write-gather pipe (the command FIFO) |
| hw-legacy | `0xCC000000`–`0xCC00FFFF` otherwise: CP, PE, VI, PI, MI, DSP, DI, SI, EXI, AI |
| hw-wii | `0xCD000000`–`0xCD00FFFF`: Hollywood (IPC, GPIO, the Wii's DI/SI/EXI/AI …) |
| mem1-unc / mem2-unc | `0xC0000000`–`0xC17FFFFF`, `0xD0000000`–`0xD3FFFFFF`: MEM1 and MEM2 through their uncached mirrors |
| locked | `0xE0000000`–`0xE0003FFF`: the locked cache |

Accesses found, by library (cells: accesses, then functions):

| Library | Plan | wgpipe | hw-legacy | hw-wii | mem1-unc | mem2-unc | locked |
|---|---|---|---|---|---|---|---|
| game and Next Level Games' engine | lift | 803 (26) |  |  |  |  |  |
| revolution/ai | replace |  | 16 (7) | 11 (1) |  |  |  |
| revolution/dsp | replace |  | 14 (7) |  |  |  |  |
| revolution/exi | replace |  |  | 7 (1) |  |  |  |
| revolution/gx | replace | 704 (79) | 4 (2) |  |  |  |  |
| revolution/ipc | replace |  |  | 2 (2) |  |  |  |
| revolution/os | replace |  | 73 (19) | 34 (8) | 6 (3) |  | 2 (2) |
| revolution/si | replace |  | 1 (1) | 15 (3) |  |  |  |
| revolution/vi | replace |  | 71 (5) |  |  |  |  |
| revolution/wpad (Wii Remote) | replace |  |  | 2 (1) |  |  |  |
| revolution/hbm (HOME Button Menu, with nw4hbm) | lift | 134 (5) |  |  |  |  |  |
| RVLFaceLibrary (Miis) | lift | 20 (1) |  |  |  |  |  |
| NdevExi2A (development-kit link) | drop |  | 1 (1) | 34 (6) |  |  |  |

**What this means for the runtime:**
- **No hardware register and no uncached mirror is touched by lifted code**, as far as the trace sees. All of them are in replaced libraries (OS, VI, AI, DSP, EXI, SI, IPC, GX, WPAD) or dropped ones (NdevExi2A). The OS's six uncached-mirror accesses and two locked-cache accesses go with it.
- **Lifted code writes the GX FIFO directly, so the command-stream decoder is required.** 26 functions in the game and engine write 803 times to `0xCC008000`. 25 of them, at `80110AE0`–`8011D650`, store only 16-bit values: the engine's mesh drawers emit vertex indices straight into the pipe after setting up a vertex format with indexed attributes. One, `801BEF94`, writes floats (an immediate-mode draw). The HOME Button Menu (134 stores in 5 functions) and RFL (20 in 1) write vertex data (floats, colours, indices) the same way, through GX's inlined vertex macros. The lifter turns each of these stores into a write to the runtime's FIFO decoder, which assembles commands and draws with the current vertex format (`roadmap-and-design-targets.md`, *Graphics*).
- **Data holds no pointer to a hardware register** outside the SDK's own: the only data words in that range are two `0xCD00CD00` fill patterns.
- **What the trace can't see:** an address passed in from a caller or computed at run time (`OSPhysicalToUncached`, a FIFO base from `GXInit`). During bring-up every access to a mirror or a hardware register traps with its address, so anything this misses shows up the first time it runs.

## C++ exceptions

The game can't throw. Its EABI runtime has `new`/`delete`, array construction, pointer-to-member calls, the global destructor chain and `__init_cpp_exceptions` (which registers the exception tables from `.ctors`), but **no `__throw`, no unwinder and no catch matching**: none of `Gecko_ExceptionPPC` is linked. `extabindex` has 170 entries (plus the table's own record): 87 in RFL, 78 in MSL and 5 in the runtime, none in the game, the engine, Wwise or the HOME Button Menu, which were built without exception tables. Three entries have cleanup actions (array construction undoing itself if a constructor throws), which can't happen.

**For the runtime:** `extab` and `extabindex` are dead data, and lifted code needs no unwinding support. `__init_cpp_exceptions` stays lifted and registers its fragment as before.

## Frame timing

What the code shows:

- **The engine renders one frame per display retrace.** Its buffer swap (`801BE304`) has four modes: 0 swaps without waiting, 1 waits for the next retrace, 2 holds to every second retrace (half rate), 3 waits only if the frame took less than a time budget. The mode variable starts at 1 in `.sdata` and no code writes it, so the game always runs in mode 1.
- **The refresh rate follows the video mode.** Video set-up (`801BA9A4`) reads the TV format and the console's EuRGB60 setting, picks the render mode, and records a rate of 50 in PAL 50 Hz and 60 otherwise (EuRGB60, NTSC, MPAL). A frame limiter (`801BE0D8`) then sleeps any frame shorter than 1,000,000 / rate µs.
- **Gameplay advances by a time step in seconds, not by counting frames.** The per-frame entry (`800E2CF4`) receives a float `dt` and passes it to the simulation update (`80156B30`) after rendering. It zeroes `dt` while the game is paused and multiplies it by a time-scale factor. The frame entry is delivered as an event whose first word is `dt` (`800B3304`). Loading screens drive their own update loop with a fixed `dt` of 1/60 s (`80037F94`).
- **Not settled by reading the code:** whether the main loop's `dt` is a fixed 1/60 or the measured frame time. If it's fixed, the PAL 50 Hz mode runs gameplay at 5/6 speed, as many PAL games did, and the game is meant to be played in 60 Hz. If it's measured, 50 and 60 Hz play the same. The first boot answers this: the runtime logs the `dt` the frame event carries.

**For the port:** the runtime presents at 60 Hz (the EuRGB60 path, which every TV the port targets can show), so gameplay timing is the 60 Hz game in either case, and dodge windows keep their original length. Higher refresh rates (roadmap step 8) depend on the open question: with a measured `dt`, the simulation can run at the display rate if its maths holds up at smaller steps; with a fixed 1/60, it runs at 60 Hz and the renderer interpolates between ticks.
