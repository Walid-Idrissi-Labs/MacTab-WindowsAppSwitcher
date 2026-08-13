#include "pch.h"
#include <wtsapi32.h>

#include "actions.h"
#include "activate.h"
#include "app.h"
#include "app_identity.h"
#include "capture.h"
#include "common.h"
#include "config.h"
#include "diag.h"
#include "foreground_history.h"
#include "hotkey.h"
#include "desktops.h"
#include "icons.h"
#include "mission.h"
#include "panel.h"
#include "settings_watch.h"
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
constexpr UINT WM_MACTAB_MC_GONE     = WM_APP + 8;   // wParam: the dead HWND

// Ticks only while Mission Control is on screen.
constexpr UINT_PTR kMissionWatchTimer = 1;

// Armed when settings.ini changes, and re-armed by every further change, so the
// reload happens once the file has stopped moving rather than once per
// notification. A save is several notifications: the editor writes a temporary
// file, renames it over the target, and the metadata update lands separately.
//
// 250 ms is well under the time it takes to look back at the screen, and well
// over the gap between the events of one save.
constexpr UINT_PTR kSettingsReloadTimer  = 2;
constexpr UINT     kSettingsReloadDelayMs = 250;

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

    // Said once per session, not once per gesture. See ReportFlatPanel.
    bool      reportedFlatPanel = false;
};

AppState g_app;

bool HasFlag(const wchar_t* cmdLine, const wchar_t* flag) {
    return cmdLine && ::StrStrIW(cmdLine, flag) != nullptr;
}

// --- Gesture handling ------------------------------------------------------

void PopulatePanel();
void RefreshIcons();

// Which chord the settings ask for. Anything unrecognised means Win+Tab, which
// is the shipped default and the one an unreadable ini should fall back to.
hotkey::Gesture MissionGesture() {
    if (!config::Current().missionEnabled) return hotkey::Gesture::None;

    const std::wstring& choice = config::Current().missionGesture;
    if (choice == L"winup") return hotkey::Gesture::WinUp;
    if (choice == L"both")  return hotkey::Gesture::Both;
    if (choice == L"none")  return hotkey::Gesture::None;
    return hotkey::Gesture::WinTab;
}

// How to describe the chord, whether or not the feature is currently on. Reads
// the setting rather than MissionGesture(), which reports None while Mission
// Control is switched off and would have the menu offer to turn on a key it then
// refused to name.
const wchar_t* MissionGestureName() {
    const std::wstring& choice = config::Current().missionGesture;
    if (choice == L"winup") return L"Win+Up";
    if (choice == L"both")  return L"Win+Tab and Win+Up";
    if (choice == L"none")  return L"the tray menu";
    return L"Win+Tab";
}

void CloseMission(bool commitDesktop = false);
void StartWatchingWindows();
void StopWatchingWindows();

