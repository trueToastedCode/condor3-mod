# Condor 3 mod: better look, big photo-scenery landscapes, landscape compiler

<p align="center"><img src="docs/screenshot.jpg" alt="Condor 3 with the mod" width="720"></p>

This package has three parts:

1. **The mod** for Condor 3: a nicer picture (colour grading, haze, contact shadows, improved terrain/sky/cloud shaders) and
   **autogen streaming**, which lets Condor fly very large 3D photo-scenery landscapes (whole cities) without running out of memory.
2. **The autogen helper** (`c3helper.exe`), a small background program the mod starts by itself. It holds the 3D buildings of the
   landscape and hands the mod what is near you.
3. **The landscape compiler** (`tiles2condor_v2`), which turns a 3D-tiles export (photogrammetry 3D tiles, for example exported
   with earth-rea as "3D Tiles - one glb per file") into a complete Condor 3 landscape.

You only need part 3 if you want to make your own landscapes.

## Requirements
- **Condor 3 version 3.1.0** exactly. The mod hooks into fixed places of Condor.exe; with another version it does not work
  (uninstall it then, see below).
- **Windows 10/11 (64-bit)** is the first-class platform.
- **macOS** (Condor in a CrossOver bottle) is supported on a best-effort basis. Setting up the bottle takes some extra steps;
  see **[README_MACOS.md](README_MACOS.md)**.
- For the compiler: the platform in the package name (`...-windows.zip`, `...-macos-x86_64.zip`, `...-linux-x86_64.zip`), a GPU
  with DirectX 12, Metal or Vulkan (or `--cpu`), enough free disk space (a city landscape takes several GB, plus a temporary work folder).

