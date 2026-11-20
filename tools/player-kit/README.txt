Punch-Out!! QilinRecomp for Windows (64-bit)
============================================

Punch-Out!! (Next Level Games / Nintendo, Wii, 2009), recompiled to run
natively: the game's own program translated into C++ and compiled for your
PC, drawn with Vulkan.

No game data is included. You need your own copy of the game: the
Europe/Australia release, disc R7PP01, revision 1, extracted with Dolphin
(right-click the game, Extract Entire Disc). That gives a folder holding
"sys" and "files".


Playing
-------
1. Unpack this folder anywhere (it needs about 2 GB once built).
2. Double-click "Play Punch-Out.bat".
   - The first time, it asks for your extracted disc folder, then builds the
     game from it: a few minutes (up to 15 on a slower PC). Your game files
     never leave your PC.
   - After that, it starts the game straight away. Punch-Out.exe in this
     folder can be started directly too, with the disc folder as its
     argument.
   - To use a different disc folder: Play Punch-Out.bat -ChooseDisc

If Windows says "Windows protected your PC": More info, then Run anyway.
(The download isn't signed.) Or before unpacking, right-click the zip,
Properties, tick Unblock, OK.

Needs: Windows 10 or 11, 64-bit, and a graphics driver with Vulkan 1.3
(any recent NVIDIA, AMD or Intel driver).


Controls
--------
A Wii Remote held sideways, buttons only (no motion controls).

  Xbox controller      Keyboard        Wii Remote
  D-pad, left stick    arrows          dodge, duck, block
  B                    Z               1 (left punch)
  A                    X               2 (right punch)
  X or Y               C, Space        A (Star Punch, OK)
  Start / Back         Enter / Bksp    + / -
  Guide                H               HOME
  right stick          mouse           the pointer (left click is A)

Back + Start together, or F1 or Esc: the settings menu (the game pauses):
controls (set by pressing them), the picture (size, a CRT look, internal
resolution, filtering), full screen, the game's language and widescreen.
Alt+Enter or F11: full screen.

Settings and saves are kept in %LOCALAPPDATA%\qilin\punchout.


What's in here
--------------
Play Punch-Out.bat  builds (once) and starts the game
kit\qilin.exe       the recompiler: turns the disc's main.dol into C++
kit\lib, include    the prebuilt rest of the game: its runtime, the renderer,
                    sound, the window and controls (SDL3), the menu (Dear ImGui)
kit\plan.tsv        which parts of the game are recompiled, and which run natively
kit\toolchain       LLVM-MinGW, a C++ compiler for Windows (Clang)
kit\ninja.exe       Ninja, which drives the compiler
work\               made when building: the recompiled code (from your disc)
LICENSES\           the licences of everything included

Source, changes and issues: https://github.com/Qilin1066/PunchOutWii-QilinRecomp