void BeginGesture(bool reverse) {
    Gesture& g = g_app.gesture;

    // Both own the keyboard and only one of them can have it. Alt+Tab wins,
    // because it is the gesture that is about to take you somewhere.
    //
    // Without committing whatever desktop was being looked at: the switcher is
    // about to decide where the user lands, and activating a window on another
    // desktop takes them there anyway. Two answers to the same question, one of
    // them arriving first, is how you get a switch that fights itself.
    CloseMission(false);

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

// Say out loud that the panel has no backdrop, once per session.
//
// The state this reports is the one that made the material look untunable: with
// no picture of the desktop behind it the panel draws a coat that is 96% opaque,
// so it reads as a grey slab and every value in settings.ini stops changing
// anything, because none of what those values describe is being drawn. From
// outside it is indistinguishable from a badly tuned material, and it stayed
// that way for five releases because nothing ever said which one it was.
//
// Once, not per gesture: a machine where the grab never works would otherwise
// pop a balloon on every Alt+Tab. The diagnostics log carries the detail, and
// CaptureSource in settings.ini is the thing to try.
void ReportFlatPanel() {
    if (g_app.reportedFlatPanel) return;

    // Not when the plate is what was asked for. The whole point of this balloon
    // is to name a failure, and telling somebody who switched the glass off that
    // the glass is off would be noise at best and would read as a fault at
    // worst.
    if (!config::Current().glassEnabled) return;

    if (!g_app.panel.BackdropIsFlat()) return;

    g_app.reportedFlatPanel = true;
    MACTAB_WARN("panel: revealed with no backdrop; the glass cannot be seen");
    g_app.tray.ShowBalloon(
        L"MacTab",
        L"The panel could not grab the desktop behind it, so it is showing a "
        L"plain tint instead of glass. Try CaptureSource=plain in settings.ini, "
        L"then bitblt, then duplication.");
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

    ReportFlatPanel();
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

// Take Mission Control down.
//
// `commitDesktop` is what separates the two ways it can close. Deliberately
// chosen by the caller rather than decided in here, because the difference is
// entirely about WHY it is closing and only the caller knows that.
//
// Walking the strip inside Mission Control only ever changes what you are
// LOOKING at; the overlay belongs to the desktop it was built on and switching
// underneath it would strand it. So the switch is made real on the way out, and
// only when the user meant to leave: Escape, the toggle, a click on the
// background. Losing focus, a display appearing, a session lock or Alt+Tab
// taking over are not decisions, and must not move anybody's desktop.
void CloseMission(bool commitDesktop) {
    if (!g_app.mission.Visible()) return;

    const int browsed = commitDesktop ? g_app.mission.BrowsedDesktop() : -1;

    StopWatchingWindows();
    hotkey::SetMissionOpen(false);

    // Never with focus restored on this path. Putting foreground back on a
    // window belonging to the desktop being left is both a flash of the wrong
    // application and, since Windows shows a window it is asked to activate, a
    // switch straight back to where it lives.
    // Committing a desktop switch cannot wait for the windows to settle back:
    // the shell animates the switch, and an overlay still on screen while the
    // desktop slides underneath it looks like a fault. Everything else gets the
    // collapse.
    g_app.mission.Hide(browsed < 0, browsed >= 0);
    MACTAB_DIAG("mission: dismissed");

    if (browsed < 0) return;

    // Re-read rather than trusting what was true when the overlay opened.
    // SwitchTo counts steps from wherever the view is NOW, and something else
    // may have moved it while Mission Control was up.
    const desktops::State state = desktops::Query(g_app.host);
    if (state.known && state.current >= 0 && state.current != browsed) {
        MACTAB_DIAG("mission: committing the desktop being looked at (%d)", browsed);
        desktops::SwitchTo(state, browsed);
    }
}

// Everything the overlay is shown with, gathered in one place because three
// paths need it now: opening, adding or closing a desktop, and a window going
// away while it is up.
struct MissionPayload {
    std::vector<MissionItem>  items;
    std::vector<MissionSpace> spaces;
    int                       current = 0;
    bool                      known   = false;
};

MissionPayload BuildMissionPayload() {
    MissionPayload payload;

    // Every desktop's windows, not just this one's, so the strip can be walked
    // from inside without leaving.
    const desktops::State state = desktops::Query(g_app.host);
    payload.known   = state.known;
    payload.current = (std::max)(0, state.current);

    if (state.known) {
        for (size_t i = 0; i < state.all.size(); ++i)
            payload.spaces.push_back(MissionSpace{ state.all[i].name, state.all[i].id,
                                                   static_cast<int>(i) == state.current });
    }

    int group = 0;

    for (const SwitcherApp& app : BuildWindowList(state.known && state.all.size() > 1)) {
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
            item.order   = static_cast<int>(payload.items.size());

            // -1 stays -1 for a pinned window, whose desktop id is a sentinel
            // that is in no list, and that is exactly right: it shows on every
            // desktop because it is on every desktop.
            item.desktop = state.known ? desktops::IndexOf(state, window.desktop) : -1;

            payload.items.push_back(std::move(item));
        }
        ++group;
    }

    return payload;
}

void OpenMission() {
    if (!config::Current().missionEnabled) return;
    if (!EnsureMission()) return;

    // A toggle, like the key it replaces and like the gesture it copies. Closing
    // it this way is a decision, so it takes you to the desktop you were looking
    // at.
    if (g_app.mission.Visible()) {
        CloseMission(true);
        return;
    }

    // Never over a switch in flight. Both own the keyboard and only one of them
    // can have it.
    if (g_app.gesture.active) return;

    MissionPayload payload = BuildMissionPayload();

    MACTAB_DIAG("mission: opening with %zu window(s) and %zu space(s)",
                payload.items.size(), payload.spaces.size());

    const int current = payload.current;
    g_app.mission.Show(std::move(payload.items), std::move(payload.spaces), current);

    // The hook stops passing Ctrl+Win+Left and Ctrl+Win+Right through while the
    // overlay is up, and posts them back to be aimed at the strip instead.
    hotkey::SetMissionOpen(g_app.mission.Visible());

    if (g_app.mission.Visible()) StartWatchingWindows();
}

void ActivateFromMission(int index) {
    // The items live inside the overlay, which is about to throw them away, so
    // the handle is read before hiding.
    const HWND target = g_app.mission.ItemWindow(index);
    hotkey::SetMissionOpen(false);

    // Hidden without putting focus back where it was, because it is about to go
    // somewhere else entirely. Restoring first would hand foreground to the old
    // window for a frame and show as a flash of the wrong app.
    //
    // The collapse runs while the window is being activated, so the tile lands
    // on the real window at the moment the real window comes forward, which is
    // the whole trick.
    StopWatchingWindows();
    g_app.mission.Hide(false);

    if (target && ::IsWindow(target)) {
        MACTAB_DIAG("mission: activating %p", static_cast<void*>(target));
        ActivateWindow(target, 0);
    }
}

// Wait for the shell to finish adding or removing a desktop.
//
// Both go through injected keystrokes, so there is no return value to wait on
// and no notification when they land: the only observable is the shell's own
// list of desktops changing length. Bounded, and a timeout means the rest of the
// work is skipped rather than done on a stale picture.
bool WaitForDesktopCount(size_t before, DWORD timeoutMs = 800) {
    // Elapsed by unsigned subtraction, not against a deadline: adding to the
    // tick count wraps every 49.7 days, and a wrapped deadline is already past.
    const DWORD started = ::GetTickCount();
    for (;;) {
        const desktops::State state = desktops::Query(g_app.host);
        if (state.known && state.all.size() != before) return true;
        if (::GetTickCount() - started >= timeoutMs) return false;
        ::Sleep(10);
    }
}

// Adding or closing a desktop, without leaving Mission Control.
//
// Both chords move the view, and these overlays belong to whichever desktop they
// were last assigned to, so the sequence is: make the change, wait for the shell
// to admit it happened, bring the overlays across to wherever the view ended up,
// and rebuild in place. macOS stays open through both, and closing and reopening
// would throw away the thing the user was in the middle of.
void ChangeSpaces(bool add, int index) {
    // A click can be sitting in the queue behind a dismissal. Acting on it then
    // would create or destroy a real desktop with the overlay already gone, and
    // re-install watchers that nothing would ever take down again.
    if (!g_app.mission.Visible()) return;

    const desktops::State before = desktops::Query(g_app.host);
    if (!before.known || before.current < 0) return;

    const size_t was = before.all.size();

    // Where the user actually is, so closing a desktop does not move them.
    //
    // Closing one you are not on means going there, and the shell then lands you
    // on a neighbour of the desktop it just removed. Deleting desktop 4 while
    // standing on desktop 1 should not put you on desktop 3.
    const GUID home = before.all[static_cast<size_t>(before.current)].id;
    const bool closingCurrent = !add && index == before.current;

    g_app.mission.BeginDesktopChurn();

    if (add) {
        if (!desktops::Create()) return;
    } else if (!desktops::CloseAt(g_app.host, index)) {
        // CloseAt refuses rather than guessing when it cannot confirm the view
        // reached the desktop being closed. Nothing happened, so nothing here
        // has to be undone.
        return;
    }

    if (!WaitForDesktopCount(was)) {
        MACTAB_WARN("mission: the desktop list never changed; leaving the strip alone");
        return;
    }

    // Back to where the user was, unless that is the desktop they just closed,
    // in which case wherever the shell put them is the only answer there is.
    if (!add && !closingCurrent) {
        const desktops::State now = desktops::Query(g_app.host);
        const int back = now.known ? desktops::IndexOf(now, home) : -1;
        if (back >= 0 && back != now.current) {
            desktops::SwitchTo(now, back);
            desktops::WaitForCurrent(home);
        }
    }

    MissionPayload payload = BuildMissionPayload();
    if (!payload.known || payload.spaces.empty()) return;

    g_app.mission.FollowDesktop(
        payload.spaces[static_cast<size_t>(payload.current)].id);

    const int current = payload.current;
    g_app.mission.Rebuild(std::move(payload.items), std::move(payload.spaces), current);
    StartWatchingWindows();
}

void HandleMissionSpace(WPARAM which) {
    if (which == Mission::kSpaceAdd) {
        ChangeSpaces(true, 0);
        return;
    }

    if (which >= Mission::kSpaceCloseBase) {
        ChangeSpaces(false, static_cast<int>(which - Mission::kSpaceCloseBase));
        return;
    }

    // Clicking a desktop LOOKS at it. It does not go there, yet.
    //
    // The windows on another desktop are shell-cloaked but they are still
    // enumerable and still have geometry, so the arrangement can be built for
    // any of them without leaving, and the strip stays put while you look
    // around. Going there for real happens on the way out, or the moment one of
    // those windows is activated, which Windows does as part of the activation.
    g_app.mission.BrowseDesktop(static_cast<int>(which));
}

// --- Keeping the overlay honest while it is up -------------------------------
//
// Mission Control is a picture of the machine, and the machine does not stop
// while it is being looked at. A window closed from its own taskbar preview, an
// application quitting, a desktop created by another tool: any of those leaves
// the overlay showing something that is not there any more.
//
// Both watchers exist only while it is open. The budget for this process while
// nothing is happening is zero, and a hook that runs all day so that a window
// which is open for two seconds can be correct is not a trade worth making.

HWINEVENTHOOK    g_missionMinimize = nullptr;
HWINEVENTHOOK    g_missionDestroy  = nullptr;
std::vector<HWND> g_missionWindows;      // sorted, for the hook's lookup
size_t           g_missionDesktopCount = 0;
int              g_missionDesktopIndex = -1;

// Deliberately narrow.
//
// EVENT_OBJECT_DESTROY fires for every menu, tooltip and popup in the session,
// which is why foreground_history.cpp will not hook it at all. Here it is
// filtered to real top-level windows and then to the handful the overlay is
// actually showing, so nearly every one is two comparisons and a binary search.
void CALLBACK MissionWatchProc(HWINEVENTHOOK, DWORD, HWND hwnd, LONG objectId,
                               LONG childId, DWORD, DWORD) {
    if (!hwnd || objectId != OBJID_WINDOW || childId != CHILDID_SELF) return;
    if (!std::binary_search(g_missionWindows.begin(), g_missionWindows.end(), hwnd))
        return;

    ::PostMessageW(g_app.host, WM_MACTAB_MC_GONE, reinterpret_cast<WPARAM>(hwnd), 0);
}

void StopWatchingWindows() {
    if (g_missionMinimize) {
        ::UnhookWinEvent(g_missionMinimize);
        g_missionMinimize = nullptr;
    }
    if (g_missionDestroy) {
        ::UnhookWinEvent(g_missionDestroy);
        g_missionDestroy = nullptr;
    }
    g_missionWindows.clear();
    ::KillTimer(g_app.host, kMissionWatchTimer);
}

void StartWatchingWindows() {
    g_missionWindows = g_app.mission.Windows();
    std::sort(g_missionWindows.begin(), g_missionWindows.end());

    const desktops::State state = desktops::Query(g_app.host);
    g_missionDesktopCount = state.all.size();
    g_missionDesktopIndex = state.current;

    // Two registrations, because SetWinEventHook takes one contiguous range and
    // these two events are nowhere near each other.
    //
    // Minimising counts as going away: minimized windows are not in the
    // arrangement, here or on macOS, because there is no size to give something
    // that does not currently have one.
    if (!g_missionMinimize)
        g_missionMinimize = ::SetWinEventHook(
            EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZESTART,
            nullptr, MissionWatchProc, 0, 0,
            WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    if (!g_missionDestroy)
        g_missionDestroy = ::SetWinEventHook(
            EVENT_OBJECT_DESTROY, EVENT_OBJECT_DESTROY,
            nullptr, MissionWatchProc, 0, 0,
            WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    // Desktops have no notification of any kind, public or otherwise, so the
    // only way to notice one appearing is to look. Twice a second, two registry
    // reads each time, and only while the overlay is on screen.
    ::SetTimer(g_app.host, kMissionWatchTimer, 500, nullptr);

    // Anything that died between the list being built and the hooks going in.
    // That gap covers the whole of Show, which is where the thumbnails are
    // registered and the reveal starts, so it is not a narrow one.
    for (HWND hwnd : g_missionWindows)
        if (!::IsWindow(hwnd))
            ::PostMessageW(g_app.host, WM_MACTAB_MC_GONE,
                           reinterpret_cast<WPARAM>(hwnd), 0);
}

// A window the overlay was showing has gone.
void ForgetMissionWindow(HWND hwnd) {
    if (!g_app.mission.Visible()) return;
    if (!g_app.mission.ForgetWindow(hwnd)) return;

    MACTAB_DIAG("mission: %p went away", static_cast<void*>(hwnd));

    g_missionWindows = g_app.mission.Windows();
    std::sort(g_missionWindows.begin(), g_missionWindows.end());
}

// The windows the overlay OUGHT to be showing, as a set.
//
// A window going away is caught by the hook and acted on within a frame, which
// is what it deserves; a window arriving cannot be, because there is no event
// for "a window has become eligible for the switcher" and the ones that come
// closest fire for every menu, tooltip and splash screen in the session. So
// arrivals are noticed by looking, which is affordable at twice a second and
// only while the overlay is up.
std::vector<HWND> EligibleWindows(const desktops::State& state) {
    std::vector<HWND> handles;

    for (const SwitcherApp& app : BuildWindowList(state.known && state.all.size() > 1))
        for (const SwitcherWindow& window : app.windows)
            if (!window.minimized) handles.push_back(window.hwnd);

    std::sort(handles.begin(), handles.end());
    return handles;
}

// Anything that changed underneath us while we were looking at it.
void SyncMission() {
    if (!g_app.mission.Visible()) return;

    const desktops::State state = desktops::Query(g_app.host);
    if (!state.known) return;

    const bool desktopsChanged = state.all.size() != g_missionDesktopCount ||
                                 state.current != g_missionDesktopIndex;

    bool windowsChanged = false;
    if (!desktopsChanged) {
        std::vector<HWND> showing = g_app.mission.Windows();
        std::sort(showing.begin(), showing.end());
        windowsChanged = (showing != EligibleWindows(state));
    }

    if (!desktopsChanged && !windowsChanged) return;

    if (desktopsChanged)
        MACTAB_DIAG("mission: the desktops changed underneath us (%zu -> %zu, current %d -> %d)",
                    g_missionDesktopCount, state.all.size(),
                    g_missionDesktopIndex, state.current);
    else
        MACTAB_DIAG("mission: the window list changed underneath us");

    const bool moved = state.current != g_missionDesktopIndex;

    MissionPayload payload = BuildMissionPayload();
    if (!payload.known || payload.spaces.empty()) return;

    // Follow the view rather than closing. The overlay would otherwise be left
    // cloaked on a desktop nobody is on, which looks exactly like it crashed.
    if (moved)
        g_app.mission.FollowDesktop(
            payload.spaces[static_cast<size_t>(payload.current)].id);

    const int last    = static_cast<int>(payload.spaces.size()) - 1;
    const int browsed = g_app.mission.BrowsedDesktop();
    const int target  = moved ? payload.current
                              : (std::max)(0, (std::min)(last, browsed));

    g_app.mission.Rebuild(std::move(payload.items), std::move(payload.spaces), target);
    StartWatchingWindows();
}

// Step the strip by one, for Ctrl+Win+Left and Ctrl+Win+Right.
//
// Those never reach the overlay: the hook has to swallow them or the shell
// switches the desktop out from under an overlay that belongs to the old one, so
// it posts them here instead.
void StepMissionSpace(int delta) {
    if (!g_app.mission.Visible()) return;
    const int browsed = g_app.mission.BrowsedDesktop();
    if (browsed < 0) return;
    g_app.mission.BrowseDesktop(browsed + delta);
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

    // The material itself. Unchecked gives the plain tinted plate, which is the
    // same one the panel falls back to when it cannot see the desktop.
    ::AppendMenuW(settings, MF_STRING | (config::Current().glassEnabled ? MF_CHECKED : 0u),
                  IDM_TRAY_GLASS, L"Glass backdrop");

    ::AppendMenuW(settings, MF_STRING | (config::Current().selectionAnimation ? MF_CHECKED : 0u),
                  IDM_TRAY_SELECTION_ANIM, L"Animate the selection");

    ::AppendMenuW(settings, MF_SEPARATOR, 0, nullptr);
    // Named after the chord that is actually configured, so somebody who has set
    // MissionGesture is not told about a key that no longer opens anything.
    std::wstring missionLabel = L"Mission Control on ";
    missionLabel += MissionGestureName();

    ::AppendMenuW(settings, MF_STRING | (config::Current().missionEnabled ? MF_CHECKED : 0u),
                  IDM_TRAY_MISSION, missionLabel.c_str());
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
    ::AppendMenuW(menu, MF_STRING, IDM_TRAY_OPEN_SETTINGS, L"Open settings.ini");
    ::AppendMenuW(menu, MF_STRING, IDM_TRAY_RELOAD_GLASS, L"Reload settings.ini");

    // Disabled for the same reason the uninstall item is: a settings file that
    // was never created is a case worth explaining rather than a menu item that
    // does nothing.
    ::AppendMenuW(menu, MF_STRING | (config::SettingsPath().empty() ? (MF_DISABLED | MF_GRAYED) : 0u),
                  IDM_TRAY_RESET_SETTINGS, L"Reset settings.ini");
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

// Opens settings.ini in whatever the shell has associated with .ini, which on
// a machine with no override is Notepad. This is the tuning loop the glass
// material was built around: type a number, save, "Reload glass from
// settings.ini" from this same menu, look. No editor bundled or shelled out to
// by name, so it also respects whatever the user has actually set .ini to open
// in.
void OpenSettingsFile(HWND owner) {
    if (config::SettingsPath().empty()) {
        ::MessageBoxW(owner,
                      L"settings.ini has not been created yet.\n\n"
                      L"It is written on first run; relaunch MacTab and try again.",
                      L"MacTab", MB_OK | MB_ICONINFORMATION);
        return;
    }
    ::ShellExecuteW(owner, L"open", config::SettingsPath().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// The whole material, re-read, without a restart.
//
// Not config::Load(), which reassigns the themes directory string the icon
// worker reads on its own thread. This touches only the two Params and the
// shared optics, and nothing off this thread reads either.
//
// The panel picks them up at the next gesture, because it copies the theme's
// material when it lays out rather than caching one at startup. Mission
// Control's bar does not: it is baked once and kept, precisely so that walking
// the desktops does not re-run a full-width displacement map, so it has to be
// thrown away by hand or a reload would reach everything except the piece most
// likely to be tuned.
//
// UI thread only, which is the reason the file watcher posts a message instead
// of calling this itself.
void ReloadGlassFromSettings() {
    config::ReloadGlass();
    g_app.mission.InvalidateBackdrop();
}

void ApplySettings();

// The whole file, re-read and put into effect.
//
// What saving settings.ini runs, and what the tray item runs. It used to be
// ReloadGlassFromSettings above, which re-read the material and nothing else, so
// changing the hold delay or the Mission Control keys in the file did nothing
// until a restart and looked exactly like a key that does not work.
void ReloadSettingsFromFile() {
    config::Reload();
    ApplySettings();
}

// Everything the running process took from settings.ini at startup, taken from
// it again.
//
// Only the reset path needs this. An ordinary edit reaches the panel on its own,
// because the material is read per gesture, but the hold delay went into the
// hook's options once at Start and Mission Control's chord is registered rather
// than polled, so those two have to be pushed rather than pulled.
void ApplySettings() {
    hotkey::SetTiming(config::Current().revealDelayMs, config::Current().leftAltOnly);
    capture::Force(capture::ParseSource(config::Current().captureSource.c_str()));

    // A machine that starts working deserves to be told about it again if it
    // stops. Cheap, and it makes the balloon a per-attempt answer rather than a
    // once-ever one while somebody is working through the CaptureSource values.
    g_app.reportedFlatPanel = false;

    const bool mission = config::Current().missionEnabled;
    if (mission) {
        // Built on demand, exactly as switching it on from the menu does, since
        // a reset can turn it on as well as off.
        if (!EnsureMission()) config::SetMissionEnabled(false);
    } else {
        CloseMission();
    }

    hotkey::SetMissionGesture(MissionGesture());
    g_app.mission.InvalidateBackdrop();
}

// Put settings.ini back the way it ships.
//
// Asks first, because it throws away tuning that may have taken a while, and
// says where the old file went. The warning about an open editor is not
// decoration: an editor holding the old text will happily write it back over
// this, and the user would read that as the reset not working.
void ResetSettingsFile(HWND owner) {
    if (config::SettingsPath().empty()) {
        ::MessageBoxW(owner,
                      L"settings.ini has not been created yet.\n\n"
                      L"It is written on first run; relaunch MacTab and try again.",
                      L"MacTab", MB_OK | MB_ICONINFORMATION);
        return;
    }

    std::wstring prompt = L"Put settings.ini back the way MacTab ships it?\n\n"
                          L"Everything you have changed in it, including the glass, "
                          L"goes back to the shipped defaults. The file you have now "
                          L"is kept as:\n\n";
    prompt += config::SettingsPath();
    prompt += L".bak\n\nClose any editor that has settings.ini open first, or saving "
              L"from it afterwards will put the old file back.";

    if (::MessageBoxW(owner, prompt.c_str(), L"MacTab",
                      MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
        return;

    if (!config::ResetSettings()) {
        ::MessageBoxW(owner,
                      L"MacTab could not reset settings.ini.\n\n"
                      L"Nothing has been changed. This usually means another program "
                      L"has the file open; close it and try again.",
                      L"MacTab", MB_OK | MB_ICONERROR);
        return;
    }

    ApplySettings();
    g_app.tray.ShowBalloon(L"MacTab",
                           L"settings.ini is back to defaults. The old one is "
                           L"settings.ini.bak.");
}

// --- Shutdown --------------------------------------------------------------

// Teardown must be idempotent: it can be reached through the tray menu, a
// WM_CLOSE from the installer, or WM_ENDSESSION at logoff.
void ShutdownSubsystems() {
    if (g_app.shuttingDown) return;
    g_app.shuttingDown = true;

    hotkey::Stop();
    settings_watch::Stop();
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
        // wParam is 1 when the user meant to leave, which is what decides
        // whether the desktop they were looking at becomes the one they are on.
        CloseMission(wParam != 0);
        return 0;

    case settings_watch::WM_MACTAB_SETTINGS_CHANGED:
        // Arming a timer that is already armed restarts it, which is the whole
        // debounce: a burst of notifications from one save collapses into a
        // single reload once the file has been still for the delay.
        ::SetTimer(hwnd, kSettingsReloadTimer, kSettingsReloadDelayMs, nullptr);
        return 0;

    case WM_TIMER:
        if (wParam == kMissionWatchTimer) SyncMission();
        if (wParam == kSettingsReloadTimer) {
            // One-shot: Win32 timers repeat, and this one has nothing to do
            // until the file changes again.
            ::KillTimer(hwnd, kSettingsReloadTimer);

            // The watch is on the folder, so most of what wakes it is not an
            // edit. Anything else we keep in there asks for a reload it does not
            // need, and with --diag that is a loop rather than an annoyance,
            // because the reload logs and the log is in the watched folder.
            if (!config::ChangedOnDisk())
                return 0;

            MACTAB_DIAG("host: settings.ini changed, re-reading it");
            ReloadSettingsFromFile();
        }
        return 0;

    case WM_MACTAB_MC_GONE:
        ForgetMissionWindow(reinterpret_cast<HWND>(wParam));
        return 0;

    case hotkey::WM_MACTAB_MISSION_STEP:
        StepMissionSpace(static_cast<int>(static_cast<INT_PTR>(wParam)));
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

            hotkey::SetMissionGesture((enabled && ready) ? MissionGesture()
                                                        : hotkey::Gesture::None);
            if (!enabled) CloseMission();

            std::wstring message = MissionGestureName();
            message += (enabled && ready) ? L" now opens Mission Control."
                                          : L" left to Windows.";
            g_app.tray.ShowBalloon(L"MacTab", message.c_str());
            return 0;
        }

        case IDM_TRAY_SELECTION_ANIM: {
            // The next keystroke, not the next gesture: the panel reads this
            // where it moves the highlight, so it applies inside a hold that is
            // already in progress.
            const bool animate = !config::Current().selectionAnimation;
            config::SetSelectionAnimation(animate);
            g_app.tray.ShowBalloon(L"MacTab",
                                   animate ? L"The selection springs across again."
                                           : L"The selection moves without animating.");
            return 0;
        }

        case IDM_TRAY_GLASS: {
            // Next gesture, like the display and appearance items above it: the
            // panel decides whether to grab the desktop when the gesture starts,
            // so there is nothing to rebuild here. Mission Control's bar is
            // baked and kept, so that one does have to be thrown away.
            const bool glass = !config::Current().glassEnabled;
            config::SetGlassEnabled(glass);
            g_app.mission.InvalidateBackdrop();
            g_app.tray.ShowBalloon(L"MacTab",
                                   glass ? L"Glass on. Alt+Tab to see it."
                                         : L"Glass off. The panel is a plain plate now.");
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

        case IDM_TRAY_OPEN_SETTINGS:
            OpenSettingsFile(hwnd);
            return 0;

        case IDM_TRAY_RESET_SETTINGS:
            ResetSettingsFile(hwnd);
            return 0;

        case IDM_TRAY_RELOAD_GLASS: {
            // Kept even though saving the file now does this on its own. It is
            // the way to re-read a file that was edited while the watcher was
            // not running, and it is the only one of the two that says out loud
            // that it worked.
            ReloadSettingsFromFile();

            // With the numbers in it, because "reloaded" on its own answers the
            // wrong question. The one thing somebody tuning by hand cannot tell
            // is whether the file was read at all or read and ignored, and on
            // this project nobody can look over their shoulder and see. Two
            // values are enough to settle it: the blur, and the dark tint's
            // alpha.
            wchar_t message[200] = L"";
            ::swprintf(message, ARRAYSIZE(message),
                       L"%d glass value%s set in the file. Blur %.4g, dark tint "
                       L"alpha %.4g. Alt+Tab to see it.",
                       config::GlassOverrides(),
                       config::GlassOverrides() == 1 ? L"" : L"s",
                       static_cast<double>(glass::g_tuning.blurSigma),
                       static_cast<double>(config::Current().glassDark.tint[3]));
            g_app.tray.ShowBalloon(L"MacTab", message);
            return 0;
        }

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
        // A monitor came or went, so the overlays are sized and positioned for
        // a desktop that no longer exists. Dropping the wallpapers is not
        // enough; the windows themselves have to be made again.
        //
        // The rest of the state that says Mission Control is open has to come
        // down with them. Without this the hook goes on swallowing Ctrl+Win+Left
        // and Ctrl+Win+Right and posting them at an overlay that no longer
        // exists, so the user's desktop-switch keys are dead until the next time
        // Mission Control is opened and closed properly, and both window watchers
        // are left installed for the rest of the session.
        if (g_app.mission.Visible()) {
            StopWatchingWindows();
            hotkey::SetMissionOpen(false);
        }
        g_app.mission.DisplaysChanged();
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
    capture::Force(capture::ParseSource(config::Current().captureSource.c_str()));
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

    // Not fatal. Without it the glass only reloads from the tray item, which is
    // exactly what shipped before this, so there is nothing here worth refusing
    // to start over.
    if (!settings_watch::Start(g_app.host))
        MACTAB_WARN("boot: settings.ini is not being watched; reload the glass from the tray");

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
    hotkeyOptions.missionGesture = MissionGesture();
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
