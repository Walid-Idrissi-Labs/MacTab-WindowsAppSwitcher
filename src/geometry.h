#pragma once

#include "pch.h"
#include <d2d1_1.h>

#include "com.h"
#include "image.h"

namespace mactab {

// Upload a CPU bitmap for use as a D2D effect input or a DrawBitmap source.
//
// Takes the bitmap by value because it has to premultiply it, and every caller
// either builds it fresh or is handing over a copy it does not need again.
//
// Here rather than in each of the three files that draws, which is where it was.
// Three copies of a bitmap upload is two too many, and the one in panel.cpp was
// the only one anything type-checked.
ComPtr<ID2D1Bitmap1> UploadBitmap(ID2D1DeviceContext* dc, Bitmap image);

// A rounded rectangle whose corners follow a superellipse rather than a
// circular arc.
//
// This is the shape of the panel, and the reason it cannot be a
// D2D1RoundedRectangle or a CompositionRoundedRectangleGeometry: a circular
// corner meets the straight edge with a discontinuity in curvature, and that
// discontinuity is precisely what the eye reads as "Windows rounded corner".
// Apple's continuous corner has no such break.
//
// `exponent` is n. It is not one number for the whole app: fitting the real
// macOS switcher gives n = 2.24 for the panel outline and n ~= 2.5 for the app
// icons, and the icon tiles are drawn with n = 5 because their corner extent is
// half the shape's own width, where the two parameterisations converge. Use
// layout::kPanelCornerExponent for the panel and its shadow, and 5 for anything
// that has to sit in the icons' shape language, like the selection highlight.
//
// The curve is emitted as a sampled polyline. At 24 segments per corner the
// result is indistinguishable from an analytic curve at panel sizes, and it
// avoids fitting Béziers to a shape that has no exact Bézier form.
ComPtr<ID2D1PathGeometry> CreateSquircleGeometry(ID2D1Factory* factory,
                                                 float width, float height, float radius,
                                                 float exponent = 5.0f);

} // namespace mactab
