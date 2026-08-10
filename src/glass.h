#pragma once

// The glass material, as numbers rather than as code.
//
// Deliberately free of windows.h and of every D2D type, for the same reason
// panel_layout.h is: tools/preview compiles and runs natively on a machine that
// is not Windows, and these six numbers per theme are chosen by eye. Choosing
// them by eye on a machine that cannot run the app is not a plan, so the preview
// applies this exact material to a wallpaper and writes a PNG, and panel.cpp
// hands the same coefficients to Direct2D. The pixel loops differ, the
// coefficients cannot.
//
// Why a colour matrix at all, rather than blur plus an alpha blend:
//
//  * Saturation. macOS vibrancy pushes the backdrop's saturation up before
//    tinting it. This is the single most recognisable difference between
//    Apple's material and a plain blurred rectangle, and it is the reason a
//    blurred Windows acrylic looks grey where the macOS one looks coloured.
//    CLSID_D2D1Saturation cannot do it: its property is documented over [0, 1],
//    so it can only desaturate, and asking it for 1.6 silently clamps to
//    identity. A colour matrix can.
//
//  * Luminosity compression. Apple's material is not an alpha blend. It
//    compresses the backdrop's luma range so that neither a white wallpaper nor
//    a black one blows through the glass. Without it, a dark panel over a white
//    desktop washes out to grey and a light panel over a black desktop goes
//    muddy. Gain and bias fold into the same matrix for free.
//
// Both live in one D2D1_MATRIX_5X4_F, so the whole material costs one effect.

namespace mactab::glass {

// Rec.709 luma weights, matching what the display pipeline already assumes.
inline constexpr float kLumaR = 0.2126f;
inline constexpr float kLumaG = 0.7152f;
inline constexpr float kLumaB = 0.0722f;

struct Params {
    float saturation;   // s, > 1 boosts. 1 is identity.
    float gain;         // g, multiplies the backdrop's luma range
    float bias;         // b, added after the gain, lifting the black point

    float tint[4];      // straight RGBA over the treated backdrop, 0..1

    // The rim. A single flat hairline reads as a border; the glass reads as
    // glass when the top edge catches more light than the bottom.
    float rimTop;
    float rimBottom;

    // Light theme only: a second, darker stroke outside the bright one, so a
    // pale panel does not dissolve into a pale wallpaper. 0 disables it.
    float rimOuterDark;
};

// Where the numbers come from, rather than taste.
//
// I fitted the transfer function off the same screenshot the corner radius came
// from: sample the wallpaper just outside the panel's rim and the glass just
// inside it, all the way along the top and bottom edges, then least-squares an
// affine fit per channel. The red channel is the only one whose input spans a
// useful range there (0.00 to 0.87; the wallpaper is blue, so green and blue
// barely move) and it gives
//
//     out = 0.439 * in + 0.221
//
// with the mean luma going from 0.467 outside to 0.550 inside. Two things
// follow. Apple compresses the backdrop's contrast to a little under half, not
// to a fifth. And in light appearance the material LIFTS the wallpaper rather
// than merely veiling it.
//
// The end-to-end gain here is (1 - tintAlpha) * gain, and the end-to-end bias is
// (1 - tintAlpha) * bias + tintAlpha * tint. The numbers below work out at
// roughly 0.50 * L + 0.05 for dark and 0.48 * L + 0.35 for light, which puts
// light within a few percent of the measurement and dark at the same contrast
// on the other side of the range.
//
// Saturation is not a taste knob either. Mixing with a neutral tint at alpha a
// removes a fraction a of the backdrop's relative saturation, so 1/(1 - a)
// restores exactly what the tint took away and nothing more. That is 1.43 at
// alpha 0.30 and 1.47 at 0.32, hence 1.45 for both. This is the difference
// between a material that looks like glass and one that looks like frosted
// plastic, and it is the reason CLSID_D2D1Saturation is no use: its property is
// documented over [0, 1], so it can only ever take saturation away.

// Dark.
//
// End to end this is about 0.50 * L + 0.05, so a white wallpaper shows through
// at ~0.55. White label text on that is around 3.3:1, under the 4.5:1 you would
// want, which is why BakeLabel draws a shadow under the glyphs. Losing the
// shadow means pulling the tint back up to about 0.42.
inline constexpr Params kDark{
    1.37f, 0.80f, 0.02f,
    { 0.09f, 0.09f, 0.11f, 0.27f },
    0.34f, 0.08f, 0.0f
};

// Light. Lower gain and a much higher bias: the backdrop is being lifted into a
// bright panel rather than pushed down into a dark one.
inline constexpr Params kLight{
    1.45f, 0.70f, 0.06f,
    { 0.97f, 0.97f, 0.98f, 0.32f },
    0.65f, 0.18f, 0.08f
};

// Row-major, [R G B A 1] * M, which is the convention D2D1_MATRIX_5X4_F uses:
// row i is the contribution of input component i to each of the four output
// columns, and row 5 is the bias. The matrix is NOT symmetric, so transposing
// it is a real bug and not a wash.
struct Matrix5x4 {
    float m[5][4];
};

constexpr Matrix5x4 BuildMatrix(const Params& p) {
    const float s = p.saturation;
    const float g = p.gain;

    // Standard saturation matrix: each output channel keeps (1 - s) of the
    // luma plus s of itself, then everything is scaled by the gain.
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

// The two invariants worth asserting, because a transcription slip in the
// matrix above is invisible until someone looks at a screenshot:
//
//   every colour column sums to the gain          (grey stays grey)
//   white maps to gain + bias                     (the white point is where
//                                                  the compression puts it)
//
// tools/preview checks both at startup.
constexpr float ColumnSum(const Matrix5x4& m, int column) {
    return m.m[0][column] + m.m[1][column] + m.m[2][column];
}

} // namespace mactab::glass
