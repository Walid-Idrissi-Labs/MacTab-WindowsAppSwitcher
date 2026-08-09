#include "pch.h"
#include <wtsapi32.h>

#include "activate.h"
#include "app.h"
#include "common.h"
#include "diag.h"
#include "foreground_history.h"
#include "hotkey.h"
#include "tray.h"
#include "resource.h"

namespace mactab {
namespace {

// Tray interactions arrive here.
constexpr UINT WM_MACTAB_TRAY = WM_APP + 1;

UINT g_msgRequestQuit    = 0;   // RegisterWindowMessage(kQuitMessageName)
UINT g_msgTaskbarCreated = 0;   // RegisterWindowMessage(L"TaskbarCreated")

// The in-flight switch gesture.
//
// Owned entirely by the UI thread. The hook thread never reads it — it posts
// intent (begin / select / commit) and this side decides what that means.
struct Gesture {
    std::vector<HWND> candidates;
    int  index      = 0;
    bool active     = false;
    bool panelShown = false;
};

struct AppState {
    HINSTANCE instance      = nullptr;
    HWND      host          = nullptr;
    Tray      tray;
    Gesture   gesture;
    bool      diagRequested = false;
    bool      wtsRegistered = false;
    bool      shuttingDown  = false;
};

AppState g_app;

bool HasFlag(const wchar_t* cmdLine, const wchar_t* flag) {
    return cmdLine && ::StrStrIW(cmdLine, flag) != nullptr;
}

// --- Gesture handling ------------------------------------------------------

void BeginGesture(bool reverse) {
    Gesture& g = g_app.gesture;

    g.candidates = foreground::Snapshot();
    g.active     = true;
    g.panelShown = false;

    if (g.candidates.size() < 2) {
        // Nothing to switch to. Stay armed so the keys are still swallowed —
        // falling back to the built-in switcher mid-gesture would be worse.
        g.index = 0;
        MACTAB_DIAG("gesture: begin with %zu candidate(s), nothing to switch to",
                    g.candidates.size());
        return;
    }

    // macOS lands on the previously-used app, not the current one. Shift starts
    // from the far end of the list instead.
    g.index = reverse ? static_cast<int>(g.candidates.size()) - 1 : 1;

    MACTAB_DIAG("gesture: begin, %zu candidates, start index %d (reverse %d)",
                g.candidates.size(), g.index, reverse ? 1 : 0);
}

void AdvanceSelection(int direction) {
    Gesture& g = g_app.gesture;
    if (!g.active || g.candidates.empty()) return;

    const int n = static_cast<int>(g.candidates.size());
    g.index = ((g.index + direction) % n + n) % n;   // wrap in both directions

    MACTAB_DIAG("gesture: select -> index %d", g.index);
}

void CommitGesture(WORD altVirtualKey) {
    Gesture& g = g_app.gesture;

    if (!g.active) {
        // Nothing in flight, but the hook still swallowed an Alt-up, so the
        // modifier state has to be repaired regardless.
        hotkey::NeutralizeAlt(altVirtualKey);
        return;
    }

    HWND target = nullptr;
    if (g.index >= 0 && g.index < static_cast<int>(g.candidates.size()))
        target = g.candidates[static_cast<size_t>(g.index)];

    g.active     = false;
    g.panelShown = false;

    if (!target) {
        hotkey::NeutralizeAlt(altVirtualKey);
        MACTAB_DIAG("gesture: commit with no target");
        return;
    }

    wchar_t title[128] = L"";
    ::GetWindowTextW(target, title, ARRAYSIZE(title));
    MACTAB_DIAG("gesture: commit -> %p \"%s\"",
                static_cast<void*>(target), ToUtf8(title).c_str());

    // ActivateWindow performs the Alt neutralisation itself, in the specific
    // order that avoids menu bleed-through.
    ActivateWindow(target, altVirtualKey);
}

void CancelGesture(WORD altVirtualKey) {
    Gesture& g = g_app.gesture;
    g.active     = false;
    g.panelShown = false;
    g.candidates.clear();

    hotkey::NeutralizeAlt(altVirtualKey);
    MACTAB_DIAG("gesture: cancelled");
}

void RevealPanel() {
    g_app.gesture.panelShown = true;

    // M3 shows the real panel here. Until then the log is how we verify that
    // the hold-versus-tap split is working: a quick Alt+Tab must never reach
    // this point, and holding Alt must always reach it exactly once.
    MACTAB_DIAG("gesture: reveal (panel would appear now, index %d of %zu)",
                g_app.gesture.index, g_app.gesture.candidates.size());
}

// --- Tray ------------------------------------------------------------------

void ShowTrayMenu(HWND hwnd, POINT screenPt) {
    HMENU menu = ::CreatePopupMenu();
    if (!menu) return;

    ::AppendMenuW(menu, MF_STRING | MF_DISABLED | MF_GRAYED, 0, L"MacTab " MACTAB_VERSION_W);
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING | (hotkey::IsRunning() ? 0u : MF_DISABLED),
                  IDM_TRAY_RELOAD_HOOK, L"Reload keyboard hook");
    ::AppendMenuW(menu, MF_STRING, IDM_TRAY_OPEN_LOG, L"Open diagnostics log");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, IDM_TRAY_QUIT, L"Quit MacTab");

