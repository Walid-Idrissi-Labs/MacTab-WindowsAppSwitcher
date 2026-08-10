#include "pch.h"
#include "window_model.h"
#include "app_identity.h"
#include "common.h"
#include "diag.h"
#include "config.h"
#include "foreground_history.h"

namespace mactab {
namespace {

// Rank given to a window the MRU tracker has never seen. Larger than any real
// rank, so unseen windows sort after seen ones but keep their relative Z-order.
constexpr int kUnrankedBase = 1'000'000;

std::wstring ClassNameOf(HWND hwnd) {
    wchar_t buffer[128] = L"";
    const int n = ::GetClassNameW(hwnd, buffer, ARRAYSIZE(buffer));
    return std::wstring(buffer, static_cast<size_t>(n > 0 ? n : 0));
}

std::wstring TitleOf(HWND hwnd) {
    const int length = ::GetWindowTextLengthW(hwnd);
    if (length <= 0) return {};

    std::wstring title(static_cast<size_t>(length) + 1, L'\0');
    const int copied = ::GetWindowTextW(hwnd, title.data(), length + 1);
    title.resize(static_cast<size_t>(copied > 0 ? copied : 0));
    return title;
}

// Cloaked windows are composited but deliberately not shown: UWP ghosts for
// apps that are not running, and, importantly, every window living on a
// different virtual desktop. This single check is what keeps the switcher
// scoped to the current desktop.
bool IsCloaked(HWND hwnd) {
    DWORD cloaked = 0;
    const HRESULT hr = ::DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    return SUCCEEDED(hr) && cloaked != 0;
}

// Where a window visually is, which is not what GetWindowRect returns.
//
// Since Vista a resizable window's rect includes an invisible border used only
// for grabbing the edge, around 7 physical pixels a side at 100% DPI. It does
// not draw anything, so a Mission Control tile built from GetWindowRect is
// consistently too wide and too tall, and worse, by a fixed amount, which reads
// as small windows having thicker margins than large ones.
// DWMWA_EXTENDED_FRAME_BOUNDS is the rect the window actually occupies.
//
// A minimized window has no meaningful rect at all; GetWindowRect gives it an
// off-screen placeholder well outside the desktop. Its restored placement is
// the only sensible geometry, and that is what GetWindowPlacement carries.
RECT BoundsOf(HWND hwnd, bool minimized) {
    RECT bounds{};

    if (minimized) {
        WINDOWPLACEMENT placement{};
        placement.length = sizeof(placement);
        if (::GetWindowPlacement(hwnd, &placement))
            return placement.rcNormalPosition;
    }

    if (SUCCEEDED(::DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS,
                                          &bounds, sizeof(bounds))) &&
        bounds.right > bounds.left && bounds.bottom > bounds.top) {
        return bounds;
    }

