#pragma once

#include "pch.h"

namespace mactab {

// Notification-area icon.
//
// Uses NOTIFYICON_VERSION_4, so the callback message arrives with screen
// coordinates already packed into wParam and the event id in LOWORD(lParam) —
// no GetCursorPos round-trip, and NIN_SELECT/WM_CONTEXTMENU are distinguished
// properly instead of guessing from raw mouse messages.
class Tray {
public:
    Tray() = default;
    ~Tray();

    Tray(const Tray&)            = delete;
    Tray& operator=(const Tray&) = delete;

    // `callbackMsg` should be a WM_APP-range message; it is posted to `owner`
    // for every tray interaction.
    bool Create(HINSTANCE instance, HWND owner, UINT callbackMsg, UINT iconResourceId,
                const wchar_t* tooltip);
    void Destroy();

    // Explorer drops every tray icon when it restarts. Owners must watch for
    // TaskbarCreatedMessage() in their WndProc and call this, or the icon
    // silently disappears until the next reboot.
    void Readd();

    void ShowBalloon(const wchar_t* title, const wchar_t* text);

    // Registered "TaskbarCreated" broadcast. Returns 0 if registration failed.
    static UINT TaskbarCreatedMessage();

private:
    NOTIFYICONDATAW MakeData() const;

    HWND         m_owner       = nullptr;
    UINT         m_callbackMsg = 0;
    HICON        m_icon        = nullptr;
    std::wstring m_tooltip;
    bool         m_added       = false;
};

} // namespace mactab
