#pragma once

#include "pch.h"

// The switcher list: eligible windows, grouped into applications, MRU-ordered.

namespace mactab {

struct SwitcherWindow {
    HWND         hwnd      = nullptr;
    std::wstring title;
    bool         minimized = false;

    // Where the window actually sits, in virtual-screen coordinates.
    //
    // The switcher does not care: it draws icons, not windows. Mission Control
    // does, because the arrangement is built from the real geometry and has to
    // preserve both the relative sizes and roughly the positions. Filled in
    // here rather than re-enumerated later so there is one definition of which
    // windows exist and where.
    //
    // For a minimized window this is the restored placement, not the
    // meaningless off-screen rect GetWindowRect returns while it is iconic.
    RECT         bounds{};

    // Which virtual desktop it lives on. All zeroes when it could not be read,
    // which happens for windows of another integrity level.
    GUID         desktop{};
};

struct SwitcherApp {
    std::wstring key;           // stable grouping key (AUMID or exe path)
    std::wstring displayName;   // panel label
    std::wstring exePath;
    std::wstring aumid;
    bool         packaged = false;

    // Most-recently-used first. Always at least one entry.
    std::vector<SwitcherWindow> windows;

    HWND PrimaryWindow() const {
        return windows.empty() ? nullptr : windows.front().hwnd;
    }
};

// Enumerate, filter, group and order. Called once per gesture. Enumeration is
// roughly a millisecond, so there is nothing to gain from keeping a live list,
// and plenty to lose (a background list would have to be invalidated on every
// window create/destroy, which is a constant CPU drip).
//
// Ordering is most-recently-used, matching macOS: index 0 is the current app,
// index 1 is the one Alt+Tab should land on.
std::vector<SwitcherApp> BuildSwitcherList();

// The same list, plus the windows living on other virtual desktops.
//
// Those windows are shell-cloaked, which is exactly the check that keeps the
// switcher scoped to the desktop you are on, so it has to be relaxed rather
// than removed: a cloaked UWP placeholder is still not a window, and only the
// DWM_CLOAKED_SHELL kind means "this is on another desktop".
//
// Mission Control needs them, because you can look through the desktops from
// inside it without leaving it.
std::vector<SwitcherApp> BuildWindowList(bool includeOtherDesktops);

// Exposed for the diag dump and for unit-style spot checks: does this window
// belong in the switcher at all?
bool IsSwitcherWindow(HWND hwnd, bool includeOtherDesktops = false);

// Writes the current list to the diag log. This is the M2 verification tool:
// what it prints should match what Windows' own Alt+Tab shows.
void LogSwitcherList();

} // namespace mactab