    ::GetWindowRect(hwnd, &bounds);
    return bounds;
}

bool HasCoreWindowChild(HWND frame) {
    HWND found = nullptr;
    ::EnumChildWindows(
        frame,
        [](HWND child, LPARAM param) -> BOOL {
            if (ClassNameOf(child) == L"Windows.UI.Core.CoreWindow") {
                *reinterpret_cast<HWND*>(param) = child;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&found));
    return found != nullptr;
}

// Shell furniture that is visible and titled but is not an application.
bool IsShellWindow(const std::wstring& className) {
    return className == L"Progman"              // desktop
        || className == L"WorkerW"              // desktop wallpaper host
        || className == L"Shell_TrayWnd"        // taskbar
        || className == L"Shell_SecondaryTrayWnd"
        || className == L"Windows.UI.Core.CoreWindow"  // Start, Search, Action Center
        || className == L"XamlExplorerHostIslandWindow"
        || className == L"MultitaskingViewFrame";
}

} // namespace

bool IsSwitcherWindow(HWND hwnd) {
    if (!hwnd || !::IsWindowVisible(hwnd))
        return false;

    // Only ever consider root windows; child and owned-popup handling is below.
    if (::GetAncestor(hwnd, GA_ROOT) != hwnd)
        return false;

    if (IsCloaked(hwnd))
        return false;

    const LONG_PTR exStyle = ::GetWindowLongPtrW(hwnd, GWL_EXSTYLE);

    if (exStyle & WS_EX_NOACTIVATE)
        return false;

    // Floating palettes and toolbars are excluded unless the app explicitly
    // asks to appear in the switcher.
    if ((exStyle & WS_EX_TOOLWINDOW) && !(exStyle & WS_EX_APPWINDOW))
        return false;

    // Owned windows (dialogs, tool palettes) are represented by their owner in
    // the switcher, not listed separately, unless they opt in.
    const HWND owner = ::GetWindow(hwnd, GW_OWNER);
    if (owner && !(exStyle & WS_EX_APPWINDOW))
        return false;

    // Untitled windows are essentially always invisible helpers.
    if (::GetWindowTextLengthW(hwnd) == 0)
        return false;

    RECT rect{};
    if (::GetWindowRect(hwnd, &rect) &&
        (rect.right - rect.left <= 0 || rect.bottom - rect.top <= 0))
        return false;

    const std::wstring className = ClassNameOf(hwnd);
    if (IsShellWindow(className))
        return false;

    // An ApplicationFrameWindow with no CoreWindow child is a shell placeholder
    // for a Store app that is not actually running. Its process is
    // ApplicationFrameHost, so leaving it in would also collapse every such
    // placeholder into one bogus "app".
    if (className == L"ApplicationFrameWindow" && !HasCoreWindowChild(hwnd))
        return false;

    // Never list ourselves.
    DWORD pid = 0;
    ::GetWindowThreadProcessId(hwnd, &pid);
    if (pid == ::GetCurrentProcessId())
        return false;

    return true;
}

namespace {

struct Enumeration {
    std::vector<HWND> windows;
};

BOOL CALLBACK EnumProc(HWND hwnd, LPARAM param) {
    if (IsSwitcherWindow(hwnd))
        reinterpret_cast<Enumeration*>(param)->windows.push_back(hwnd);
    return TRUE;
}

} // namespace

std::vector<SwitcherApp> BuildSwitcherList() {
    MACTAB_DIAG_TIMER("window_model: BuildSwitcherList");

    // EnumWindows walks in Z-order, top first, which is a sane fallback
    // ordering for anything the MRU tracker has not seen yet.
    Enumeration enumeration;
    ::EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&enumeration));

    // MRU rank lookup. foreground::Snapshot() is already most-recent-first.
    std::unordered_map<HWND, int> rank;
    {
        const std::vector<HWND> mru = foreground::Snapshot();
        rank.reserve(mru.size());
        for (size_t i = 0; i < mru.size(); ++i)
            rank.emplace(mru[i], static_cast<int>(i));
    }

    auto rankOf = [&](HWND hwnd, size_t zOrderIndex) {
        const auto it = rank.find(hwnd);
        return (it != rank.end()) ? it->second
                                  : kUnrankedBase + static_cast<int>(zOrderIndex);
    };

    struct Group {
        SwitcherApp app;
        int         bestRank = INT_MAX;
        // Parallel to app.windows; used only while sorting.
        std::vector<int> windowRanks;
    };

    std::vector<Group> groups;
    std::unordered_map<std::wstring, size_t> indexByKey;

    for (size_t z = 0; z < enumeration.windows.size(); ++z) {
        HWND hwnd = enumeration.windows[z];

        const AppIdentity* identity = ResolveApp(hwnd);
        if (!identity) continue;   // unreadable process, e.g. elevated

        SwitcherWindow window;
        window.hwnd      = hwnd;
        window.title     = TitleOf(hwnd);
        window.minimized = ::IsIconic(hwnd) != FALSE;
        window.bounds    = BoundsOf(hwnd, window.minimized);

        const int windowRank = rankOf(hwnd, z);

        const auto existing = indexByKey.find(identity->key);
        if (existing == indexByKey.end()) {
            Group group;
            group.app.key         = identity->key;
            group.app.displayName = identity->displayName;
            group.app.exePath     = identity->exePath;
            group.app.aumid       = identity->aumid;
            group.app.packaged    = identity->packaged;

            // Packaged apps have no friendly name until the icon worker
            // resolves it from the package manifest; the window title is a
            // decent stand-in and is never empty here (untitled windows are
            // filtered out above).
            if (group.app.displayName.empty())
                group.app.displayName = window.title;

            group.app.windows.push_back(std::move(window));
            group.windowRanks.push_back(windowRank);
            group.bestRank = windowRank;

            indexByKey.emplace(identity->key, groups.size());
            groups.push_back(std::move(group));
        } else {
            Group& group = groups[existing->second];
            group.app.windows.push_back(std::move(window));
            group.windowRanks.push_back(windowRank);
            group.bestRank = (std::min)(group.bestRank, windowRank);
        }
    }

    // Order windows inside each app, then the apps themselves. An app's
    // position is set by its most recently used window.
    for (Group& group : groups) {
        std::vector<size_t> order(group.app.windows.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;

        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            return group.windowRanks[a] < group.windowRanks[b];
        });

        std::vector<SwitcherWindow> sorted;
        sorted.reserve(order.size());
        for (size_t i : order)
            sorted.push_back(std::move(group.app.windows[i]));
        group.app.windows = std::move(sorted);
    }

    std::stable_sort(groups.begin(), groups.end(),
                     [](const Group& a, const Group& b) { return a.bestRank < b.bestRank; });

    std::vector<SwitcherApp> result;
    result.reserve(groups.size());
    for (Group& group : groups)
        result.push_back(std::move(group.app));

    // GroupByApp=0 asks for Windows' own per-window behaviour with macOS
    // styling. Splitting here rather than skipping the grouping entirely keeps
    // one code path: the ordering, identity and icon lookup all still work off
    // the app the window belongs to.
    if (!config::Current().groupByApp) {
        std::vector<SwitcherApp> split;
        for (SwitcherApp& app : result) {
            for (SwitcherWindow& window : app.windows) {
                SwitcherApp single = app;
                single.displayName = window.title.empty() ? app.displayName : window.title;
                single.windows     = { window };
                split.push_back(std::move(single));
            }
        }
        return split;
    }

    return result;
}

void LogSwitcherList() {
    if (!diag::Enabled()) return;

    const std::vector<SwitcherApp> apps = BuildSwitcherList();
    MACTAB_DIAG("switcher list: %zu app(s)", apps.size());

    for (size_t i = 0; i < apps.size(); ++i) {
        const SwitcherApp& app = apps[i];
        MACTAB_DIAG("  [%zu] %s  (%s, %zu window(s))",
                    i,
                    ToUtf8(app.displayName).c_str(),
                    app.packaged ? "packaged" : "win32",
                    app.windows.size());

        for (const SwitcherWindow& window : app.windows) {
            MACTAB_DIAG("        %p %s%ldx%ld at %ld,%ld \"%s\"",
                        static_cast<void*>(window.hwnd),
                        window.minimized ? "[min] " : "",
                        window.bounds.right - window.bounds.left,
                        window.bounds.bottom - window.bounds.top,
                        window.bounds.left, window.bounds.top,
                        ToUtf8(window.title).c_str());
        }
    }
}

} // namespace mactab
