#include "pch.h"
#include "app.h"
#include "common.h"
#include "diag.h"
#include "tray.h"
#include "resource.h"

namespace mactab {
namespace {

// Tray interactions arrive here.
constexpr UINT WM_MACTAB_TRAY = WM_APP + 1;

UINT g_msgRequestQuit    = 0;   // RegisterWindowMessage(kQuitMessageName)
UINT g_msgTaskbarCreated = 0;   // RegisterWindowMessage(L"TaskbarCreated")

struct AppState {
    HINSTANCE instance = nullptr;
    HWND      host     = nullptr;
    Tray      tray;
    bool      diagRequested = false;
};

AppState g_app;

bool HasFlag(const wchar_t* cmdLine, const wchar_t* flag) {
    // Command line is tiny and flags are few; a substring scan is plenty and
    // avoids dragging in CommandLineToArgvW (which allocates and needs shell32
    // to be loaded early).
    return cmdLine && ::StrStrIW(cmdLine, flag) != nullptr;
}

void ShowTrayMenu(HWND hwnd, POINT screenPt) {
    HMENU menu = ::CreatePopupMenu();
    if (!menu) return;

    ::AppendMenuW(menu, MF_STRING | MF_DISABLED | MF_GRAYED, 0, L"MacTab " MACTAB_VERSION_W);
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, IDM_TRAY_OPEN_LOG, L"Open diagnostics log");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, IDM_TRAY_QUIT, L"Quit MacTab");

    // Menus only dismiss correctly when their owner is foreground, and the
    // trailing WM_NULL is the documented workaround for the menu sticking
    // around after a click elsewhere.
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

LRESULT CALLBACK HostWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // Registered messages are runtime values, so they cannot appear in the
    // switch below.
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
    case WM_MACTAB_TRAY: {
        // NOTIFYICON_VERSION_4 packs the event id in LOWORD(lParam) and the
        // anchor point in wParam.
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

    case WM_ENDSESSION:
        // Logoff/shutdown. Tear down explicitly so the tray icon does not
        // linger as a ghost in the notification area.
        if (wParam) {
            g_app.tray.Destroy();
            diag::Shutdown();
        }
        return 0;

    case WM_DESTROY:
        g_app.tray.Destroy();
        ::PostQuitMessage(0);
        return 0;

    default:
        break;
    }

    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

// Returns false if another instance already owns the session mutex. The mutex
// handle is intentionally leaked for the process lifetime — the OS releases it
// on exit, and holding it in a global would just be ceremony.
bool ClaimSingleInstance() {
    const HANDLE mutex = ::CreateMutexW(nullptr, TRUE, kSingleInstanceMutex);
    if (!mutex) return true;   // cannot tell; better to run than to refuse

    if (::GetLastError() == ERROR_ALREADY_EXISTS) {
        ::CloseHandle(mutex);
        return false;
    }
    return true;
}

void NudgeExistingInstance() {
    const HWND existing = ::FindWindowW(kHostWindowClass, nullptr);
    if (!existing) return;

    // Nothing to show — just let the user know it is already running.
    ::MessageBoxW(nullptr,
                  L"MacTab is already running.\n\nLook for it in the notification area.",
                  L"MacTab", MB_OK | MB_ICONINFORMATION);
}

} // namespace
} // namespace mactab

int APIENTRY wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE, _In_ LPWSTR cmdLine, _In_ int) {
    using namespace mactab;

    // Deliberately no CoInitializeEx here.
    //
    // The Composition layer (M3) creates its DispatcherQueueController on this
    // thread with DQTAT_COM_ASTA, which initialises the apartment itself. If we
    // had already put the thread in an STA, that call would fail. Shell APIs
    // that genuinely need COM run on the icon worker thread, which initialises
    // its own apartment.

    g_app.instance      = instance;
    g_app.diagRequested = HasFlag(cmdLine, L"--diag");

    diag::Init(g_app.diagRequested);
    MACTAB_DIAG("boot: command line \"%s\"", ToUtf8(cmdLine ? cmdLine : L"").c_str());

    if (!ClaimSingleInstance()) {
        MACTAB_DIAG("boot: another instance holds the session mutex, exiting");
        NudgeExistingInstance();
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

    // A real (never-shown) top-level window rather than HWND_MESSAGE: message-only
    // windows cannot be brought foreground, and TrackPopupMenu needs a
    // foregroundable owner to dismiss correctly.
    g_app.host = ::CreateWindowExW(WS_EX_TOOLWINDOW, kHostWindowClass, L"MacTab",
                                   WS_POPUP, 0, 0, 0, 0,
                                   nullptr, nullptr, instance, nullptr);
    if (!g_app.host) {
        MACTAB_FAIL("boot: CreateWindowEx failed (err %lu)", ::GetLastError());
        diag::Shutdown();
        return 1;
    }

    if (!g_app.tray.Create(instance, g_app.host, WM_MACTAB_TRAY, IDI_APPICON,
                           L"MacTab " MACTAB_VERSION_W)) {
        // Not fatal: the switcher still works, the user just has no menu.
        MACTAB_WARN("boot: tray icon unavailable; continuing headless");
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

    MACTAB_DIAG("exit: message loop ended");
    diag::Shutdown();
    return static_cast<int>(msg.wParam);
}
