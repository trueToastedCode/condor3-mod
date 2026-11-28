#!/usr/bin/env bash
# Installs / removes the post-processing build in the Condor 3 folder.
#   scripts/postfx.sh install     copies the game's d3dx.dll to real_d3dx.dll (if not there yet), installs build/d3dx.dll, postfx/postfx.hlsl + colour LUT,
#                                 the patched shaders (shaders/build/Effects/fx -> <game>/Effects/fx) and build/c3helper.exe
#   scripts/postfx.sh uninstall   restores the backed-up d3dx.dll and removes <game>/postfx, <game>/Effects/fx, c3helper.exe
#   scripts/postfx.sh sync        copies only postfx.hlsl (the running game hot-reloads it)
#   scripts/postfx.sh flight-planner-fix   ONLY the flight-planner (hangar preview) fix: installs the minimal build-hangar/d3dx.dll
#                                 (needs scripts/flight_planner_bug.sh first). No post effects, no streaming, no shaders.
#                                 Undo with: scripts/postfx.sh uninstall
# Game folder (the one with Condor.exe): second argument, else $CONDOR_DIR, else ~/Condor3.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GAME="${2:-${CONDOR_DIR:-$HOME/Condor3}}"
case "${1:-}" in install|uninstall|sync|flight-planner-fix) [ -f "$GAME/Condor.exe" ] || { echo "$GAME does not contain Condor.exe: $0 ${1} /path/to/Condor3"; exit 1; } ;; esac
REAL="$GAME/real_d3dx.dll"            # the mod forwards all d3dx calls to this (the game's original d3dx.dll)
OLDBAK="$GAME/d3dx.dll.before_postfx"  # backup name used by older versions of this script
case "${1:-}" in
  install)
    [ -f "$ROOT/build/d3dx.dll" ] || { echo "build first: scripts/build_docker.sh [Debug]"; exit 1; }
    if [ ! -f "$REAL" ]; then
      if [ -f "$OLDBAK" ]; then cp "$OLDBAK" "$REAL"
      else
        [ -f "$GAME/d3dx.dll" ] || { echo "$GAME/d3dx.dll is missing: reinstall Condor or restore d3dx.dll first"; exit 1; }
        if cmp -s "$GAME/d3dx.dll" "$ROOT/build/d3dx.dll"; then
          echo "$GAME/d3dx.dll is already the mod and real_d3dx.dll is missing: restore the original d3dx.dll first"; exit 1
        fi
        cp "$GAME/d3dx.dll" "$REAL"
      fi
    fi
    rm -f "$OLDBAK"
    cp "$ROOT/build/d3dx.dll" "$GAME/d3dx.dll"
    mkdir -p "$GAME/postfx"; cp "$ROOT/postfx/postfx.hlsl" "$GAME/postfx/"
    if [ ! -f "$GAME/postfx/lut.cube" ]; then
      if [ -f "$ROOT/postfx/user/lut.cube" ]; then cp "$ROOT/postfx/user/lut.cube" "$GAME/postfx/lut.cube"; else cp "$ROOT/postfx/default_lut/lut.cube" "$GAME/postfx/lut.cube"; fi
    fi
    [ -f "$ROOT/postfx/user/tune.txt" ] && [ ! -f "$GAME/postfx/tune.txt" ] && cp "$ROOT/postfx/user/tune.txt" "$GAME/postfx/tune.txt"
    if ls "$ROOT"/shaders/build/Effects/fx/*.fx >/dev/null 2>&1; then
      mkdir -p "$GAME/Effects/fx"; cp "$ROOT"/shaders/build/Effects/fx/*.fx "$GAME/Effects/fx/"; echo "Shaders: Effects/fx installed"
    else echo "Note: shaders/build/Effects/fx not found (python3 shaders/build_shaders.py build): the game's own shaders stay"; fi
    if [ -f "$ROOT/build/c3helper.exe" ]; then cp "$ROOT/build/c3helper.exe" "$GAME/c3helper.exe"; echo "Autogen streaming: c3helper.exe installed"
    else echo "Note: build/c3helper.exe not found (scripts/build_docker.sh builds it from external/condor3-helper): autogen streaming stays off"; fi
    echo "Installed. Start Condor normally.  F9 = effect on/off, F8 = reload shader, F10 = capture frame."
    echo "Log: $GAME/postfx/postfx.log   Remove again: scripts/postfx.sh uninstall" ;;
  flight-planner-fix)
    [ -f "$ROOT/build-hangar/d3dx.dll" ] || { echo "build first: scripts/flight_planner_bug.sh"; exit 1; }
    if [ ! -f "$REAL" ]; then
      if [ -f "$OLDBAK" ]; then cp "$OLDBAK" "$REAL"
      else
        [ -f "$GAME/d3dx.dll" ] || { echo "$GAME/d3dx.dll is missing: reinstall Condor or restore d3dx.dll first"; exit 1; }
        if cmp -s "$GAME/d3dx.dll" "$ROOT/build-hangar/d3dx.dll" || { [ -f "$ROOT/build/d3dx.dll" ] && cmp -s "$GAME/d3dx.dll" "$ROOT/build/d3dx.dll"; }; then
          echo "$GAME/d3dx.dll is already the mod and real_d3dx.dll is missing: restore the original d3dx.dll first"; exit 1
        fi
        cp "$GAME/d3dx.dll" "$REAL"
      fi
    fi
    rm -f "$OLDBAK"
    cp "$ROOT/build-hangar/d3dx.dll" "$GAME/d3dx.dll"
    echo "Flight-planner-fix installed (hangar preview fix only).  Remove again: scripts/postfx.sh uninstall" ;;
  uninstall)
    if [ -f "$REAL" ]; then mv -f "$REAL" "$GAME/d3dx.dll"
    elif [ -f "$OLDBAK" ]; then mv -f "$OLDBAK" "$GAME/d3dx.dll"
    else echo "Warning: real_d3dx.dll not found - d3dx.dll was left unchanged"; fi
    rm -f "$OLDBAK"
    rm -rf "$GAME/postfx" "$GAME/Effects/fx"; rm -f "$GAME/c3helper.exe"
    echo "Restored the previous d3dx.dll, removed $GAME/postfx, $GAME/Effects/fx and c3helper.exe" ;;
  sync)
    cp "$ROOT/postfx/postfx.hlsl" "$GAME/postfx/" && echo "postfx.hlsl copied" ;;
  *) echo "usage: $0 install|flight-planner-fix|uninstall|sync [/path/to/Condor3]"; exit 2 ;;
esac
