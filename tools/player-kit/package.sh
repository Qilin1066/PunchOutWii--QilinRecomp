#!/bin/sh
# Builds the Windows download: tools/player-kit/package.sh <version> <out.zip>
# (as the APB recomp's).
#
# Cross-compiles on Linux with llvm-mingw, then lays out:
#   PunchOutWii--QilinRecomp/Play Punch-Out.bat, README.txt, LICENSES/
#   PunchOutWii--QilinRecomp/kit/  qilin.exe, lib/libpunchout_kit.a, include/, kit.ninja,
#                            plan.tsv, play.ps1, ninja.exe, toolchain/ (llvm-mingw, trimmed), VERSION
# Nothing in it is made from the game: the player's PC lifts their own
# main.dol and compiles that code with the bundled compiler (play.ps1).
#
# Environment:
#   LLVM_MINGW_CROSS    the Linux-hosted llvm-mingw release, unpacked
#   LLVM_MINGW_WINDOWS  the x86_64 Windows-hosted release of the same version, unpacked
#   NINJA_WINDOWS       a folder with ninja.exe (Windows) and Ninja's COPYING
#   BUILD_DIR           where to build (default build-player-kit)
#   CMAKE_EXTRA         more cmake arguments (e.g. -DFETCHCONTENT_SOURCE_DIR_IMGUI=...)
set -eu
version=$1
out=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
root=$(cd "$(dirname "$0")/../.." && pwd)
build=${BUILD_DIR:-$root/build-player-kit}
stage=$(mktemp -d)
pkg="$stage/PunchOutWii--QilinRecomp"

# shellcheck disable=SC2086
cmake -S "$root" -B "$build" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$root/tools/player-kit/mingw-cross.cmake" \
      -DCMAKE_BUILD_TYPE=Release -DQILIN_PLAYER_KIT=ON -DQILIN_WERROR=ON ${CMAKE_EXTRA:-}
cmake --build "$build" --target qilin punchout_kit punchout_kit_link_check --parallel
cmake --install "$build" --component player --prefix "$pkg"

"$root/tools/player-kit/trim-toolchain.sh" "$LLVM_MINGW_WINDOWS" "$pkg/kit/toolchain"
cp "$NINJA_WINDOWS/ninja.exe" "$pkg/kit/"
cp "$root/tools/player-kit/play.ps1" "$pkg/kit/"
sed 's/$/\r/' "$root/tools/player-kit/Play Punch-Out.bat" > "$pkg/Play Punch-Out.bat"
sed 's/$/\r/' "$root/tools/player-kit/README.txt" > "$pkg/README.txt"
printf '%s\n' "$version" > "$pkg/kit/VERSION"

# Every licence of what's included, by component.
lic="$pkg/LICENSES"
mkdir -p "$lic/PunchOutWii--QilinRecomp" "$lic/llvm-mingw" "$lic/ninja" "$lic/SDL3" "$lic/Dear ImGui" "$lic/Vulkan-Headers" "$lic/volk"
mv "$pkg/kit/toolchain/licenses/"* "$lic/llvm-mingw/"
rmdir "$pkg/kit/toolchain/licenses"
sed 's/$/\r/' "$root/LICENSE" > "$lic/PunchOutWii--QilinRecomp/LICENSE.txt"  # GPL-3.0
cp "$NINJA_WINDOWS/COPYING" "$lic/ninja/"
cp "$build/_deps/sdl3-src/LICENSE.txt" "$lic/SDL3/"
imgui=$(sed -n 's/^FETCHCONTENT_SOURCE_DIR_IMGUI:PATH=//p' "$build/CMakeCache.txt")
[ -n "$imgui" ] || imgui="$build/_deps/imgui-src"
cp "$imgui/LICENSE.txt" "$lic/Dear ImGui/"
cp "$build/_deps/vulkanheaders-src/LICENSE.md" "$lic/Vulkan-Headers/" 2>/dev/null || cp "$build/_deps/vulkanheaders-src/LICENSE"* "$lic/Vulkan-Headers/"
cp "$build/_deps/volk-src/LICENSE.md" "$lic/volk/"

rm -f "$out"
(cd "$stage" && zip -q -r -9 "$out" PunchOutWii--QilinRecomp)
rm -rf "$stage"
echo "Packaged $out ($(du -h "$out" | cut -f1))"
