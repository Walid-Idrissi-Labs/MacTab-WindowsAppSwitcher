#pragma once

#include "pch.h"
#include "image.h"

namespace mactab {

// One window in the arrangement.
struct MissionItem {
    HWND         hwnd = nullptr;
    std::wstring title;
    std::wstring appName;
    std::wstring appKey;     // grouping key, and how a late icon finds its way here
    Bitmap       icon;       // the app's, shared by all its windows
    RECT         bounds{};   // where the window really is, virtual-screen coords
    int          group = 0;  // app index, for the grouped arrangement
    int          order = 0;  // most recently used first

    // Which desktop it lives on, as an index into the spaces. -1 means the
    // window is pinned to every desktop, or its desktop could not be read, and
    // either way it shows on all of them.
    int          desktop = -1;
};

// One entry in the bar across the top.
struct MissionSpace {
    std::wstring name;
    GUID         id{};
    bool         current = false;
};

// Mission Control.
//
// One overlay per display, all shown together, sharing one compositor and one
// set of devices. macOS puts a separate Mission Control on every screen and so
// does this.
//
// A separate window from the switcher's panel, deliberately. The panel is
// pre-warmed and never destroyed so Alt+Tab can reveal in one frame, and
// stretching it to full screen would put this feature's costs on that path.
class Mission {
public:
    Mission();
    ~Mission();

    Mission(const Mission&)            = delete;
    Mission& operator=(const Mission&) = delete;

    // Interaction is reported to `notifyWindow`:
    //   activateMessage  wParam is the item index to switch to
    //   dismissMessage   wParam is 0
    //   spaceMessage     wParam is the space index, or a sentinel below
    bool Initialize(HINSTANCE instance, HWND notifyWindow,
                    UINT activateMessage, UINT dismissMessage, UINT spaceMessage);
    void Shutdown();
    bool Ready() const;

    // Do now everything that can be done before the first invocation: read the
    // wallpapers, bake the backdrops, and bake the shadow and outline textures.
    //
    // The gesture is judged on how fast it comes up, and almost all of the cost
    // is work that does not depend on which windows are open. Doing it here
    // means the first Win+Tab costs what the hundredth costs. Safe to call more
    // than once.
    void Prewarm();

    // wParam values for spaceMessage.
    //
    // A bare index means "look at this desktop". kSpaceAdd means the plus button.
    // kSpaceCloseBase plus an index means the little cross on that desktop, which
    // has to name which one rather than meaning "the current one": the whole
    // point of the strip is that you are looking at a desktop you are not on.
    static constexpr WPARAM kSpaceAdd       = 0xFFFF;
    static constexpr WPARAM kSpaceCloseBase = 0x8000;

    // Build and reveal. Items may name windows on any display and any desktop;
    // each is arranged on the display that holds it, and only the windows of
    // `desktop` are shown to begin with. An empty list shows the bar over an
    // empty desktop, exactly as macOS does with nothing open.
    void Show(std::vector<MissionItem> items, std::vector<MissionSpace> spaces,
              int desktop);

    // Look at another desktop without leaving. The arrangement slides out in
    // the direction of travel and the new one slides in behind it; the desktop
    // itself is not switched until a window on it is activated, which Windows
    // does as part of the activation.
    void BrowseDesktop(int index);

    // Which desktop is being looked at, or -1.
    int  BrowsedDesktop() const;

    // Move the overlays onto `desktop` and put them back in front.
    //
    // For the two things that change the machine from inside: adding a desktop
    // and closing one both move the view, and these windows keep whichever
    // desktop they were assigned to, so without this the overlay is left cloaked
    // somewhere nobody is looking.
    void FollowDesktop(const GUID& desktop);

    // Rebuild in place, for a change to the machine rather than to the view: a
    // desktop added or closed, or a window that has gone away. Everything flies
    // from where it currently is to where it now belongs, so the overlay reads
    // as reacting rather than as being thrown away and made again.
    //
    // `desktop` is the one to show afterwards; pass BrowsedDesktop() to stay.
    void Rebuild(std::vector<MissionItem> items, std::vector<MissionSpace> spaces,
                 int desktop);

    // Drop a window that no longer exists, and re-relax everything around it.
    // False when the overlay was not showing that window anyway.
    bool ForgetWindow(HWND hwnd);

    // Every window the overlay is currently showing, so the caller can watch
    // them without keeping a second list in step with this one.
    std::vector<HWND> Windows() const;

    // Lower the windows back onto the desktop and leave.
    //
    // `restoreFocus` puts foreground back where it was before the overlay took
    // it. Pass false when the caller is about to activate something itself, so
    // the old window does not flash up in between.
    //
    // `immediate` skips the collapse. For the paths that cannot wait for it: a
    // desktop switch runs its own animation underneath, and an overlay still on
    // screen while the desktop slides under it looks like a fault.
    void Hide(bool restoreFocus = true, bool immediate = false);

    // Take it off the screen this instant, collapse or no collapse. For shutdown
    // and for anything that must not leave a full-screen window up.
    void HideNow();
    bool Visible() const;

    // The window behind item `index`, or null. Read before Hide(), which throws
    // the items away.
    HWND ItemWindow(int index) const;

    // A late icon from the worker. Redraws only the windows of that app.
    void UpdateIcon(const std::wstring& appKey, const Bitmap& icon);

    // Drop the baked backdrops, for a wallpaper change.
    void InvalidateBackdrop();

    // Rebuild the per-display overlays.
    //
    // Not the same as invalidating the backdrops. A monitor that was unplugged
    // leaves an overlay sized and positioned for a screen that no longer
    // exists, and a new one has no overlay at all, so the windows themselves
    // have to be made again.
    void DisplaysChanged();

    struct Impl;

private:
    std::unique_ptr<Impl> m_impl;
};

} // namespace mactab
