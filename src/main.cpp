#include "pch.h"
#include <wtsapi32.h>

#include "actions.h"
#include "activate.h"
#include "app.h"
#include "app_identity.h"
#include "common.h"
#include "config.h"
#include "diag.h"
#include "foreground_history.h"
#include "hotkey.h"
#include "desktops.h"
#include "icons.h"
#include "mission.h"
#include "panel.h"
#include "thumbnail.h"
#include "wallpaper.h"
#include "tray.h"
#include "window_model.h"
#include "resource.h"

namespace mactab {
namespace {

// Tray interactions arrive here.
constexpr UINT WM_MACTAB_TRAY = WM_APP + 1;

// Posted by the icon worker when one or more tiles finish.
constexpr UINT WM_MACTAB_ICON_READY = WM_APP + 2;

// Posted by the panel window when the pointer moves over or clicks a tile.
constexpr UINT WM_MACTAB_HOVER = WM_APP + 3;
constexpr UINT WM_MACTAB_CLICK = WM_APP + 4;

// Posted by the Mission Control overlay.
constexpr UINT WM_MACTAB_MC_ACTIVATE = WM_APP + 5;
constexpr UINT WM_MACTAB_MC_DISMISS  = WM_APP + 6;
constexpr UINT WM_MACTAB_MC_SPACE    = WM_APP + 7;

UINT g_msgRequestQuit    = 0;   // RegisterWindowMessage(kQuitMessageName)
UINT g_msgTaskbarCreated = 0;   // RegisterWindowMessage(L"TaskbarCreated")

// The in-flight switch gesture.
//
// Owned entirely by the UI thread. The hook thread never reads it; it posts
// intent (begin / select / commit) and this side decides what that means.
struct Gesture {
    std::vector<SwitcherApp> apps;
    int  index      = 0;
    bool active     = false;
    bool panelShown = false;

