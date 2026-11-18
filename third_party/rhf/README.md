# Rhythm Heaven Fever decompilation: symbols and splits

From [conhlee/rhf](https://github.com/conhlee/rhf) at commit `02b140c9e7007e2a4ddd61c80ac3f2eb35251ed7`, a work-in-progress decompilation of the US release (`SOME01`). Licence: **CC0 1.0** (`LICENSE`).

| File | What |
|---|---|
| `SOME01/symbols.txt` | Every symbol of the US build: name, section, address, type, size (22,221 symbols, 8,103 functions) |
| `SOME01/splits.txt` | Which address ranges belong to which source file (translation unit) |

Here it names the libraries Punch-Out!! shares with Rhythm Heaven Fever (the Revolution SDK, MSL, the EABI runtime, the HOME Button Menu, RFL): `tools/analysis/identify_libraries.py` matches them by function size sequences, and `docs/code-analysis.md` has the results. Copied from BeatTheBeat-QilinComp, where the same files port the names to Beat the Beat's PAL build. The files are unchanged copies; update them by copying the decomp's `config/SOME01/` again and noting the new commit here.
