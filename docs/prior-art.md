# Prior art: Wii and PowerPC recompilation, and this game (researched 2026-11-18)

What exists, what each project chose, and what it means for this one (C++23, Vulkan 1.3, cross-platform including low-end ARM boards, efficiency and reliability first, the same targets as GBTG-Qilincomp). Sections 4–9 are BeatTheBeat-QilinComp's survey, which applies to every Wii game, carried over unchanged.

## 1. Sibling projects: GBTG-Qilincomp and BeatTheBeat-QilinComp
- **GBTG-Qilincomp** (Grabbed by the Ghoulies, Xbox) set the requirements and scope this project shares: static recompilation to C++, the platform's system libraries replaced natively, Vulkan 1.3 only, a Raspberry Pi 4–class board as the low-end reference.
- **BeatTheBeat-QilinComp** (Beat the Beat: Rhythm Paradise, Wii) applied them to the Wii, and wrote the recompiler core this project started from (v0.4.0): the DOL loader, the Broadway decoder and its binutils cross-check, function discovery and CFG, coverage, the CLI, the tests and CI.

**Takeaways:** the core is game-independent apart from the supported build's hash. Punch-Out!! exercised it harder (56% more code, three times the indirect calls) and needed four new switch rules and a soundness fix (`recompiler-core.md`). They belong in the shared core, so they should go back to BeatTheBeat-QilinComp too.

## 2. This game: no decompilation, no symbols
- There's no public decompilation of Punch-Out!! (Wii), and none of the public Wii symbol collections (for example `riptl/wii-symbols`, 21 game IDs) has `R7P*`. The disc ships no linker map or ELF.
- What the game does give: the SDK's build banners, MSL's and MetroTRK's strings, C++ class names in `.rodata` (`AK::`, `homebutton::`, `nw4hbm::`, Next Level Games' `nl*`, `Sl*` and `FE*` classes), and a disc whose file names are readable (`art/`, `art/wwiseaudio/SoundbanksInfo.xml`).
- The libraries it shares with other games have names elsewhere: the Revolution SDK, MSL, the EABI runtime and MetroTRK are named by the Rhythm Heaven Fever decomp (CC0, vendored in BeatTheBeat-QilinComp) and other Wii decomps (Wii Sports, Super Paper Mario, Mario Kart Wii); the HOME Button Menu and `nw4hbm` by the decomps that link it.

**Takeaways:** names for the libraries come from matching against those decomps by function size sequences and strings, as BeatTheBeat-QilinComp ported US names to PAL; the game and engine code keep address names, with class names where vtables give them. Done for the Rhythm Heaven Fever decomp in v0.2.0 (`code-analysis.md`): it names 1,988 functions of the SDK, MSL, the EABI runtime, the HOME Button Menu and RFL.

## 3. Audiokinetic Wwise on the Wii
- Wwise is a commercial sound engine; its Wii version runs on top of AX and includes Wii Remote speaker output (`CAkWiimoteMgr`). The game's banks were generated with Wwise's authoring tool (the disc keeps `SoundBanksGeneration.log` and `SoundbanksInfo.xml`, which name the version and every event).

**Takeaways:** Wwise is lifted with the game, like NW4R's sound code in Beat the Beat, and AX underneath it is replaced. The banks' metadata on the disc will help name Wwise's functions and check its behaviour.

## 4. DolRecomp, ModernGekko, RecompCore (ExpansionPak)
- **DolRecomp:** a static recompiler for GameCube and Wii DOLs and RELs (and early Wii U), with C and LLVM back ends. It handles the CPU side only; the README leaves graphics, audio, input and the platform to the host.
- **ModernGekko:** "a runtime for GameCube/Wii recomps", crediting the Dolphin team as its foundation, with a code-mod ABI. **RecompCore** is the Dolphin-derived runtime underneath it (per-file GPL-2.0-or-later). Games in its hall of fame include Super Mario Sunshine, Brawl, Melee, Luigi's Mansion and Kirby Wii.

**Takeaways:** proof that CodeWarrior-compiled Wii code recompiles statically. The model (C output, guest registers in a context struct, an interpreter for whatever the translator doesn't cover, Dolphin's hardware emulation as the runtime) is what we're avoiding, for the same reasons Ghoulies avoided xboxrecomp's.

