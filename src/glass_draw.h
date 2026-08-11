#pragma once

#include "pch.h"

#include <d2d1_1.h>

#include "capture.h"
#include "glass.h"
#include "glass_map.h"

// Cutting one piece of the glass material and drawing it into a surface.
//
// This used to be a private member of Panel::Impl, which was fine while the
// switcher was the only thing made of glass. It is not any more: Mission Control
// has to be the same material, and "the same material" has to mean the same
// code, not a second implementation that agrees with this one on the day it is
// written and drifts afterwards.
//
// Two things worth knowing about the shape of this file.
//
// It has no C++/WinRT in it, deliberately. panel.cpp and mission.cpp both do,
// and neither can be compiled anywhere but on a Windows machine with the SDK,
// which on this project means neither can be compiled before CI. Direct2D has
// mingw-w64 headers, so moving the material here puts the most intricate drawing
// code in the project somewhere ./tools/syntax-check.sh can actually type-check.
//
// And it takes the backdrop as a capture::Frame rather than reaching for one.
// The switcher's frame is a grab of the live desktop. Mission Control's is the
// wallpaper, because its whole point is that the windows have been lifted off
// the desktop and a grab would put every one of them back again, twice. Same
// material, different thing behind it, which is what glass is.

namespace mactab::glass {

// Everything one piece of glass needs to know.
//
// `frame` may be null, or hold no pixels, which is the degraded path: a wedged
// GPU, a remote session, a capture that missed its deadline. That case is drawn
// as a nearly opaque base coat rather than left transparent, because a material
// with nothing behind it reads as a rendering fault.
struct Piece {
    ID2D1DeviceContext*   dc      = nullptr;
    ID2D1Factory*         factory = nullptr;
    const capture::Frame* frame   = nullptr;

    // The theme's material, BEFORE adaptation. Draw() adapts it to whatever the
    // backdrop under this particular rectangle turns out to be, so two pieces of
    // the same glass in different places over the same wallpaper get their own
    // operating points.
    Params base = kDark;

    float dpiScale = 1.0f;

    // Corner extent parameterisation, see geometry.h. The panel outline is
    // fitted at 2.24; anything that has to sit in the icons' shape language uses
    // 5.
    float cornerExponent = 2.24f;
};

// The mean luma of the frame under a rect, in the same 0..1 sRGB-encoded space
// the material arithmetic uses. Half when there is no frame, which is the
// neutral answer and the one that leaves the base parameters alone.
float BackdropLumaIn(const capture::Frame* frame, const RECT& screenRect);

// The material adapted to what is actually behind that rectangle.
//
// Exposed because callers need it for decisions made before anything is drawn:
// whether a label needs a shadow, what a rim will reflect. Draw() calls it
// itself, so drawing does not need this first.
Params MaterialFor(const capture::Frame* frame, const Params& base,
                   const RECT& screenRect);

// Draw one piece: the region `screenRect` of the backdrop, treated, refracted
// and tinted, clipped to a squircle of `radius`, with its top-left landing at
// `surfaceOffset` in the current surface.
//
// Leaves the device context's transform as identity and its primitive blend as
// source-over.
void Draw(const Piece& piece, POINT surfaceOffset, const RECT& screenRect,
          float radius);

// How much desktop to grab around a piece before drawing it.
//
// Whichever of the two needs more: the blur wants 1.5 sigma, and the refraction
// pulls in from up to kMaxDisplacement outside the outline. The max is not
// decoration; at a sigma of 8 the lens is the one that wins, and taking the
// blur's number alone leaves the rim sampling pixels that were never captured.
float MarginPx(float dpiScale);

} // namespace mactab::glass
