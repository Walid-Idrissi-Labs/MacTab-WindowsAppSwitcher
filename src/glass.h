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
// The target is Liquid Glass, the macOS 26 material, not the older frosted
// NSVisualEffectView one. The geometry in panel_layout.h was already fitted to a
// Tahoe screenshot, and the two things this material was missing (you could not
// see through it, and it did not bend anything) are exactly the two things
// Liquid Glass added over the old vibrancy stack.
//
// Four things make it glass rather than a blurred rectangle, in order of how
// much each contributes:
//
//  1. Refraction at the rim. A pane with thickness bends what is behind it, so
//     content just outside the panel gets pulled inward and squeezed against the
//     edge. This is the cue that separates glass from frosted plastic, and it is
//     the one 0.3 had none of. The optics live in glass_map.h; the numbers that
//     drive them are below.
//
//  2. Seeing through it at all, which is the one this project kept getting
//     wrong. Blur sigma and how much of the backdrop's contrast survives, and
//     both are now measured off the reference rather than argued about: sigma
//     0.044 of the panel's height, and a transfer of 0.71 * L + 0.068.
//
//     0.2 blurred at 34, 0.3 at 52 and 0.4.0 at 30, all on the reasoning that
//     the macOS backdrop is unrecognisable mush. It is not. In the reference you
//     can count the floors of the building behind it.
//
//  3. Saturation, well past unity. macOS vibrancy pushes the backdrop's
//     saturation up hard. Measuring the reference gives relative saturation
//     0.737 outside the panel against 0.642 inside, a ratio of 0.87, which is
//     far past the 1/(1 - tintAlpha) that merely undoes what the tint took away.
//
//     The light number is tuned against that ratio rather than by eye: the
//     preview prints relative saturation in and out on a blue-gradient wallpaper
//     and fails the build if the ratio leaves [0.84, 0.90]. Dark needs less
//     because its tint is near black, and mixing toward black barely touches
//     relative saturation at all.
//
//     CLSID_D2D1Saturation cannot do any of it: its property is documented over
//     [0, 1], so it can only desaturate, and asking for more clamps to identity
//     without complaining. A colour matrix can.
//
//  4. The lit edge. Measured on the reference, the rim's lift over the adjacent
//     interior is +33 luma at the top, +23 at the bottom and +22 at the sides.
//     That is symmetric left to right, so the light is straight overhead rather
//     than up and to the left. Modelled as an ambient lift on the whole rim plus
//     a lobe that only the upward-facing part of the surface sees, plus a thin
//     bright filament along the top. All three come off the surface normal, so
//     the corners sweep between top and side on their own.
//
//     0.3 did this as a vertical gradient stroke plus a separate inner glow,
//     which is two disconnected guesses at one physical thing, and the gradient
//     put the mid-height sides at 0.11 where they measure 0.086.
//
// The adaptive operating point in Adapt() stays. A fixed transfer cannot serve
// both a white wallpaper and a black one, and Apple does not use one either: it
// flips treatment on backdrop luminance and offers a higher-opacity state for
// busy content. What we have and Apple does not is that our backdrop is a FROZEN
// frame, so its mean luma is known before anything is drawn.

