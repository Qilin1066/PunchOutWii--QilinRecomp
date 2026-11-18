# main.dol: analysis (roadmap step 1)

Last updated 2026-11-18 (v0.1.0: the PAL `main.dol` run through every tool: hash, sections, instruction profile, decoder cross-check, functions and coverage; what's linked in, from the SDK's own strings).

## Getting main.dol

The tools read `main.dol` from the game's data partition. Nothing from the game goes into git (`.gitignore` covers it). To extract it with Dolphin: right-click the game → **Properties** → **Filesystem** → right-click the data partition → **Extract System Data**. That writes `sys/main.dol` (and `sys/boot.bin`, `sys/apploader.img`, `sys/fst.bin`). **Extract Entire Disc** gives the `files/` the runtime will read later. Wiimms ISO Tools (`wit extract`) does the same from a WBFS image.

```sh
build/qilin info    path/to/main.dol            # SHA-256, supported build, entry point, sections, BSS (JSON)
build/qilin profile path/to/main.dol            # instruction profile of the text sections
build/qilin insns   path/to/main.dol -o main.insns.txt
build/qilin cfg     path/to/main.dol --json cfg.json   # functions, jump tables, summary
build/qilin coverage path/to/main.dol --list           # every word: code, padding, dead or unknown
build/qilin suspects path/to/main.dol                  # and the rejected starts
python3 tools/check/verify_decoder.py --qilin build/qilin --dol path/to/main.dol
```

## The supported build

