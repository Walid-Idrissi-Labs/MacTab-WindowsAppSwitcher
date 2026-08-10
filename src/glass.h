#pragma once

#include <algorithm>
#include <cmath>

// The glass material, as numbers rather than as code.
//
// Deliberately free of windows.h and of every D2D type, for the same reason
// panel_layout.h is: tools/preview compiles and runs natively on a machine that
// is not Windows, and these numbers are chosen by eye. Choosing them by eye on a
// machine that cannot run the app is not a plan, so the preview applies this
// exact material to a wallpaper and writes a PNG, and panel.cpp hands the same
// coefficients to Direct2D. The pixel loops differ, the coefficients cannot.
//
// Four things make it a material rather than a blurred rectangle, in order of
// how much each contributes:
//
//  1. Adaptive operating point. A fixed transfer cannot serve both a white
//     wallpaper and a black one: the panel either washes out or goes to a slab.
//     Apple gets away with a fixed material because theirs is a stack of
//     lighten and darken blends with soft knees at both ends, which is not
//     reproducible in one colour matrix. What IS available here and not to
//     Apple is that the backdrop is a FROZEN frame, so its mean luma is known
//     before anything is drawn and the bias can be bent per gesture to land the
//     panel inside a target band. See Adapt().
//
//  2. Saturation, well past unity. macOS vibrancy pushes the backdrop's
//     saturation up hard. Measuring the reference gives relative saturation
//     0.737 outside the panel against 0.642 inside, a ratio of 0.87, which is
//     far past the 1/(1 - tintAlpha) that merely undoes what the tint took
//     away. This is most of the difference between "glass" and "frosted
//     plastic".
//
//     The light number is tuned against that ratio rather than by eye: the
//     preview prints relative saturation in and out on a blue-gradient
//     wallpaper, and 2.80 lands on 0.877 against the reference's 0.87. Dark
//     needs less because its tint is near black, and mixing toward black barely
//     touches relative saturation at all.
//
//     CLSID_D2D1Saturation cannot do any of it: its property is documented over
//     [0, 1], so it can only desaturate, and asking for more clamps to identity
//     without complaining. A colour matrix can.
//
//  3. An inner glow at the top edge. Measured on the reference: interior luma
//     161 just inside the top rim, decaying to 148.8 about 45px in, then rising
//     again as the wallpaper's own gradient reasserts. The bottom shows the same
//     shape at a quarter of the strength. That falloff is what reads as a lit
//     curved surface rather than a flat pane.
//
//  4. The rim, which is additive and very nearly symmetric. The measured lift
//     over the adjacent interior is +33 luma at the top, +23 at the bottom and
//     +22 at the sides: a ratio of 1.4:1, not the 4:1 that looks plausible. The
//     colour delta at the peak is (19,22,13), near neutral, and it adds rather
//     than covering, so it is drawn with D2D1_PRIMITIVE_BLEND_ADD.

namespace mactab::glass {

// Rec.709 luma weights, matching what the display pipeline already assumes.
inline constexpr float kLumaR = 0.2126f;
inline constexpr float kLumaG = 0.7152f;
inline constexpr float kLumaB = 0.0722f;

constexpr float Luma(float r, float g, float b) {
    return kLumaR * r + kLumaG * g + kLumaB * b;
}

// Backdrop blur, in logical pixels at 96 DPI.
//
// Here rather than in panel.cpp because it is the single most visually decisive
// number in the material and the preview has to use the same one. Sharing it by
// comment, which is what this was, means it drifts the first time anybody
// retunes it on the Windows side.
//
// The macOS switcher's backdrop is unrecognisable mush. 34 left window edges
// readable through the glass, which gives it away immediately as a blurred
// screenshot rather than a material.
inline constexpr float kBlurSigma = 52.0f;

// Downsample before blurring. A 52px sigma at quarter resolution costs what a
// 13px sigma costs, and after the matching upscale the difference is invisible
// under a tint. The preview blurs at full resolution instead, which is the same
// picture for more work; it has no frame budget.
inline constexpr float kBlurDownscale = 0.25f;

struct Params {
    float saturation;   // s. See note 2 above; this is well above 1.
    float gain;         // g, multiplies the backdrop's luma range
    float bias;         // b, added after the gain. Adapt() moves this one.

    float tint[4];      // straight RGBA over the treated backdrop, 0..1

