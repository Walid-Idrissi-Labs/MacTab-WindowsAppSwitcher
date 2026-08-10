#pragma once

#include "pch.h"
#include <d2d1_1.h>

#include "com.h"

namespace mactab {

// A rounded rectangle whose corners follow a superellipse rather than a
// circular arc.
//
// This is the shape of the panel, and the reason it cannot be a
// D2D1RoundedRectangle or a CompositionRoundedRectangleGeometry: a circular
// corner meets the straight edge with a discontinuity in curvature, and that
// discontinuity is precisely what the eye reads as "Windows rounded corner".
// Apple's continuous corner has no such break. n = 5 is the usual
// approximation.
//
// The curve is emitted as a sampled polyline. At 24 segments per corner the
// result is indistinguishable from an analytic curve at panel sizes, and it
// avoids fitting Béziers to a shape that has no exact Bézier form.
ComPtr<ID2D1PathGeometry> CreateSquircleGeometry(ID2D1Factory* factory,
                                                 float width, float height, float radius);

} // namespace mactab