namespace mactab::glass {

// Rec.709 luma weights, matching what the display pipeline already assumes.
inline constexpr float kLumaR = 0.2126f;
inline constexpr float kLumaG = 0.7152f;
inline constexpr float kLumaB = 0.0722f;

constexpr float Luma(float r, float g, float b) {
    return kLumaR * r + kLumaG * g + kLumaB * b;
}

// --- Optics -----------------------------------------------------------------
//
// Theme-independent: the shape of a piece of glass does not change with the
// colour scheme. All in logical pixels at 96 DPI.

// Backdrop blur.
//
// Here rather than in panel.cpp because it is the single most visually decisive
// number in the material and the preview has to use the same one. Sharing it by
// comment, which is what this was, means it drifts the first time anybody
// retunes it on the Windows side.
// Measured, not chosen. tools/measure has the method: the reference screenshot
// puts the panel as a horizontal band over a photo of a building whose fins run
// diagonally, so a row just inside the panel and a row just outside it see the
// same structure shifted sideways by a known slope. Align the two, then fit the
// blur and the transfer that turn one into the other.
//
// That gives sigma 14.6 on a panel 330 screenshot pixels tall, so 0.044 of the
// panel's own height, which on our 172px panel is 7.6. Rounded to 8.
//
// The history here is worth keeping. 0.2 used 34, 0.3 used 52 on the reasoning
// that the macOS backdrop is "unrecognisable mush". It is not: you can read the
// window frames of a building through it. 52 was four to seven times too much,
// and no amount of tuning the tint was ever going to fix a panel that had
// already thrown the desktop away.
inline constexpr float kBlurSigma = 8.0f;

// Downsample before blurring. Also what macOS does, and it is nearly free: a
// 30px sigma at quarter resolution costs what a 7.5px sigma costs, and after the
// matching upscale the difference is invisible under a tint. The preview blurs
// at full resolution instead, which is the same picture for more work; it has no
// frame budget.
inline constexpr float kBlurDownscale = 0.25f;

// The bezel: how far in from the edge the surface is curved rather than flat.
// Fitted from the Tahoe switcher, where the lens band runs about 45 screenshot
// pixels on a 588px-tall panel, scaled to our 172px panel height.
inline constexpr float kBezelWidth = 14.0f;

// How thick the pane is. Sets the peak displacement through the Snell formula
// below rather than being a displacement in its own right: at 24 against a 14px
// bezel the peak comes out at 12.5px, which is the ratio (peak about 0.9 of the
// bezel width) that shader recreations of Liquid Glass converge on.
inline constexpr float kGlassDepth = 24.0f;

// Crown glass. Not a free parameter, it is what glass is.
inline constexpr float kRefractiveIndex = 1.5f;

// Ceiling on the displacement, and the scale the 8-bit map is encoded against.
// The physics peaks at 12.5, so there is headroom to retune kGlassDepth without
// re-plumbing the encoding. Also a safety bound: the capture margin is 1.5
// sigma, so nothing can be pulled in from outside the captured frame.
inline constexpr float kMaxDisplacement = 16.0f;

// How far in the lit edge reaches before it is gone. From the reference, where
// interior luma runs 161 just inside the top rim and 148.8 about 45 screenshot
// pixels in.
inline constexpr float kRimSpan = 13.0f;

// How tightly the overhead lobe wraps around the corner. 2 puts the 45 degree
// point midway between the top and side values, which is the sweep the reference
// shows.
inline constexpr float kRimExp = 2.0f;

// The filament is confined to the near-horizontal top run and is gone by the
// corners, so it needs a much tighter exponent than the lobe.
inline constexpr float kSpecExp = 6.0f;

// Where the filament sits, as distance in from the edge. Starts at 1.0 so it
// clears the 1px dark outer stroke instead of fighting it.
inline constexpr float kSpecInner = 1.0f;
inline constexpr float kSpecOuter = 2.5f;

struct Params {
    float saturation;   // s. See note 3 above; this is well above 1.
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

    // The lit edge, as ADDITIVE amounts rather than alphas over. Measured to be
    // near neutral in colour, so one number each rather than three.
    //
    //   ambient   the whole rim gets this, whichever way it faces
    //   lobe      extra for the part of the surface facing up, falling off as
    //             cos(angle from vertical) ^ kRimExp
    //   specLine  the thin bright filament along the top run
    float rimAmbient;
    float rimLobe;
    float specLine;

    // A darker stroke on the outermost pixel. Not optional now that there is no
    // drop shadow: without it a dark panel on a dark wallpaper has no boundary
    // at all, and a pale one dissolves into a pale wallpaper.
    float rimOuterDark;