    // Down-arrow expands the highlighted app into its individual windows, and
    // Tab then cycles those instead of apps; the macOS behaviour. `appIndex`
    // remembers where to return to when the expansion collapses.
    bool windowMode = false;
    int  appIndex   = 0;
};

struct AppState {
    HINSTANCE instance      = nullptr;
    HWND      host          = nullptr;
    Tray      tray;
    Panel     panel;
    Mission   mission;
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

void PopulatePanel();
void RefreshIcons();

void CloseMission();

void BeginGesture(bool reverse) {
    Gesture& g = g_app.gesture;

    // Both own the keyboard and only one of them can have it. Alt+Tab wins,
    // because it is the gesture that is about to take you somewhere.
    CloseMission();

    g.apps       = BuildSwitcherList();
    g.active     = true;
    g.panelShown = false;

    if (g.apps.empty()) {
        // Nothing at all to show. Stay armed so the keys are still swallowed:
        // falling back to the built-in switcher mid-gesture would be worse.
        g.index = 0;
        MACTAB_DIAG("gesture: begin with no eligible apps");
        return;
    }

    // macOS lands on the previously-used app, not the current one, so a forward
    // gesture starts at index 1. Shift starts from the far end instead.
    //
    // A single app is not a special case any more. There is nowhere else to
    // land, so the selection stays on it and committing is a no-op, but the
    // panel still comes up and shows it, which is what macOS does and what the
    // user asked for. Suppressing it meant the switcher looked broken on a
    // freshly booted machine with one window open.
    const int count = static_cast<int>(g.apps.size());
    g.index = (count < 2) ? 0 : (reverse ? count - 1 : 1);

    MACTAB_DIAG("gesture: begin, %d app(s), start index %d (reverse %d)",
                count, g.index, reverse ? 1 : 0);

    PopulatePanel();
}

// The icon request for one app at the panel's current tile size.
icons::Request MakeIconRequest(const SwitcherApp& app, int tileSize) {
    icons::Request request;
    request.key            = app.key;
    request.exePath        = app.exePath;
    request.aumid          = app.aumid;
    request.packaged       = app.packaged;
    request.fallbackWindow = app.PrimaryWindow();
    request.size           = tileSize;
    return request;
}

// Push newly-arrived tiles into the panel without rebuilding it.
//
// A full PopulatePanel per worker completion would re-run layout, re-upload
// every tile and start another capture; once per icon, during a gesture.
void RefreshIcons() {
    Gesture& g = g_app.gesture;
    if (!g_app.panel.Ready() || g.apps.empty()) return;

    const int tileSize = g_app.panel.TileSizePx();

    for (const SwitcherApp& app : g.apps) {
        Bitmap icon;
        if (icons::Acquire(MakeIconRequest(app, tileSize), icon) && !icon.Empty())
            g_app.panel.UpdateIcon(app.key, icon);
    }
}

// Build the panel's item list, pulling whatever icons are already cached and
// queueing the rest. Tiles that are not ready yet render as placeholders and
// are filled in when WM_MACTAB_ICON_READY arrives, so this never blocks.
void PopulatePanel() {
    Gesture& g = g_app.gesture;
    if (!g_app.panel.Ready()) return;

    // Resolve the layout first. TileSizePx() reflects the monitor the panel
    // will actually appear on; reading it before the panel has been laid out
    // returns the unscaled default, which on a 150% display would extract and
    // disk-cache every icon at the wrong size.
    g_app.panel.PrepareLayout(static_cast<int>(g.windowMode
        ? (g.appIndex >= 0 && g.appIndex < static_cast<int>(g.apps.size())
               ? g.apps[static_cast<size_t>(g.appIndex)].windows.size()
               : 0)
        : g.apps.size()));

    const int tileSize = g_app.panel.TileSizePx();

    std::vector<PanelItem> items;

    if (g.windowMode) {
        // Every entry is a window of one app, so they all share its icon and
        // are told apart by title.
        if (g.appIndex < 0 || g.appIndex >= static_cast<int>(g.apps.size())) return;
        const SwitcherApp& app = g.apps[static_cast<size_t>(g.appIndex)];

        Bitmap shared;
        icons::Acquire(MakeIconRequest(app, tileSize), shared);

        items.reserve(app.windows.size());
        for (const SwitcherWindow& window : app.windows) {
            PanelItem item;
            item.key   = app.key;
            item.label = window.title.empty() ? app.displayName : window.title;
            item.icon  = shared;
            items.push_back(std::move(item));
        }

        g_app.panel.SetItems(std::move(items), g.index);
        return;
    }

    items.reserve(g.apps.size());

    for (const SwitcherApp& app : g.apps) {
        PanelItem item;
        item.key = app.key;

        // A packaged app's friendly name only becomes available once the icon
        // worker has read its manifest; until then the window title stands in.
        const std::wstring resolved = icons::DisplayName(app.key);
        item.label = resolved.empty() ? app.displayName : resolved;

        icons::Acquire(MakeIconRequest(app, tileSize), item.icon);   // false = not yet
        items.push_back(std::move(item));
    }

    g_app.panel.SetItems(std::move(items), g.index);
}

void AdvanceSelection(int direction) {
    Gesture& g = g_app.gesture;
    if (!g.active || g.apps.empty()) return;

    // The list being cycled is the app list normally, and the expanded app's
    // windows when the panel is showing those.
    int n = static_cast<int>(g.apps.size());
    if (g.windowMode && g.appIndex >= 0 && g.appIndex < n)
        n = static_cast<int>(g.apps[static_cast<size_t>(g.appIndex)].windows.size());
    if (n <= 0) return;

    g.index = ((g.index + direction) % n + n) % n;   // wrap in both directions

    MACTAB_DIAG("gesture: select -> index %d of %d (%s)", g.index, n,
                g.windowMode ? "window" : "app");

    g_app.panel.SetSelection(g.index);
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
    std::wstring targetName;

    if (g.windowMode && g.appIndex >= 0 && g.appIndex < static_cast<int>(g.apps.size())) {
        const SwitcherApp& app = g.apps[static_cast<size_t>(g.appIndex)];
        if (g.index >= 0 && g.index < static_cast<int>(app.windows.size())) {
            target     = app.windows[static_cast<size_t>(g.index)].hwnd;
            targetName = app.windows[static_cast<size_t>(g.index)].title;
        }
    } else if (g.index >= 0 && g.index < static_cast<int>(g.apps.size())) {
        const SwitcherApp& app = g.apps[static_cast<size_t>(g.index)];
        target     = app.PrimaryWindow();
        targetName = app.displayName;
    }

    g.active     = false;
    g.panelShown = false;
    g.windowMode = false;
    g_app.panel.Hide();

    if (!target) {
        hotkey::NeutralizeAlt(altVirtualKey);
        MACTAB_DIAG("gesture: commit with no target");
        return;
    }

    MACTAB_DIAG("gesture: commit -> %s (%p)",
                ToUtf8(targetName).c_str(), static_cast<void*>(target));

    // ActivateWindow performs the Alt neutralisation itself, in the specific
    // order that avoids menu bleed-through.
    ActivateWindow(target, altVirtualKey);
}

void CancelGesture(WORD altVirtualKey) {
    Gesture& g = g_app.gesture;
    g.active     = false;
    g.panelShown = false;
    g.windowMode = false;
    g.apps.clear();
    g_app.panel.Hide();

    hotkey::NeutralizeAlt(altVirtualKey);
    MACTAB_DIAG("gesture: cancelled");
}

void RevealPanel() {
    Gesture& g = g_app.gesture;

    // An empty list means BeginGesture returned before populating the panel, so
    // there is nothing valid to show; revealing would display the previous
    // gesture's tiles, complete with stale window handles. One app is fine and
    // gets a panel of its own.
    if (!g.active || g.apps.empty()) return;

    g.panelShown = true;

    // A quick Alt+Tab must never reach this point, and holding Alt must reach
    // it exactly once; that split is the macOS behaviour, and this log line is
    // how it gets verified without being able to watch the screen.
    const double started = NowMs();
    g_app.panel.Show();
    MACTAB_DIAG("gesture: reveal, index %d of %zu, shown in %.2f ms",
                g.index, g.apps.size(), NowMs() - started);
}

// Q / W / H / backtick, dispatched once the panel is up.
void HandleActionKey(WORD virtualKey) {
    Gesture& g = g_app.gesture;
    if (!g.active || g.apps.empty()) return;

    // Bound g.index against the list it actually indexes, which is the expanded
    // app's WINDOWS in window mode, not the app list. Testing it against
    // g.apps.size() meant that expanding an app with more windows than there are
    // apps and arrowing past that count silently killed every action key,
    // including the Up that collapses back out, so the only way out was to
    // release Alt.
    int n = static_cast<int>(g.apps.size());
    if (g.windowMode && g.appIndex >= 0 && g.appIndex < n)
        n = static_cast<int>(g.apps[static_cast<size_t>(g.appIndex)].windows.size());
    if (g.index < 0 || g.index >= n) return;

    // In window mode the highlighted entry is a window, but Q/W/H still act on
    // the owning app, which is the one that was expanded.
    const int appSlot = g.windowMode ? g.appIndex : g.index;
    if (appSlot < 0 || appSlot >= static_cast<int>(g.apps.size())) return;

    SwitcherApp& app = g.apps[static_cast<size_t>(appSlot)];

    switch (virtualKey) {
    case 'Q':
        QuitApp(app);
        // The app is going away, so drop its tile and keep the gesture alive.
        // macOS lets you quit several apps in one Cmd-Tab hold. Quitting the
        // app you were looking inside also collapses the expansion.
        g.apps.erase(g.apps.begin() + appSlot);
        g.windowMode = false;
        if (g.apps.empty()) {
            g_app.panel.Hide();
            return;
        }
        g.index = (std::min)(appSlot, static_cast<int>(g.apps.size()) - 1);
        PopulatePanel();
        return;

    case 'W':
        CloseFrontWindow(app);
        return;

    case 'H':
        HideApp(app);
        return;

    case hotkey::kActionCycleWindows:   // the key above Tab
        if (g.windowMode) {
            // Already looking at the windows; just advance the selection.
            AdvanceSelection(1);
            return;
        }
        if (app.windows.size() > 1) {
            // Rotating makes the next window primary, so committing now lands
            // on it without leaving the app row.
            std::rotate(app.windows.begin(), app.windows.begin() + 1, app.windows.end());
            MACTAB_DIAG("gesture: cycled to window %p of %s",
                        static_cast<void*>(app.PrimaryWindow()),
                        ToUtf8(app.displayName).c_str());
            PopulatePanel();
        }
        return;

    case VK_DOWN:
        // Expand the app into its windows, as macOS does. Pointless for a
        // single-window app, so leave the row alone in that case.
        if (!g.windowMode && app.windows.size() > 1) {
            g.appIndex   = appSlot;
            g.windowMode = true;
            g.index      = 0;
            MACTAB_DIAG("gesture: expanded %s into %zu windows",
                        ToUtf8(app.displayName).c_str(), app.windows.size());
            PopulatePanel();
        }
        return;

    case VK_UP:
        // Collapse back to the app row, landing on the app we came from.
        if (g.windowMode) {
            g.windowMode = false;
            g.index      = g.appIndex;
            MACTAB_DIAG("gesture: collapsed back to the app row");
            PopulatePanel();
        }
        return;

    default:
        return;
    }
}

// --- Mission Control --------------------------------------------------------

// The size icons are asked for in the arrangement.
//
// A fixed number rather than the badge's size in pixels: the icon cache is keyed
// by size, and asking for a different one per display scale would extract and
// disk-cache the same artwork several times over. 128 is what the switcher
// already asks for at 100%, so on most machines this is a cache hit.
constexpr int kMissionIconSize = 128;

// Bring the overlay up to readiness, building it if this is the first time.
//
// Deliberately lazy. Mission Control is off by default, and initialising it
// means a compositor, a D3D device, a D2D device and a window, which is real
// memory to spend on a feature nobody has asked for. Turning it on from the
// tray pays that cost once, at a moment when a few milliseconds cannot be felt.
bool EnsureMission() {
    if (g_app.mission.Ready()) return true;

    if (!g_app.mission.Initialize(g_app.instance, g_app.host, WM_MACTAB_MC_ACTIVATE,
                                  WM_MACTAB_MC_DISMISS, WM_MACTAB_MC_SPACE)) {
        MACTAB_WARN("mission: initialisation failed; Win+Tab left alone");
        return false;
    }

    const std::wstring& forced = config::Current().missionThumbnails;
    if (forced == L"shared")        thumbnail::Force(thumbnail::Tier::SharedVisual);
    else if (forced == L"snapshot") thumbnail::Force(thumbnail::Tier::Snapshot);
    else if (forced == L"icon")     thumbnail::Force(thumbnail::Tier::IconOnly);

    // Everything that can be done before the first invocation is done now, so
    // the first Win+Tab costs what the hundredth costs.
    g_app.mission.Prewarm();
    return true;
}

void CloseMission() {
    if (g_app.mission.Visible()) {
        g_app.mission.Hide();
        MACTAB_DIAG("mission: dismissed");
    }
}

void OpenMission() {
    if (!config::Current().missionEnabled) return;
    if (!EnsureMission()) return;

    // A toggle, like the key it replaces and like the gesture it copies.
    if (g_app.mission.Visible()) {
        CloseMission();
        return;
    }

    // Never over a switch in flight. Both own the keyboard and only one of them
    // can have it.
    if (g_app.gesture.active) return;

    std::vector<MissionItem> items;
    int group = 0;

    for (const SwitcherApp& app : BuildSwitcherList()) {
        // Whatever the icon worker already has. A miss queues the work and
        // WM_MACTAB_ICON_READY brings it back, so an app seen for the first
        // time gets its icon a moment later rather than never.
        Bitmap icon;
        icons::Acquire(MakeIconRequest(app, kMissionIconSize), icon);

        const std::wstring resolved = icons::DisplayName(app.key);

        for (const SwitcherWindow& window : app.windows) {
            // Minimized windows are not in macOS' Mission Control either. They
            // are in the Dock, which here is the taskbar, and putting them in
            // the arrangement would mean inventing a size for something that
            // does not currently have one.
            if (window.minimized) continue;

            MissionItem item;
            item.hwnd    = window.hwnd;
            item.title   = window.title;
            item.appName = resolved.empty() ? app.displayName : resolved;
            item.appKey  = app.key;
            item.icon    = icon;
            item.bounds  = window.bounds;
            item.group   = group;
            item.order   = static_cast<int>(items.size());
            items.push_back(std::move(item));
        }
        ++group;
    }

    std::vector<MissionSpace> spaces;
    const desktops::State state = desktops::Query(g_app.host);
    if (state.known) {
        for (size_t i = 0; i < state.all.size(); ++i)
            spaces.push_back(MissionSpace{ state.all[i].name,
                                           static_cast<int>(i) == state.current });
    }

    MACTAB_DIAG("mission: opening with %zu window(s) and %zu space(s)",
                items.size(), spaces.size());

    g_app.mission.Show(std::move(items), std::move(spaces));
}

void ActivateFromMission(int index) {
    // The items live inside the overlay, which is about to throw them away, so
    // the handle is read before hiding.
    const HWND target = g_app.mission.ItemWindow(index);

    // Hidden without putting focus back where it was, because it is about to go
    // somewhere else entirely. Restoring first would hand foreground to the old
    // window for a frame and show as a flash of the wrong app.
    g_app.mission.Hide(false);

    if (target && ::IsWindow(target)) {
        MACTAB_DIAG("mission: activating %p", static_cast<void*>(target));
        ActivateWindow(target, 0);
    }
}

void HandleMissionSpace(WPARAM which) {
    // The overlay comes down first, always.
    //
    // Switching desktops may animate, and an overlay still on screen while the
    // desktop slides underneath it looks like a bug. Creating a desktop has the
    // same problem and additionally moves the foreground, which the overlay
    // would immediately read as a reason to close itself.
    const desktops::State state = desktops::Query(g_app.host);
    CloseMission();

    if (which == Mission::kSpaceAdd) {
        desktops::Create();
        return;
    }
    if (which == Mission::kSpaceClose) {
        desktops::CloseCurrent();
        return;
    }

    desktops::SwitchTo(state, static_cast<int>(which));
}

// --- Tray ------------------------------------------------------------------

// Settings submenu.
//
// Ownership: appended to the parent with MF_POPUP, so DestroyMenu on the parent
// destroys this too. Do not destroy it separately.
HMENU CreateSettingsMenu() {
    HMENU settings = ::CreatePopupMenu();
    if (!settings) return nullptr;

    ::AppendMenuW(settings, MF_STRING | MF_DISABLED | MF_GRAYED, 0, L"Panel appears on");
    ::AppendMenuW(settings, MF_STRING, IDM_TRAY_DISPLAY_ACTIVE, L"    Active window's display");
    ::AppendMenuW(settings, MF_STRING, IDM_TRAY_DISPLAY_MOUSE,  L"    Display with the mouse");
    ::AppendMenuW(settings, MF_STRING, IDM_TRAY_DISPLAY_MAIN,   L"    Main display");

    UINT checked = IDM_TRAY_DISPLAY_ACTIVE;
    switch (config::Current().panelDisplay) {
        case config::PanelDisplay::Mouse:   checked = IDM_TRAY_DISPLAY_MOUSE; break;
        case config::PanelDisplay::Primary: checked = IDM_TRAY_DISPLAY_MAIN;  break;
        default: break;
    }
    // Radio rather than check marks: the three are mutually exclusive, and this
    // is the only way to get the bullet glyph instead of a tick.
    ::CheckMenuRadioItem(settings, IDM_TRAY_DISPLAY_ACTIVE, IDM_TRAY_DISPLAY_MAIN,
                         checked, MF_BYCOMMAND);

    ::AppendMenuW(settings, MF_SEPARATOR, 0, nullptr);

    ::AppendMenuW(settings, MF_STRING | MF_DISABLED | MF_GRAYED, 0, L"Appearance");
    ::AppendMenuW(settings, MF_STRING, IDM_TRAY_THEME_AUTO,  L"    Follow Windows");
    ::AppendMenuW(settings, MF_STRING, IDM_TRAY_THEME_LIGHT, L"    Light");
    ::AppendMenuW(settings, MF_STRING, IDM_TRAY_THEME_DARK,  L"    Dark");

    const std::wstring& theme = config::Current().theme;
    const UINT checkedTheme = (theme == L"light") ? IDM_TRAY_THEME_LIGHT
                            : (theme == L"dark")  ? IDM_TRAY_THEME_DARK
                                                  : IDM_TRAY_THEME_AUTO;
    ::CheckMenuRadioItem(settings, IDM_TRAY_THEME_AUTO, IDM_TRAY_THEME_DARK,
                         checkedTheme, MF_BYCOMMAND);

    ::AppendMenuW(settings, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(settings, MF_STRING | (config::Current().missionEnabled ? MF_CHECKED : 0u),
                  IDM_TRAY_MISSION, L"Mission Control on Win+Tab");
    ::AppendMenuW(settings, MF_STRING | (config::AutostartEnabled() ? MF_CHECKED : 0u),
                  IDM_TRAY_AUTOSTART, L"Start when I sign in");

    return settings;
}

void ShowTrayMenu(HWND hwnd, POINT screenPt) {
    HMENU menu = ::CreatePopupMenu();
    if (!menu) return;

    ::AppendMenuW(menu, MF_STRING | MF_DISABLED | MF_GRAYED, 0, L"MacTab " MACTAB_VERSION_W);
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    if (HMENU settings = CreateSettingsMenu()) {
        // Ownership only transfers on success; on failure it is still ours.
        if (!::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(settings), L"Settings"))
            ::DestroyMenu(settings);
    }

    ::AppendMenuW(menu, MF_STRING | (hotkey::IsRunning() ? 0u : MF_DISABLED),
                  IDM_TRAY_RELOAD_HOOK, L"Reload keyboard hook");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, IDM_TRAY_RELOAD_GLASS, L"Reload glass from settings.ini");
    ::AppendMenuW(menu, MF_STRING, IDM_TRAY_DUMP_LIST, L"Log current switcher list");
    ::AppendMenuW(menu, MF_STRING, IDM_TRAY_DUMP_DESKTOPS, L"Log virtual desktops");
    ::AppendMenuW(menu, MF_STRING, IDM_TRAY_OPEN_LOG, L"Open diagnostics log");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    // Disabled rather than hidden when there is no uninstall entry: running the
    // bare exe without installing is supported, and a greyed item explains why
    // nothing happens where a missing one would just look like a bug.
    const bool installed = !config::UninstallCommand().empty();
    ::AppendMenuW(menu, MF_STRING | (installed ? 0u : (MF_DISABLED | MF_GRAYED)),
                  IDM_TRAY_UNINSTALL, L"Uninstall MacTab...");
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
    icons::Stop();
    g_app.mission.Shutdown();
    g_app.panel.Shutdown();

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
        HandleActionKey(static_cast<WORD>(wParam));
        return 0;

