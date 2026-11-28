# The game's details for the packaging scripts (sourced), as Toy Commander's
# games.sh. Everything game-specific in a download comes from here: the
# README templates' @NAME@ places (fill) and kit/game.txt (write_game_txt),
# which is all the launcher knows about the game
# (tools/player-kit/launcher/ADOPTING.md).
game=punchout
REPO="Qilin1066/PunchOutWii--QilinRecomp"
TITLE="Punch-Out"; PACKAGE="PunchOutWii--QilinRecomp"; EXE="Punch-Out.exe"; BOOT="main.dol"
BUILD_TITLE="Punch-Out!! (Europe, Australia)"; PRODUCT="R7PP01"; DISC_VERSION="1"
RELEASE="the Europe/Australia release, disc R7PP01, revision 1"
STUDIO="Next Level Games / Nintendo, Wii, 2009"
# The game's full name, for the READMEs' prose; TITLE (the launcher's "Play
# Punch-Out", the app, the menu entry) is the first downloads' "Play Punch-Out",
# whose .bat the launcher then removes from an old folder (tidy.cpp).
FULL_TITLE="Punch-Out!!"
# Fills a template's @NAME@ places with the game's details.
fill() {
    sed -e "s|@GAME@|$game|g" -e "s|@FULL_TITLE@|$FULL_TITLE|g" -e "s|@TITLE@|$TITLE|g" -e "s|@PACKAGE@|$PACKAGE|g" -e "s|@EXE@|$EXE|g" \
        -e "s|@BOOT@|$BOOT|g" -e "s|@BUILD_TITLE@|$BUILD_TITLE|g" -e "s|@RELEASE@|$RELEASE|g" -e "s|@STUDIO@|$STUDIO|g" "$1"
}
# Writes kit/game.txt, one name=value a line:
#   write_game_txt <file> <exe> <compiler> [<compiler_args>]
# with Windows line ends where GAME_TXT_CRLF=1. Every key is written, the
# launcher's defaults too, so the file says the whole story: the game's
# names, the data it builds from (an extracted disc's folder; `build` is
# recomp/src/dol.cpp's known build's title, which `qilin identify --build`
# checks), how it is recompiled (one step: qilin lift of sys/main.dol with
# the library plan, into 32 files, as CMakeLists.txt's punchout target),
# what a compile needs, how the game starts (punchout <folder>), and where
# releases are published (tags vX.Y.Z).
#
# compile_memory_mb: measured, not guessed. The largest of the 32 files
# qilin lift writes from the supported main.dol (lifted_003.cpp, 5.0 MB of
# C++) peaked at 750 MB of working set compiled with the kit's cxxflags and
# -march=native by zig 0.17.1's clang 22 on Windows x86-64 (2026-11-28; the
# next largest, 4.8 MB, at 690 MB). 1200 leaves about 60% for the other
# compilers (llvm-mingw's, Apple's), ARM64 and a larger file after a lifter
# change. The whole lift's output (gen/) is 64 MB.
write_game_txt() {
    {
        printf 'game=%s\ntitle=%s\nexe=%s\nboot=%s\n' "$game" "$TITLE" "$2" "$BOOT"
        printf 'build=%s\nproduct=%s\ndisc_version=%s\nrelease=%s\n' "$BUILD_TITLE" "$PRODUCT" "$DISC_VERSION" "$RELEASE"
        printf 'package=%s\nrepo=%s\nrelease_tag_prefix=v\nrelease_asset_prefix=%s-\n' "$PACKAGE" "$REPO" "$PACKAGE"
        printf 'input=folder\ninput_noun=extracted disc\n'
        printf 'input_hint=Choose the folder you extracted the disc into with Dolphin (right-click the game, Properties, Filesystem tab, right-click the disc, Extract Entire Disc): the one holding sys and files, or the DATA folder inside it.\n'
        printf 'identify={qilin} identify {input} --json --build {game:build}\n'
        printf 'step.1={qilin} lift {input}/sys/main.dol -o {gen} --plan {kit}/plan.tsv --files 32\n'
        printf 'compile_memory_mb=1200\nbuild_space_mb=2048\ncache_mb=2048\nrun_args={input}\n'
        # What the downloads before the launcher (Play Punch-Out.bat and
        # kit/play.ps1, up to 0.22.0) left, and where play.ps1 remembered
        # the disc folder: removed, and the folder taken over (launcher 2).
        printf 'old_files=Play Punch-Out.bat;kit/play.ps1;kit/ninja.exe;kit/kit.ninja;work/build.ninja;work/.ninja_log;work/.ninja_deps;work/disc-folder.txt.tmp;Punch-Out.exe.new;Punch-Out.exe.previous\n'
        printf 'old_input_file=work/disc-folder.txt\n'
        printf 'compiler=%s\n' "$3"
        if [ -n "${4:-}" ]; then printf 'compiler_args=%s\n' "$4"; fi
    } | if [ "${GAME_TXT_CRLF:-0}" = 1 ]; then sed 's/$/\r/'; else cat; fi > "$1"
}
