#pragma once

// Keep the Win32 surface small. This measurably shortens compile times and,
// more usefully, stops windows.h macros (min/max, GetMessage, ...) from
// colliding with standard library names.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define NOSERVICE
#define NOMCX
#define NOIME
#define NOHELP

#include <windows.h>

#include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM
#include <shellapi.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <dwmapi.h>

// Deliberately NOT included here: <unknwn.h> and <winrt/base.h>.
//
// The C++/WinRT projection headers only exist in the Windows SDK, so pulling
// them into the shared PCH would make every translation unit parseable only by
// MSVC-on-Windows. Keeping them out means the Win32-only sources (everything
// except the Composition rendering layer) can be syntax-checked cross-platform
// against the mingw-w64 headers, see tools/syntax-check.sh. That is the only
// verification available while developing away from a Windows machine, so it is
// worth protecting.
//
// Files that need WinRT include "winrt_pch.h" instead.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