    case hotkey::WM_MACTAB_MISSION:
        OpenMission();
        return 0;

    case WM_MACTAB_MC_ACTIVATE:
        ActivateFromMission(static_cast<int>(static_cast<INT_PTR>(wParam)));
        return 0;

    case WM_MACTAB_MC_DISMISS:
        CloseMission();
        return 0;

    case WM_MACTAB_MC_SPACE:
        HandleMissionSpace(wParam);
        return 0;

    case WM_MACTAB_HOVER: {
        // Hovering moves the selection, exactly as arrowing does.
        const int index = static_cast<int>(static_cast<INT_PTR>(wParam));
        Gesture& g = g_app.gesture;
        if (g.active && index >= 0 && index != g.index) {
            g.index = index;
            g_app.panel.SetSelection(g.index);
        }
        return 0;
    }

    case WM_MACTAB_CLICK: {
        // Clicking commits immediately. Alt is usually still physically down,
        // so the same neutralisation the keyboard path uses still applies.
        const int index = static_cast<int>(static_cast<INT_PTR>(wParam));
        Gesture& g = g_app.gesture;
        if (g.active && index >= 0) {
            g.index = index;
            // End the hook's gesture without a cancellation coming back: that
            // would inject a second Alt-up behind the one CommitGesture already
            // synthesised. Use the key that actually opened the gesture rather
            // than the generic VK_MENU, so the release is symmetric.
            //
            // EndGestureQuietly is asynchronous, so a physical Alt-up can still
            // reach the hook first. That path is harmless: the hook posts
            // COMMIT, g.active is already false, and CommitGesture just
            // neutralises a key that is logically up again. GestureAltKey may
            // return 0 in the same race, which NeutralizeAlt treats as VK_MENU.
            const WORD altKey = hotkey::GestureAltKey();
            hotkey::EndGestureQuietly();
            CommitGesture(altKey);
        }
        return 0;
    }