    ::SetForegroundWindow(hwnd);
    ::TrackPopupMenuEx(menu, TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_TOPALIGN,
                       screenPt.x, screenPt.y, hwnd, nullptr);
    ::PostMessageW(hwnd, WM_NULL, 0, 0);

    ::DestroyMenu(menu);
}

void OpenDiagnosticsLog(HWND owner) {
    if (!diag::Enabled() || diag::LogPath().empty()) {
        ::MessageBoxW(owner,
                      L"Diagnostics logging is off for this session.\n\n"
                      L"Quit MacTab and relaunch it with the --diag flag to record a log.",
                      L"MacTab", MB_OK | MB_ICONINFORMATION);
        return;
    }
    ::ShellExecuteW(owner, L"open", diag::LogPath().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// --- Shutdown --------------------------------------------------------------

// Teardown must be idempotent: it can be reached through the tray menu, a
// WM_CLOSE from the installer, or WM_ENDSESSION at logoff.
void ShutdownSubsystems() {
    if (g_app.shuttingDown) return;
    g_app.shuttingDown = true;

    hotkey::Stop();
    foreground::Stop();

    if (g_app.wtsRegistered && g_app.host) {
        ::WTSUnRegisterSessionNotification(g_app.host);
        g_app.wtsRegistered = false;
    }

    g_app.tray.Destroy();
}

LRESULT CALLBACK HostWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // Registered messages carry runtime values and cannot appear in a switch.
    if (msg == g_msgTaskbarCreated && g_msgTaskbarCreated != 0) {
        g_app.tray.Readd();
        return 0;
    }
    if (msg == g_msgRequestQuit && g_msgRequestQuit != 0) {
        MACTAB_DIAG("host: quit requested by another process");
        ::DestroyWindow(hwnd);
        return 0;
    }

    switch (msg) {
    // --- Switcher gesture, posted from the hook thread ---------------------
    case hotkey::WM_MACTAB_BEGIN:
        BeginGesture(wParam != 0);
        return 0;

    case hotkey::WM_MACTAB_SELECT:
        // Direction was posted as a signed value; round-trip through INT_PTR
        // so the -1 survives WPARAM's unsignedness.
        AdvanceSelection(static_cast<int>(static_cast<INT_PTR>(wParam)));
        return 0;

    case hotkey::WM_MACTAB_REVEAL:
        RevealPanel();
        return 0;

    case hotkey::WM_MACTAB_COMMIT:
        CommitGesture(static_cast<WORD>(wParam));
        return 0;

    case hotkey::WM_MACTAB_CANCEL:
        CancelGesture(static_cast<WORD>(wParam));
        return 0;

    case hotkey::WM_MACTAB_ACTION:
        // M6 implements these. Logged now so the key routing can be verified
        // independently of the actions themselves.
        MACTAB_DIAG("gesture: action key 0x%02X (not implemented yet)",
                    static_cast<unsigned>(wParam));
        return 0;

    // --- Tray ---------------------------------------------------------------
    case WM_MACTAB_TRAY: {
        const UINT event = LOWORD(lParam);
        if (event == WM_CONTEXTMENU || event == NIN_SELECT || event == WM_LBUTTONUP) {
            POINT pt{ static_cast<LONG>(GET_X_LPARAM(wParam)),
                      static_cast<LONG>(GET_Y_LPARAM(wParam)) };
            ShowTrayMenu(hwnd, pt);
        }
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDM_TRAY_RELOAD_HOOK:
            // Low-level hooks are last-installed-first-served and there is no
            // notification when another tool takes priority from us, so
            // reinstalling is the only recovery and has to be user-driven.
            if (hotkey::Reload())
                g_app.tray.ShowBalloon(L"MacTab", L"Keyboard hook reinstalled.");
            else
                g_app.tray.ShowBalloon(L"MacTab", L"Could not reinstall the keyboard hook.");
            return 0;

        case IDM_TRAY_OPEN_LOG:
            OpenDiagnosticsLog(hwnd);
            return 0;

        case IDM_TRAY_QUIT:
            MACTAB_DIAG("host: quit selected from tray menu");
            ::DestroyWindow(hwnd);
            return 0;

        default:
            break;
        }
        break;

    // --- Session state ------------------------------------------------------
    case WM_WTSSESSION_CHANGE:
        // The secure desktop (lock screen, UAC) means we may never see the Alt
        // release for a gesture that was in flight, which would leave the state
        // machine stuck believing Alt is held.
        if (wParam == WTS_SESSION_LOCK || wParam == WTS_SESSION_UNLOCK ||
            wParam == WTS_CONSOLE_DISCONNECT || wParam == WTS_REMOTE_DISCONNECT) {
            MACTAB_DIAG("session: change %llu, aborting any in-flight gesture",
                        static_cast<unsigned long long>(wParam));
            hotkey::AbortGesture();
        }
        return 0;

    // --- Shutdown contract --------------------------------------------------
    // Restart Manager (used by the installer to upgrade over a running copy)
    // sends WM_QUERYENDSESSION with ENDSESSION_CLOSEAPP, then WM_ENDSESSION,
    // then WM_CLOSE, and force-kills anything still alive after its timeout.
    // Answering all three is what lets an upgrade replace the exe without
    // killing us mid-flight and leaking the tray icon.
    case WM_QUERYENDSESSION:
        // Agree to exit, but do not tear down yet — the session may still be
        // cancelled by another application.
        return TRUE;

    case WM_ENDSESSION:
        if (wParam) {
            MACTAB_DIAG("host: WM_ENDSESSION, shutting down");
            ShutdownSubsystems();
            diag::Shutdown();
        }
        return 0;

    case WM_CLOSE:
        MACTAB_DIAG("host: WM_CLOSE received");
        ::DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        ShutdownSubsystems();
        ::PostQuitMessage(0);
        return 0;

    default:
        break;
    }

    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool ClaimSingleInstance() {
    const HANDLE mutex = ::CreateMutexW(nullptr, TRUE, kSingleInstanceMutex);
    if (!mutex) return true;   // cannot tell; better to run than to refuse

    if (::GetLastError() == ERROR_ALREADY_EXISTS) {
        ::CloseHandle(mutex);
        return false;
    }
    return true;   // handle intentionally leaked; the OS releases it on exit
}

} // namespace
} // namespace mactab

