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

    // Build and reveal. Items may name windows on any display; each is arranged
    // on the display that holds it. An empty list shows the bar over an empty
    // desktop, exactly as macOS does with nothing open.
    void Show(std::vector<MissionItem> items, std::vector<MissionSpace> spaces);

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

    // Drop the baked backdrops, for a wallpaper or display change.
    void InvalidateBackdrop();

    struct Impl;

private:
    std::unique_ptr<Impl> m_impl;
};

} // namespace mactab