    case WM_MACTAB_ICON_READY:
        if (g_app.mission.Visible()) {
            for (const SwitcherApp& app : BuildSwitcherList()) {
                Bitmap icon;
                if (icons::Acquire(MakeIconRequest(app, kMissionIconSize), icon) &&
                    !icon.Empty())
                    g_app.mission.UpdateIcon(app.key, icon);
            }
        }
        // Tiles usually finish during the hold delay, before the panel is
        // shown, so this must not be gated on the panel being visible; doing
        // that left the first gesture showing placeholders for its whole
        // lifetime, because the worker never posts again for a cached tile.
        if (g_app.gesture.active)
            RefreshIcons();
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

        case IDM_TRAY_MISSION: {
            const bool enabled = !config::Current().missionEnabled;
            config::SetMissionEnabled(enabled);

            // Built on the first switch-on rather than at boot, so the cost
            // lands here instead of on the first keystroke.
            const bool ready = enabled ? EnsureMission() : true;
            if (enabled && !ready) config::SetMissionEnabled(false);

            hotkey::SetMissionOnWinTab(enabled && ready);
            if (!enabled) CloseMission();

            g_app.tray.ShowBalloon(L"MacTab",
                                   (enabled && ready)
                                       ? L"Win+Tab now opens Mission Control."
                                       : L"Win+Tab left to Windows.");
            return 0;
        }

