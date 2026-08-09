#include "pch.h"
#include "hotkey.h"
#include "common.h"
#include "diag.h"

namespace mactab::hotkey {
namespace {

// Tag on every key event we synthesise. We must recognise our own injected
// input and pass it straight through, or we would re-enter the state machine
// with our own Alt-up and loop.
//
// Deliberately NOT keyed off LLKHF_INJECTED: that flag is set by *any*
// injector, so AutoHotkey, PowerToys Keyboard Manager and similar remappers
// would be skipped too. Their input should be treated as real; only ours is
// special.
constexpr ULONG_PTR kInjectionTag = 0x4D414354;   // 'MACT'

// Thread messages to the hook thread. Sent with PostThreadMessage, so they
// arrive with msg.hwnd == nullptr and are handled in the loop, not dispatched.
constexpr UINT TM_STOP   = WM_USER + 1;
constexpr UINT TM_RELOAD = WM_USER + 2;
constexpr UINT TM_ABORT  = WM_USER + 3;

enum class State {
    Idle,     // no gesture in flight
    Armed,    // Tab seen, reveal timer running, panel still hidden
    Panel,    // reveal delay elapsed, panel is up
};

// --- Shared between the UI thread and the hook thread -----------------------
// Only these three are touched from both. Everything else below is hook-thread
// only and needs no synchronisation, because the hook callback and WM_TIMER are
// serialised on that single thread.
HANDLE  g_thread     = nullptr;
DWORD   g_threadId   = 0;
HWND    g_uiWindow   = nullptr;
Options g_options{};
volatile LONG g_running = 0;

// --- Hook-thread state ------------------------------------------------------
HHOOK    g_hook          = nullptr;
State    g_state         = State::Idle;
UINT_PTR g_revealTimer   = 0;
WORD     g_gestureAltVk  = 0;   // which Alt key opened the gesture

bool KeyDown(int vk) {
    return (::GetAsyncKeyState(vk) & 0x8000) != 0;
}

void KillRevealTimer() {
    if (g_revealTimer) {
        // hwnd == nullptr timers are killed with a nullptr window too.
        ::KillTimer(nullptr, g_revealTimer);
        g_revealTimer = 0;
    }
}

void PostToUi(UINT message, WPARAM wParam = 0) {
    if (g_uiWindow) ::PostMessageW(g_uiWindow, message, wParam, 0);
}

void EndGesture(UINT message) {
    KillRevealTimer();
    PostToUi(message, g_gestureAltVk);
    g_state        = State::Idle;
    g_gestureAltVk = 0;
}

// Is the Alt that should drive the switcher currently held?
//
// leftAltOnly deliberately ignores LLKHF_ALTDOWN, because AltGr (Ctrl +
// RightAlt) sets it. Combined with the Ctrl guard at the call site, this keeps
// the switcher out of the way of French/Arabic layout typing.
bool SwitcherAltHeld() {
    return g_options.leftAltOnly ? KeyDown(VK_LMENU)
                                 : (KeyDown(VK_LMENU) || KeyDown(VK_RMENU));
}

bool IsAltKey(DWORD vk) {
    return vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU;
}

LRESULT CALLBACK LowLevelKeyboardProc(int code, WPARAM wParam, LPARAM lParam) {
    // Everything in here runs under a hard deadline. No allocation, no I/O, no
    // locks, no SendMessage. Exceeding LowLevelHooksTimeout means being
    // silently unhooked with no way to detect it.
    if (code != HC_ACTION)
        return ::CallNextHookEx(nullptr, code, wParam, lParam);

    const auto* key = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);

    // Our own synthetic keys: pass through untouched and never re-process.
    if (key->dwExtraInfo == kInjectionTag)
        return ::CallNextHookEx(nullptr, code, wParam, lParam);

    const bool down = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
    const bool up   = (wParam == WM_KEYUP   || wParam == WM_SYSKEYUP);
    const DWORD vk  = key->vkCode;

