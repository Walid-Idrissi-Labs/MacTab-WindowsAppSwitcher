#pragma once

#include "pch.h"
#include "image.h"

namespace mactab {

struct PanelItem {
    std::wstring key;
    std::wstring label;
    Bitmap       icon;      // empty renders a neutral placeholder
};

// The switcher panel.
//
// A single pre-warmed, never-destroyed window whose whole visual tree is built
// once at startup and only re-laid-out per invocation. That is what makes the
// one-frame reveal budget reachable: showing the panel is a SetWindowPos plus a
// handful of property writes, never a construction.
class Panel {
public:
    Panel();
    ~Panel();

    Panel(const Panel&)            = delete;
    Panel& operator=(const Panel&) = delete;

    // Creates the apartment, compositor, devices and the hidden window.
    // Must be called on the UI thread, and that thread must then pump messages.
    bool Initialize(HINSTANCE instance);
    void Shutdown();

    bool Ready() const;

    // Replace the contents. Cheap: reuses existing visuals where possible.
    void SetItems(std::vector<PanelItem> items, int selectedIndex);

    // Move the highlight. Animates.
    void SetSelection(int index);

    // Update one item's icon in place, for when the worker finishes late.
    void UpdateIcon(const std::wstring& key, const Bitmap& icon);

    void Show();
    void Hide();
    bool Visible() const;

    // Index under a screen point, or -1. The window has no per-pixel hit
    // testing (see the WS_EX_NOREDIRECTIONBITMAP note in panel.cpp), so this is
    // a plain test against the computed layout.
    int HitTest(POINT screenPoint) const;

    HWND Hwnd() const;

    // Physical-pixel tile size for the monitor the panel will appear on, so the
    // icon worker can rasterise at exactly the right size.
    int TileSizePx() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace mactab
