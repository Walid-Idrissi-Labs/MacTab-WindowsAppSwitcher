#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "glass.h"
#include "image.h"

// The two bitmaps that make the panel read as a piece of glass rather than a
// blurred rectangle: where the rim bends what is behind it, and where its
// surface catches the light.
//
// Free of windows.h and of every D2D type, same as glass.h and image.h, so
// tools/preview generates the identical maps on macOS and can measure them. The
// alternative was a Direct2D pixel shader, which would have put the one part of
// the material nobody can look at before shipping into the one place nothing can
// check it.
//
// Built on the CPU once per gesture, which is affordable because the backdrop is
// a frozen frame: the panel is drawn once and then sits there. Every pixel goes
// through the distance function, so a 1900x350 physical panel costs about two
// million square roots, a few milliseconds, and only the band within a bezel
// width of the edge does more work than that.

namespace mactab::glass {

// A piece of glass, in physical pixels.
struct Surface {
    float width    = 0.0f;
    float height   = 0.0f;
    float radius   = 0.0f;   // corner extent
    float dpiScale = 1.0f;
};

// The optics constants from glass.h, in physical pixels and scaled down for
// shapes too short to carry a full bezel.
//
// The app name's capsule is 28 logical pixels tall against a 14px bezel and a
// 13px rim span, so without this it would be lens all the way through and the
// text would sit on nothing but distortion. The depth scales with the bezel so
// the profile keeps its shape and only its size changes; scaling the bezel alone
// would make a short capsule refract harder than the panel, not softer.
struct Optics {
    float bezel = 0.0f;
    float depth = 0.0f;
    float rimSpan = 0.0f;
    float specInner = 0.0f;
    float specOuter = 0.0f;
    float maxDisplacement = 0.0f;
};

inline Optics OpticsFor(const Surface& s) {
    const float shortest = (std::min)(s.width, s.height);
    const float full     = kBezelWidth * s.dpiScale;

    Optics o;

    // 0.30 rather than 0.35, and the feather scales with the bezel, so that a
    // short shape keeps a frosted core. The two together have to stop short of
    // the middle: the capsule is 28 logical pixels tall, and at 0.35 with an
    // unscaled 6px feather the clear tap still read 30% at the capsule's own
    // midline, which put the app name on a pane of sigma-8 wallpaper while the
    // panel above it stayed frosted. At 0.30 the fade is done 2px short of the
    // middle.
    o.bezel = (std::min)(full, shortest * 0.30f);

    const float ratio = (full > 0.0f) ? (o.bezel / full) : 0.0f;
    o.depth   = kGlassDepth * s.dpiScale * ratio;

    o.rimSpan = (std::min)(kRimSpan * s.dpiScale, shortest * 0.35f);
    o.specInner = kSpecInner * s.dpiScale;
    o.specOuter = kSpecOuter * s.dpiScale;
    o.maxDisplacement = kMaxDisplacement * s.dpiScale;
    return o;
}

// Signed distance to the outline, negative inside, plus the outward normal.
//
// Circular corners, not the n = 2.24 superellipse the panel is actually clipped
// to. At a 62px corner the two outlines differ by at most 1.7px, at the 45
// degree point, and an implicit superellipse SDF needs numerical differentiation
// per pixel to get a normal out of it.
//
// For the displacement that is free: the backdrop it bends has been through a
// 30px sigma, so 1.7px is a fraction of one blurred pixel. For the lit edge it
// is not free, because that edge is sharp. Expect the rim light to sit up to
// 1.7px inboard of the outline at the four corner diagonals and nowhere else,
// which should read as the corner falling off rather than as a defect. If it
// does not, this is the function to fix, and only the corner quadrants need it.
inline float SignedDistance(const Surface& s, float x, float y,
                            float& nx, float& ny) {
    const float px = x - s.width  * 0.5f;
    const float py = y - s.height * 0.5f;

    const float r = (std::min)(s.radius,
                               (std::min)(s.width, s.height) * 0.5f);

    // Quilez's rounded box: distance measured to the inner rectangle whose
    // corners are the corner-arc centres, then pulled in by the radius.
    const float qx = std::fabs(px) - s.width  * 0.5f + r;
    const float qy = std::fabs(py) - s.height * 0.5f + r;

    const float sx = (px < 0.0f) ? -1.0f : 1.0f;
    const float sy = (py < 0.0f) ? -1.0f : 1.0f;

    float d;
    if (qx > 0.0f && qy > 0.0f) {
        const float len = std::sqrt(qx * qx + qy * qy);
        d = len - r;
        if (len > 1e-6f) { nx = sx * qx / len; ny = sy * qy / len; }
        else             { nx = sx; ny = 0.0f; }
    } else {
        d = (std::max)(qx, qy) - r;
        if (qx >= qy) { nx = sx;   ny = 0.0f; }
        else          { nx = 0.0f; ny = sy;   }
    }
    return d;
}

// How far a ray is pushed sideways by the bezel, at `inside` pixels in from the
// edge. Zero at the edge itself, zero once past the bezel, peaking near the
// outside.
//
// Single refraction event, air into glass, ray arriving perpendicular to the
// backdrop. The surface profile is the convex squircle
//
//     h(x) = (1 - (1 - x)^4) ^ (1/4)
//
// which is what Apple uses, and the reason for it is the join: it meets the flat
// interior with zero slope, so the refraction fades out smoothly instead of
// stopping at a visible ring the way a circular bezel does.
//
// The displacement is the remaining glass under the surface point times the
// tangent of the deflection angle, so it is largest where the surface is steep
// and there is still glass under it, which lands it in the outer fifth of the
// bezel. At kGlassDepth 24 against kBezelWidth 14 that peak is 12.5 logical
// pixels, and the compression around it is roughly 2.7 to 1.
inline float Displacement(float inside, const Optics& o) {
    if (inside <= 0.0f || inside >= o.bezel || o.bezel <= 0.0f) return 0.0f;

    const float x  = inside / o.bezel;
    const float u  = 1.0f - x;
    const float u4 = u * u * u * u;
    const float k  = 1.0f - u4;
    if (k <= 1e-6f) return 0.0f;

    const float h = std::pow(k, 0.25f);

    // The slope goes to infinity at the edge. atan() would handle that, but the
    // pow() underneath it would not, so it is capped where the angle is already
    // within a rounding error of 90 degrees.
    float slope = u * u * u * std::pow(k, -0.75f);
    if (!(slope < 1e3f)) slope = 1e3f;

    const float alpha = std::atan((o.depth / o.bezel) * slope);
    const float sinB  = std::sin(alpha) / kRefractiveIndex;
    const float beta  = std::asin((std::min)(1.0f, sinB));

    const float d = o.depth * h * std::tan(alpha - beta);
    return (std::min)(o.maxDisplacement, (std::max)(0.0f, d));
}

// The displacement map, in the encoding D2D1DisplacementMap reads: the effect
// samples its image input at p + scale * (channel - 0.5), so the channels carry
// the displacement as a signed fraction of the scale.
//
// Set the effect's SCALE property to 2 * maxDisplacement and the round trip is
// exact. Neutral is 128, which is what the whole interior is filled with.
//
// Alpha is 255 everywhere and must stay that way: D2D effect inputs are
// premultiplied, so any alpha below 255 would scale the encoded values on the
// way in and bend the interior.
inline Bitmap BuildDisplacementMap(const Surface& s) {
    const int w = static_cast<int>(s.width);
    const int h = static_cast<int>(s.height);
    if (w <= 0 || h <= 0) return {};

    const Optics o = OpticsFor(s);
    Bitmap map = Bitmap::Create(w, h, MakePixel(128, 128, 128, 255));
    if (o.maxDisplacement <= 0.0f) return map;

    auto encode = [&](float v) {
        const float t = 0.5f + 0.5f * v / o.maxDisplacement;
        const int   q = static_cast<int>(t * 255.0f + 0.5f);
        return static_cast<uint8_t>(q < 0 ? 0 : (q > 255 ? 255 : q));
    };

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float nx = 0.0f, ny = 0.0f;
            const float d = SignedDistance(s, x + 0.5f, y + 0.5f, nx, ny);
            if (d >= 0.0f || -d >= o.bezel) continue;

            const float amount = Displacement(-d, o);
            if (amount <= 0.0f) continue;

            map.At(x, y) = MakePixel(encode(nx * amount), encode(ny * amount),
                                     128, 255);
        }
    }
    return map;
}

