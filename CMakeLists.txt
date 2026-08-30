# 3.13 for target_link_options() (static libstdc++ on Linux).
cmake_minimum_required(VERSION 3.13)

# Read VERSION file at the repo root
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" REAPER_AMBIX_VERSION LIMIT_COUNT 1)
string(STRIP "${REAPER_AMBIX_VERSION}" REAPER_AMBIX_VERSION)

project (reaper_ambix VERSION ${REAPER_AMBIX_VERSION})

# Build mode:
#  - OFF (default): leave the plugin in the build tree, so build_osx.sh /
#                   build_win.bat / build_linux.sh can stage + sign + package
#                   it, and so a stray build never ends up somewhere REAPER
#                   loads from.
#  - ON  (dev): drop the plugin straight into the user's REAPER plugin folder
#               so a simple `make` reloads it in REAPER. scripts/setup.sh
#               configures the dev tree with this on.
#
# Default OFF on purpose. REAPER scans both the user plugin folder and the
# system one, and the user copy wins on identical filenames — so a dev build
# left behind silently shadows anything the installer or ReaPack puts in the
# system folder, and you keep running the old binary with no warning.
option(REAPER_AMBIX_INSTALL_USER_PLUGINS
       "Copy the built plugin into the user's REAPER UserPlugins folder"
       OFF)

# Unit tests for the loudness measurement (tests/test_loudness.cpp). Off by
# default so installer/CI plugin builds stay lean; the release workflow turns
# it on in a dedicated job.
option(REAPER_AMBIX_BUILD_TESTS "Build the loudness test suite" OFF)

# The plugin itself needs the WDL / WavPack / libambix submodules. The loudness
# tests need none of them, so a test-only build can skip the whole plugin (and
# therefore the submodule checkout) by turning this off.
option(REAPER_AMBIX_BUILD_PLUGIN "Build the REAPER extension" ON)

# Default to MinSizeRel, but honor a caller-provided type (the build scripts
# pass -DCMAKE_BUILD_TYPE=Release). Skipped for multi-config generators
# (Visual Studio/Xcode), which pick the config at build time via --config.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE MinSizeRel CACHE STRING "Build type" FORCE)
endif()

# Statically link the MSVC runtime so the plugin does not depend on the VC++
# redistributable. CMP0091 (CMake >= 3.15) selects the runtime via an
# abstraction that also propagates into the vendored WavPack/libambix builds.
if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# The static libambix/WavPack archives get linked into our shared plugin, so
# they must be position-independent (required on Linux/ELF, no-op elsewhere).
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

IF(APPLE)
   # Universal binary so one .dylib covers Apple Silicon and Intel REAPER.
   SET( CMAKE_OSX_ARCHITECTURES "arm64;x86_64" )
   SET( CMAKE_OSX_DEPLOYMENT_TARGET 10.13 )
   SET ( LINK_FLAGS MINSIZEREL "-dead_strip" )
	
	FIND_LIBRARY(CARBON_LIBRARY Carbon)
	FIND_LIBRARY(COCOA_LIBRARY Cocoa)
    	
	SET(CMAKE_MACOSX_RPATH 1)
ENDIF (APPLE)