        case IDM_TRAY_AUTOSTART:
            // Read-modify-write the Run key itself. There is deliberately no
            // mirrored setting: the key, this menu and Task Manager's Startup
            // tab all read the same place and cannot disagree.
            config::SetAutostart(!config::AutostartEnabled());
            return 0;

        // Takes effect on the next gesture: the panel picks its monitor in
        // Layout, which runs per gesture, so nothing has to be rebuilt here.
        case IDM_TRAY_DISPLAY_ACTIVE:
            config::SetPanelDisplay(config::PanelDisplay::ActiveWindow);
            return 0;

        case IDM_TRAY_DISPLAY_MOUSE:
            config::SetPanelDisplay(config::PanelDisplay::Mouse);
            return 0;

        case IDM_TRAY_DISPLAY_MAIN:
            config::SetPanelDisplay(config::PanelDisplay::Primary);
            return 0;

        // Also next-gesture: SetItems re-resolves the theme every time.
        case IDM_TRAY_THEME_AUTO:  config::SetTheme(L"auto");  return 0;
        case IDM_TRAY_THEME_LIGHT: config::SetTheme(L"light"); return 0;
        case IDM_TRAY_THEME_DARK:  config::SetTheme(L"dark");  return 0;

        case IDM_TRAY_RELOAD_GLASS:
            // The whole material, re-read, without a restart.
            //
            // Not config::Load(), which reassigns the themes directory string
            // the icon worker reads on its own thread. This touches only the two
            // Params and the shared optics, and nothing off this thread reads
            // either.
            //
            // The panel picks them up at the next gesture, because it copies the
            // theme's material when it lays out rather than caching one at
            // startup.
            config::ReloadGlass();
            g_app.tray.ShowBalloon(L"MacTab",
                                   L"Glass reloaded. Alt+Tab to see it.");
            return 0;

