#pragma once

#include "pch.h"
#include "image.h"

// Grabbing the desktop pixels that sit behind the panel.
//
// WHY NOT Windows.Graphics.Capture, which is the modern answer: two hard
// blockers. CreateForMonitor requires Windows 10 1903, above this project's
// 1803 floor. And the system draws a yellow border around a captured monitor;
// GraphicsCaptureSession.IsBorderRequired, which turns it off, only exists from
// build 20348, so no Windows 10 client build can remove it. A full-screen
// yellow flash on every Alt+Tab would be fatal to the illusion.
//
// WHY NOT the host backdrop brush, which would avoid capturing at all:
// DWMWA_USE_HOSTBACKDROPBRUSH is Windows 11 22000+, and the documentation for
// CreateHostBackdropBrush states the app cannot read its pixels back, so there
// is no way to detect the known case where it renders solid black, and no way
// to probe it. Its transparency is also user- and power-policy-controlled,
// meaning battery saver can silently flatten the effect.
//
// So: capture one frame ourselves and blur it. For a panel that is on screen
// for a moment while the desktop behind it is static, a frozen backdrop is
// indistinguishable from a live one, and it buys exact control over radius,
// tint and blur, which is the whole point.

namespace mactab::capture {

enum class Source {
    None,                 // capture failed; caller should fall back to a flat tint
    DesktopDuplication,   // preferred: no border, no prompt, GPU-side frame
    GdiBitBlt,            // works everywhere including RDP and VMs
    GdiPlain,             // the same blit without CAPTUREBLT
};

const char* SourceName(Source source);

// Force one path, from settings.ini, or Source::None for "try them all".
//
// Here for the reason MissionThumbnails is: which of these works is a property
// of a machine nobody developing MacTab can log into, and a user who can see the
// panel is a better test than any amount of reasoning about DWM. Set
// CaptureSource in settings.ini and the answer comes back as "this one works",
// which is worth more than a theory.
//
// Written on the UI thread when the settings are read, and read on the capture
// worker. A plain enum, for the same reason hotkey::SetMissionGesture is safe:
// it cannot tear, and a stale read costs one gesture the path it would have
// preferred.
void Force(Source source);
Source Forced();

// Parse the settings keyword: "auto", "duplication", "bitblt" or "plain".
Source ParseSource(const wchar_t* keyword);

struct Frame {
    Bitmap pixels;
    Source source = Source::None;

    // Where `pixels` actually came from, in virtual-screen coordinates.
    //
    // This is NOT always the rect that was asked for: the inflated capture
    // region is clamped to the monitor, so a panel near a screen edge gets back
    // a smaller, shifted frame. Callers must position from this rather than
    // assuming the requested origin, or the blur slides sideways.
    RECT bounds{};
};

// Capture the screen region `rect` (virtual-screen coordinates).
//
// `rect` should already be inflated by the blur margin; capturing only the
// panel's neighbourhood rather than the whole monitor is the key economy here:
// a 1100x260 panel plus margin moves about 1.5 MB, where a 4K monitor would be
// 33 MB.
//
// Safe to call from a worker thread. Never throws; on total failure returns a
// Frame with source == None.
Frame GrabRegion(const RECT& rect);

// Release cached devices. Desktop duplication is deliberately not kept open
// between gestures: whether an idle open duplication makes DWM do extra
// per-frame work is undocumented, and a 0%-idle budget cannot absorb
// "probably fine".
void ReleaseCachedResources();

} // namespace mactab::capture
