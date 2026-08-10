#pragma once

#include "pch.h"

// The switcher list: eligible windows, grouped into applications, MRU-ordered.

namespace mactab {

struct SwitcherWindow {
    HWND         hwnd      = nullptr;
    std::wstring title;
    bool         minimized = false;
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

// Exposed for the diag dump and for unit-style spot checks: does this window
// belong in the switcher at all?
bool IsSwitcherWindow(HWND hwnd);

// Writes the current list to the diag log. This is the M2 verification tool:
// what it prints should match what Windows' own Alt+Tab shows.
void LogSwitcherList();

} // namespace mactab