    if (g_state == State::Idle) {
        // Gesture opens on Alt+Tab. Note Tab arrives as WM_SYSKEYDOWN because
        // Alt is down, which is why `down` tests both message forms.
        //
        // Alt-down itself is never swallowed: at Alt-down time we cannot know a
        // gesture is coming, and eating it globally would break menu access and
        // every Alt mnemonic in every app.
        if (down && vk == VK_TAB && SwitcherAltHeld() && !KeyDown(VK_CONTROL)) {
            g_state        = State::Armed;
            g_gestureAltVk = KeyDown(VK_LMENU) ? VK_LMENU : VK_RMENU;

            PostToUi(WM_MACTAB_BEGIN, KeyDown(VK_SHIFT) ? 1u : 0u);

            // One-shot: killed in the WM_TIMER handler. Win32 has no one-shot
            // flag. This is the only timer in the process and it exists only
            // between the first Tab and the commit, so idle cost stays at zero.
            g_revealTimer = ::SetTimer(nullptr, 0, g_options.revealDelayMs, nullptr);
            return 1;
        }
        return ::CallNextHookEx(nullptr, code, wParam, lParam);
    }

    // --- A gesture is in flight (Armed or Panel) ---------------------------

    // Alt released: commit.
    if (up && IsAltKey(vk)) {
        // Record the exact VK we are swallowing so the UI thread can inject a
        // matching key-up. Symmetry matters — releasing VK_MENU when the user
        // held VK_LMENU can leave the async state inconsistent.
        g_gestureAltVk = static_cast<WORD>(vk);
        EndGesture(WM_MACTAB_COMMIT);
        return 1;   // swallow the real Alt-up; a replacement is injected instead
    }

    if (down) {
        switch (vk) {
        case VK_TAB:
        case VK_RIGHT:
            PostToUi(WM_MACTAB_SELECT, static_cast<WPARAM>(KeyDown(VK_SHIFT) ? -1 : 1));
            return 1;

        case VK_LEFT:
            PostToUi(WM_MACTAB_SELECT, static_cast<WPARAM>(-1));
            return 1;

        case VK_ESCAPE:
            EndGesture(WM_MACTAB_CANCEL);
            return 1;

        case 'Q':   // quit app
        case 'W':   // close window
        case 'H':   // minimise all windows of the app
        case VK_DOWN:      // expand the app's windows
        case VK_OEM_3:     // backquote: cycle windows within the app
            // Only meaningful once the panel is visible. Before that the user
            // is mid-quick-switch and these should not fire.
            if (g_state == State::Panel) {
                PostToUi(WM_MACTAB_ACTION, static_cast<WPARAM>(vk));
                return 1;
            }
            return 1;   // swallow regardless; do not leak into the background app

        default:
            break;
        }
    }

    // Modifiers must keep flowing so the system's own Shift/Ctrl/Alt state
    // stays accurate while the gesture runs.
    if (IsAltKey(vk) || vk == VK_SHIFT  || vk == VK_LSHIFT || vk == VK_RSHIFT ||
                        vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL)
        return ::CallNextHookEx(nullptr, code, wParam, lParam);

    // While the switcher owns the keyboard, everything else is sunk rather than
    // leaked to whatever app happens to be behind the panel.
    return 1;
}

void HandleRevealTimer() {
    KillRevealTimer();
    if (g_state != State::Armed) return;

    g_state = State::Panel;
    PostToUi(WM_MACTAB_REVEAL);
}

bool InstallHook() {
    g_hook = ::SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc,
                                 ::GetModuleHandleW(nullptr), 0);
    if (!g_hook) {
        MACTAB_FAIL("hotkey: SetWindowsHookEx(WH_KEYBOARD_LL) failed (err %lu)",
                    ::GetLastError());
        return false;
    }
    MACTAB_DIAG("hotkey: hook installed on thread %lu", ::GetCurrentThreadId());
    return true;
}

void RemoveHook() {
    if (g_hook) {
        ::UnhookWindowsHookEx(g_hook);
        g_hook = nullptr;
        MACTAB_DIAG("hotkey: hook removed");
    }
}

struct StartupContext {
    HANDLE ready   = nullptr;
    bool   success = false;
};

