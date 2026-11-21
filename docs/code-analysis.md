# Code analysis: libraries and the lifted/replaced boundary

Last updated 2026-11-18 (v0.2.0: every library in `main.dol` found, its range fixed, and the boundary decided per library).

## How the libraries are found

There is no decomp of Punch-Out!! (`prior-art.md` §2), so `tools/analysis/identify_libraries.py` puts four kinds of evidence together. Each labels some of `qilin cfg`'s functions:

| Evidence | Labels | How |
|---|---|---|
| Size sequences against the Rhythm Heaven Fever decomp (`third_party/rhf`, CC0) | 2,058 functions, 1,988 with the decomp's name | Library by library, the decomp's function sizes in link order are voted into place by runs of three equal sizes, then aligned with `difflib` in that window. A match inside a run of three or more equal sizes counts ("anchored"), and so does a run of two near the library's main cluster (versions differ). Each library keeps only its largest cluster. NW4R isn't linked by this game, so its matches count only inside the HOME Button Menu, which carries a private copy (`nw4hbm`) |
| C++ RTTI | 233 vtables, Wwise's and the HOME Button Menu's | Each vtable starts with a pointer to its class's type info, whose first word points at the class name (`AK::CAkStreamMgr`, `CAkActionPlay`, `homebutton::gui::Component` …). The engine and game classes have no RTTI |
| SDK version banners | 22 functions | Each SDK library's init function loads its `__XXXVersion` pointer ("<< RVL_SDK - THP release build …") |
| Strings that name their library | a few | `arc.c`, `TPL.c`, `GCN_Mem_Alloc.c`, `DBExceptionDestination`, `/shared2/test2/nanderr.log`, the Bluetooth stack's `BTA_HH` events, MetroTRK's banner … (whole strings, not pointers into the middle of one) |

Then the text is cut into contiguous runs, one library each. A short run of size-only labels inside another library's labels (three or fewer) is dropped as a coincidence. Between two labelled functions of different libraries, the boundary goes where the unlabelled functions' calls agree best: each leans towards the side its callers and callees outside the gap are on, and on a tie towards the side used by the same client code (callers within 16 KB of each other). Where the evidence still ties, a library stays as small as it can next to the game's code, and between two libraries the boundary goes in the middle of the tie. The script reports how many positions each boundary could move.

```sh
python3 tools/analysis/identify_libraries.py --qilin build/qilin --dol path/to/main.dol [--names-out pal_names.txt]
```

The names file maps the decomp's names to PAL addresses. It's derived from the game binary, so it stays out of git.

**Cross-check:** the two strongest kinds of evidence are independent, and they agree. Seven functions are both found by their version banner and matched by size, and in every case the decomp's name is the init function's: `AIInit`, `DSPInit`, `DVDInit`, `EXIInit`, `GXInit`, `VIInit`, `SCInit`. The SDK turns out to be linked in alphabetical order of library (`ai`, `arc`, `ax`, `axfx`, `base`, `db`, `dsp`, `dvd`, `esp`, `exi`, `fs`, `gx`, `ipc`, `kpad`, `mem`, `mtx`, `os`, `si`, `thp`, `tpl`, `vi`, `wbc`, `wpad`), then the Bluetooth stack, USB, SC, NAND, the HOME Button Menu, RFL and NdevExi2A, which is a second check on the order the evidence produced.

## Libraries in the PAL build

Words are 4-byte instructions in the library's functions; *fp* counts floating-point instructions, *ps* paired singles, *priv* privileged instructions. *Decomp names*: functions matched by size to a named decomp function. **Plan**: *lift* (recompiled with the game), *replace* (native at the library's API), or *drop* (never runs).

