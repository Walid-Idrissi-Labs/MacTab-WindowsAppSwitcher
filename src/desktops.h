#pragma once

#include "pch.h"

// Windows' virtual desktops, which are macOS' Spaces.
//
// WHY THIS IS ALL PUBLIC API, WHEN EVERY OTHER TOOL USES THE PRIVATE ONE.
//
// Almost everything interesting about virtual desktops lives on
// IVirtualDesktopManagerInternal, which is undocumented and whose interface ID
// changes with nearly every Windows build. That is survivable for a project
// with a test machine. It is not survivable here: the interface was
// restructured in 24H2 WITHOUT changing its ID, so QueryInterface succeeded,
// the vtable was wrong, and the tools that used it crashed rather than failed.
// A crash on somebody else's machine, on a build we have never seen, found out
// about weeks later, is the single worst failure this project can have.
//
// So everything below is built from three public things:
//
//   the registry     the ordered list of desktop GUIDs and their names, which
//                    the shell keeps current rather than writing lazily
//   IVirtualDesktopManager   GetWindowDesktopId, to bucket windows, and
//                    IsWindowOnCurrentVirtualDesktop
//   SendInput        the same keyboard shortcuts a user would press
//
// What that cannot do is close a desktop other than the current one, and move
// another process's window to another desktop. Both need the private interface,
// and both are deliberately absent rather than risky.

namespace mactab::desktops {

struct Desktop {
    GUID         id{};
    std::wstring name;      // never empty; synthesised when the shell has none
};

struct State {
    std::vector<Desktop> all;

    // Index into `all`, or -1 when it could not be determined.
    int current = -1;

    // False when the registry told us nothing, which means desktops are either
    // unavailable or this build keeps them somewhere else. Callers should hide
    // the spaces strip entirely rather than show a wrong one.
    bool known = false;
};

// Read the current arrangement. Cheap: two registry reads and one COM call.
// Called per invocation rather than tracked, because there is no public
// notification for a desktop change and polling for one would cost idle CPU.
//
// `ownWindow` is any parentless window belonging to this process. It is the
// documented way to discover the current desktop when the registry does not
// say: a window with no parent is always on the desktop being viewed.
State Query(HWND ownWindow);

// The desktop a window lives on. False if it could not be read, which happens
// for windows of another integrity level.
bool DesktopIdOf(HWND hwnd, GUID& out);

// Position of `id` in the ordered list, or -1.
//
// A window can legitimately report a GUID that is not in the list: pinned
// windows and pinned apps get sentinel IDs meaning "show me on every desktop".
// Treat -1 as exactly that rather than as an error.
int IndexOf(const State& state, const GUID& id);

// Switch the view to `targetIndex` by injecting the same shortcut a user would
// press, once per step. There is no "go to desktop N" chord in any Windows
// build to date, so switching four desktops is four injections.
//
// Callers must hide their own overlay FIRST. The switch may animate, and an
// overlay still on screen while the desktop slides underneath it looks broken.
bool SwitchTo(const State& state, int targetIndex);

// Ctrl+Win+D. The new desktop is created after the current one and becomes
// current, which is what Windows does and what the strip has to assume.
bool Create();

// Ctrl+Win+F4, which closes the desktop currently being viewed and only that
// one. Closing an arbitrary desktop is not possible on public API.
bool CloseCurrent();

void LogState(HWND ownWindow);

} // namespace mactab::desktops