        case IDM_TRAY_DUMP_LIST:
            // M2 verification: what this prints should match what Windows'
            // own Alt+Tab shows, app-grouped.
            if (diag::Enabled()) {
                LogSwitcherList();
                g_app.tray.ShowBalloon(L"MacTab", L"Switcher list written to the diagnostics log.");
            } else {
                ::MessageBoxW(hwnd,
                              L"Diagnostics logging is off for this session.\n\n"
                              L"Relaunch MacTab with --diag to capture the list.",
                              L"MacTab", MB_OK | MB_ICONINFORMATION);
            }
            return 0;

        case IDM_TRAY_DUMP_DESKTOPS:
            // The one thing about virtual desktops that cannot be reasoned
            // about from here: whether this build keeps the ordered list and
            // the current desktop where we look for them.
            if (diag::Enabled()) {
                desktops::LogState(hwnd);
                g_app.tray.ShowBalloon(L"MacTab", L"Desktops written to the diagnostics log.");
            } else {
                ::MessageBoxW(hwnd,
                              L"Diagnostics logging is off for this session.\n\n"
                              L"Relaunch MacTab with --diag to capture them.",
                              L"MacTab", MB_OK | MB_ICONINFORMATION);
            }
            return 0;

        case IDM_TRAY_OPEN_LOG:
            OpenDiagnosticsLog(hwnd);
            return 0;

