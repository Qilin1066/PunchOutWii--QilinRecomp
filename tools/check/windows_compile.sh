#!/usr/bin/env sh
# Compile the code with Windows-only paths (thread start-up, memory
# reservation) and the tests that use it for Windows, with MinGW-w64, on a
# Linux machine: catches windows.h trouble (min/max macros, redefinitions)
# without a Windows box. Needs x86_64-w64-mingw32-g++-posix (Debian/Ubuntu:
# apt install mingw-w64). MinGW's GCC may be older than the GCC 14 the tools
# need, so this compiles the runtime and game code directly, not through CMake.
# The executables run under Wine where Wine works (they print "all passed").
#
# With a game build's fetched headers (SDL3, Vulkan, volk under
# build-game/_deps; or pass the folder as the second argument), it also
# checks that every hand-written game and host file compiles for Windows.
set -eu
cd "$(dirname "$0")/../.."
out=${1:-build-windows-check}
mkdir -p "$out"
cxx="x86_64-w64-mingw32-g++-posix -std=c++23 -O2 -Wall -Wextra -Wpedantic -Wshadow -Werror -static"
inc="-Iruntime/include -Ihost/include -Ipunchout/src"
$cxx $inc -o "$out/threads_test.exe" punchout/tests/threads_test.cpp punchout/src/threads.cpp punchout/src/os.cpp punchout/src/restart.cpp punchout/src/priority.cpp runtime/src/*.cpp
$cxx $inc -o "$out/settings_test.exe" punchout/tests/settings_test.cpp punchout/src/settings.cpp runtime/src/*.cpp
echo "Windows build of the runtime, thread model and settings: ok ($out)"

deps=${2:-build-game/_deps}
if [ -d "$deps/sdl3-src" ]; then
    flags="-std=c++23 -fsyntax-only -Wall -Wextra -Wpedantic -Wshadow -Werror -Wno-missing-field-initializers -DVK_NO_PROTOTYPES -DIMGUI_IMPL_VULKAN_NO_PROTOTYPES"
    # Dear ImGui (the settings menu): CMake's download, or the folder in IMGUI_DIR.
    imgui=${IMGUI_DIR:-$deps/imgui-src}
    inc="$inc -isystem $imgui -isystem $imgui/backends"
    for f in host/src/*.cpp punchout/src/*.cpp; do
        x86_64-w64-mingw32-g++-posix $flags $inc -Irecomp/src -I"$deps/sdl3-src/include" \
            -I"$deps/vulkanheaders-src/include" -I"$deps/volk-src" -I"$(dirname "$deps")/generated" "$f"
    done
    echo "Every game and host source compiles for Windows: ok"
else
    echo "(no $deps: skipped the game and host sources)"
fi
