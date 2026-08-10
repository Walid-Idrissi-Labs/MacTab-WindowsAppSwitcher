#include "pch.h"
#include "foreground_history.h"
#include "diag.h"

namespace mactab::foreground {
namespace {

// Deep enough that the switcher never runs out of candidates, shallow enough
// that the linear scans below stay trivially cheap.
constexpr size_t kMaxTracked = 64;

HWINEVENTHOOK    g_eventHook = nullptr;
std::deque<HWND> g_mru;   // most recent at the front

// Cheap pre-filter. This is NOT the real Alt-Tab eligibility test; that needs
// cloaking checks, owner-chain walking and UWP unwrapping, and it lands in M2.
// Here we only need to keep obvious noise (tooltips, the shell, our own
// windows) out of the history.
bool LooksSwitchable(HWND hwnd) {
    if (!hwnd || !::IsWindowVisible(hwnd)) return false;

    // Only ever track root windows.
    if (::GetAncestor(hwnd, GA_ROOT) != hwnd) return false;

    const LONG_PTR exStyle = ::GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (exStyle & WS_EX_TOOLWINDOW) return false;
    if (exStyle & WS_EX_NOACTIVATE) return false;

    // A zero-length title is almost always an invisible helper window.
    if (::GetWindowTextLengthW(hwnd) == 0) return false;

    return true;
}

void Remember(HWND hwnd) {
    if (!LooksSwitchable(hwnd)) return;

    const auto it = std::find(g_mru.begin(), g_mru.end(), hwnd);
    if (it != g_mru.end())
        g_mru.erase(it);

    g_mru.push_front(hwnd);

    if (g_mru.size() > kMaxTracked)
        g_mru.pop_back();
}

void CALLBACK EventProc(HWINEVENTHOOK, DWORD event, HWND hwnd,
                        LONG idObject, LONG idChild, DWORD, DWORD) {
    // OBJID_WINDOW/CHILDID_SELF only: foreground events also fire for
    // accessibility sub-objects we do not care about.
    if (event != EVENT_SYSTEM_FOREGROUND) return;
    if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;

    Remember(hwnd);
}

} // namespace

bool Start() {
    // WINEVENT_SKIPOWNPROCESS keeps our own panel out of the history, which
    // would otherwise become the most-recent window every time it is shown.
    g_eventHook = ::SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
        nullptr, EventProc, 0, 0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    if (!g_eventHook) {
        MACTAB_FAIL("foreground: SetWinEventHook failed (err %lu)", ::GetLastError());
        return false;
    }

    // Seed with whatever is focused right now, so the very first Alt+Tab of a
    // session has something to switch to.
    Remember(::GetForegroundWindow());

    MACTAB_DIAG("foreground: MRU tracking started");
    return true;
}

void Stop() {
    if (g_eventHook) {
        ::UnhookWinEvent(g_eventHook);
        g_eventHook = nullptr;
    }
    g_mru.clear();
}

std::vector<HWND> Snapshot() {
    std::vector<HWND> live;
    live.reserve(g_mru.size());

    // Prune closed windows in the same pass that builds the result.
    for (auto it = g_mru.begin(); it != g_mru.end();) {
        if (::IsWindow(*it) && ::IsWindowVisible(*it)) {
            live.push_back(*it);
            ++it;
        } else {
            it = g_mru.erase(it);
        }
    }
    return live;
}

HWND Previous() {
    const std::vector<HWND> live = Snapshot();
    if (live.empty()) return nullptr;

    const HWND current = ::GetForegroundWindow();
    for (HWND hwnd : live) {
        if (hwnd != current) return hwnd;
    }
    return nullptr;
}

} // namespace mactab::foreground
