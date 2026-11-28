# Condor 3 on macOS (CrossOver)

macOS is supported on a **best-effort** basis; Windows is the first-class platform. Condor runs in a CrossOver bottle, and the
bottle needs a few extra pieces depending on your Mac.

The mod itself is useful here even without the picture effects: under Wine (at least on the Mac) the 3D aircraft preview of the
flight planner (Hangar -> 3D view) is otherwise drawn over the other menu tabs. The mod fixes that menu glitch.

> **Tip:** Claude (or another AI assistant) is very good at helping with this setup. If a step fails, paste the command and the
> error message and ask.

## Intel Macs
Use this custom DXVK, [trueToastedCode/DXVK-macOS](https://github.com/trueToastedCode/DXVK-macOS), together with MoltenVK.

## Apple Silicon
You need [DXMT](https://github.com/3Shain/dxmt) (Direct3D 11 on Metal) and [RosettaHack x87 + JIT](https://github.com/Lifeisawful/rosettax87_jit)
(fast x87 floating point under Rosetta, which Condor needs).

The commands below assume CrossOver is in `/Applications/CrossOver.app`, the bottle is called `Condor3` and the downloads are in
the current folder. Adapt the paths if yours differ.

**Downloads and macOS blocking:** files downloaded with a browser get a quarantine flag, and macOS will then block the unsigned
binaries ("cannot be opened"). Either download with `curl -LO <url>` in Terminal (no flag is set), or remove the flag from the
unpacked folder once: `xattr -dr com.apple.quarantine <folder>`.

### 1. CrossOver and the Condor bottle
1. Install **CrossOver 26.3.0 exactly** (the patched `ntdll.so` in step 2 is made for that version):
   [codeweavers.com/crossover/download-now](https://www.codeweavers.com/crossover/download-now).
2. Create a bottle (named `Condor3`) and install Condor into it, best at `~/Condor3`. When asked for the program, choose
   `Condor.exe`; close Condor when it opens.

### 2. Rosetta x87 + JIT
1. Download and unpack the precompiled build:
   [rosettax87-jit_e858b62.zip](https://github.com/trueToastedCode/rosettax87_jit/releases/download/initial/rosettax87-jit_e858b62.zip).
   Put the folder somewhere permanent (e.g. `~/rosettax87`); the file you need is `runtime_loader`.
2. Download the patched `ntdll.so` (needed so the Rosetta environment works inside CrossOver):
   [patched_ntdll-crossover-26.3.0.zip](https://github.com/trueToastedCode/rosettax87_jit/releases/download/initial/patched_ntdll-crossover-26.3.0.zip).
3. Quit CrossOver, then install it (in the folder with the unpacked `ntdll.so`):

       codesign --remove-signature /Applications/CrossOver.app
       codesign --remove-signature /Applications/CrossOver.app/Contents/SharedSupport/CrossOver/lib/wine/x86_64-unix/wine
       cp ./ntdll.so /Applications/CrossOver.app/Contents/SharedSupport/CrossOver/lib/wine/x86_64-unix/ntdll.so
       sudo xattr -cr /Applications/CrossOver.app

4. **Sign CrossOver again, locally.** With its signature removed, CrossOver no longer starts at all, and the patched `ntdll.so`
   has changed the app, so it needs at least a local (ad-hoc) signature:

       sudo codesign --force --deep --sign - /Applications/CrossOver.app

5. Point the bottle at the loader: open `~/Library/Application Support/CrossOver/Bottles/Condor3/cxbottle.conf` and add at the
   bottom of the `[EnvironmentVariables]` section (use your own path):

       "ROSETTA_X87_PATH" = "/Users/<you>/rosettax87/runtime_loader"

6. **Allow `runtime_loader` in the privacy settings.** macOS blocks it the first time it is used. Open **System Settings ->
   Privacy & Security** and allow `runtime_loader` there (scroll down to the security
   section and click *Allow Anyway* after the block message appeared). Without this, Condor does not start in the bottle.

### 3. DXMT 0.80 (Condor 3 build)
Use our DXMT build for Condor 3, not the upstream one: download the zip from the release
[v0.80-condor3-r1](https://github.com/trueToastedCode/dxmt/releases/tag/v0.80-condor3-r1) (under *Assets*).

Unpack it and run the included script (it backs up every file it replaces):

    ./winemetal.sh install --crossover /Applications/CrossOver.app --bottle Condor3
    ./winemetal.sh restore      # undo

`--dry-run` shows what it would do. Manual install steps are in the zip's `README.txt`.

### 4. The mod
Start Condor once to check it runs, close it, then install the mod as described in [README.md](README.md#install):

    scripts/postfx.sh install ~/Condor3