        case IDM_TRAY_UNINSTALL:
            // Deliberately no PostQuitMessage. The uninstaller posts WM_CLOSE
            // to our host window before it removes anything, so this process
            // exits on its own; quitting first would leave nothing running if
            // the user cancels the wizard.
            if (!config::RunUninstaller()) {
                ::MessageBoxW(hwnd,
                              L"MacTab could not find its uninstaller.\n\n"
                              L"This usually means it is running as a standalone "
                              L"executable rather than an installed copy, in which "
                              L"case there is nothing to uninstall: delete "
                              L"MacTab.exe and the MacTab folder in %LOCALAPPDATA%.",
                              L"MacTab", MB_OK | MB_ICONINFORMATION);
            }
            return 0;

        case IDM_TRAY_QUIT:
            MACTAB_DIAG("host: quit selected from tray menu");
            ::DestroyWindow(hwnd);
            return 0;

        default:
            break;
        }
        break;

    // The wallpaper is baked once and kept, so the one thing that has to be
    // noticed is it changing. SPI_SETDESKWALLPAPER arrives here as a broadcast
    // and costs nothing until Mission Control next opens.
    case WM_SETTINGCHANGE:
        if (wParam == SPI_SETDESKWALLPAPER)
            g_app.mission.InvalidateBackdrop();
        return 0;

    case WM_DISPLAYCHANGE:
        // Monitors moved, so every cached wallpaper is the wrong size and every
        // baked backdrop is for a screen that is not there any more.
        g_app.mission.InvalidateBackdrop();
        return 0;

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
            CloseMission();

