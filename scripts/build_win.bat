@echo off
REM ============================================================================
REM  reaper_ambix - Windows build, codesign and installer pipeline.
REM
REM  Usage:  scripts\build_win.bat [--no-sign]
REM
REM  Prereqs (all optional but the obvious ones are required for a real build):
REM    - Visual Studio 2022 (with "Desktop development with C++" workload)
REM    - CMake 3.20+ on PATH
REM    - Inno Setup 6 ("ISCC.exe" on PATH or in default Program Files dir)
REM    - signtool.exe (Windows SDK) for codesigning
REM
REM  Output:
REM    _WIN_RELEASE\reaper_ambix_vX.Y.Z_win64_setup.exe -- signed installer
REM    that drops:
REM      %APPDATA%\REAPER\UserPlugins\reaper_ambix.dll
REM    (statically linked: libambix + WavPack + WDL — no external deps)
REM ============================================================================

setlocal EnableExtensions EnableDelayedExpansion

cd /d "%~dp0\.."
set ROOT=%CD%
set BUILD_DIR=%ROOT%\_build_win
set STAGE_DIR=%BUILD_DIR%\stage
set RELEASE_DIR=%ROOT%\_WIN_RELEASE

REM -- read VERSION
set /p VERSION=<"%ROOT%\VERSION"
echo === reaper_ambix v%VERSION% - Windows installer build ===

REM -- parse args
set SKIP_SIGN=0
:parse_args
if "%~1"=="" goto args_done
if /I "%~1"=="--no-sign" (set SKIP_SIGN=1) else (
    echo Unknown option: %~1
    echo Usage: %~nx0 [--no-sign]
    exit /b 1
)
shift
goto parse_args
:args_done

REM -- load credentials env (cmd-style) by re-reading the bash one
if "%SKIP_SIGN%"=="1" goto skip_load_creds
if not exist "%ROOT%\scripts\codesign.env" (
    echo Error: scripts\codesign.env not found.
    exit /b 1
)
for /f "usebackq tokens=1,* delims==" %%A in ("%ROOT%\scripts\codesign.env") do (
    set "_K=%%A"
    set "_V=%%B"
    if defined _V set "_V=!_V:"=!"
    set "_K=!_K: =!"
    if /I "!_K!"=="WINDOWS_CODESIGN_SUBJECT" set "WINDOWS_CODESIGN_SUBJECT=!_V!"
    if /I "!_K!"=="WINDOWS_CODESIGN_PFX" set "WINDOWS_CODESIGN_PFX=!_V!"
    if /I "!_K!"=="WINDOWS_CODESIGN_PFX_PASSWORD" set "WINDOWS_CODESIGN_PFX_PASSWORD=!_V!"
    if /I "!_K!"=="WINDOWS_TIMESTAMP_URL" set "WINDOWS_TIMESTAMP_URL=!_V!"
)
if "!WINDOWS_TIMESTAMP_URL!"=="" set "WINDOWS_TIMESTAMP_URL=http://timestamp.digicert.com"
:skip_load_creds

REM -- clean
if exist "%BUILD_DIR%" rmdir /S /Q "%BUILD_DIR%"
mkdir "%BUILD_DIR%"
mkdir "%STAGE_DIR%"
if not exist "%RELEASE_DIR%" mkdir "%RELEASE_DIR%"

REM ============================================================================
REM Configure + build
REM ============================================================================
echo.
echo === Configuring (Visual Studio 2022 x64) ===
REM No vcpkg / external toolchain needed: libambix and WavPack are vendored
REM and statically linked. (We deliberately ignore VCPKG_ROOT — VS 2022 sets it
REM to a Program Files path whose embedded space breaks unquoted command-line
REM expansion.)
cmake -S "%ROOT%" -B "%BUILD_DIR%" ^
      -G "Visual Studio 17 2022" -A x64 ^
      -DCMAKE_BUILD_TYPE=Release ^
      -DREAPER_AMBIX_INSTALL_USER_PLUGINS=OFF
if errorlevel 1 exit /b 1

