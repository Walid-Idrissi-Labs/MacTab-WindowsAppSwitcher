#include "pch.h"
#include "activate.h"
#include "hotkey.h"
#include "diag.h"

namespace mactab {

bool ActivateWindow(HWND target, WORD altVirtualKey) {
    // Release Alt FIRST, before anything can return early.
    //
    // We swallowed the real Alt-up, so until this runs the system still believes
    // Alt is held and every subsequent keystroke arrives as an Alt chord: menu
    // bars open, accelerators fire, and the user has to tap Alt to get out of
    // it. The dead-target path below is not hypothetical, it is what happens
    // when the app you were switching to is closed or crashes while the panel is
    // up, and it used to return without repairing the modifier.
    //
    // Injecting a lone Alt-up when Alt is already up is a no-op: a key-up on its
    // own never activates a menu bar. So there is no cost to doing this
    // unconditionally and a stuck modifier if it is skipped.
    hotkey::NeutralizeAlt(altVirtualKey);

    if (!target || !::IsWindow(target)) {
        MACTAB_WARN("activate: target window is gone");
        return false;
    }

    // Order matters here.
    //
    // The synthetic Alt-up must land while the OLD window is still foreground.
    // Its queue processes the key-up after we have already switched away, so it
    // is no longer active and never opens its menu bar, which is exactly the
    // bleed-through we are avoiding. Doing this after SetForegroundWindow would
    // deliver a lone Alt to the newly activated app instead.
    //
    // It also earns us the right to change foreground at all: a process may
    // only call SetForegroundWindow if it "received the last input event",
    // among other conditions, and synthesising input is the accepted way to
    // qualify. This is why we do not use AttachThreadInput: attaching our
    // input queue to another app's means a hang in that app hangs our hook
    // thread, which would take the keyboard down system-wide.
    //
    // If the app has an active modal dialog, focus belongs there rather than on
    // a main window that cannot accept input.
    HWND focusTarget = ::GetLastActivePopup(target);
    if (!focusTarget || !::IsWindow(focusTarget))
        focusTarget = target;

    // Restore before taking foreground; the reverse order leaves the window
    // foreground but still iconic on some shells.
    //
    // ShowWindow on another thread's window is SYNCHRONOUS, so a wedged app
    // would block this thread here, and this thread is the one that has to keep
    // processing gesture messages. ShowWindowAsync for a hung window only: the
    // synchronous call in the healthy case is what keeps the restore ordered
    // before the foreground change.
    if (::IsIconic(target)) {
        if (::IsHungAppWindow(target))
            ::ShowWindowAsync(target, SW_RESTORE);
        else
            ::ShowWindow(target, SW_RESTORE);
    }

    if (::SetForegroundWindow(focusTarget))
        return true;

    // Denied. Usually means another process holds a foreground lock.
    //
    // No GetLastError here: SetForegroundWindow does not set it, so logging one
    // would just print whatever unrelated call ran last. SetActiveWindow is not
    // useful either; it only affects windows owned by the calling thread.
    MACTAB_WARN("activate: SetForegroundWindow(%p) refused, falling back",
                static_cast<void*>(focusTarget));

    ::BringWindowToTop(focusTarget);

    const bool ok = (::GetForegroundWindow() == focusTarget);
    if (!ok)
        MACTAB_FAIL("activate: could not foreground %p", static_cast<void*>(focusTarget));
    return ok;
}

} // namespace mactab