            // Repair the modifier again once input belongs to this desktop.
            //
            // AbortGesture on LOCK injects the replacement Alt-up while Winlogon
            // owns the input desktop, where SendInput is dropped, so the
            // swallowed Alt-up is never actually replaced and the first thing
            // the user types after unlocking is an Alt chord. By UNLOCK the
            // gesture is already Idle, so nothing else would ever do it.
            //
            // Unconditional and harmless: a lone Alt-up when Alt is already up
            // does nothing, since a key-up on its own never opens a menu bar.
            if (wParam == WTS_SESSION_UNLOCK)
                hotkey::NeutralizeAlt(VK_MENU);

            // Processes very likely came and went while the session was locked,
            // and PIDs get reused. Cheaper to drop the cache than to validate it.
            ClearIdentityCache();
        }
        return 0;

    // --- Shutdown contract --------------------------------------------------
    // Restart Manager (used by the installer to upgrade over a running copy)
    // sends WM_QUERYENDSESSION with ENDSESSION_CLOSEAPP, then WM_ENDSESSION,
    // then WM_CLOSE, and force-kills anything still alive after its timeout.
    // Answering all three is what lets an upgrade replace the exe without
    // killing us mid-flight and leaking the tray icon.
    case WM_QUERYENDSESSION:
        // Agree to exit, but do not tear down yet; the session may still be
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

    // COM is initialised by the panel layer (M3), not here.
    //
    // Worth spelling out because the obvious reading of Microsoft's own Win32
    // Composition sample is wrong: it passes DQTAT_COM_ASTA to
    // CreateDispatcherQueueController, but that field is documented as relevant
    // ONLY when threadType is DQTYPE_THREAD_DEDICATED. With
    // DQTYPE_THREAD_CURRENT, which is what a Win32 app uses; it is ignored,
    // so it does not initialise anything. The thread must therefore be put in
    // an STA explicitly (winrt::init_apartment(apartment_type::single_threaded))
    // with DQTAT_COM_NONE, because a Compositor has thread affinity and WinRT
    // activation on an uninitialised thread silently lands in the MTA.
    //
    // Nothing before that point needs COM: identity resolution deliberately
    // uses GetApplicationUserModelId rather than the shell property store, and
    // shell APIs that do need COM run on the icon worker, which initialises its
    // own apartment.

    g_app.instance      = instance;
    g_app.diagRequested = HasFlag(cmdLine, L"--diag");

    diag::Init(g_app.diagRequested);
    config::Load();
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

    // The panel is created and fully pre-warmed at startup, then shown and
    // hidden for the rest of the session. Building the visual tree on demand
    // would make the one-frame reveal budget unreachable.
    if (!g_app.panel.Initialize(instance, g_app.host,
                                WM_MACTAB_HOVER, WM_MACTAB_CLICK)) {
        ::MessageBoxW(nullptr,
                      L"MacTab could not initialise its rendering layer.\n\n"
                      L"This needs Windows 10 version 1803 or later. Run with --diag "
                      L"for details.",
                      L"MacTab", MB_OK | MB_ICONERROR);
        MACTAB_FAIL("boot: panel initialisation failed, exiting");
        ::DestroyWindow(g_app.host);
        diag::Shutdown();
        return 1;
    }

    // Mission Control needs the compositing stack the panel just set up, so it
    // can only come after it. Built at boot only when it is switched on, so the
    // first Win+Tab is not the one that pays for it.
    if (config::Current().missionEnabled) EnsureMission();

    icons::Start(g_app.host, WM_MACTAB_ICON_READY);

    // Lock/unlock notifications, so a gesture interrupted by the secure desktop
    // does not leave the state machine stuck.
    g_app.wtsRegistered =
        ::WTSRegisterSessionNotification(g_app.host, NOTIFY_FOR_THIS_SESSION) != FALSE;
    if (!g_app.wtsRegistered)
        MACTAB_WARN("boot: WTSRegisterSessionNotification failed (err %lu)", ::GetLastError());

    if (!foreground::Start())
        MACTAB_WARN("boot: MRU tracking unavailable; switching will be unordered");

    hotkey::Options hotkeyOptions{};
    hotkeyOptions.revealDelayMs = config::Current().revealDelayMs;
    hotkeyOptions.leftAltOnly   = config::Current().leftAltOnly;
    hotkeyOptions.missionOnWinTab = config::Current().missionEnabled;
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

    MSG msg{};
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