echo.
echo === Building (Release) ===
cmake --build "%BUILD_DIR%" --config Release -j
if errorlevel 1 exit /b 1

REM Locate the built reaper_ambix.dll - CMake on MSVC writes to BUILD\Release\
set PLUGIN_BUILT=%BUILD_DIR%\Release\reaper_ambix.dll
if not exist "%PLUGIN_BUILT%" (
    REM fall back to a recursive find
    for /r "%BUILD_DIR%" %%F in (reaper_ambix.dll) do set PLUGIN_BUILT=%%F
)
if not exist "%PLUGIN_BUILT%" (
    echo Error: build did not produce reaper_ambix.dll
    exit /b 1
)
echo Built plugin: %PLUGIN_BUILT%

REM ============================================================================
REM Stage payload
REM ============================================================================
set INSTALL_PARENT=%STAGE_DIR%\REAPER\UserPlugins
mkdir "%INSTALL_PARENT%"

copy /Y "%PLUGIN_BUILT%" "%INSTALL_PARENT%\reaper_ambix.dll" >nul

REM ============================================================================
REM Codesign
REM ============================================================================
if "%SKIP_SIGN%"=="1" goto skip_codesign_dlls
echo.
echo === Codesigning DLLs ===
set SIGN_ARGS=/fd SHA256 /td SHA256 /tr "!WINDOWS_TIMESTAMP_URL!"
if not "!WINDOWS_CODESIGN_PFX!"=="" goto sign_with_pfx
set SIGN_ARGS=!SIGN_ARGS! /a /n "!WINDOWS_CODESIGN_SUBJECT!"
goto sign_dlls
:sign_with_pfx
set SIGN_ARGS=!SIGN_ARGS! /f "!WINDOWS_CODESIGN_PFX!"
if not "!WINDOWS_CODESIGN_PFX_PASSWORD!"=="" set SIGN_ARGS=!SIGN_ARGS! /p "!WINDOWS_CODESIGN_PFX_PASSWORD!"
:sign_dlls
for %%F in ("%INSTALL_PARENT%\reaper_ambix.dll") do (
    echo signing %%F
    signtool sign !SIGN_ARGS! "%%~F"
    if errorlevel 1 (
        echo Error: signtool failed on %%F
        exit /b 1
    )
)
:skip_codesign_dlls

REM ============================================================================
REM Inno Setup compile
REM ============================================================================
set ISS=%ROOT%\scripts\installer\reaper_ambix.iss
if not exist "%ISS%" (
    echo Error: %ISS% not found
    exit /b 1
)

REM Find ISCC.exe
set "ISCC="
if exist "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" set "ISCC=C:\Program Files (x86)\Inno Setup 6\ISCC.exe"
if not defined ISCC if exist "C:\Program Files\Inno Setup 6\ISCC.exe" set "ISCC=C:\Program Files\Inno Setup 6\ISCC.exe"
if defined ISCC goto iscc_found
where iscc >nul 2>nul
if not errorlevel 1 set "ISCC=iscc"
if defined ISCC goto iscc_found
echo Error: ISCC.exe not found. Install Inno Setup 6.
exit /b 1
:iscc_found

echo.
echo === Compiling installer (Inno Setup) ===
"%ISCC%" /Qp ^
    /DReaperAmbixVersion=%VERSION% ^
    /DReaperAmbixStageDir=%INSTALL_PARENT% ^
    /DReaperAmbixOutputDir=%RELEASE_DIR% ^
    "%ISS%"
if errorlevel 1 exit /b 1

set INSTALLER=%RELEASE_DIR%\reaper_ambix_v%VERSION%_win64_setup.exe

REM ============================================================================
REM Codesign installer itself
REM ============================================================================
if "%SKIP_SIGN%"=="1" goto skip_codesign_installer
if not exist "%INSTALLER%" goto skip_codesign_installer
echo.
echo === Codesigning installer ===
signtool sign !SIGN_ARGS! "%INSTALLER%"
if errorlevel 1 exit /b 1
signtool verify /pa "%INSTALLER%"
:skip_codesign_installer

echo.
echo Done!
dir "%RELEASE_DIR%\*.exe"
endlocal
