#pragma once

#include "pch.h"

// Windows' virtual desktops, which are macOS' Spaces.
//
// TWO INTERFACES, AND WHY THE SECOND ONE IS ALLOWED.
//
// Most of this file is public API:
//
//   the registry     the ordered list of desktop GUIDs and their names, which
//                    the shell keeps current rather than writing lazily
//   IVirtualDesktopManager   GetWindowDesktopId, to bucket windows, and
//                    MoveWindowToDesktop for this process's own windows
//   SendInput        the same keyboard shortcuts a user would press
//
// None of that can move ANOTHER process's window to another desktop, which is
// the one thing Mission Control exists to let you do. Only the shell's own
// IVirtualDesktopManagerInternal can, and it is undocumented. It has bitten
// every tool that used it, and the way it bit is the design of the code here:
//
//   Its IID changes between builds. That is survivable: QueryService with an
//   IID the shell does not know fails cleanly.
//
//   Its vtable has also changed WITHOUT the IID changing. 26100.2605 inserted
//   SwitchDesktopAndMoveForegroundView at slot 10 and kept the IID, so every
//   method from slot 10 on moved by one and the tools that called them crashed
//   or did something else entirely. Slots 3 to 9 (GetCount, MoveViewToDesktop,
//   CanViewMoveDesktops, GetCurrentDesktop, GetDesktops, GetAdjacentDesktop,
//   SwitchDesktop) have the same order and signatures in every layout from
//   Windows 10 1809 to Windows 11 25H2.
//
// So only slots 3 to 9 are declared, for three known IIDs, and nothing is moved
// until the interface has listed the desktops back in exactly the order the
// registry has them, which a shifted vtable cannot do by accident. A build that
// fails that is refused for the session and gets the public behaviour back:
// a window from another application dropped on a desktop goes back where it
// was. DesktopMoves=off in settings.ini refuses it everywhere.
//
// Not covered: Windows 10 1803 and Windows 11 before 22621.2215, whose layouts
// take a monitor handle in the slots above. They get the public behaviour.
//
// Every call into the shell here is cross-process COM from an STA, which runs a
// modal loop that dispatches timers and posted messages while it waits. Callers
// must not hold references into anything those could tear down. Mission
// Control posts its moves to the host window for exactly that reason.

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

// Move a window to another desktop.
//
// This process's own windows go through the public API. Anything else goes
// through the shell's private interface, when CanMoveWindows() says it is
// usable on this machine. False means nothing moved.
bool MoveWindowTo(HWND hwnd, const GUID& desktop);

// Switch the view to `desktop` through the shell's interface: no animation, and
// nothing injected, so keys the user is holding stay held. False when that
// interface is not usable here, and the caller falls back to SwitchTo.
bool SwitchToId(const GUID& desktop);

// Whether windows of other applications can be moved between desktops here.
//
// Connects to the shell and checks it on first use. A failure because explorer
// is not up yet is retried next time; a build that fails the check is not.
bool CanMoveWindows();

// Drop the connection to the shell, for when explorer restarts. The next call
// reconnects and checks again.
void ResetShell();

// Whether a window is on the desktop being viewed, which a window pinned to
// every desktop always is. True when it cannot be told, which is the answer
// that changes nothing.
bool IsOnCurrentDesktop(HWND hwnd);

// Block until the shell reports `target` as the desktop being viewed, or give up.
//
// The one observable a switch has. Reading it costs two registry values, and
// this is only ever called with an overlay already off screen, so a short stall
// on the UI thread is cheaper than the machinery to avoid one.
bool WaitForCurrent(const GUID& target, DWORD timeoutMs = 600);

// Close the desktop at `index`, whichever one is being viewed.
//
// Public API can only close the current desktop, so this goes there first and
// confirms it arrived before sending the close. False means nothing was closed,
// which is the only safe way to fail: closing the WRONG desktop because a chord
// went missing is not recoverable.
bool CloseAt(HWND ownWindow, int index);

// Ctrl+Win+D. The new desktop is created after the current one and becomes
// current, which is what Windows does and what the strip has to assume.
bool Create();

// Ctrl+Win+F4, which closes the desktop currently being viewed and only that
// one. Closing an arbitrary desktop is not possible on public API.
bool CloseCurrent();

void LogState(HWND ownWindow);

} // namespace mactab::desktops
