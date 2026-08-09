#pragma once

#include "pch.h"

// MACTAB_VERSION comes from CMake as a narrow literal; widen it for the Win32
// APIs that want wchar_t. Two levels of indirection so the macro argument is
// expanded before the L is pasted on.
#define MACTAB_WIDEN_(x)  L##x
#define MACTAB_WIDEN(x)   MACTAB_WIDEN_(x)
#ifndef MACTAB_VERSION
#define MACTAB_VERSION "0.0.0"
#endif
#define MACTAB_VERSION_W  MACTAB_WIDEN(MACTAB_VERSION)

namespace mactab {

// UTF-16 <-> UTF-8. The diag log is UTF-8 on disk so it opens cleanly
// everywhere; everything else in the process stays UTF-16.
std::string  ToUtf8(std::wstring_view w);
std::wstring FromUtf8(std::string_view s);

// %LOCALAPPDATA%\MacTab, created on first call. Returns an empty string if the
// directory could not be resolved or created — callers must handle that rather
// than assuming a valid path.
const std::wstring& AppDataDir();

// Monotonic milliseconds since process start. QPC-backed, so it is immune to
// wall-clock adjustments and has sub-microsecond resolution. Used for the
// timing numbers in the diag log.
double NowMs();

// Windows build number (e.g. 22621), or 0 if it could not be determined.
// Read via RtlGetVersion, which — unlike GetVersionEx — is not subject to
// manifest-based version lying.
uint32_t WindowsBuildNumber();

// Small RAII wrapper for HANDLEs that use CloseHandle.
struct HandleDeleter {
    void operator()(HANDLE h) const noexcept {
        if (h && h != INVALID_HANDLE_VALUE) ::CloseHandle(h);
    }
};
using UniqueHandle = std::unique_ptr<std::remove_pointer_t<HANDLE>, HandleDeleter>;

} // namespace mactab