## 5. CastawayRecomp and BlackKnightRecomp (yunophilia) — Wii, static, C++
- *The Sims 2: Castaway* statically recompiled to C++ for Windows x64 and Linux x64; the recompiler and most of the runtime come from the same author's BlackKnightRecomp. Function names and starts come from a linker ELF the disc happens to ship.
- The recompiler decodes every Gekko/Broadway instruction, paired singles and quantised loads and stores included, and emits one C++ function per guest function.
- **The game's own Revolution OS runs recompiled; guest threads are host fibers.** VI, PI, CP/PE, DSP/AI, EXI, SI and IPC are emulated at a high level, IOS's `/dev/di` and `/dev/fs` are served from the installed disc files and a `nand/` folder. GX renders through Direct3D 11 (DXVK on Linux); audio goes through an AX HLE to SDL3. GPL-2.0-only, with Dolphin-derived runtime code.
- Testing: an end-to-end TAS script that plays from first boot, saves, quits and reloads, checking game memory at each step; headless on Linux.

**Takeaways:** a working Wii static recompilation with the same CPU decisions as ours. Its system boundary is lower than ours (the SDK runs recompiled on emulated hardware registers); ours replaces the SDK's hardware-facing libraries, as Ghoulies replaces the kernel and D3D, so no hardware register emulation is needed beyond the write-gather pipe. Fibers for guest threads are a data point for the scheduler decision. The TAS-style end-to-end test is worth copying at step 5.

## 6. NWiiRecomp (BlackLineInteractive) and Mario Kart Wiicompiled (patchzy)
- **NWiiRecomp:** a static recompiler and runtime for GameCube and Wii, PowerPC → C++, stating no instruction-level emulation and no Dolphin code; it implements the time base and decrementer, and paired singles with GQR-based quantisation.
- **Mario Kart Wiicompiled:** announced in July 2026 as the first static recompilation of a Wii game, in public beta since August, with Windows and x86-64 builds and, since September, an aarch64 one. It was built with AI coding tools, which drew criticism.

**Takeaways:** Wii static recompilation is now practical and done more than one way; none of these targets Vulkan 1.3 with an ARM board as the low-end reference.

## 7. XenonRecomp and ReXGlue (Xbox 360) — the most mature PowerPC → C++ lifters
- **XenonRecomp** (hedge-dev; Unleashed Recompiled) converts Xbox 360 executables into C++ that compiles for any platform; jump tables and other details come from configuration. Exceptions aren't supported. **ReXGlue** builds on it with a Xenia-based runtime and C++23 output, tracks which registers it can keep as locals, models CR fields for branches, and implements the MSR's interrupt-enable bit as a global lock.

**Takeaways:** a different CPU (64-bit, VMX, no paired singles) but the same instruction-set family; their CR handling and register-as-local tracking are the nearest prior art for our lifter's register model.

## 8. Dolphin
- The reference emulator, GPL-2.0-or-later. Its CPU interpreter documents Broadway's corner cases from hardware tests (paired-single rounding, the estimate tables, quantisation), and its GX and AX code define what correct output looks like.

**Takeaways:** ground truth for behaviour; we don't copy its code. Where we need a hardware fact that only Dolphin's source records (the `fres`/`frsqrte` tables, for instance), we derive it from hardware documentation or measurement and check it against Dolphin's results, and note the source here.

## 9. GNU binutils' `broadway` disassembler
- `objdump -M broadway,raw` decodes the 750CL's instructions with the paired singles, without simplified mnemonics. It is our decoder's cross-check (`tools/check/verify_decoder.py`). It differs from Broadway in reviewed ways: it leaves out `mftb`, accepts `fsqrt`, `fsqrts`, `tlbia`, `tlbld` and `tlbli` (not on the 750CL), accepts later ISAs' hint fields that are reserved on Broadway, and doesn't check several invalid forms. The full list is in the script.

## Decisions changed or added
1. **Core:** BeatTheBeat-QilinComp's recompiler core, extended where this game needed it; the extensions go back to it.
2. **Decoder:** our own, cross-checked against binutils on every word, in CI over a sweep of the encoding space (unchanged).
3. **Names:** no decomp exists for this game. Libraries are named by matching against the CC0 Rhythm Heaven Fever decomp and other Wii decomps; the rest keeps address names, with class names from vtables.
4. **System boundary:** replace the SDK's hardware-facing libraries at their API (no hardware register emulation beyond the write-gather pipe); lift the C/C++ runtime, the pure-software SDK libraries, THP, RFL, the HOME Button Menu, Wwise and Next Level Games' engine. Confirm per library in analysis.
5. **References:** Dolphin for behaviour and timing; CastawayRecomp's TAS test as the model for our end-to-end test.
