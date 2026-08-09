#pragma once

#include "pch.h"

// Working out which *application* a window belongs to.
//
// This is the whole basis of macOS-style switching: Cmd-Tab switches between
// apps, not windows, so every window has to be reduced to a stable app key that
// several windows can share.
//
// Two cases make it awkward:
//
//  * Packaged (UWP/Store) apps do not own their own top-level window. The
//    visible window is an ApplicationFrameWindow belonging to
//    ApplicationFrameHost.exe, and the real app lives in a
//    Windows.UI.Core.CoreWindow child owned by a different process. Grouping on
//    the frame window's process would collapse every Store app into one.
//
//  * Two different installs of the same executable path are the same app, but
//    the same PID can be reused after a process exits. The cache keys on
//    (pid, process creation time) to avoid stale hits.
//
// Deliberately COM-free. AUMIDs come from GetApplicationUserModelId in
// kernel32, not SHGetPropertyStoreForWindow, so this can run on the UI thread
// during a gesture without needing an apartment — which matters because the
// Composition layer wants that thread in an ASTA it initialises itself.
// Packaged apps' friendly display names DO need the shell, so they are filled
// in later by the icon worker (M4), which has COM.

namespace mactab {

struct AppIdentity {
    // Grouping key. Lowercased AUMID for packaged apps, lowercased full exe
    // path otherwise. Never empty for a successfully resolved app.
    std::wstring key;

    // Human-readable name for the panel label. May be empty for packaged apps
    // until the icon worker resolves it; callers should fall back to the
    // window title.
    std::wstring displayName;

    std::wstring exePath;   // empty if the path could not be read
    std::wstring aumid;     // empty for non-packaged apps
    bool packaged = false;
};

// Resolve the application owning `hwnd`, unwrapping UWP frame windows.
// Returns nullptr if the owning process could not be inspected (most often a
// higher-integrity process we are not allowed to open).
//
// The returned pointer is owned by the cache and stays valid until
// ClearIdentityCache(); do not store it across gestures.
const AppIdentity* ResolveApp(HWND hwnd);

// Drop cached identities. Cheap, and worth doing on session change.
void ClearIdentityCache();

// Fill in a packaged app's friendly display name. Called from the icon worker,
// which has COM available. No-op if the key is unknown or already named.
void SetDisplayName(const std::wstring& key, const std::wstring& displayName);

} // namespace mactab