| | |
|---|---|
| Disc | `R7PP01`, **revision 1**: *Punch-Out!!*, Europe/Australia (`sys/boot.bin` and the disc header start `R7PP01`; the header's revision byte is 1). Redump: "Punch-Out!! (Europe, Australia) (En,Fr,De,Es,It) (Rev 1)", ISO MD5 `19807f20a3866ed657c8cfeb3734b238` |
| `main.dol` SHA-256 | `6ae3388ff644758f030bb28de28f898885dc7c2da76bff082faae9eb60e66137` ✅ the user's copy (v0.1.0) |
| `main.dol` size | 3,605,792 bytes |
| Entry point | `0x80006124` (`__start`) |

## Sections

From `qilin info`. The names are CodeWarrior's usual layout, confirmed by `__init_data`'s own tables (below): its ROM copy table lists exactly these ten sections, in this order, and its BSS table the three BSS ranges.

| Slot | Address | Size (bytes) | Section |
|---|---|---|---|
| text0 | `0x80004000` | 9,440 | `.init`: start-up code, the debugger stub's entry, `memcpy`/`memset`, `__init_data`'s tables |
| data0 | `0x800064E0` | 1,408 | `extab` (C++ exception actions) |
| data1 | `0x80006A60` | 2,080 | `extabindex` (function → exception table) |
| text1 | `0x80007280` | 3,187,360 | `.text` |
| data2 | `0x80311520` | 1,536 | `.ctors` (static initialisers) |
| data3 | `0x80311B20` | 32 | `.dtors` |
| data4 | `0x80311B40` | 49,280 | `.rodata` |
| data5 | `0x8031DBC0` | 329,664 | `.data` |
| BSS | `0x8036E380` | 667,360 | `.bss` |
| data6 | `0x80411260` | 12,448 | `.sdata` (small data, addressed from `r13`) |
| BSS | `0x80414300` | 8,112 | `.sbss` |
| data7 | `0x804162C0` | 12,288 | `.sdata2` (small constants, addressed from `r2`) |
| BSS | `0x804192C0` | 36 | `.sbss2`, up to `0x804192E4` |

The header's BSS range is `0x8036E380`, 700,260 bytes: it spans `.bss`, `.sdata`, `.sbss`, `.sdata2` and `.sbss2`, and the loader keeps the two small-data sections' bytes inside it. (Beat the Beat's linker put `.sbss` first; this one puts `.bss` first. Nothing depends on the order.)

Compared with Beat the Beat, `.text` is 56% larger (3.19 MB against 2.04 MB) and `.rodata` is a twentieth of the size: Punch-Out!! keeps its data on disc, in Next Level Games' resource files.

## What's linked in

The SDK libraries print their build banners into `.rodata`. All carry the SDK revision `0x4199_60831`:

| Library | Built | | Library | Built |
|---|---|---|---|---|
| AI, AX, DSP, DVD, EXI, SC, SI, WBC | 30 Jul 2008 | | OS | 2 Oct 2008 |
| GX, KPAD, NAND, VI, WPAD | 11 Sep 2008 | | HBM (HOME Button Menu) | 16 Jun 2008 |
| THP (movies) | 30 Jul 2008 | | RFL (Mii) | 6 Mar 2008 |

Other code, from its strings and class names:
- **MSL and the PowerPC EABI runtime** ("Metrowerks CW runtime library initializing default heap").
- **MetroTRK**, the debugger stub ("MetroTRK for Revolution v0.1", "Metrowerks Target Resident Kernel for PowerPC").
- **The HOME Button Menu** (`homebutton::…`) with its private copy of NW4R's layout and font code (`nw4hbm::lyt`, `nw4hbm::ut`). There is no NW4R proper: no sound sequencer, g3d or layout in the game itself.
- **Audiokinetic Wwise** (`AK::…`, `CAk…`): the sound engine, with its stream manager, plug-in interfaces and Wii Remote speaker support. The disc's `art/wwiseaudio/` has 60 soundbanks and 2,382 streamed files.
- **Next Level Games' engine**: `nl*` (resources: `nlReadAsync`; events: `nlEventRegistry`, `nlEventGroups`), a scripting language (`SlScriptManager`, `ScriptModule`, `ScriptClass`), the front end (`FESceneManager`), animation blending (`TransitionBlenderNode`, `ScaleBlenderNode`), particle emission, and material techniques (`NLG_CELLRAMP`, `NLG_RIMLIGHTMAP`, `NLG_NORMALMAP` …).

The library ranges (where each starts and ends in `.text`) are the next analysis step; see the roadmap.

## Instructions

`qilin profile`: 799,200 words in the text sections, 172 distinct instructions. The most common are `addi` (17.0%), `lwz` (14.1%), `b` (10.5%), `stw` (10.3%) and `bc` (7.7%). 54,785 floating-point instructions, of which 4,005 are paired singles (`psq_l`/`psq_st` 3,280, the rest arithmetic). 159 privileged instructions (`mfmsr` 58, `rfi` 39, `mtmsr` 27, `mtsr`/`mfsr` 16 each …), in the SDK's OS and the debugger stub, which are replaced or dropped. `mftb` appears 6 times, `sc` 3 times, `dcbz_l` once.

**Decoder cross-check** (`tools/check/verify_decoder.py --dol`): all 799,200 words agree with GNU objdump; 795,109 valid and identical, 3,895 invalid for both, and the only known-difference classes are `mfcr`'s printed field (190) and `mftb` (6). Of the 3,895 invalid words, 3,876 are zero padding; the other 19 are data in `.init` (15) and four `lmw r0,0(r3)`, an invalid form the debugger stub's hand-written register restore uses. The debugger stub isn't lifted. (These 19 are the same as in Beat the Beat.)

## Functions and coverage

`qilin cfg` (1.9 s): **13,487 functions** (1 entry, 7,566 call targets, 344 reached only by tail calls, 670 only through `lis`/`addi` pointers, 4,858 only through pointers in data such as vtables, 48 found in gaps), 794,194 reachable instructions, **187 jump tables, all proven** by their bound check, 6,815 indirect calls, 353 indirect tail calls, 1,406 tail calls, and 33 functions that fall into the next one (the runtime's register save/restore chains).

The share of functions reached only through data (36%, against 26% in Beat the Beat) and the number of indirect calls (6,815 against 2,560) say this is a C++ engine built on virtual calls: Wwise, the engine's classes and the script runtime.

Four switch shapes this game has and Beat the Beat didn't needed new CFG rules (`recompiler-core.md`). Without them, a 416-case switch (`0x8000E44C`, the script interpreter's opcode dispatch, by its shape) broke into 286 false functions, and four other switches went unproven.

**One suspect:** the debugger stub's second exception entry (`0x801C95E8`, called directly) reaches the `lmw` invalid form. **Three starts rejected** by validation: `__init_data`'s two tables and the debugger stub's first exception entry.

`qilin coverage`, by word:

| Section | Code | Padding | Dead | Unknown |
|---|---|---|---|---|
| `.init` (2,360 words) | 679 | 1,632 | 1 | 48 |
| `.text` (796,840 words) | 793,515 (99.58%) | 2,561 | 711 | 53 |

Padding is zero words and `nop`s that nothing reaches. Dead code is 712 words the compiler left unreachable: inside functions (typically a `b` right after another unconditional branch, or a switch case nothing selects), and just past a function's end (the `blr` CodeWarrior keeps after a final tail call, the epilogue after a final call through `bctr`). Nothing branches to them and no pointer in data points into them. **The 101 unknown words are all identified, and none of them is code we lift:**

| Where | Words | What |
|---|---|---|
| `0x80004054` | 12 | The string "Metrowerks Target Resident Kernel for PowerPC" (the debugger stub) |
| `0x80006420` | 30 | `__init_data`'s ROM copy table: ten (source, destination, size) entries, source = destination |
| `0x800064A4` | 6 | `__init_data`'s BSS table: three (address, size) ranges |
| `0x801C9550`, `0x801C9640` | 53 | The debugger stub's exception entries and register restore (`stmw r0`, `lmw r0`) |

`qilin coverage` exits with status 3 while any word is unknown, and these 101 keep it there by design: they're data and debugger code, accounted for here rather than hidden by the tool.
