#!/usr/bin/env sh
# Cross-builds everything for 64-bit ARM Linux (the Raspberry Pi 4's
# Cortex-A72) on an x86-64 Linux machine, the code lifted from the disc
# included when a main.dol is given, and runs the unit tests under qemu:
#
#   tools/check/arm64_build.sh [<extracted disc>/sys/main.dol] [build folder]
#
# Needs g++-14-aarch64-linux-gnu and qemu-user (Debian/Ubuntu:
# apt install g++-14-aarch64-linux-gnu qemu-user). The lifter itself runs
# under qemu too. SDL3 is built without a window system or sound here (the
# cross sysroot has none of their headers): the result is for checking, not
# for playing; on a Pi, build natively (./punchout.sh).
set -eu
cd "$(dirname "$0")/../.."
dol=${1:-}
out=${2:-build-arm64}
deps=build-game/_deps  # sources CMake fetched for another build, when there are some
fetched=""
if [ -d "$deps/sdl3-src" ]; then
    fetched="-DFETCHCONTENT_SOURCE_DIR_SDL3=$PWD/$deps/sdl3-src -DFETCHCONTENT_SOURCE_DIR_VULKANHEADERS=$PWD/$deps/vulkanheaders-src -DFETCHCONTENT_SOURCE_DIR_VOLK=$PWD/$deps/volk-src"
fi
[ -n "${IMGUI_DIR:-}" ] && fetched="$fetched -DFETCHCONTENT_SOURCE_DIR_IMGUI=$IMGUI_DIR"
game=""
[ -n "$dol" ] && game="-DQILIN_GAME_DOL=$dol"
# shellcheck disable=SC2086
cmake -S . -B "$out" -G Ninja -DCMAKE_TOOLCHAIN_FILE=tools/pi/aarch64-cross.cmake -DCMAKE_BUILD_TYPE=Release -DQILIN_WERROR=ON \
      -DSDL_UNIX_CONSOLE_BUILD=ON -DSDL_WAYLAND=OFF -DSDL_X11=OFF -DSDL_KMSDRM=OFF -DSDL_PIPEWIRE=OFF -DSDL_PULSEAUDIO=OFF \
      -DSDL_ALSA=OFF -DSDL_JACK=OFF -DSDL_SNDIO=OFF -DSDL_LIBUDEV=OFF -DSDL_DBUS=OFF -DSDL_IBUS=OFF $fetched $game
cmake --build "$out" --parallel
# The tests that need no GPU, window or disc, under qemu (not the restart
# test: it runs itself again, as an ARM program, which only works with qemu
# registered with the kernel's binfmt_misc).
ctest --test-dir "$out" --output-on-failure -R "qilin_tests|qilin_frame_times_test|punchout_threads_test|punchout_settings_test" \
      -E punchout_threads_test_restart
echo "ARM64 (Cortex-A72) build: ok ($out)"
