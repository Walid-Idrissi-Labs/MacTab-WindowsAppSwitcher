#pragma once

// Like image.h, free of windows.h so it can be compiled and run natively for
// visual verification, see tools/preview/.
#include <cstdint>
#include <vector>

#include "image.h"

// Turning a Windows app icon into something that reads as macOS.
//
// The shape is a superellipse, |x/a|^n + |y/a|^n = 1 with n = 5, which is the
// usual close approximation of Apple's continuous-corner curve. A true rounded
// rectangle is visibly wrong next to it: the curvature jumps discontinuously
// where the arc meets the straight edge, and that discontinuity is exactly what
// makes Windows' rounded corners look like Windows.
//
// Proportions follow the macOS icon grid: on a 1024 canvas the shape is 824
// wide, so the artwork occupies ~80.5% of the tile.
//
// One important non-goal: this does NOT force every icon into a squircle.
// macOS does not either; Chrome is a circle on macOS and stays a circle. The
// mask only clips artwork that would otherwise stick out past the shape.
// Synthesising a coloured tile is reserved for icons that are small glyphs on
// transparency, which would otherwise float in the panel with nothing behind
// them.

namespace mactab {

// Antialiased coverage mask, `size` x `size`, one byte per pixel.
//
// Cached per size and reused across every icon, so the (comparatively
// expensive) rasterisation happens a handful of times per session rather than
// once per app.
const std::vector<uint8_t>& SquircleMask(int size);

struct IconAnalysis {
    // True when the icon covers enough of its canvas to stand on its own.
    // False means it is a glyph and needs a tile generated behind it.
    bool artwork = false;

    // Vertical gradient for the synthesised tile, derived from the icon's own
    // dominant colour. Unused when `artwork` is true.
    uint32_t tintTop    = 0;
    uint32_t tintBottom = 0;
};

IconAnalysis AnalyzeIcon(const Bitmap& icon);

// The finished tile, `size` x `size`, straight alpha, transparent padding
// around the shape.
Bitmap MakeIconTile(const Bitmap& source, int size);

} // namespace mactab
