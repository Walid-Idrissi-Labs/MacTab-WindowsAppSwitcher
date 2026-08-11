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
    // True when the mark is a solid shape that can stand on its own. False
    // means it is a sparse glyph and needs a tile generated behind it.
    bool artwork = false;

    // Vertical gradient for the synthesised tile: the background colour that
    // was stripped off the source when there was one, otherwise the icon's own
    // dominant colour. Unused when `artwork` is true.
    uint32_t tintTop    = 0;
    uint32_t tintBottom = 0;
};

// An icon reduced to the part of it that is actually artwork.
struct IconPrep {
    Bitmap       content;    // background removed, cropped to the mark
    BorderFill   fill;       // the background that was removed, if any
    IconAnalysis analysis;
};

// Strip any baked-in background, crop to the mark, and decide how to present it.
//
// The cropping matters more than it sounds. A Windows icon is very often a
// small mark adrift in a mostly empty 256x256 canvas, either because the app
// ships nothing bigger than a 48px frame and the shell centred it, or because
// the background was painted in. Scaling that canvas as a whole reproduces all
// the emptiness and leaves a dot in the middle of the tile.
//
// The classification looks at how full the mark's OWN bounding box is, not how
// much of the canvas it covers, because the canvas is mostly an artifact of
// wherever the icon came from. A circle fills 79% of its box, a rounded square
// most of it, a letterform around a third.
IconPrep PrepareIcon(const Bitmap& source);

// The finished tile, `size` x `size`, straight alpha, transparent padding
// around the shape.
Bitmap MakeIconTile(const Bitmap& source, int size);

} // namespace mactab