int APIENTRY wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE, _In_ LPWSTR cmdLine, _In_ int) {
    using namespace mactab;

    // Deliberately no CoInitializeEx here.
    //
    // The Composition layer (M3) creates its DispatcherQueueController on this
    // thread with DQTAT_COM_ASTA, which initialises the apartment itself and
    // fails if the thread is already in an STA. Shell APIs that need COM run on
    // the icon worker thread, which initialises its own apartment.

    g_app.instance      = instance;
    g_app.diagRequested = HasFlag(cmdLine, L"--diag");

    diag::Init(g_app.diagRequested);
    MACTAB_DIAG("boot: command line \"%s\"", ToUtf8(cmdLine ? cmdLine : L"").c_str());

    if (!ClaimSingleInstance()) {
        MACTAB_DIAG("boot: another instance holds the session mutex, exiting");
        ::MessageBoxW(nullptr,
                      L"MacTab is already running.\n\nLook for it in the notification area.",
                      L"MacTab", MB_OK | MB_ICONINFORMATION);
        diag::Shutdown();
        return 0;
    }

    g_msgRequestQuit    = ::RegisterWindowMessageW(kQuitMessageName);
    g_msgTaskbarCreated = Tray::TaskbarCreatedMessage();

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = HostWndProc;
    wc.hInstance     = instance;
    wc.lpszClassName = kHostWindowClass;
    wc.hIcon         = ::LoadIconW(instance, MAKEINTRESOURCEW(IDI_APPICON));

    if (!::RegisterClassExW(&wc)) {
        MACTAB_FAIL("boot: RegisterClassEx failed (err %lu)", ::GetLastError());
        diag::Shutdown();
        return 1;
    }

    // A real (never-shown) top-level window rather than HWND_MESSAGE:
    // message-only windows cannot take foreground, and TrackPopupMenu needs a
    // foregroundable owner or the menu will not dismiss.
    g_app.host = ::CreateWindowExW(WS_EX_TOOLWINDOW, kHostWindowClass, L"MacTab",
                                   WS_POPUP, 0, 0, 0, 0,
                                   nullptr, nullptr, instance, nullptr);
    if (!g_app.host) {
        MACTAB_FAIL("boot: CreateWindowEx failed (err %lu)", ::GetLastError());
        diag::Shutdown();
        return 1;
    }

    if (!g_app.tray.Create(instance, g_app.host, WM_MACTAB_TRAY, IDI_APPICON,
                           L"MacTab " MACTAB_VERSION_W))
        MACTAB_WARN("boot: tray icon unavailable; continuing headless");

    // Lock/unlock notifications, so a gesture interrupted by the secure desktop
    // does not leave the state machine stuck.
    g_app.wtsRegistered =
        ::WTSRegisterSessionNotification(g_app.host, NOTIFY_FOR_THIS_SESSION) != FALSE;
    if (!g_app.wtsRegistered)
        MACTAB_WARN("boot: WTSRegisterSessionNotification failed (err %lu)", ::GetLastError());

    if (!foreground::Start())
        MACTAB_WARN("boot: MRU tracking unavailable; switching will be unordered");

    hotkey::Options hotkeyOptions{};
    if (!hotkey::Start(g_app.host, hotkeyOptions)) {
        ::MessageBoxW(nullptr,
                      L"MacTab could not install its keyboard hook, so Alt+Tab cannot "
                      L"be intercepted.\n\n"
                      L"This usually means another switcher or a security product is "
                      L"blocking it. Run with --diag for details.",
                      L"MacTab", MB_OK | MB_ICONERROR);
        MACTAB_FAIL("boot: keyboard hook unavailable, exiting");
        ::DestroyWindow(g_app.host);
        diag::Shutdown();
        return 1;
    }

    MACTAB_DIAG("boot: ready in %.2f ms", NowMs());

    MSG msg;
    BOOL rc;
    while ((rc = ::GetMessageW(&msg, nullptr, 0, 0)) != 0) {
        if (rc == -1) {
            MACTAB_FAIL("loop: GetMessage failed (err %lu)", ::GetLastError());
            break;
        }
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    ShutdownSubsystems();
    MACTAB_DIAG("exit: message loop ended");
    diag::Shutdown();
    return static_cast<int>(msg.wParam);
}
