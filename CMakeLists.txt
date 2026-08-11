cmake_minimum_required(VERSION 3.21)
project(MacTab VERSION 0.1.0 LANGUAGES CXX)

if(NOT MSVC)
    message(FATAL_ERROR
        "MacTab requires MSVC (Visual Studio 2022 or Build Tools 2022).\n"
        "MinGW/clang-mingw cannot consume the C++/WinRT projection headers.")
endif()

# ---------------------------------------------------------------------------
# Version.
#
# Single-sourced so the binary's VERSIONINFO, the version string the app shows,
# and the version the installer registers in Add/Remove Programs cannot drift.
# CI passes -DMACTAB_VERSION_STRING=<from git tag>; a local build falls back to
# the project() version.
# ---------------------------------------------------------------------------
set(MACTAB_VERSION_STRING "${PROJECT_VERSION}" CACHE STRING
    "Version stamped into the binary and its resources")

if(MACTAB_VERSION_STRING MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)")
    set(MACTAB_VER_MAJOR "${CMAKE_MATCH_1}")
    set(MACTAB_VER_MINOR "${CMAKE_MATCH_2}")
    set(MACTAB_VER_PATCH "${CMAKE_MATCH_3}")
else()
    message(FATAL_ERROR
        "MACTAB_VERSION_STRING ('${MACTAB_VERSION_STRING}') must start with x.y.z")
endif()

set(MACTAB_RES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/res")
configure_file(res/app.rc.in "${CMAKE_BINARY_DIR}/app.rc" @ONLY)
message(STATUS "MacTab version: ${MACTAB_VERSION_STRING}")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static CRT: no vcruntime/msvcp redistributable to chase. Costs ~200 KB.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# We embed our own manifest through the .rc file, so stop the linker
# generating a second one (they conflict and the linker errors out).
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /MANIFEST:NO")

# ---------------------------------------------------------------------------
# Locate the C++/WinRT projection headers.
#
# These ship inside the Windows SDK (Include/<ver>/cppwinrt) but that folder is
# not on the default include path, and vcvars does not add it. Look at the env
# vars vcvars *does* set first, then fall back to scanning the standard kit
# locations newest-first.
# ---------------------------------------------------------------------------
function(mactab_find_cppwinrt out_var)
    set(candidates "")

    if(DEFINED ENV{WindowsSdkDir} AND DEFINED ENV{WindowsSDKVersion})
        # WindowsSDKVersion carries a trailing backslash, e.g. "10.0.22621.0\"
        string(REGEX REPLACE "[\\/]+$" "" _ver "$ENV{WindowsSDKVersion}")
        list(APPEND candidates "$ENV{WindowsSdkDir}Include/${_ver}/cppwinrt")
    endif()

    foreach(root
            "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/Include"
            "$ENV{ProgramFiles}/Windows Kits/10/Include"
            "C:/Program Files (x86)/Windows Kits/10/Include"
            "C:/Program Files/Windows Kits/10/Include")
        if(IS_DIRECTORY "${root}")
            file(GLOB versions "${root}/*")
            list(SORT versions)
            list(REVERSE versions)   # newest SDK first
            foreach(v ${versions})
                list(APPEND candidates "${v}/cppwinrt")
            endforeach()
        endif()
    endforeach()

    foreach(dir ${candidates})
        if(EXISTS "${dir}/winrt/base.h")
            set(${out_var} "${dir}" PARENT_SCOPE)
            return()
        endif()
    endforeach()

    set(${out_var} "" PARENT_SCOPE)
endfunction()

mactab_find_cppwinrt(CPPWINRT_INCLUDE_DIR)
if(NOT CPPWINRT_INCLUDE_DIR)
    message(FATAL_ERROR
        "Could not find the C++/WinRT headers (cppwinrt/winrt/base.h).\n"
        "Install the Windows 10/11 SDK (10.0.17763.0 or newer) via the Visual\n"
        "Studio Installer, then build from a Developer Command Prompt so that\n"
        "WindowsSdkDir is set. build.bat does this for you.")
endif()
message(STATUS "C++/WinRT headers: ${CPPWINRT_INCLUDE_DIR}")

# ---------------------------------------------------------------------------
# Target
# ---------------------------------------------------------------------------
add_executable(MacTab WIN32
    src/main.cpp
    src/panel.cpp
    src/actions.cpp
    src/activate.cpp
    src/app_identity.cpp
    src/capture.cpp
    src/com.cpp
    src/config.cpp
    src/desktops.cpp
    src/common.cpp
    src/diag.cpp
    src/foreground_history.cpp
    src/geometry.cpp
    src/glass_draw.cpp
    src/hotkey.cpp
    src/icon_source.cpp
    src/icons.cpp
    src/image.cpp
    src/mission.cpp
    src/package_assets.cpp
    src/settings_watch.cpp
    src/squircle.cpp
    src/thumbnail.cpp
    src/tray.cpp
    src/wallpaper.cpp
    src/window_model.cpp
    "${CMAKE_BINARY_DIR}/app.rc"
)

target_precompile_headers(MacTab PRIVATE src/pch.h)

# The pure image/geometry sources must NOT receive the precompiled header.
#
# target_precompile_headers force-includes pch.h into every translation unit,
# which drags windows.h in behind their backs, defeating the property that lets
# tools/preview compile and run them natively, and exposing them to the Win32
# macro minefield (rpcndr.h's `#define small char` broke squircle.cpp exactly
# this way, invisibly to the mingw syntax check, which does not force-include).
set_source_files_properties(
    src/image.cpp
    src/squircle.cpp
    PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

target_include_directories(MacTab PRIVATE
    src
    res
    "${CPPWINRT_INCLUDE_DIR}"
)

target_compile_definitions(MacTab PRIVATE
    UNICODE _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    MACTAB_VERSION="${MACTAB_VERSION_STRING}"
)

target_compile_options(MacTab PRIVATE
    /W4              # high warning level; we build clean at W4
    /permissive-     # standards conformance
    /Zc:preprocessor # conforming preprocessor, needed for __VA_OPT__ in the DIAG macros
    /utf-8           # source and execution charset
    /GR-             # no RTTI; C++/WinRT does not need it
    $<$<NOT:$<CONFIG:Debug>>:/O2>
    $<$<NOT:$<CONFIG:Debug>>:/Gy>   # function-level linking, lets /OPT:ICF fold
    $<$<NOT:$<CONFIG:Debug>>:/Gw>   # global data COMDATs, lets /OPT:REF strip
)

target_link_options(MacTab PRIVATE
    $<$<NOT:$<CONFIG:Debug>>:/OPT:REF>
    $<$<NOT:$<CONFIG:Debug>>:/OPT:ICF>
)

target_link_libraries(MacTab PRIVATE
    windowsapp     # WinRT umbrella (ships with the SDK, present on Win10 1803+)
    user32
    shell32
    shlwapi
    ole32
    dwmapi
    wtsapi32       # session lock/unlock notifications
    d2d1
    dxguid         # CLSID_D2D1GaussianBlur / CLSID_D2D1Scale live here
    d3d11
    dxgi
    dwrite
    shcore         # GetDpiForMonitor
    windowscodecs  # PNG decoding: icon theme packs, and 256px icon frames
    version        # FileDescription from executables' version resources
    xmllite        # reading a package manifest for its logo and tile colour
)

set_target_properties(MacTab PROPERTIES
    OUTPUT_NAME "MacTab"
    RUNTIME_OUTPUT_DIRECTORY         "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
)
