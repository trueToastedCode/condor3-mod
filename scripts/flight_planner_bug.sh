#!/usr/bin/env bash
# Builds a minimal d3dx.dll with ONLY the hangar preview fix (no postfx, no autogen streaming, no c3helper).
# Output: build-hangar/d3dx.dll  (Flight-Planner-Bug: Flugzeugvorschau wird ueber den Planner gezeichnet)
set -euo pipefail
cd "$(dirname "$0")/.."
docker image inspect c3dev >/dev/null 2>&1 || docker build -t c3dev .devcontainer
docker run --rm -v "$PWD":/workspace c3dev bash -lc "cd /workspace && bash scripts/build_dobby.sh \
  && cmake -B build-hangar -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-toolchain.cmake -DCMAKE_BUILD_TYPE=Release \
       -DENABLE_POSTFX=OFF -DENABLE_AGSTREAM=OFF -DENABLE_HANGAR_PREVIEW_FIX=ON \
  && cmake --build build-hangar -j4 --target d3dx_proxy"
ls -l build-hangar/d3dx.dll
