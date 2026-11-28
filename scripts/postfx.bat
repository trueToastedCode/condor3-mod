@echo off
rem Installs / removes the Condor 3 post-processing mod on Windows (the equivalent of scripts/postfx.sh).
rem
rem   scripts\postfx.bat install   "C:\path\to\Condor3"
rem   scripts\postfx.bat uninstall "C:\path\to\Condor3"
rem   scripts\postfx.bat sync      "C:\path\to\Condor3"     (copies only postfx.hlsl; the running game hot-reloads it)
rem
rem The second argument is the folder that contains Condor.exe. If it is left out, the environment variable
rem CONDOR_DIR is used.
rem
rem Flag (anywhere in the arguments):
rem   --no-shaders   install without the patched game shaders: they are not needed (and not copied, even if built);
rem                  the game keeps its own shaders. Shaders from an earlier install are left in place (uninstall removes them).
rem
rem What install does:
rem   1. copies the game's own d3dx.dll to real_d3dx.dll (only if real_d3dx.dll does not exist yet).
rem      The mod forwards all d3dx calls to real_d3dx.dll, so it must be the original DLL.
rem   2. copies build\d3dx.dll (the prebuilt mod) over d3dx.dll
rem   3. creates <game>\postfx with postfx.hlsl and the colour LUT (postfx\user\lut.cube, else the default one)
rem   4. copies the patched shaders from shaders\build\Effects\fx into <game>\Effects\fx (not with --no-shaders)
rem   5. copies build\c3helper.exe (64-bit autogen helper, built by scripts/build_docker.sh from external/condor3-helper) next to Condor.exe, if present
rem
rem Needs the same Condor version the mod was made for (3.1.0): the mod patches fixed addresses in Condor.exe.
rem Keys in the game: F9 = effect on/off, F8 = reload shader, F10 = capture frame.
rem Log: <game>\postfx\postfx.log

setlocal
set "ROOT=%~dp0.."
set "CMD="
set "GAME="
set "NOSHADERS="
:parse
if "%~1"=="" goto parsed
if /i "%~1"=="--no-shaders" (
  set "NOSHADERS=1"
) else if not defined CMD (
  set "CMD=%~1"
) else if not defined GAME (
  set "GAME=%~1"
)
shift
goto parse
:parsed
if "%GAME%"=="" set "GAME=%CONDOR_DIR%"
if "%CMD%"=="" goto usage
if "%GAME%"=="" goto usage
if not exist "%GAME%\Condor.exe" (
  echo "%GAME%" does not contain Condor.exe - pass the Condor 3 folder as the second argument.
  exit /b 1
)
set "REAL=%GAME%\real_d3dx.dll"
rem backup name used by older versions of this script
set "OLDBAK=%GAME%\d3dx.dll.before_postfx"

if /i "%CMD%"=="install" goto install
if /i "%CMD%"=="uninstall" goto uninstall
if /i "%CMD%"=="sync" goto sync
goto usage

