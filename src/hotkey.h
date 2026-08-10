#pragma once

#include "pch.h"

// Alt+Tab interception.
//
// RegisterHotKey cannot bind MOD_ALT|VK_TAB (Windows reserves it) and would
// not give us the press/hold/release semantics we need anyway, since it reports
// a discrete chord with no key-up. So this is a WH_KEYBOARD_LL hook that returns
// non-zero for the keys it consumes, which stops the built-in switcher ever
// arming.
//
// Two structural rules, both load-bearing:
//
//  1. The hook runs on a DEDICATED thread with its own message loop. A
//     low-level hook callback executes on the thread that installed it, and if
//     that thread stalls longer than LowLevelHooksTimeout (capped at 1000 ms
//     since Windows 10 1709) Windows silently unhooks us with no notification
//     of any kind. The UI thread will be doing GPU work, icon decoding and
//     screen capture, so it is not a safe host.
//
//  2. The gesture state machine lives ON the hook thread. Whether to swallow a
//     key is the hook procedure's return value and cannot be deferred to
//     another thread. The UI thread is a pure consumer of the messages below,
//     it is never asked a question the hook is waiting on. Communication is
//     PostMessage only, never SendMessage, which would block the hook thread on
//     a possibly-stalled UI thread and get us unhooked.

namespace mactab::hotkey {

// Posted to the UI window passed to Start(). All are fire-and-forget.
enum : UINT {
    // A switch gesture started. wParam: 1 if Shift was held (reverse order).
    // The panel is NOT shown yet, see WM_MACTAB_REVEAL.
    WM_MACTAB_BEGIN  = WM_APP + 10,

    // Move the selection. wParam: +1 forward, -1 backward.
    WM_MACTAB_SELECT = WM_APP + 11,

    // The hold delay elapsed with Alt still down: show the panel. A quick
    // tap-and-release never produces this, which is how macOS gets its
    // "instant switch to previous app without any UI" behaviour.
    WM_MACTAB_REVEAL = WM_APP + 12,

    // Alt released: activate the current selection. wParam carries the VK of
    // the Alt key whose release we swallowed, which must be handed to
    // NeutralizeAlt() before activating.
    WM_MACTAB_COMMIT = WM_APP + 13,

    // Escape, or the gesture was aborted (session lock, hook reload).
    // wParam carries the Alt VK to neutralise, or 0 if none is outstanding.
    WM_MACTAB_CANCEL = WM_APP + 14,

    // A quick-action key was pressed while the panel was up.
    // wParam: the virtual key ('Q', 'W', 'H', VK_OEM_3 for backquote, VK_DOWN).
    WM_MACTAB_ACTION = WM_APP + 15,
};

struct Options {
    // How long Alt must be held before the panel appears. Below this, the
    // gesture commits invisibly.
    UINT revealDelayMs = 180;

    // Trigger on Left Alt only, and never while Ctrl is down.
    //
    // This matters on French and Arabic layouts, where AltGr is Ctrl+RightAlt
    // and is used constantly for @ # { } [ ]. Without both guards, ordinary
    // typing fires the switcher.
    bool leftAltOnly = true;
};

// Spawns the hook thread and installs the hook. Blocks until the hook is
// installed (or failed), so a false return is definitive.
bool Start(HWND uiWindow, const Options& options);

// Unhooks and joins the hook thread.
void Stop();

// Unhook and re-hook.
//
// Low-level hooks are last-installed-first-served, so another tool installing
// after us can eat Tab before we see it. There is no way to reclaim priority
// other than reinstalling, and no notification when we lose it, hence the
// manual escape hatch in the tray menu.
bool Reload();

bool IsRunning();

// Abort any in-flight gesture and clear the stuck-Alt state. Called on session
// lock/unlock: the secure desktop means we may never see the Alt release.
void AbortGesture();

// End the gesture WITHOUT posting a cancellation.
//
// For paths that have already committed by other means, such as clicking a
// tile, so
// the hook stops waiting for an Alt release without a second commit/cancel
// arriving behind it and injecting a duplicate key-up.
void EndGestureQuietly();

// The Alt key that opened the current gesture, or 0 if none is in flight.
//
// Written on the hook thread, read on the UI thread. It is a single WORD whose
// only readers use it to pick which key-up to synthesise, so a torn read is not
// possible and a stale one is harmless.
WORD GestureAltKey();

// Release a modifier the hook swallowed.
//
// Because we consume the real Alt-up, the system still believes Alt is held.
// Left alone, the next app to receive focus opens its menu bar (lone-Alt
// activates the menu), and other apps misbehave on their next keystroke. So we
// inject a replacement key-up tagged as ours.
//
// The injection doubles as the SetForegroundWindow unlock: a process may take
// foreground if it "received the last input event", and synthesising input is
// the accepted way to qualify. Call this immediately before activating.
void NeutralizeAlt(WORD altVirtualKey);

// True if the key event currently being processed was injected by us. Exposed
// for the activation path, which must not treat its own synthetic input as a
// user gesture.
bool IsOwnInjection(ULONG_PTR extraInfo);

} // namespace mactab::hotkey