## What is in the package
| file / folder | what it is |
|---|---|
| `README.md` | this file |
| `README_POSTFX.md` | technical notes on the picture effects and the full [remote control](README_POSTFX.md#remote-control) reference |
| `VERSION.txt` | which version this is, for which platform the compiler is |
| `build/d3dx.dll` | the mod. It replaces Condor's own `d3dx.dll` (the original is kept as `real_d3dx.dll`, which the mod forwards to) |
| `build/c3helper.exe` | the autogen helper (64-bit). Without it the mod leaves autogen to Condor as usual |
| `postfx/postfx.hlsl` | the picture effects (shader). Copied to `<Condor>\postfx\`; its first lines list everything you can tune |
| `postfx/default_lut/lut.cube`, `postfx/user/lut.cube` | colour looks (LUTs). The installer uses `user/lut.cube` if present, else the default |
| `postfx/user/tune.txt` | (if present) tuning values that are installed as your starting point |
| `shaders/build/Effects/fx/*.fx` | improved versions of Condor's own shaders (terrain, sky, clouds, trees, grass). Installed to `<Condor>\Effects\fx\`; Condor's original files are not touched |
| `scripts/postfx.bat` | install / uninstall on Windows |
| `scripts/postfx.sh` | install / uninstall on the Mac (CrossOver) |
| `scripts/tune.sh` | Mac: change tuning values while the game runs (`scripts/tune.sh lut_strength 0.5`) |
| `bin/tiles2condor_v2` (`.exe` on Windows) | the landscape compiler |
| `examples/orly_custom_runway.json`, `examples/eiffel.json` | example compiler settings (Paris Orly with its three custom high-resolution runways; Eiffel tower area) |
| `assets/runway/` | textures for custom runways (asphalt, concrete, grass, markings paint) |
| `docs/TILES2CONDOR.md` | complete reference of the compiler settings and output files |
| `licenses/` | licences of included third-party code (Dobby hooking library) |

## Install

Close Condor first.

**Windows** (in a command prompt opened in the package folder; the path is the folder that contains `Condor.exe`):

    scripts\postfx.bat install "C:\Condor3"

**Mac (CrossOver)** (in Terminal, in the package folder; the path is the Condor folder inside the bottle):

    scripts/postfx.sh install ~/Condor3

Then start Condor as usual. The installer copies `d3dx.dll`, `c3helper.exe`, the `postfx` folder and the shaders, and keeps your own
`lut.cube`, `tune.txt` and `agstream.txt` if they already exist (so updating keeps your settings).

**Without the improved shaders:** add `--no-shaders` (for example `scripts\postfx.bat install "C:\Condor3" --no-shaders`) to install
only the mod and the picture effects. The improved terrain/sky/cloud shaders are then neither needed nor copied, and the game keeps its
own. Shaders from an earlier install stay in place until you uninstall.

**Update:** install the new package over the old one the same way.
**Uninstall:** `scripts\postfx.bat uninstall "C:\Condor3"` / `scripts/postfx.sh uninstall ~/Condor3`. This puts the original
`d3dx.dll` back and removes `postfx`, `Effects\fx` and `c3helper.exe` (your settings in `postfx` are deleted too).

## Compiling from source (Windows)
1. Install WSL (Windows Subsystem for Linux, e.g. with Ubuntu) and Docker Desktop (with its WSL integration turned on).
2. In the WSL (Ubuntu) terminal, in the repository folder (e.g. `cd /mnt/c/Users/<you>/Desktop/condor3-mod`). Not inside the
   dev container: `build_docker.sh` builds and starts the dev container image (`c3dev`) itself, so it needs Docker from WSL.

       scripts/build_docker.sh                      # builds build/d3dx.dll and build/c3helper.exe
       ln -s /mnt/c/Condor3 ~/Condor3               # once; the shader build expects the game in ~/Condor3
       python3 shaders/build_shaders.py build       # patched game shaders -> shaders/build/Effects/fx

   `build_docker.sh` passes further arguments on to cmake, which is how you switch the optional parts of `d3dx.dll` off
   (`ENABLE_POSTFX`, `ENABLE_AGSTREAM`, `ENABLE_HANGAR_PREVIEW_FIX`, `ENABLE_MOVE_PATCHES`; see `CMakeLists.txt` for the defaults):

       scripts/build_docker.sh Release -DENABLE_POSTFX=OFF -DENABLE_AGSTREAM=OFF
       scripts/build_docker.sh -DENABLE_POSTFX=OFF          # build type left out = Release

   cmake remembers these values in `build/`, so to go back to a default pass it explicitly (`-DENABLE_POSTFX=ON`). If you do not
   want the improved shaders, skip the `build_shaders.py` line and install with `--no-shaders` (step 3).

   (VS Code with the "Dev Containers" extension and "Reopen in Container" is only for editing with the same tools; build from WSL.)
3. Close Condor and install from a Windows command prompt opened in the repository folder:

       scripts\postfx.bat install "C:\Condor3"

   (add `--no-shaders` if you did not build the shaders)

If something does not work, ask Claude (or another AI assistant) with the error message first, before opening an issue.

## In the game
| key | |
|---|---|
| F9 | picture effects on / off (compare) |
| F8 | reload `postfx.hlsl` after you edited it |
| F10 | capture a frame (for debugging) |
| Ctrl+Shift+F | freeze the glider in place (look around, take screenshots); again to continue flying |
| Ctrl+Shift+D | save the current position into `<Condor>\postfx\positions.txt` |

**Remote control:** the mod can also be driven by text commands (freeze the glider, jump to saved spots, free camera, effect on/off,
statistics), for example for exact comparison screenshots or automated tests: write them into `<Condor>\postfx\cmd.txt`, results go
to `postfx.log`. All commands are documented in [README_POSTFX.md - Remote control](README_POSTFX.md#remote-control).

**Large landscapes:** the 3D buildings appear a little after the flight starts (the helper reads the landscape first: about half a
minute for a whole city, longer the first time after a reboot when the disk is cold). The terrain and photo are there at once.

## Settings (all in `<Condor>\postfx\`, all of them can be edited while the game runs)

**`agstream.txt` - how much 3D detail where** (autogen streaming). Written with explanations on the first start; delete it to get
the defaults back. Distances are metres from the camera.

| setting | default | meaning |
|---|---|---|
| `range` | 15000 | no 3D buildings beyond this distance |
| `L19` | 3000 | the detailed 3D data (export level 19) is shown up to this distance. Higher = sharp buildings farther out, costs memory |
| `L20`, `L18`, `L17` | 1500, 6000, 12000 | the same for other export levels. The coarsest level of an area is always kept out to `range` |
| `blobs` | 15000 | simplified "blob" versions are only used beyond this distance (where real coarser data exists) |
| `full`, `blob1`, `blob2` | 2000, 5000, 12000 | for landscapes made with the old compiler (no coarser levels): full detail, then simplified versions |
| `budget` | 3 | milliseconds per frame the mod may spend on loading. Lower = smoother, buildings appear a bit slower |
| `enable` | 1 | 0 = Condor loads the 3D buildings itself as without the mod (needs a restart; big landscapes may then not fit in memory) |

**`tune.txt` - the picture.** One `name value` per line; the names and their defaults are the `//@tune` lines at the top of
`postfx.hlsl` (for example `lut_strength 0.75`, `warmth`, `terrain_contrast`, `contact_shadow`). Delete a line to go back to the default.

**`lut.cube` - the colour look.** Replace it with any `.cube` file you like; `lut_strength 0` in `tune.txt` turns it off.

**Logs** (look here when something is wrong): `postfx.log` (mod), `agstream.log` (autogen streaming, including frames that took
longer than 30 ms and why), `c3helper.log` (the helper loading the landscape).

## Making a landscape

1. **Export** the area with earth-rea as "3D Tiles - one glb per file". Several exports can be combined (earth-rea limits one export
   to 5000 tiles), and they may mix detail levels: everywhere the finest one is used. A good mix: level 17 for the surroundings,
   level 19 for the area you fly in, level 20 for landmarks.
2. **Settings:** copy `examples/orly_custom_runway.json` and adapt it (airports with their runway ends, texture size, margin around the data).
   The full list is in `docs/TILES2CONDOR.md`.
3. **Compile** into Condor's `Landscapes` folder; the last part of the path becomes the landscape name:

       bin/tiles2condor_v2 <export folder> [<more export folders> ...] my_settings.json "<Condor>/Landscapes/MyCity"

   On Windows: `bin\tiles2condor_v2.exe ...` with Windows paths.
4. Start Condor, choose the landscape and an airport in the flight planner. Install the mod for large landscapes (see above),
   otherwise Condor may run out of memory with many 3D buildings.

**Runways:** by default an airport's runway is the satellite photo. Add `"runway_look": "custom"` to an airport in your settings for a
sharp, high-resolution runway with markings instead (asphalt, `"surface": "concrete"` or `"surface": "grass"`); see
`examples/orly_custom_runway.json` and `docs/TILES2CONDOR.md`. The textures for it are in `assets/runway/` (replace them with your
own of the same names if you like). Rebuild only the airports with `--only airports` (takes seconds).

Useful options:
- `--only plan` takes seconds and shows what will be built: which detail levels are used where, texture sizes, runways
  (picture `plan_levels.png` in the work folder). Do this first for a new area.
- `--budget SECONDS` stops cleanly after that time; run the same command again to continue where it stopped (also after an interruption).
- `--work DIR` puts the temporary work folder elsewhere (default: `_condor_work_v2` inside the first export folder; it can be deleted
  after a successful build).
- `--threads N` uses fewer CPU cores, `--cpu` renders without the GPU (slower; for machines without a suitable GPU).
- Building again into an existing landscape keeps files that are already there: use a new name, or delete the old landscape folder.

With mixed-level exports the compiler also writes coarser versions of the 3D buildings (`Autogen/oXXYY_L17.c3d` ... and
`Autogen/lods.txt`). The mod uses them for the distance, so far-away buildings look like the real thing instead of blobs.

**Mac:** if the Mac says the compiler "cannot be opened" (downloaded program), run `xattr -d com.apple.quarantine bin/tiles2condor_v2` once.

## Troubleshooting
- **No 3D buildings at all on a big landscape:** wait a minute, then look at `agstream.log` / `c3helper.log`. If `c3helper.exe` is missing
  next to `Condor.exe`, install again.
- **Condor crashes at start or the mod does nothing:** check that Condor is version 3.1.0; otherwise uninstall.
- **Stutters:** `agstream.log` lists every frame over 30 ms. Lines with "streaming 0.0 ms" are Condor itself, not the mod.
  Lower `budget` or `L19` in `agstream.txt` if the mod is the cause.
- **Buildings blurry in the distance:** the landscape was probably made with an old compiler or from an export without coarser
  levels; rebuild it with `tiles2condor_v2` from a mixed-level export. Raise `L19` for sharper buildings farther out.

## Legal notice and disclaimer

- **Research project.** This software is an independent, non-commercial research and educational project about game engine
  interoperability, rendering and 3D data processing. It is published in the hope that it is useful, without any promise that it is.
- **No warranty.** The software is provided "as is", without warranty of any kind, express or implied, including but not limited to
  the warranties of merchantability, fitness for a particular purpose, accuracy, compatibility and non-infringement. You use it
  entirely at your own risk. Back up your Condor installation and your data before installing.
- **No liability.** To the maximum extent permitted by applicable law, the author(s) and contributors shall not be liable for any
  claim, damage or other liability, whether direct, indirect, incidental, special, consequential or exemplary (including but not
  limited to loss of data, damage to software or hardware, loss of profits, account suspensions or legal costs), arising from, out of
  or in connection with the software, its use, its misuse or the inability to use it, even if advised of the possibility of such damage.
- **Your responsibility.** You alone are responsible for making sure that your use of this software is lawful where you live and
  complies with every licence and terms of service that applies to you, in particular:
  - the end-user licence agreement and terms of use of Condor 3 (this software modifies how the game runs and replaces one of its
    files; whether that is allowed for you is your decision and your responsibility);
  - the terms of service, licences and attribution requirements of any map, imagery or 3D-tiles data you export, convert or use
    (they may restrict exporting, caching, offline use, modification and redistribution of the content), and the copyrights of the
    imagery providers;
  - any laws on copyright, databases, privacy, aviation and the use of geographic data that apply to you.
- **No data included, no redistribution rights granted.** This package contains no map, imagery or 3D-tiles data. Landscapes you
  create may contain third-party content; this software grants you no rights to that content. Do not share or publish such
  landscapes unless you hold the necessary rights.
- **Not affiliated.** This project is not affiliated with, endorsed by or supported by the developers or publishers of Condor or
  by any data provider. All product names, trademarks and registered trademarks are the property of their respective
  owners and are used for identification only. Do not contact the Condor developers for support with this software.
- **Third-party components** are subject to their own licences (see `licenses/` and `postfx/default_lut/ATTRIBUTION.txt`).
- If any part of this notice is held invalid or unenforceable, the remaining parts stay in effect. If you do not agree with this
  notice, do not install or use the software.
