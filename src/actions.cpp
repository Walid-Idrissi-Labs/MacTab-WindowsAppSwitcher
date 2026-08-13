#include "pch.h"
#include "actions.h"
#include "common.h"
#include "diag.h"

namespace mactab {
namespace {

// PostMessage, never SendMessage: an app showing a modal save prompt would
// block us for as long as the prompt is up, and we are on the UI thread.
void RequestClose(HWND window) {
    if (!window || !::IsWindow(window)) return;

    if (!::PostMessageW(window, WM_CLOSE, 0, 0)) {
        // Almost always UIPI: an unelevated process cannot post to a
        // higher-integrity window.
        MACTAB_WARN("actions: PostMessage(WM_CLOSE) to %p refused (err %lu)",
                    static_cast<void*>(window), ::GetLastError());
    }
}

} // namespace

void QuitApp(const SwitcherApp& app) {
    MACTAB_DIAG("actions: quit %s (%zu window(s))",
                ToUtf8(app.displayName).c_str(), app.windows.size());

    for (const SwitcherWindow& window : app.windows)
        RequestClose(window.hwnd);
}

void CloseWindow(const SwitcherApp& app, size_t index) {
    if (index >= app.windows.size()) return;

    MACTAB_DIAG("actions: close window %zu of %zu of %s", index + 1,
                app.windows.size(), ToUtf8(app.displayName).c_str());
    RequestClose(app.windows[index].hwnd);
}

void HideApp(const SwitcherApp& app) {
    MACTAB_DIAG("actions: minimise all windows of %s", ToUtf8(app.displayName).c_str());

    for (const SwitcherWindow& window : app.windows) {
        if (!window.hwnd || !::IsWindow(window.hwnd)) continue;
        // SW_MINIMIZE rather than SW_FORCEMINIMIZE: the latter is for hung
        // windows and skips the animation, which looks broken on a healthy app.
        //
        // Async because ShowWindow on another thread's window is synchronous,
        // and this runs on the UI thread during a gesture. One wedged window in
        // the app would otherwise freeze the panel and, worse, delay the pending
        // commit that releases Alt, so the user would be typing chords until the
        // hang cleared.
        ::ShowWindowAsync(window.hwnd, SW_MINIMIZE);
    }
}

} // namespace mactab
