# Naming

**Qilin** is the author's name. It goes on the *tools*, the things that do the recompiling. It doesn't go on the game. This is the same rule as GBTG-Qilincomp's.

| Thing | Name | Why |
|---|---|---|
| The repo / project | PunchOutWii-QilinComp | The game plus the toolchain that ports it |
| The recompiler CLI | `qilin` | The author's tool: short, unique, easy to type |
| C++ namespaces of the recompiler and runtime | `qilin`, `qilin::ppc` (the decoder), `qilin::rt`, `qilin::gen` | Project namespace; avoids collisions with game, SDK or system names |
| Build options, macros, CMake targets | `QILIN_*`, `qilin_*` | Same prefix as the namespace, so it's obvious what owns them |
| The game executable players run | `punchout` | Named after the game. Players launch Punch-Out!!, not a tool |
| Settings file, save folder, window title | after the game (`qilin/punchout` is the data folder) | Same reason |
| Lifted functions | `fn_<address>`, e.g. `fn_80004050` | Named by their address in the original `main.dol`; library functions get real names where analysis matches them against other decomps (`prior-art.md` §2) |

New code follows the same rule: the prefix marks the toolchain, and anything a player sees is named after the game.
