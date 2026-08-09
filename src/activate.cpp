#include "pch.h"
#include "activate.h"
#include "hotkey.h"
#include "diag.h"

namespace mactab {

bool ActivateWindow(HWND target, WORD altVirtualKey) {
    if (!target || !::IsWindow(target)) {
        MACTAB_WARN("activate: target window is gone");
        return false;
    }

    // Order matters here.
    //
    // The synthetic Alt-up must land while the OLD window is still foreground.
    // Its queue processes the key-up after we have already switched away, so it
    // is no longer active and never opens its menu bar — which is exactly the
    // bleed-through we are avoiding. Doing this after SetForegroundWindow would
    // deliver a lone Alt to the newly activated app instead.
    //
    // It also earns us the right to change foreground at all: a process may
    // only call SetForegroundWindow if it "received the last input event",
    // among other conditions, and synthesising input is the accepted way to
    // qualify. This is why we do not use AttachThreadInput — attaching our
    // input queue to another app's means a hang in that app hangs our hook
    // thread, which would take the keyboard down system-wide.
    hotkey::NeutralizeAlt(altVirtualKey);

    // If the app has an active modal dialog, focus belongs there rather than on
    // a main window that cannot accept input.
    HWND focusTarget = ::GetLastActivePopup(target);
    if (!focusTarget || !::IsWindow(focusTarget))
        focusTarget = target;

    // Restore before taking foreground — the reverse order leaves the window
    // foreground but still iconic on some shells.
    if (::IsIconic(target))
        ::ShowWindow(target, SW_RESTORE);

    if (::SetForegroundWindow(focusTarget))
        return true;

    // Denied. Usually means another process holds a foreground lock. Try the
    // weaker primitives rather than giving up outright.
    MACTAB_WARN("activate: SetForegroundWindow(%p) refused (err %lu), retrying",
                static_cast<void*>(focusTarget), ::GetLastError());

    ::BringWindowToTop(focusTarget);
    ::SetActiveWindow(focusTarget);

    const bool ok = (::GetForegroundWindow() == focusTarget);
    if (!ok)
        MACTAB_FAIL("activate: could not foreground %p", static_cast<void*>(focusTarget));
    return ok;
}

} // namespace mactab
