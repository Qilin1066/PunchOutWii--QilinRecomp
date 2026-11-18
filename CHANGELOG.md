# Changelog

All notable changes to this project are recorded here, newest first. Past entries are never rewritten.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow [Semantic Versioning](https://semver.org/spec/v2.0.0.html):

- **0.x.y** while the recompiler is being built. A **minor** bump (0.**x**) is a milestone, for example a new lifter slice or the runtime booting. A **patch** bump (0.x.**y**) is a fix.
- **1.0.0** will be the first build that plays the game end to end at original spec.

Each release is a git tag (`vX.Y.Z`) with a matching GitHub release. The version is set once, in `project(... VERSION ...)` in `CMakeLists.txt`, and `qilin --version` prints it.

## [Unreleased]

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
