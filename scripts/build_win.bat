@echo off
REM ============================================================================
REM  reaper_ambix - Windows build, codesign and installer pipeline.
REM
REM  Usage:  scripts\build_win.bat [--no-sign]
REM
REM  Prereqs (all optional but the obvious ones are required for a real build):
REM    - Visual Studio 2022 (with "Desktop development with C++" workload)
REM    - CMake 3.20+ on PATH
REM    - libsndfile.dll on PATH or in one of the common install locations
REM      (vcpkg installed/x64-windows/bin, Mega-Nerd, ...). Override with
REM      LIBSNDFILE_DLL env var to a full path if auto-detection misses it.
REM    - Inno Setup 6 ("ISCC.exe" on PATH or in default Program Files dir)
REM    - signtool.exe (Windows SDK) for codesigning
REM
REM  Output:
REM    _WIN_RELEASE\reaper_ambix_vX.Y.Z_win64_setup.exe -- signed installer
REM    that drops:
REM      %APPDATA%\REAPER\UserPlugins\reaper_ambix.dll
REM      %APPDATA%\REAPER\UserPlugins\reaper_ambix-libs\libsndfile.dll
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
if "%SKIP_SIGN%"=="0" (
    if not exist "%ROOT%\scripts\codesign.env" (
        echo Error: scripts\codesign.env not found.
        exit /b 1
    )
    for /f "tokens=1,* delims==" %%A in (%ROOT%\scripts\codesign.env) do (
        set "_K=%%A"
        set "_V=%%B"
        REM strip surrounding quotes from value
        if defined _V (
            set "_V=!_V:"=!"
        )
        set "_K=!_K: =!"
        REM only set the WINDOWS_* keys we care about
        if /I "!_K!"=="WINDOWS_CODESIGN_SUBJECT" set "WINDOWS_CODESIGN_SUBJECT=!_V!"
        if /I "!_K!"=="WINDOWS_CODESIGN_PFX" set "WINDOWS_CODESIGN_PFX=!_V!"
        if /I "!_K!"=="WINDOWS_CODESIGN_PFX_PASSWORD" set "WINDOWS_CODESIGN_PFX_PASSWORD=!_V!"
        if /I "!_K!"=="WINDOWS_TIMESTAMP_URL" set "WINDOWS_TIMESTAMP_URL=!_V!"
    )
    if "!WINDOWS_TIMESTAMP_URL!"=="" set "WINDOWS_TIMESTAMP_URL=http://timestamp.digicert.com"
)

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
set "CMAKE_TOOLCHAIN_ARGS="
if defined VCPKG_ROOT if exist "%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" (
    set "CMAKE_TOOLCHAIN_ARGS=-DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows"
)
cmake -S "%ROOT%" -B "%BUILD_DIR%" ^
      -G "Visual Studio 17 2022" -A x64 ^
      -DCMAKE_BUILD_TYPE=Release ^
      -DREAPER_AMBIX_INSTALL_USER_PLUGINS=OFF ^
      %CMAKE_TOOLCHAIN_ARGS%
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
REM Locate libsndfile.dll for bundling
REM ============================================================================
set "LIBSNDFILE_FOUND="
if defined LIBSNDFILE_DLL if exist "%LIBSNDFILE_DLL%" set "LIBSNDFILE_FOUND=%LIBSNDFILE_DLL%"

if not defined LIBSNDFILE_FOUND (
    for %%P in (
        "%VCPKG_ROOT%\installed\x64-windows\bin\sndfile.dll"
        "%VCPKG_ROOT%\installed\x64-windows\bin\libsndfile-1.dll"
        "C:\vcpkg\installed\x64-windows\bin\sndfile.dll"
        "C:\vcpkg\installed\x64-windows\bin\libsndfile-1.dll"
        "C:\Program Files\Mega-Nerd\libsndfile\bin\libsndfile-1.dll"
    ) do (
        if exist %%P set "LIBSNDFILE_FOUND=%%~P"
    )
)

if not defined LIBSNDFILE_FOUND (
    echo Error: libsndfile.dll not found. Set LIBSNDFILE_DLL=full\path\to\libsndfile.dll
    exit /b 1
)
echo libsndfile.dll: %LIBSNDFILE_FOUND%

REM ============================================================================
REM Stage payload
REM ============================================================================
set INSTALL_PARENT=%STAGE_DIR%\REAPER\UserPlugins
set LIBS_SUBDIR=reaper_ambix-libs
mkdir "%INSTALL_PARENT%\%LIBS_SUBDIR%"

copy /Y "%PLUGIN_BUILT%" "%INSTALL_PARENT%\reaper_ambix.dll" >nul
copy /Y "%LIBSNDFILE_FOUND%" "%INSTALL_PARENT%\%LIBS_SUBDIR%\" >nul

REM ============================================================================
REM Codesign
REM ============================================================================
if "%SKIP_SIGN%"=="0" (
    echo.
    echo === Codesigning DLLs ===
    set SIGN_ARGS=/fd SHA256 /td SHA256 /tr "!WINDOWS_TIMESTAMP_URL!"
    if not "!WINDOWS_CODESIGN_PFX!"=="" (
        set SIGN_ARGS=!SIGN_ARGS! /f "!WINDOWS_CODESIGN_PFX!"
        if not "!WINDOWS_CODESIGN_PFX_PASSWORD!"=="" (
            set SIGN_ARGS=!SIGN_ARGS! /p "!WINDOWS_CODESIGN_PFX_PASSWORD!"
        )
    ) else (
        set SIGN_ARGS=!SIGN_ARGS! /a /n "!WINDOWS_CODESIGN_SUBJECT!"
    )
    for %%F in ("%INSTALL_PARENT%\reaper_ambix.dll" "%INSTALL_PARENT%\%LIBS_SUBDIR%\*.dll") do (
        echo signing %%F
        signtool sign !SIGN_ARGS! "%%~F"
        if errorlevel 1 (
            echo Error: signtool failed on %%F
            exit /b 1
        )
    )
)

REM ============================================================================
REM Inno Setup compile
REM ============================================================================
set ISS=%ROOT%\scripts\installer\reaper_ambix.iss
if not exist "%ISS%" (
    echo Error: %ISS% not found
    exit /b 1
)

REM Find ISCC.exe
set ISCC=
for %%P in (
    "C:\Program Files (x86)\Inno Setup 6\ISCC.exe"
    "C:\Program Files\Inno Setup 6\ISCC.exe"
) do (
    if exist %%P set "ISCC=%%~P"
)
where iscc >nul 2>nul && set "ISCC=iscc"

if not defined ISCC (
    echo Error: ISCC.exe (Inno Setup) not found. Install Inno Setup 6.
    exit /b 1
)

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
if "%SKIP_SIGN%"=="0" if exist "%INSTALLER%" (
    echo.
    echo === Codesigning installer ===
    signtool sign !SIGN_ARGS! "%INSTALLER%"
    if errorlevel 1 exit /b 1
    signtool verify /pa "%INSTALLER%"
)

echo.
echo Done!
dir "%RELEASE_DIR%\*.exe"
endlocal
