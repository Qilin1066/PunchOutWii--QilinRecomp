#!/usr/bin/env sh
# Compile the code with Windows-only paths (thread start-up, memory
# reservation) and the tests that use it for Windows, with MinGW-w64, on a
# Linux machine: catches windows.h trouble (min/max macros, redefinitions)
# without a Windows box. Needs x86_64-w64-mingw32-g++-posix (Debian/Ubuntu:
# apt install mingw-w64). MinGW's GCC may be older than the GCC 14 the tools
# need, so this compiles the runtime and game code directly, not through CMake.
# The executables run under Wine where Wine works (they print "all passed").
set -eu
cd "$(dirname "$0")/../.."
out=${1:-build-windows-check}
mkdir -p "$out"
cxx="x86_64-w64-mingw32-g++-posix -std=c++23 -O2 -Wall -Wextra -Wpedantic -Wshadow -Werror -static"
inc="-Iruntime/include -Ihost/include -Ipunchout/src"
$cxx $inc -o "$out/threads_test.exe" punchout/tests/threads_test.cpp punchout/src/threads.cpp punchout/src/os.cpp runtime/src/*.cpp
$cxx $inc -o "$out/settings_test.exe" punchout/tests/settings_test.cpp punchout/src/settings.cpp runtime/src/*.cpp
echo "Windows build of the runtime, thread model and settings: ok ($out)"
