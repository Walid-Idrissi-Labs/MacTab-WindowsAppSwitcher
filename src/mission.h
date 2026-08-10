#pragma once

#include "pch.h"
#include "image.h"

namespace mactab {

// One window in the arrangement.
struct MissionItem {
    HWND         hwnd = nullptr;
    std::wstring title;
    std::wstring appName;
    Bitmap       icon;       // the app's, shared by all its windows
    RECT         bounds{};   // where the window really is, virtual-screen coords
    int          group = 0;  // app index, for the grouped arrangement
    int          order = 0;  // most recently used first
};

// One entry in the strip across the top.
struct MissionSpace {
    std::wstring name;
    bool         current = false;
};

// Mission Control.
//
// A second overlay window rather than a mode of the switcher's panel, sharing
// the same process, thread, compositor and D2D device. Sharing the compositor
// is not an economy, it is a requirement: the thumbnail visuals DWM hands back
// have to be inserted into the tree that compositor owns.
//
// Not sharing the WINDOW is equally deliberate. The panel is pre-warmed and
// never destroyed so that Alt+Tab can reveal in one frame, and stretching it to
// full screen would put Mission Control's costs on that path.
class Mission {
public:
    Mission();
    ~Mission();

    Mission(const Mission&)            = delete;
    Mission& operator=(const Mission&) = delete;

    // `compositorOwner` must already have created a Compositor on this thread,
    // which the panel does at startup. Mission Control attaches to it.
    //
    // Interaction is reported to `notifyWindow`:
    //   activateMessage  wParam is the item index to switch to
    //   dismissMessage   wParam is 0
    //   spaceMessage     wParam is the space index, or the sentinel below
    bool Initialize(HINSTANCE instance, HWND notifyWindow,
                    UINT activateMessage, UINT dismissMessage, UINT spaceMessage);
    void Shutdown();
    bool Ready() const;

    // wParam values for spaceMessage that are not a space index.
    static constexpr WPARAM kSpaceAdd   = 0xFFFF;
    static constexpr WPARAM kSpaceClose = 0xFFFE;

    // Build and reveal. `items` may be empty, which shows the spaces strip over
    // an empty desktop, exactly as macOS does with nothing open.
    void Show(HMONITOR monitor, std::vector<MissionItem> items,
              std::vector<MissionSpace> spaces);
    // `restoreFocus` puts foreground back where it was before the overlay took
    // it. Pass false when the caller is about to activate something itself, so
    // the old window does not flash up in between.
    void Hide(bool restoreFocus = true);
    bool Visible() const;

    // The window behind tile `index`, or null. Read before Hide(), which throws
    // the items away.
    HWND ItemWindow(int index) const;

    HWND Hwnd() const;

    struct Impl;

private:
    std::unique_ptr<Impl> m_impl;
};

} // namespace mactab
