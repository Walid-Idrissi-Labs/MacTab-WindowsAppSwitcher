#include "pch.h"
#include "settings_watch.h"
#include "common.h"
#include "diag.h"

namespace mactab::settings_watch {
namespace {

// Written on the UI thread in Start() before the thread exists, and cleared in
// Stop() after it has been joined, so the watch thread is the only reader and
// never races either write.
HANDLE g_thread   = nullptr;
HANDLE g_change   = nullptr;   // FindFirstChangeNotification, not a real object
HANDLE g_stop     = nullptr;   // manual-reset, created in Start(), set in Stop()
HWND   g_uiWindow = nullptr;

// Everything that reaches this thread is one of two things: the folder changed,
// or we are shutting down. Both are a single wait, which is why there is no
// message loop here the way there is on the hook thread.
DWORD WINAPI WatchThreadProc(LPVOID) {
    // Stop first in the array so that a shutdown racing a save is resolved in
    // favour of the shutdown: WaitForMultipleObjects returns the LOWEST index
    // that is signalled, not the one that was signalled first.
    const HANDLE waits[2] = { g_stop, g_change };

    for (;;) {
        const DWORD result = ::WaitForMultipleObjects(2, waits, FALSE, INFINITE);

        if (result == WAIT_OBJECT_0)
            break;

        if (result != WAIT_OBJECT_0 + 1) {
            MACTAB_FAIL("settings_watch: wait failed (result %lu, err %lu)",
                        result, ::GetLastError());
            break;
        }

        MACTAB_DIAG("settings_watch: settings folder changed, waking the UI thread");
        ::PostMessageW(g_uiWindow, WM_MACTAB_SETTINGS_CHANGED, 0, 0);

        // Re-arm before waiting again. The handle stays signalled until this is
        // called, so skipping it turns the wait into a spin.
        if (!::FindNextChangeNotification(g_change)) {
            MACTAB_FAIL("settings_watch: FindNextChangeNotification failed (err %lu)",
                        ::GetLastError());
            break;
        }
    }

    return 0;
}

} // namespace

bool Start(HWND uiWindow) {
    if (g_thread) return true;
    if (!uiWindow) return false;

    // The folder, not the file. A notification handle is per-directory, and the
    // file the user is editing may be replaced rather than written in place,
    // which would invalidate anything opened on the file itself.
    const std::wstring& dir = AppDataDir();
    if (dir.empty()) {
        MACTAB_WARN("settings_watch: no app data directory, live reload is off");
        return false;
    }

    g_stop = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_stop) {
        MACTAB_FAIL("settings_watch: CreateEvent failed (err %lu)", ::GetLastError());
        return false;
    }

    // FILE_NAME as well as LAST_WRITE and SIZE, because a save is very often not
    // a write at all: Notepad and most programmer's editors write a temporary
    // file and rename it over the target, so the only event the real file
    // produces is a rename. Watching all three costs nothing, since the reload
    // is debounced on the other side anyway.
    g_change = ::FindFirstChangeNotificationW(dir.c_str(), FALSE,
                                              FILE_NOTIFY_CHANGE_LAST_WRITE |
                                              FILE_NOTIFY_CHANGE_SIZE |
                                              FILE_NOTIFY_CHANGE_FILE_NAME);
    if (g_change == INVALID_HANDLE_VALUE) {
        MACTAB_FAIL("settings_watch: FindFirstChangeNotification(%s) failed (err %lu)",
                    ToUtf8(dir).c_str(), ::GetLastError());
        g_change = nullptr;
        ::CloseHandle(g_stop);
        g_stop = nullptr;
        return false;
    }

    g_uiWindow = uiWindow;

    g_thread = ::CreateThread(nullptr, 0, WatchThreadProc, nullptr, 0, nullptr);
    if (!g_thread) {
        MACTAB_FAIL("settings_watch: CreateThread failed (err %lu)", ::GetLastError());
        ::FindCloseChangeNotification(g_change);
        g_change   = nullptr;
        ::CloseHandle(g_stop);
        g_stop     = nullptr;
        g_uiWindow = nullptr;
        return false;
    }

    MACTAB_DIAG("settings_watch: watching %s for edits", ToUtf8(dir).c_str());
    return true;
}

void Stop() {
    if (!g_thread) return;

    ::SetEvent(g_stop);

    const bool joined = ::WaitForSingleObject(g_thread, 3000) == WAIT_OBJECT_0;
    if (!joined)
        MACTAB_WARN("settings_watch: watch thread did not exit within 3 s");

    ::CloseHandle(g_thread);
    g_thread = nullptr;

    // Only released once the thread is known to be gone. Closing a handle that
    // another thread is still waiting on is a use-after-free of a kernel object,
    // and leaking two handles in a case that should never happen, on a path that
    // runs at exit, is the cheaper mistake.
    if (joined) {
        ::FindCloseChangeNotification(g_change);
        g_change = nullptr;
        ::CloseHandle(g_stop);
        g_stop = nullptr;
    }

    g_uiWindow = nullptr;
    MACTAB_DIAG("settings_watch: stopped");
}

} // namespace mactab::settings_watch