    // The band Adapt() steers the panel's resulting mean luma into.
    float targetMin, targetMax;
};

// Dark.
//
// The gain and the bias are the fit off the reference: end to end it runs
// 0.71 * L + 0.068, so nearly three quarters of the desktop's contrast reaches
// the screen. 0.3 ran 0.42 and 0.4.0 ran 0.53, which is why it still read as a
// slab after the blur came down.
inline constexpr Params kDark{
    1.70f, 0.789f, 0.0654f,
    { 0.09f, 0.09f, 0.11f, 0.10f },
    0.96f,
    0.065f, 0.035f, 0.045f,
    0.30f,
    0.16f, 0.44f
};

// Light.
//
// Same slope, since that is a property of the glass rather than of the theme,
// and a much higher intercept, since that is what makes it the light one. There
// is no light-mode reference to fit against, so the intercept is set to put a
// mid wallpaper at 0.69 and the band is set by what dark text stays readable on.
inline constexpr Params kLight{
    1.84f, 0.789f, 0.2199f,
    { 0.97f, 0.97f, 0.98f, 0.10f },
    0.96f,
    0.085f, 0.045f, 0.055f,
    0.12f,
    0.50f, 0.88f
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
// material, fitted on the only channel whose input spans a useful range. We now
// run a steeper slope than that on purpose: the fit was taken through the
// frosted interior of one screenshot, and the verdict on 0.3 was that the
// shipped result reads opaque.

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

// How much of the desktop's contrast survives the material. The preview fails
// the build below this, which is what makes "it looks opaque" a regression
// somebody has to argue with rather than one that creeps back in.
inline constexpr float kMinEndGain = 0.65f;

// Bounds on the adapted bias.
//
// Both ends are load-bearing rather than round numbers. The dark theme over a
// pure white desktop needs -0.193 to reach its ceiling, and the light theme over
// pure black needs 0.458 to reach its floor. A clamp inside either of those
// strands the panel outside its band, which the preview's band assertion then
// fails, so these move whenever the gain or a target does.
//
// The floor has 0.007 of margin, which is thin: a dark gain above 0.628 needs it
// moved. The band assertion will catch that, but it reports the panel landing in
// the wrong place rather than naming the constant, so look here first.
inline constexpr float kBiasFloor   = -0.36f;
inline constexpr float kBiasCeiling =  0.50f;

// Bend the bias so the panel lands inside [targetMin, targetMax].
//
// This is the step that makes one material work over a white wallpaper and a
// black one. Only the bias moves: the gain sets how much of the desktop's
// contrast survives, which is a property of the material, while the bias is only
// where that window sits, which is a property of what is behind it today.
//
// `backdropLuma` is the mean luma of the captured frame under the panel, in the
// same 0..1 sRGB-encoded space everything else here uses. With no captured frame
// there is no mean, so the caller must use the base parameters unadapted.
inline Params Adapt(const Params& base, float backdropLuma) {
    Params p = base;

    const float a = p.tint[3];
    if (a >= 1.0f) return p;

    const float panel = PanelLuma(p, backdropLuma);

    if (panel < p.targetMin)
        p.bias += (p.targetMin - panel) / (1.0f - a);
    else if (panel > p.targetMax)
        p.bias -= (panel - p.targetMax) / (1.0f - a);

    // Nothing below the floor is dangerous, because the matrix output is clamped
    // at zero, so a more negative bias only crushes blacks. The floor is there
    // to stop a retune from wandering, not to protect anything.
    p.bias = (std::min)(kBiasCeiling, (std::max)(kBiasFloor, p.bias));
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

// Mean amount the lit edge adds over a shape `height` physical pixels tall.
//
// The top run gets ambient plus the full lobe and the bottom run gets ambient
// alone, both falling off linearly over the rim span, so each contributes its
// amplitude times half the span. The filament adds its own amount over the 1.5px
// band it occupies.
//
// Spans are clamped exactly as the generator clamps them, because on a short
// shape (the app name's capsule is 28 logical pixels tall against a 13px span)
// the lit edge covers most of it and is not a rounding error.
//
// This exists so the estimate of what the capsule will read at, which is made
// before it is drawn, matches what actually gets drawn. Without it the estimate
// is systematically too dark and the text shadow switches on when it is not
// needed.
inline float MeanRimAlpha(const Params& p, float height, float dpiScale) {
    if (height <= 0.0f) return 0.0f;

    const float span = (std::min)(kRimSpan * dpiScale, height * 0.35f);
    const float top    = (p.rimAmbient + p.rimLobe) * span * 0.5f;
    const float bottom =  p.rimAmbient              * span * 0.5f;
    const float line   =  p.specLine * (kSpecOuter - kSpecInner) * dpiScale;

    return (top + bottom + line) / height;
}

// White composited over `luma` at `alpha`, which is what the lit edge does.
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
// D2D1_COLORMATRIX_PROP_CLAMP_OUTPUT does it, and a saturation above 2 drives
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
