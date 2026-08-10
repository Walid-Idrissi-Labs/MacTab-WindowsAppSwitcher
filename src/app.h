#pragma once

#include "pch.h"

namespace mactab {

// Well-known names, shared with the installer.
//
// The installer needs to shut a running instance down before it can replace the
// exe. Rather than killing the process (which would leak the tray icon and skip
// the keyboard-hook teardown), it finds this window class and posts the quit
// message, letting us exit cleanly.
inline constexpr wchar_t kHostWindowClass[] = L"MacTabHostWindow";

// Registered via RegisterWindowMessage, so the name, not a numeric id, is the
// contract. Any process can broadcast it to ask MacTab to exit.
inline constexpr wchar_t kQuitMessageName[] = L"MacTab.RequestQuit";

// Session-local: one instance per interactive logon session, which is what we
// want because WH_KEYBOARD_LL hooks are session-scoped. A "Global\" mutex would
// wrongly block a second user on the same machine (fast user switching, RDP).
inline constexpr wchar_t kSingleInstanceMutex[] =
    L"Local\\MacTab.SingleInstance.6f1c3f4e-2b7a-4a1e-9c3d-8d2f5b6a7e10";

} // namespace mactab