| Library | PAL range | Functions | Decomp names | Words | fp | ps | priv | Plan |
|---|---|---|---|---|---|---|---|---|
| .init: debugger stub and interrupt vectors | `80004000`–`80005F88` | 28 | 0 | 386 | 0 | 0 | 58 | drop (MetroTRK's entry, and templates it copies to low memory) |
| .init: OS start-up | `80005F88`–`80005FB4` | 1 | 0 | 11 | 0 | 0 | 0 | replace (the runtime starts the game) |
| .init: EABI runtime (memcpy, __fill_mem, memset) | `80005FB4`–`800060E8` | 3 | 0 | 77 | 0 | 0 | 0 | lift |
| .init: OS start-up | `800060E8`–`80006420` | 8 | 0 | 205 | 0 | 0 | 2 | replace (the runtime starts the game) |
| game and Next Level Games' engine | `80007280`–`801C4294` | 8,325 | 0 | 455,592 | 36,824 | 2,321 | 0 | lift |
| Wwise plug-in: silence source | `801C4294`–`801C4790` | 14 | 0 | 318 | 60 | 4 | 0 | lift |
| MetroTRK (debugger stub) | `801C4790`–`801CA388` | 151 | 0 | 5,828 | 12 | 4 | 51 | drop |
| EABI MSL (C library) | `801CA388`–`801DAF34` | 153 | 44 | 17,130 | 1,689 | 26 | 0 | lift |
| EABI runtime | `801DAF34`–`801DBE54` | 59 | 17 | 968 | 51 | 0 | 0 | lift |
| revolution/ai | `801DBE54`–`801DC3D8` | 13 | 10 | 353 | 0 | 0 | 0 | replace |
| revolution/arc (U8 archives) | `801DC3D8`–`801DCD50` | 12 | 5 | 606 | 0 | 0 | 0 | lift |
| revolution/ax | `801DCD50`–`801E07E8` | 92 | 58 | 3,750 | 0 | 0 | 0 | replace |
| revolution/axfx (reverb, chorus, delay on the CPU) | `801E07E8`–`801E1688` | 14 | 11 | 936 | 220 | 6 | 0 | lift |
| revolution/base (CPU registers: HID, MMCR …) | `801E1688`–`801E1820` | 25 | 24 | 102 | 9 | 0 | 2 | replace |
| revolution/db (debugger hooks) | `801E1820`–`801E1890` | 3 | 0 | 28 | 0 | 0 | 2 | replace (no debugger) |
| revolution/dsp | `801E1890`–`801E2440` | 16 | 11 | 748 | 16 | 0 | 0 | replace |
| revolution/dvd | `801E2440`–`801E83A8` | 87 | 66 | 6,101 | 0 | 0 | 0 | replace |
| revolution/esp (e-ticket services) | `801E83A8`–`801EABF0` | 33 | 13 | 2,578 | 0 | 0 | 0 | replace |
| revolution/exi | `801EABF0`–`801ED3A8` | 35 | 17 | 2,542 | 0 | 0 | 0 | replace |
| revolution/fs | `801ED3A8`–`801EEB94` | 29 | 25 | 1,529 | 0 | 0 | 0 | replace |
| revolution/gx | `801EEB94`–`801F6754` | 185 | 144 | 7,920 | 438 | 78 | 0 | lift (v0.8.0: its command stream is the boundary, see below) |
| revolution/ipc | `801F6754`–`801F8878` | 37 | 22 | 2,121 | 0 | 0 | 0 | replace |
| revolution/kpad (Wii Remote processing over WPAD) | `801F8878`–`801FC1E4` | 32 | 9 | 3,675 | 1,367 | 52 | 0 | lift |
| revolution/mem (heaps) | `801FC1E4`–`801FD7E0` | 22 | 20 | 1,407 | 140 | 4 | 0 | lift |
| revolution/mtx | `801FD7E0`–`801FE598` | 23 | 15 | 878 | 720 | 398 | 0 | lift |
| revolution/os | `801FE598`–`8020CE64` | 272 | 182 | 14,893 | 482 | 117 | 42 | replace |
| revolution/si | `8020CE64`–`8020D86C` | 9 | 0 | 642 | 0 | 0 | 0 | replace |
| revolution/thp (movie decoder) | `8020D86C`–`8021119C` | 17 | 0 | 3,660 | 540 | 424 | 0 | lift (the decoder; its file and audio paths use replaced APIs) |
| revolution/tpl (texture files) | `8021119C`–`802112D0` | 2 | 0 | 77 | 0 | 0 | 0 | lift |
| revolution/vi | `802112D0`–`80215FF8` | 46 | 30 | 4,938 | 22 | 0 | 0 | replace |
| revolution/wbc (Balance Board) | `80215FFC`–`8021636C` | 5 | 0 | 219 | 8 | 0 | 0 | lift (maths over WPAD's data, like KPAD) |
| revolution/wpad (Wii Remote) | `8021636C`–`8022989C` | 197 | 65 | 19,786 | 298 | 18 | 0 | replace |
| revolution/bte (Bluetooth stack, under WPAD) | `8022989C`–`80257474` | 738 | 690 | 46,811 | 16 | 0 | 0 | replace (unused once WPAD is) |
| revolution/usb (under the Bluetooth stack) | `80257474`–`80258968` | 14 | 13 | 1,341 | 16 | 0 | 0 | replace (unused once WPAD is) |
| revolution/sc (console settings) | `80258968`–`8025A9C4` | 44 | 35 | 2,071 | 0 | 0 | 0 | replace |
| revolution/nand | `8025A9C4`–`8025E840` | 84 | 60 | 3,999 | 8 | 0 | 0 | replace |
| revolution/hbm (HOME Button Menu, with nw4hbm) | `8025E840`–`8027E01C` | 479 | 330 | 32,233 | 3,584 | 182 | 0 | lift |
| RVLFaceLibrary (Miis) | `8027E01C`–`80288410` | 123 | 59 | 10,492 | 599 | 50 | 0 | lift |
| NdevExi2A (development-kit link) | `80288414`–`80288EB0` | 14 | 13 | 678 | 0 | 0 | 0 | drop |
| Audiokinetic Wwise | `80288EB0`–`80311504` | 2,043 | 0 | 137,403 | 7,666 | 320 | 0 | lift (sound engine on AX) |

Totals in `.text` and the code part of `.init`: **673,591 words lifted (85%)**, 114,549 replaced (14%), 6,892 dropped (1%), with GX lifted (v0.8.0; 665,671, 122,469 and 6,892 before). Beat the Beat lifts 74%: here Wwise (137,403 words) and Next Level Games' engine take the place of NW4R, and they're lifted.

**Boundaries the evidence leaves open**, in functions either way: MetroTRK/MSL ±16 (the size matches put MSL's allocator at `801CA388`; the call evidence alone ties), VI/WBC ±8, NAND/HOME Button Menu ±3, `db`/`dsp` ±2, KPAD/MEM ±2, RFL/NdevExi2A ±2, and ±1 at `base`/`db`, DVD/ESP and SI/THP. The boundary between the game's code and Wwise's silence source plug-in is where the plug-in's first virtual function is; a non-virtual function or two before it could still be the plug-in's. Where an open boundary separates a lifted library from a replaced one (VI/WBC, NAND/HBM, SI/THP), the runtime settles it: an unregistered call into replaced code stops with its address.

## Checks of the boundary against the code

- **Every privileged instruction is on the replaced or dropped side:** 58 in `.init`'s debugger vectors, 2 in `.init`'s start-up, 2 in `base`, 2 in `db`, 42 in the OS, 51 in MetroTRK, and the 2 `mtmsr` in MetroTRK's exception entries that no function reaches (`dol-analysis.md`). 159 in all, none in lifted code.
- **No unknown word (`qilin coverage`) is in lifted code:** the debugger stub's name string in `.init` (12) and MetroTRK's exception entries (53) are on the dropped side, and `__init_data`'s tables (36) are data after the start-up code, in no library.
- **The paired-single code is on the lifted side** (game and engine 2,321, Wwise 320, THP 424, MTX 398, HOME Button Menu 182, KPAD 52, RFL 50 …), GX's matrix and vertex helpers (78) included since GX is lifted, except the OS's context switch, which saves and restores the paired-single registers (117) and is replaced.

## Why each library is lifted or replaced

The rules are RhythmHeavenFever--QilinRecomp's: a library that drives hardware registers or IOS directly is **replaced** at its API (OS, `base`, DVD, VI, AI, AX, DSP, EXI, SI, FS, IPC, NAND, SC, ESP, WPAD, the Bluetooth stack and USB under it); software whose exact behaviour matters more than native speed, reaching the hardware only through a replaced API, is **lifted** (MSL, the EABI runtime, ARC, AXFX, KPAD, MEM, MTX, TPL, the HOME Button Menu, RFL). **GX is the exception** (v0.8.0): it's lifted whole, and the hardware it drives is emulated instead. The engine writes GPU commands straight into the FIFO (`runtime-analysis.md`), so a decoder for GX's command stream is needed whatever happens to GX; with it, GX's own state tracking and register packing stay exactly as shipped, and the runtime only adds the few registers GX touches (command processor, pixel engine, the FIFO's pointers) and the draw-done and token interrupts. This is how Dolphin treats GX too.

The libraries this game adds:

- **Audiokinetic Wwise** (`80288EB0`–`80311504`, 2,043 functions) and its **silence source plug-in**: lifted. Its mixing, streaming and event timing are the game's audio behaviour, and it reaches the hardware only through replaced APIs: AX for output, and the game's file layer over DVD for banks and streams. The Wii Remote speaker output it drives goes through WPAD.
- **THP**: lifted. The decoder is pure computation (most of the 424 paired singles); its file reading and audio go through DVD and AX.
- **WBC** (Balance Board): lifted, like KPAD: calibration and weight maths over the data WPAD delivers. 8 floating-point instructions and no privileged ones. The port has no Balance Board (buttons only), but the game still calls WBC, which then sees none connected.
- **`db`** (debugger hooks): replaced by nothing (there is no debugger); its 2 privileged instructions go with it.
- **ESP** (e-ticket services over IOS): replaced.
- **MetroTRK, its `.init` vectors, and NdevExi2A**: dropped. They exist for the development kit's debugger.

**Game and engine** (`80007280`–`801C4294`, 8,325 functions, 57% of the code) is one segment for now. Separating Next Level Games' engine (`nl*`, the script VM, the renderer, networking) from the game itself is a naming job, not a boundary one: all of it is lifted.

**Decided when the runtime reached them** (v0.7.0, `runtime.md`): the OS's heap and arena code, `OSReport` and the EABI's constructor and destructor code are lifted (narrower ranges in `data/R7PP01/plan.tsv`); thread and alarm bookkeeping is replaced with the scheduler; the start-up path is run natively instead of `__start`.

## The whole-disc audit (v0.19.0)

A second pass over everything, done independently of `qilin`'s own discovery (scripts over the raw DOL and the disc's file table), to answer "is every byte of the game's code found?".

**`main.dol`, every word of its two code sections (799,200 words):**

| | Words | What |
|---|---|---|
| Code in functions | 794,194 | 13,487 functions: 11,672 lifted, 1,630 replaced, 185 dropped; 0 stubs; 100% of the instructions outside replaced and dropped code lifted |
| Padding | 4,193 | zero words between functions |
| Dead | 712 | 360 runs nothing reaches (`qilin coverage --list` now lists them): 200 lone `blr`s and 101 epilogue fragments after a `b` or `bctr`, and 59 runs a `b` jumps straight over (the compiler's leftovers, mostly in Wwise's containers). No branch, data word or built address anywhere points at any of the 712 |
| Not code | 101 | `__rom_copy_info` and `__bss_init_info` (tables), MetroTRK's name string and two exception entries |

- **Pointers:** every 4-byte word of the data sections that is a text address is a function's start (9,301) or a jump-table case inside one (6,448); 36 others land inside functions and are bitmap and table data that happen to look like addresses (each read). Every address lifted code builds with `lis`+`addi`/`ori` is a function's start (1,627) or a coincidence (31: constants such as `0x80018000`), or one of four known data addresses.
- **Function pointers into replaced code** were the gap in `check_replacements.py` (it followed direct calls only). With `--dol` it now checks them, and found two things, both fixed:
  - **MSL's `FILE` procedures on the debugger's host.** `stdin`, `stdout` and `stderr` hold `__read_console` (`801C9494`) and `__close_console` (`801C9354`), which `exit`'s `__close_all` calls; `fopen` stores four more. All were in the dropped debugger stub, so the game's own `exit` would have stopped at an unanswered call. They return 1 (an I/O error) with no debugger, and are now replaced by that.
  - **The VI/WBC boundary** was 1,128 bytes late: the four functions after VI's retrace handler (`80215B90`–`80215FF8`) are the Balance Board's weight maths and its WPAD callbacks, whose addresses the lifted WBC code takes. They're lifted now. (No Balance Board is ever reported, so they don't run; they're the game's code all the same.)
- **No code is made or loaded at run time:** `ICInvalidateRange` and `ICFlashInvalidate` are called only by the OS and the debugger stub, never by the game or its engine, and there is no module linker (`OSLink`) in the DOL.

**The rest of the disc (6,215 files, 3.78 GB, from `sys/fst.bin`):** no `.rel`, `.dol`, `.elf` or `.sel`. By type: 2,382 `.wav`, 1,194 each of `.data`/`.debug`/`.dict` (the engine's asset containers), 84 `.thp`, 64 `.txt`, 60 `.bnk`, 10 `.loc`, 10 `.arc`, 9 `.tpl`, and 14 others (fonts, ini, csv, xml, the banner). The 795 non-media files staged in the development session (every type but `.wav` and `.thp`) hold no PowerPC function prologue and no run of `blr`s. So `main.dol` is all the game's code; everything else is data its lifted code reads.