    // Alpha for the base coat used when there is no captured frame at all: a
    // wedged GPU, a remote session, a capture that missed its deadline.
    //
    // Nearly opaque on purpose. This is a degraded state, and a degraded state
    // should look deliberate rather than broken. Anything leaving more than a
    // few percent of a SHARP desktop showing through reads as a rendering bug,
    // because there is no blur to soften window edges into the material.
    float fallbackAlpha;

    // Inner glow. Alpha at the edge, and how far in it takes to reach zero, in
    // logical pixels at 96 DPI.
    float glowTop,    glowTopSpan;
    float glowBottom, glowBottomSpan;

    // Rim, as ADDITIVE grey levels rather than an alpha over. Measured to be
    // near neutral and near symmetric; the vertical gradient between these two
    // puts the sides in the middle, which is where they measured.
    float rimTop, rimBottom;

    // A darker stroke outside the bright one. Not optional now that there is no
    // drop shadow: without it a dark panel on a dark wallpaper has no boundary
    // at all, and a pale one dissolves into a pale wallpaper.
    float rimOuterDark;

    // The band Adapt() steers the panel's resulting mean luma into.
    float targetMin, targetMax;
};

// Dark.
inline constexpr Params kDark{
    1.70f, 0.55f, 0.10f,
    { 0.09f, 0.09f, 0.11f, 0.24f },
    0.96f,
    0.11f, 13.0f,  0.03f, 10.0f,
    0.10f, 0.06f,
    0.30f,
    0.15f, 0.38f
};

// Light.
inline constexpr Params kLight{
    2.80f, 0.59f, 0.14f,
    { 0.97f, 0.97f, 0.98f, 0.24f },
    0.96f,
    0.11f, 13.0f,  0.03f, 10.0f,
    0.13f, 0.09f,
    0.12f,
    0.55f, 0.85f
};

// --- The transfer, end to end -----------------------------------------------
//
// The matrix produces g*L + b, then the tint is composited over it at alpha a,
// so what actually reaches the screen is
//
//     out = (1 - a) * (g * L + b) + a * tintLuma
//
// which is affine in L. These spell out its slope and intercept, because every
// decision below is made in terms of those rather than the four knobs that
// produce them. The reference measures 0.44 * L + 0.22 for Apple's light
// material, fitted on the only channel whose input spans a useful range.

constexpr float TintLuma(const Params& p) {
    return Luma(p.tint[0], p.tint[1], p.tint[2]);
}

constexpr float EndGain(const Params& p) {
    return (1.0f - p.tint[3]) * p.gain;
}

constexpr float EndBias(const Params& p) {
    return (1.0f - p.tint[3]) * p.bias + p.tint[3] * TintLuma(p);
}

// What the panel will read at a given backdrop luma.
constexpr float PanelLuma(const Params& p, float backdropLuma) {
    return EndGain(p) * backdropLuma + EndBias(p);
}

// Bend the bias so the panel lands inside [targetMin, targetMax].
//
// This is the step that makes one material work over a white wallpaper and a
// black one. Only the bias moves: the gain sets how much of the desktop's
// contrast survives, which is a property of the material, while the bias is
// only where that window sits, which is a property of what is behind it today.
//
// `backdropLuma` is the mean luma of the captured frame under the panel, in the
// same 0..1 sRGB-encoded space everything else here uses. With no captured
// frame there is no mean, so the caller must use the base parameters unadapted.
inline Params Adapt(const Params& base, float backdropLuma) {
    Params p = base;

    const float a = p.tint[3];
    if (a >= 1.0f) return p;

    const float panel = PanelLuma(p, backdropLuma);

    if (panel < p.targetMin)
        p.bias += (p.targetMin - panel) / (1.0f - a);
    else if (panel > p.targetMax)
        p.bias -= (panel - p.targetMax) / (1.0f - a);

    // A negative bias is legitimate: pinning a bright wallpaper down to the dark
    // theme's ceiling needs one. The floor only stops it inverting.
    p.bias = (std::min)(0.45f, (std::max)(-0.05f, p.bias));
    return p;
}

// --- Contrast ---------------------------------------------------------------
//
// Used to decide whether the app name needs a shadow. WCAG relative luminance,
// which is not the Rec.709 luma above: that one works on sRGB-encoded values and
// is what the material arithmetic is expressed in, this one linearises first and
// is what contrast ratios are defined on.

inline float RelativeLuminance(float srgb) {
    const float c = (std::min)(1.0f, (std::max)(0.0f, srgb));
    return (c <= 0.04045f) ? (c / 12.92f) : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

inline float ContrastRatio(float srgbA, float srgbB) {
    const float a = RelativeLuminance(srgbA);
    const float b = RelativeLuminance(srgbB);
    const float hi = (std::max)(a, b);
    const float lo = (std::min)(a, b);
    return (hi + 0.05f) / (lo + 0.05f);
}

inline constexpr float kMinTextContrast = 4.5f;

// Mean alpha the inner glow adds over a shape `height` physical pixels tall.
//
// The falloff is linear from each edge, so each edge contributes alpha * span / 2
// of area. Spans are clamped exactly as DrawInnerGlow clamps them, because on a
// short shape (the app name's capsule is 28 logical pixels tall against a 13px
// top span) the glow covers most of it and is not a rounding error.
//
// This exists so the estimate of what the capsule will read at, which is made
// before it is drawn, matches what actually gets drawn. Without it the estimate
// is systematically too dark and the text shadow switches on when it is not
// needed.
inline float MeanGlowAlpha(const Params& p, float height, float dpiScale) {
    if (height <= 0.0f) return 0.0f;

    const float topSpan    = (std::min)(p.glowTopSpan    * dpiScale, height * 0.45f);
    const float bottomSpan = (std::min)(p.glowBottomSpan * dpiScale, height * 0.45f);

    return (p.glowTop * topSpan * 0.5f + p.glowBottom * bottomSpan * 0.5f) / height;
}

// White composited over `luma` at `alpha`, which is what the glow does.
constexpr float LitBy(float luma, float alpha) {
    return luma * (1.0f - alpha) + alpha;
}

// --- The colour matrix ------------------------------------------------------
//
// Row-major, [R G B A 1] * M, which is the convention D2D1_MATRIX_5X4_F uses:
// row i is the contribution of input component i to each of the four output
// columns, and row 5 is the bias. The matrix is NOT symmetric, so transposing it
// is a real bug and not a wash.
struct Matrix5x4 {
    float m[5][4];
};

constexpr Matrix5x4 BuildMatrix(const Params& p) {
    const float s = p.saturation;
    const float g = p.gain;

    // Standard saturation matrix: each output channel keeps (1 - s) of the luma
    // plus s of itself, then everything is scaled by the gain.
    const float rr = (kLumaR + (1.0f - kLumaR) * s) * g;
    const float gg = (kLumaG + (1.0f - kLumaG) * s) * g;
    const float bb = (kLumaB + (1.0f - kLumaB) * s) * g;

    const float r_ = kLumaR * (1.0f - s) * g;   // input R into G' and B'
    const float g_ = kLumaG * (1.0f - s) * g;   // input G into R' and B'
    const float b_ = kLumaB * (1.0f - s) * g;   // input B into R' and G'

    return Matrix5x4{ {
        { rr,     r_,     r_,     0.0f },
        { g_,     gg,     g_,     0.0f },
        { b_,     b_,     bb,     0.0f },
        { 0.0f,   0.0f,   0.0f,   1.0f },
        { p.bias, p.bias, p.bias, 0.0f },
    } };
}

// The invariants worth asserting, because a transcription slip in the matrix
// above is invisible until someone looks at a screenshot:
//
//   every colour column sums to the gain          (grey stays grey)
//   white maps to gain + bias                     (the white point is where the
//                                                  compression puts it)
//
// tools/preview checks both at startup.
constexpr float ColumnSum(const Matrix5x4& m, int column) {
    return m.m[0][column] + m.m[1][column] + m.m[2][column];
}

// Run one colour through the whole material: matrix, clamp, tint over.
//
// The clamp sits on the matrix output because that is where Direct2D's
// D2D1_COLORMATRIX_PROP_CLAMP_OUTPUT does it, and a saturation of 2.0 drives
// strongly coloured pixels well out of range. Clamping only the final pixel
// instead carries a fraction (1 - a) of the overshoot into the result.
inline void Apply(const Params& p, const float in[3], float out[3]) {
    const Matrix5x4 m = BuildMatrix(p);
    auto unit = [](float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };

    for (int c = 0; c < 3; ++c) {
        const float v = unit(in[0] * m.m[0][c] + in[1] * m.m[1][c] +
                             in[2] * m.m[2][c] + m.m[4][c]);
        out[c] = v * (1.0f - p.tint[3]) + p.tint[c] * p.tint[3];
    }
}

} // namespace mactab::glass