DWORD WINAPI HookThreadProc(LPVOID param) {
    auto* ctx = static_cast<StartupContext*>(param);

    // Force the message queue to exist before anyone can PostThreadMessage to
    // us; without this there is a window where posts are silently dropped.
    MSG bootstrap;
    ::PeekMessageW(&bootstrap, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

    // Input latency is what this thread exists for, and it does almost no work.
    ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

    ctx->success = InstallHook();
    ::SetEvent(ctx->ready);          // ctx is invalid after this point
    ctx = nullptr;

    if (!g_hook) return 1;

    MSG msg;
    BOOL rc;
    while ((rc = ::GetMessageW(&msg, nullptr, 0, 0)) != 0) {
        if (rc == -1) {
            MACTAB_FAIL("hotkey: GetMessage failed (err %lu)", ::GetLastError());
            break;
        }

        // Thread messages and hwnd-less timers arrive with msg.hwnd == nullptr
        // and are not dispatched anywhere, so handle them here.
        if (msg.hwnd == nullptr) {
            if (msg.message == WM_TIMER && msg.wParam == g_revealTimer) {
                HandleRevealTimer();
                continue;
            }
            if (msg.message == TM_STOP) {
                break;
            }
            if (msg.message == TM_RELOAD) {
                RemoveHook();
                if (!InstallHook())
                    MACTAB_FAIL("hotkey: reload failed to reinstall the hook");
                continue;
            }
            if (msg.message == TM_ABORT) {
                if (g_state != State::Idle) {
                    MACTAB_DIAG("hotkey: aborting in-flight gesture");
                    EndGesture(WM_MACTAB_CANCEL);
                }
                continue;
            }
        }

        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    KillRevealTimer();
    RemoveHook();
    return 0;
}

} // namespace

bool IsOwnInjection(ULONG_PTR extraInfo) {
    return extraInfo == kInjectionTag;
}

void NeutralizeAlt(WORD altVirtualKey) {
    if (altVirtualKey == 0) altVirtualKey = VK_MENU;

    INPUT input{};
    input.type           = INPUT_KEYBOARD;
    input.ki.wVk         = altVirtualKey;
    input.ki.wScan       = static_cast<WORD>(::MapVirtualKeyW(altVirtualKey, MAPVK_VK_TO_VSC));
    input.ki.dwFlags     = KEYEVENTF_KEYUP;
    input.ki.dwExtraInfo = kInjectionTag;

    if (::SendInput(1, &input, sizeof(INPUT)) != 1)
        MACTAB_WARN("hotkey: SendInput(alt-up) failed (err %lu)", ::GetLastError());
}

bool Start(HWND uiWindow, const Options& options) {
    if (g_thread) return true;

    g_uiWindow = uiWindow;
    g_options  = options;

    StartupContext ctx{};
    ctx.ready = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ctx.ready) {
        MACTAB_FAIL("hotkey: CreateEvent failed (err %lu)", ::GetLastError());
        return false;
    }

    g_thread = ::CreateThread(nullptr, 0, HookThreadProc, &ctx, 0, &g_threadId);
    if (!g_thread) {
        MACTAB_FAIL("hotkey: CreateThread failed (err %lu)", ::GetLastError());
        ::CloseHandle(ctx.ready);
        return false;
    }

    // Block until the hook is installed so callers get a definitive answer.
    ::WaitForSingleObject(ctx.ready, INFINITE);
    const bool ok = ctx.success;
    ::CloseHandle(ctx.ready);

    if (!ok) {
        ::WaitForSingleObject(g_thread, 2000);
        ::CloseHandle(g_thread);
        g_thread   = nullptr;
        g_threadId = 0;
        return false;
    }

    ::InterlockedExchange(&g_running, 1);
    MACTAB_DIAG("hotkey: started (revealDelay %u ms, leftAltOnly %d)",
                g_options.revealDelayMs, g_options.leftAltOnly ? 1 : 0);
    return true;
}

void Stop() {
    if (!g_thread) return;

    ::PostThreadMessageW(g_threadId, TM_STOP, 0, 0);

    if (::WaitForSingleObject(g_thread, 3000) == WAIT_TIMEOUT)
        MACTAB_WARN("hotkey: hook thread did not exit within 3 s");

    ::CloseHandle(g_thread);
    g_thread   = nullptr;
    g_threadId = 0;
    g_uiWindow = nullptr;
    ::InterlockedExchange(&g_running, 0);
    MACTAB_DIAG("hotkey: stopped");
}

bool Reload() {
    if (!g_threadId) return false;
    MACTAB_DIAG("hotkey: reload requested");
    return ::PostThreadMessageW(g_threadId, TM_RELOAD, 0, 0) != 0;
}

void AbortGesture() {
    if (!g_threadId) return;
    ::PostThreadMessageW(g_threadId, TM_ABORT, 0, 0);
}

bool IsRunning() {
    return ::InterlockedCompareExchange(&g_running, 0, 0) != 0;
}

} // namespace mactab::hotkey
