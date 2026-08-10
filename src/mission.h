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

    // wParam values for spaceMessage that are not a space index.
    static constexpr WPARAM kSpaceAdd   = 0xFFFF;
    static constexpr WPARAM kSpaceClose = 0xFFFE;

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

    // `restoreFocus` puts foreground back where it was before the overlay took
    // it. Pass false when the caller is about to activate something itself, so
    // the old window does not flash up in between.
    void Hide(bool restoreFocus = true);
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