################################################
# Tests — the loudness measurement is pure C++ with no REAPER dependency, so it
# can be exercised standalone:
#     cmake -S . -B build -DREAPER_AMBIX_BUILD_TESTS=ON -DREAPER_AMBIX_BUILD_PLUGIN=OFF
#     cmake --build build && ctest --test-dir build --output-on-failure
#
# Defined before any plugin-specific directory properties (swell prefix, WDL
# include path) so the test target does not inherit them.
if(REAPER_AMBIX_BUILD_TESTS)
    enable_testing()

    add_executable(test_loudness
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_loudness.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/loudness.cpp)
    target_include_directories(test_loudness PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    set_target_properties(test_loudness PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON)

    if(NOT MSVC)
        target_compile_options(test_loudness PRIVATE -fsigned-char)
    else()
        target_compile_definitions(test_loudness PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()

    add_test(NAME loudness COMMAND test_loudness)
endif()

################################################
# Everything below builds the extension itself and needs the vendored
# submodules. A test-only configure stops here, so `git clone` without
# --recursive is enough to run the test suite.
if(NOT REAPER_AMBIX_BUILD_PLUGIN)
    return()
endif()

# set unique name for swell functions
ADD_DEFINITIONS(-DSWELL_APP_PREFIX=SWELL_AMBIX)

################################################
# INCLUDES
include_directories (BEFORE ${CMAKE_CURRENT_SOURCE_DIR}/WDL/WDL)
include_directories (BEFORE ${CMAKE_CURRENT_SOURCE_DIR}/src)

################################################
# vendored WavPack (lossless compression) - static, minimal
set(WAVPACK_BUILD_PROGRAMS           OFF CACHE BOOL "" FORCE)
set(WAVPACK_ENABLE_LEGACY            OFF CACHE BOOL "" FORCE)
set(WAVPACK_ENABLE_DSD               OFF CACHE BOOL "" FORCE)
set(WAVPACK_ENABLE_LIBICONV          OFF CACHE BOOL "" FORCE)
set(WAVPACK_ENABLE_ASM               OFF CACHE BOOL "" FORCE)
set(WAVPACK_INSTALL_CMAKE_MODULE     OFF CACHE BOOL "" FORCE)
set(WAVPACK_INSTALL_DOCS             OFF CACHE BOOL "" FORCE)
set(WAVPACK_INSTALL_PKGCONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS                OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING                    OFF CACHE BOOL "" FORCE)
add_subdirectory(WavPack)

# vendored libambix (with WavPack backend on)
set(LIBAMBIX_ENABLE_WAVPACK ON  CACHE BOOL "" FORCE)
set(LIBAMBIX_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
add_subdirectory(libambix)

set (reaper_ambix_sources
	${CMAKE_CURRENT_SOURCE_DIR}/src/wrapperclass.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/pcmsrc_ambix.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/pcmsink_ambix.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/loudness.cpp
	${CMAKE_CURRENT_SOURCE_DIR}/src/normalize_action.cpp
	)

IF(WIN32)
	# ressource file for gui
	set (win_resource_files
		${CMAKE_CURRENT_SOURCE_DIR}/src/res.rc
		)
	set(reaper_ambix_sources ${reaper_ambix_sources} ${win_resource_files})
ENDIF(WIN32)

IF(APPLE)
	
	# macOS: compile our own SWELL instance from the Cocoa (.mm) sources.
	# this filelist is from the swell example: (app_main.cpp)
	set (swell_sources
		${CMAKE_CURRENT_SOURCE_DIR}/WDL/WDL/swell/swell-dlg.mm
		${CMAKE_CURRENT_SOURCE_DIR}/WDL/WDL/swell/swell-gdi.mm
		${CMAKE_CURRENT_SOURCE_DIR}/WDL/WDL/swell/swell-ini.cpp
		${CMAKE_CURRENT_SOURCE_DIR}/WDL/WDL/swell/swell-kb.mm
		${CMAKE_CURRENT_SOURCE_DIR}/WDL/WDL/swell/swell-menu.mm
		${CMAKE_CURRENT_SOURCE_DIR}/WDL/WDL/swell/swell-misc.mm
		${CMAKE_CURRENT_SOURCE_DIR}/WDL/WDL/swell/swell-miscdlg.mm
		${CMAKE_CURRENT_SOURCE_DIR}/WDL/WDL/swell/swell-wnd.mm
		${CMAKE_CURRENT_SOURCE_DIR}/WDL/WDL/swell/swell.cpp
		${CMAKE_CURRENT_SOURCE_DIR}/WDL/WDL/swell/swell-appstub.mm
		)
	
	set(reaper_ambix_sources ${reaper_ambix_sources} ${swell_sources})

ELSEIF(NOT WIN32)

	# Linux / other Unix: bind to REAPER's host SWELL via the modstub. REAPER
	# calls SWELL_dllMain() at load to populate the swell function pointers, so
	# we don't compile/link the full generic SWELL + LICE + GTK stack here.
	add_definitions(-DSWELL_PROVIDED_BY_APP)
	# swell-types.h otherwise defines min()/max() function-like macros that
	# clobber libstdc++'s <cmath> on Linux (Apple's libc++ tolerates it).
	add_definitions(-DWDL_NO_DEFINE_MINMAX)
	set(reaper_ambix_sources ${reaper_ambix_sources}
		${CMAKE_CURRENT_SOURCE_DIR}/WDL/WDL/swell/swell-modstub-generic.cpp)

ENDIF()

IF(REAPER_AMBIX_INSTALL_USER_PLUGINS)
  # Convenience for development: write the plugin straight into the user's
  # REAPER plugin folder so a rebuild = reload-in-Reaper.
  IF(APPLE)
    SET(CMAKE_LIBRARY_OUTPUT_DIRECTORY "$ENV{HOME}/Library/Application Support/REAPER/UserPlugins")
  ELSEIF(NOT WIN32)
    SET(CMAKE_LIBRARY_OUTPUT_DIRECTORY "$ENV{HOME}/.config/REAPER/UserPlugins")
  ENDIF()
ENDIF()

add_library (reaper_ambix SHARED
	${reaper_ambix_sources}
	)

SET_TARGET_PROPERTIES(reaper_ambix PROPERTIES PREFIX "")

IF(APPLE)
	target_link_libraries (reaper_ambix ${COCOA_LIBRARY} ${CARBON_LIBRARY})
ENDIF(APPLE)


if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(reaper_ambix PRIVATE
        -Wno-error=c++11-narrowing
    )
elseif(NOT MSVC)
    # GCC spells the same family of narrowing conversions differently.
    target_compile_options(reaper_ambix PRIVATE -Wno-narrowing)
else()
    # Disable MSVC warnings about double to float truncation in matrix literals
    target_compile_options(reaper_ambix PRIVATE /wd4305)
    target_compile_definitions(reaper_ambix PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# vendored libambix (statically linked, in turn pulls in vendored WavPack)
target_link_libraries (reaper_ambix ambix wavpack)

# Since we're linking libambix statically, define AMBIX_STATIC to avoid dllimport
target_compile_definitions(reaper_ambix PRIVATE AMBIX_STATIC)

# WDL hard-asserts at compile time that `char` is signed (wdltypes.h). GCC/Clang
# default `char` to UNSIGNED on ARM/AArch64 (e.g. the Linux aarch64 runner),
# which turns that assert into a negative-sized array and fails the build.
# No-op where char is already signed (x86/x86_64, Apple's arm64 ABI).
if(NOT MSVC)
	target_compile_options(reaper_ambix PRIVATE -fsigned-char)
endif()

# Statically link libstdc++/libgcc on Linux so the plugin does not pull in the
# build host's (often newer) GCC runtime at load time. Without this, a binary
# built on current CI references GLIBCXX symbol versions absent on older
# distros (Debian 12 / MX 23), and REAPER silently fails to dlopen it.
if(NOT APPLE AND NOT MSVC)
	target_link_options(reaper_ambix PRIVATE -static-libstdc++ -static-libgcc)
endif()
