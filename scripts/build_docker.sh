#!/usr/bin/env bash
# Builds d3dx.dll (+ pfxtest.exe, monitor) and the 64-bit autogen helper build/c3helper.exe (git submodule external/condor3-helper)
# inside the dev container image "c3dev".
#   scripts/build_docker.sh            Release
#   scripts/build_docker.sh Debug      Debug (runtime logging to the monitor)
# Further arguments go to cmake when configuring d3dx.dll (not the helper), e.g. to switch the optional patches
# (ENABLE_MOVE_PATCHES, ENABLE_HANGAR_PREVIEW_FIX, ENABLE_POSTFX, ENABLE_AGSTREAM; see CMakeLists.txt):
#   scripts/build_docker.sh Release -DENABLE_POSTFX=OFF -DENABLE_AGSTREAM=OFF
#   scripts/build_docker.sh -DENABLE_POSTFX=OFF        (build type left out = Release)
# Note: cmake remembers -D values in build/CMakeCache.txt; to go back to a default pass it explicitly (-DENABLE_POSTFX=ON).
set -euo pipefail
cd "$(dirname "$0")/.."
TYPE=Release
if [ $# -gt 0 ] && [[ "$1" != -* ]]; then TYPE="$1"; shift; fi
CMAKE_ARGS=""
for a in "$@"; do CMAKE_ARGS+=" $(printf '%q' "$a")"; done
docker image inspect c3dev >/dev/null 2>&1 || docker build -t c3dev .devcontainer
[ -f external/condor3-helper/CMakeLists.txt ] || git submodule update --init external/condor3-helper
docker run --rm -v "$PWD":/workspace c3dev bash -lc "cd /workspace && bash scripts/build_dobby.sh && cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-toolchain.cmake -DCMAKE_BUILD_TYPE=$TYPE$CMAKE_ARGS && cmake --build build -j4 \
  && cmake -S external/condor3-helper -B build/helper -DCMAKE_TOOLCHAIN_FILE=/workspace/external/condor3-helper/cmake/mingw64-toolchain.cmake -DCMAKE_BUILD_TYPE=Release >/dev/null \
  && cmake --build build/helper -j4 && cp build/helper/c3helper.exe build/c3helper.exe"