// The lit edge, as white with the amount to ADD carried in the alpha.
//
// Three terms, all off the surface normal:
//
//   ambient    the whole rim, whichever way it faces, falling off linearly over
//              the rim span
//   lobe       extra where the surface faces up, as cos^kRimExp of the angle
//              from vertical, over the same falloff
//   filament   a thin bright line along the top run only
//
// Multiplied by the shape's own antialiased coverage, so this can be drawn after
// the clip layer is popped without needing a second one. That also keeps the
// additive pass out of the layer, which is one less thing to be wrong about on a
// driver nobody here can test.
inline Bitmap BuildEdgeLight(const Surface& s, const Params& p) {
    const int w = static_cast<int>(s.width);
    const int h = static_cast<int>(s.height);
    if (w <= 0 || h <= 0) return {};

    const Optics o = OpticsFor(s);
    Bitmap light = Bitmap::Create(w, h, 0);

    // The lit face starts one pixel in, because the outermost pixel belongs to
    // the dark outer stroke. Lighting that pixel as well cancels the stroke
    // almost exactly: 0.30 alpha of black on a 0.30 panel gives 0.21, and the
    // ambient term puts 0.10 straight back on top of it. Dark line first, lit
    // face just inside it, which is also how the edge of real glass reads.
    const float stroke = s.dpiScale;
    const float reach  = (std::max)(o.rimSpan + stroke, o.specOuter) + 1.0f;

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float nx = 0.0f, ny = 0.0f;
            const float d = SignedDistance(s, x + 0.5f, y + 0.5f, nx, ny);
            if (d >= 0.5f) continue;

            const float inside = -d;
            if (inside >= reach) continue;

            // Screen y grows downward, so a normal pointing up has ny = -1.
            const float up = (ny < 0.0f) ? -ny : 0.0f;

            float v = 0.0f;

            const float lit = inside - stroke;
            if (o.rimSpan > 0.0f && lit >= 0.0f && lit < o.rimSpan) {
                const float falloff = 1.0f - lit / o.rimSpan;
                v += (p.rimAmbient + p.rimLobe * std::pow(up, kRimExp)) * falloff;
            }

            if (inside >= o.specInner - 0.5f && inside <= o.specOuter + 0.5f) {
                // Half a pixel of feather at each end, or the filament aliases
                // into a dotted line around the corners.
                const float edge =
                    (std::min)(1.0f,
                               (std::min)((inside - (o.specInner - 0.5f)) / 0.5f,
                                          ((o.specOuter + 0.5f) - inside) / 0.5f));
                if (edge > 0.0f)
                    v += p.specLine * std::pow(up, kSpecExp) * edge;
            }

            if (v <= 0.0f) continue;

            const float coverage = (std::min)(1.0f, (std::max)(0.0f, 0.5f - d));
            const int a = static_cast<int>(v * coverage * 255.0f + 0.5f);
            if (a <= 0) continue;

            light.At(x, y) = MakePixel(255, 255, 255,
                                       static_cast<uint8_t>(a > 255 ? 255 : a));
        }
    }
    return light;
}

} // namespace mactab::glass
