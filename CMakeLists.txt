cmake_minimum_required(VERSION 3.10)

# Read VERSION file at the repo root
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" REAPER_AMBIX_VERSION LIMIT_COUNT 1)
string(STRIP "${REAPER_AMBIX_VERSION}" REAPER_AMBIX_VERSION)

project (reaper_ambix VERSION ${REAPER_AMBIX_VERSION})

# Build mode:
#  - ON  (default): drop the .dylib straight into the user's REAPER plugin
#                   folder so a simple `make` reloads in Reaper for dev work.
#  - OFF (CI / installer): leave it in the build tree so build_osx.sh /
#                          build_win.bat can stage + sign + package it.
option(REAPER_AMBIX_INSTALL_USER_PLUGINS
       "Copy the built plugin into the user's REAPER UserPlugins folder"
       ON)

SET(CMAKE_BUILD_TYPE MinSizeRel)

IF(APPLE)
   #SET( CMAKE_OSX_ARCHITECTURES x86_64 )
   #SET( CMAKE_OSX_DEPLOYMENT_TARGET 10.9 )
   SET ( LINK_FLAGS MINSIZEREL "-dead_strip" )
	
	FIND_LIBRARY(CARBON_LIBRARY Carbon)
	FIND_LIBRARY(COCOA_LIBRARY Cocoa)
    	
	SET(CMAKE_MACOSX_RPATH 1)
ENDIF (APPLE)

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
	)

IF(WIN32)
	# ressource file for gui
	set (win_resource_files
		${CMAKE_CURRENT_SOURCE_DIR}/src/res.rc
		)
	set(reaper_ambix_sources ${reaper_ambix_sources} ${win_resource_files})
ENDIF(WIN32)

IF(NOT WIN32)
	
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
	
ENDIF(NOT WIN32)

IF(APPLE AND REAPER_AMBIX_INSTALL_USER_PLUGINS)
  # Convenience for development: write the .dylib straight into the user's
  # REAPER plugin folder so a rebuild = reload-in-Reaper.
  SET(CMAKE_LIBRARY_OUTPUT_DIRECTORY "$ENV{HOME}/Library/Application Support/REAPER/UserPlugins")
ENDIF()

add_library (reaper_ambix SHARED
	${reaper_ambix_sources}
	)

SET_TARGET_PROPERTIES(reaper_ambix PROPERTIES PREFIX "")

IF(APPLE)
	target_link_libraries (reaper_ambix ${COCOA_LIBRARY} ${CARBON_LIBRARY})
	# libsndfile is the only remaining dynamic dependency. Its install_name is
	# @rpath/libsndfile.1.dylib, so the loaded plugin needs LC_RPATH entries
	# pointing at where libsndfile actually lives (Homebrew on Apple Silicon
	# vs Intel, /usr/local fallback).
	set_target_properties(reaper_ambix PROPERTIES
		INSTALL_RPATH "/opt/homebrew/opt/libsndfile/lib;/opt/homebrew/lib;/usr/local/opt/libsndfile/lib;/usr/local/lib"
		BUILD_WITH_INSTALL_RPATH TRUE
		MACOSX_RPATH TRUE
	)
ENDIF(APPLE)


if(NOT MSVC)
    target_compile_options(reaper_ambix PRIVATE
        -Wno-error=c++11-narrowing
    )
endif()

# vendored libambix (statically linked, in turn pulls in vendored WavPack
# + system libsndfile via libambix's own CMakeLists)
target_link_libraries (reaper_ambix ambix wavpack)