:install
if not exist "%ROOT%\build\d3dx.dll" (
  echo build\d3dx.dll is missing - copy the whole condor3-mod folder including build\.
  exit /b 1
)
tasklist /fi "imagename eq Condor.exe" 2>nul | find /i "Condor.exe" >nul
if not errorlevel 1 (
  echo Condor.exe is running - close it first, then run install again.
  exit /b 1
)
if not exist "%REAL%" (
  if exist "%OLDBAK%" (
    copy /y "%OLDBAK%" "%REAL%" >nul || exit /b 1
  ) else (
    if not exist "%GAME%\d3dx.dll" (
      echo "%GAME%\d3dx.dll" is missing - reinstall Condor or restore d3dx.dll first.
      exit /b 1
    )
    fc /b "%GAME%\d3dx.dll" "%ROOT%\build\d3dx.dll" >nul 2>&1 && (
      echo "%GAME%\d3dx.dll" is already the mod and real_d3dx.dll is missing - restore the original d3dx.dll first.
      exit /b 1
    )
    copy /y "%GAME%\d3dx.dll" "%REAL%" >nul || exit /b 1
  )
)
if exist "%OLDBAK%" del /q "%OLDBAK%"
copy /y "%ROOT%\build\d3dx.dll" "%GAME%\d3dx.dll" >nul || exit /b 1
if not exist "%GAME%\postfx" mkdir "%GAME%\postfx"
copy /y "%ROOT%\postfx\postfx.hlsl" "%GAME%\postfx\postfx.hlsl" >nul || exit /b 1
if not exist "%GAME%\postfx\lut.cube" (
  if exist "%ROOT%\postfx\user\lut.cube" (
    copy /y "%ROOT%\postfx\user\lut.cube" "%GAME%\postfx\lut.cube" >nul
  ) else (
    copy /y "%ROOT%\postfx\default_lut\lut.cube" "%GAME%\postfx\lut.cube" >nul
  )
)
if exist "%ROOT%\postfx\user\tune.txt" if not exist "%GAME%\postfx\tune.txt" copy /y "%ROOT%\postfx\user\tune.txt" "%GAME%\postfx\tune.txt" >nul
if defined NOSHADERS (
  echo Shaders: skipped ^(--no-shaders^), the game's own shaders stay
  goto shadersdone
)
if not exist "%ROOT%\shaders\build\Effects\fx" (
  echo shaders\build\Effects\fx is missing - the shader patches were not copied. ^(Use --no-shaders to install without them.^)
  exit /b 1
)
if not exist "%GAME%\Effects\fx" mkdir "%GAME%\Effects\fx"
copy /y "%ROOT%\shaders\build\Effects\fx\*.fx" "%GAME%\Effects\fx\" >nul || exit /b 1
:shadersdone
if exist "%ROOT%\build\c3helper.exe" (
  copy /y "%ROOT%\build\c3helper.exe" "%GAME%\c3helper.exe" >nul || exit /b 1
  echo Autogen streaming: c3helper.exe installed ^(log %GAME%\postfx\agstream.log^)
) else echo Note: build\c3helper.exe not found - autogen streaming stays off, the game loads autogen itself.
echo Installed. Start Condor normally.  F9 = effect on/off, F8 = reload shader, F10 = capture frame.
echo Log: %GAME%\postfx\postfx.log
echo Remove again: scripts\postfx.bat uninstall "%GAME%"
exit /b 0

:uninstall
tasklist /fi "imagename eq Condor.exe" 2>nul | find /i "Condor.exe" >nul
if not errorlevel 1 (
  echo Condor.exe is running - close it first, then run uninstall again.
  exit /b 1
)
if exist "%REAL%" (
  move /y "%REAL%" "%GAME%\d3dx.dll" >nul || exit /b 1
) else if exist "%OLDBAK%" (
  move /y "%OLDBAK%" "%GAME%\d3dx.dll" >nul || exit /b 1
) else (
  echo Warning: real_d3dx.dll not found - d3dx.dll was left unchanged.
)
if exist "%OLDBAK%" del /q "%OLDBAK%"
if exist "%GAME%\postfx" rmdir /s /q "%GAME%\postfx"
rem the game's own .fxc shaders are never modified; the mod only adds Effects\fx
if exist "%GAME%\Effects\fx" rmdir /s /q "%GAME%\Effects\fx"
if exist "%GAME%\c3helper.exe" del /q "%GAME%\c3helper.exe"
echo Restored the previous d3dx.dll, removed %GAME%\postfx, %GAME%\Effects\fx and c3helper.exe
exit /b 0

:sync
copy /y "%ROOT%\postfx\postfx.hlsl" "%GAME%\postfx\postfx.hlsl" >nul && echo postfx.hlsl copied
exit /b 0

:usage
echo usage: %~nx0 install^|uninstall^|sync "C:\path\to\Condor3" [--no-shaders]
echo        (or set CONDOR_DIR and leave the folder out)
exit /b 2
