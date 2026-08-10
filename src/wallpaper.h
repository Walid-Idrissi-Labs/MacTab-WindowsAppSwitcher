#pragma once

#include "pch.h"
#include "image.h"

// The desktop wallpaper, as pixels.
//
// WHY THIS EXISTS RATHER THAN ANOTHER SCREEN CAPTURE.
//
// The switcher captures the screen because its panel floats over whatever is
// there and has to refract it. Mission Control is the opposite gesture: the
// windows lift off and the desktop is what remains. Its backdrop is the
// wallpaper, and everything else in the shot is a window that is currently
// flying to a new position.
//
// Capturing the screen for it would put every window into the blurred backdrop
// as well as into the arrangement, so each one would appear twice, once sharp
// and once as a ghost behind itself. It also costs a full-screen readback on
// the reveal path.
//
// The wallpaper does not change while the machine is idle, so this is baked
// once, cached, and thrown away when the wallpaper changes. The reveal path
// then costs nothing at all.

namespace mactab::wallpaper {

// The wallpaper for `monitor`, scaled to cover `width` by `height` and cropped
// to it. Empty if there is no wallpaper set or it could not be read, in which
// case callers should fall back to SolidColour().
//
// Cached: repeated calls with the same monitor and size return the same bitmap
// without touching the disk. Safe to call from a worker thread.
Bitmap ForMonitor(HMONITOR monitor, int width, int height);

// The desktop background colour, which is what shows through when no wallpaper
// is set or the picture does not cover the screen. Always valid.
uint32_t SolidColour();

// Drop the cache. Call when the wallpaper changes, which arrives as
// WM_SETTINGCHANGE with SPI_SETDESKWALLPAPER.
void Invalidate();

} // namespace mactab::wallpaper
