#include "pch.h"

#include <d2d1_1.h>
#include <d2d1effects_2.h>

#include "glass_draw.h"
#include "com.h"
#include "config.h"
#include "diag.h"
#include "geometry.h"

// mingw-w64's d2d1effects.h is missing a property enum MSVC has. Defining it
// here rather than skipping the check keeps this file inside what
// ./tools/syntax-check.sh compiles, which is the whole reason it exists.
#ifndef D2D1_SCALE_PROP_SCALE
#define D2D1_SCALE_PROP_SCALE ((D2D1_PROPERTY)0)
#endif

namespace mactab::glass {
namespace {

D2D1_COLOR_F TintColour(const Params& p) {
    return D2D1::ColorF(p.tint[0], p.tint[1], p.tint[2], p.tint[3]);
}

// BuildMatrix is constexpr and D2D-free, so it lives with the numbers it belongs
// to and tools/preview evaluates the identical coefficients. This just unpacks
// it into the type Direct2D wants. Written out longhand rather than memcpy'd
// over the union: the layout does match, but nothing checks that it still does.
D2D1_MATRIX_5X4_F ToD2D(const Matrix5x4& g) {
    return D2D1::Matrix5x4F(
        g.m[0][0], g.m[0][1], g.m[0][2], g.m[0][3],
        g.m[1][0], g.m[1][1], g.m[1][2], g.m[1][3],
        g.m[2][0], g.m[2][1], g.m[2][2], g.m[2][3],
        g.m[3][0], g.m[3][1], g.m[3][2], g.m[3][3],
        g.m[4][0], g.m[4][1], g.m[4][2], g.m[4][3]);
}

// The dark outer stroke.
//
// All that is left of the old rim. The bright half of it is now the lit edge,
// which comes off the surface normal instead of a vertical gradient, so it lands
// on the corners correctly instead of reading the same all the way down the
// sides.
//
// This one still has to be a stroke rather than part of the generated map: it is
// drawn OUTSIDE the clip layer, on the antialiased boundary itself, and a CPU
// bitmap composited source-over would double the coverage there.
//
// INSET by half its own width rather than centred on the outline. D2D centres
// strokes on the path, the surface is exactly piece-sized with no margin, and
// the surface edge is a rectangle while the outline is not: a centred stroke
// therefore loses its outer half along the straight runs and keeps almost all of
// it through the corners, so it would visibly thicken at the corners at anything
// above 100% DPI. Inset, the whole stroke stays inside the surface and the width
// is uniform.
//
// The radius is inset by the same amount, which is what keeps the inner outline
// concentric with the outer one. Reusing the outer radius pinches the corners.
void DrawOuterStroke(const Piece& piece, const D2D1_MATRIX_3X2_F& toSurface,
                     float width, float height, float radius, const Params& p) {
    if (p.rimOuterDark <= 0.0f) return;

    const float sw    = piece.dpiScale;
    const float inset = sw * 0.5f;

    auto geometry = CreateSquircleGeometry(piece.factory,
                                           width  - inset * 2.0f,
                                           height - inset * 2.0f,
                                           radius - inset,
                                           piece.cornerExponent);
    if (!geometry) return;

    ComPtr<ID2D1SolidColorBrush> dark;
    if (FAILED(piece.dc->CreateSolidColorBrush(
            D2D1::ColorF(0.0f, 0.0f, 0.0f, p.rimOuterDark), dark.Put()))) {
        return;
    }

    piece.dc->SetTransform(D2D1::Matrix3x2F::Translation(inset, inset) * toSurface);
    piece.dc->DrawGeometry(geometry.Get(), dark.Get(), sw);
    piece.dc->SetTransform(toSurface);
}

// The lit edge: ambient over the whole rim, a lobe where the surface faces up,
// and a filament along the top run. See glass_map.h for what each is and where
// the amounts came from.
//
// Generated on the CPU and added, rather than stroked. The whole point is that
// the amount depends on which way the surface faces, and there is no brush that
// varies with a normal.
//
// ADD, not source-over. The bitmap is premultiplied white with the amount in its
// alpha, so adding it adds exactly that amount. It carries the shape's own
// antialiased coverage, which is why this can be drawn after the clip layer is
// popped and does not need a second one.
void DrawEdgeLight(const Piece& piece, const Surface& surface, const Params& p,
                   const LumaField* env) {
    ComPtr<ID2D1Bitmap1> light =
        UploadBitmap(piece.dc, BuildEdgeLight(surface, p, env));
    if (!light) return;

    piece.dc->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_ADD);
    piece.dc->DrawBitmap(light.Get(),
                         D2D1::RectF(0.0f, 0.0f, surface.width, surface.height),
                         1.0f, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
    piece.dc->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
}

} // namespace

float MarginPx(float dpiScale) {
    const float forBlur = g_tuning.blurSigma * 1.5f;
    const float forLens = g_tuning.maxDisplacement + 4.0f;
    return std::ceil((std::max)(forBlur, forLens) * dpiScale);
}

float BackdropLumaIn(const capture::Frame* frame, const RECT& screenRect) {
    if (!frame || frame->pixels.Empty()) return 0.5f;

    const uint32_t mean = MeanColourIn(frame->pixels,
                                       screenRect.left   - frame->bounds.left,
                                       screenRect.top    - frame->bounds.top,
                                       screenRect.right  - frame->bounds.left,
                                       screenRect.bottom - frame->bounds.top);
    return Luma(RedOf(mean) / 255.0f, GreenOf(mean) / 255.0f, BlueOf(mean) / 255.0f);
}

Params MaterialFor(const capture::Frame* frame, const Params& base,
                   const RECT& screenRect) {
    if (!frame || frame->pixels.Empty()) return base;
    return Adapt(base, BackdropLumaIn(frame, screenRect));
}

// Backdrop -> downscale -> blur -> colour matrix -> upscale -> refract -> tint,
// clipped to the squircle by a D2D layer, then the outer stroke and the lit edge
// over the top of it.
//
// Because the source is a single frozen bitmap this is a draw-time operation
// rather than a live effect graph, which is what lets us avoid Win2D and a
// hand-rolled IGraphicsEffectD2D1Interop entirely.
//
// The colour matrix sits AFTER the blur, at quarter resolution. It is per-pixel
// linear and the blur is spatially linear, so the two commute and this is the
// cheaper of the two orderings by a factor of sixteen. See glass.h for what the
// matrix is doing and why it is not a saturation effect.
void Draw(const Piece& piece, POINT surfaceOffset, const RECT& screenRect,
          float radius) {
    if (!piece.dc || !piece.factory) return;

    ID2D1DeviceContext* dc = piece.dc;

    const float width  = static_cast<float>(screenRect.right  - screenRect.left);
    const float height = static_cast<float>(screenRect.bottom - screenRect.top);
    if (width <= 0.0f || height <= 0.0f) return;

    auto geometry = CreateSquircleGeometry(piece.factory, width, height, radius,
                                           piece.cornerExponent);
    if (!geometry) return;

    const D2D1_MATRIX_3X2_F toSurface =
        D2D1::Matrix3x2F::Translation(static_cast<float>(surfaceOffset.x),
                                      static_cast<float>(surfaceOffset.y));
    dc->SetTransform(toSurface);

    ComPtr<ID2D1Layer> layer;
    dc->CreateLayer(nullptr, layer.Put());
    dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), geometry.Get(),
                                         D2D1_ANTIALIAS_MODE_PER_PRIMITIVE),
                  layer.Get());

    const bool haveBackdrop = piece.frame && !piece.frame->pixels.Empty();
    const Surface glassSurface{ width, height, radius, piece.dpiScale };

    // This piece's own operating point, not the whole overlay's.
    const Params material = MaterialFor(piece.frame, piece.base, screenRect);

    if (haveBackdrop) {
        const capture::Frame& frame = *piece.frame;

        ComPtr<ID2D1Bitmap1> captured = UploadBitmap(dc, frame.pixels);

        // The downscale is shared by both taps, which is most of what makes the
        // second one nearly free: it blurs a quarter-resolution image with a
        // sigma of two.
        ComPtr<ID2D1Effect> scale;
        if (captured) dc->CreateEffect(CLSID_D2D1Scale, scale.Put());
        if (scale) {
            scale->SetInput(0, captured.Get());
            scale->SetValue(D2D1_SCALE_PROP_SCALE,
                            D2D1::Vector2F(kBlurDownscale, kBlurDownscale));
        }

        // Where the backdrop ACTUALLY starts, not where this piece does. For the
        // switcher these differ because the grab is clamped to the monitor near
        // a screen edge and comes back shifted; assuming the requested origin
        // slides the blur sideways.
        const float dx = static_cast<float>(frame.bounds.left - screenRect.left);
        const float dy = static_cast<float>(frame.bounds.top  - screenRect.top);

        ComPtr<ID2D1Bitmap1> map;
        if (scale && config::Current().glassRefraction)
            map = UploadBitmap(dc, BuildDisplacementMap(glassSurface));

        // Blur, treat, place at this piece's origin, bend at the rim.
        //
        // A lambda only so the early returns can each hand back the last stage
        // that did get built: every step past the blur is optional, and a piece
        // with no refraction is worth far more than no piece.
        //
        // `srcScale` is the resolution `src` is already at: a quarter for the
        // interior, which is blurred cheaply and upscaled, and full for the rim,
        // which must not be.
        //
        // That distinction is the whole reason the rim tap works. In 0.4.0 both
        // taps shared the quarter-resolution downsample, so the "sharp" one was
        // a sigma of 0.5 followed by a 4x bilinear upscale, and the upscale on
        // its own softens more than that. The two came out indistinguishable and
        // the second was deleted as redundant. It was not redundant, it was
        // being thrown away one stage before it was used.
        auto buildTap = [&](ID2D1Image* src, float sigma,
                            float srcScale) -> ComPtr<ID2D1Effect> {
            ComPtr<ID2D1Effect> blur, matrix, place;
            dc->CreateEffect(CLSID_D2D1GaussianBlur, blur.Put());
            dc->CreateEffect(CLSID_D2D1ColorMatrix, matrix.Put());
            dc->CreateEffect(CLSID_D2D12DAffineTransform, place.Put());
            if (!blur || !matrix || !place || !src) return {};

            blur->SetInput(0, src);
            blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION,
                           sigma * piece.dpiScale * srcScale);
            // HARD border mode: SOFT would fade the blur toward transparent at
            // the backdrop's edges and halo the piece.
            blur->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE,
                           D2D1_BORDER_MODE_HARD);

            matrix->SetInputEffect(0, blur.Get());
            matrix->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX,
                             ToD2D(BuildMatrix(material)));
            // The backdrop is opaque everywhere, so premultiplied and straight
            // are numerically the same here and the bias needs no division. Left
            // at the default rather than set, so that stays true by construction
            // if the input ever gains an alpha channel and somebody has to think
            // about it.
            //
            // Clamping is NOT optional: a saturation of 2 drives strongly
            // coloured pixels well out of range in both directions, and those
            // want clipping at the effect rather than wherever the upscale
            // interpolator meets them next.
            matrix->SetValue(D2D1_COLORMATRIX_PROP_CLAMP_OUTPUT, TRUE);

            // Undo the downscale and shift so this piece's own area sits at the
            // origin. This cannot be done with the device context transform: the
            // displacement map is a second input and both inputs have to share a
            // coordinate space, so the placement has to happen inside the graph.
            place->SetInputEffect(0, matrix.Get());
            place->SetValue(D2D1_2DAFFINETRANSFORM_PROP_TRANSFORM_MATRIX,
                            D2D1::Matrix3x2F::Scale(1.0f / srcScale,
                                                    1.0f / srcScale) *
                            D2D1::Matrix3x2F::Translation(dx, dy));
            place->SetValue(D2D1_2DAFFINETRANSFORM_PROP_INTERPOLATION_MODE,
                            D2D1_2DAFFINETRANSFORM_INTERPOLATION_MODE_LINEAR);

            if (!map) return place;

            // Extend the placed backdrop by repeating its edge pixels, before
            // anything samples outside it.
            //
            // Not optional, and the reason is the screen edge. The grab is
            // clamped to the monitor, so a piece wide enough to reach the edge
            // comes back with no margin at all on that side. The lens then
            // samples up to 12px past the placed image, where a HARD-border blur
            // leaves transparent black, and that whole run of rim renders as
            // tint over nothing: a dark band exactly where the glass should be.
            // Clamping turns that into a smear of the edge pixel, which is what
            // the blur does at the same boundary anyway.
            ComPtr<ID2D1Effect> extend;
            dc->CreateEffect(CLSID_D2D1Border, extend.Put());
            if (!extend) return place;

            extend->SetInputEffect(0, place.Get());
            extend->SetValue(D2D1_BORDER_PROP_EDGE_MODE_X, D2D1_BORDER_EDGE_MODE_CLAMP);
            extend->SetValue(D2D1_BORDER_PROP_EDGE_MODE_Y, D2D1_BORDER_EDGE_MODE_CLAMP);

            // Refraction. The map is piece-local and starts at the origin, which
            // is exactly where the placement step puts the backdrop, so the two
            // line up by construction rather than by arithmetic.
            //
            // The effect samples at p + scale * (channel - 0.5), so a scale of
            // twice the ceiling makes the encoding in glass_map.h exact.
            ComPtr<ID2D1Effect> lens;
            dc->CreateEffect(CLSID_D2D1DisplacementMap, lens.Put());
            if (!lens) return place;

            lens->SetInputEffect(0, extend.Get());
            lens->SetInput(1, map.Get());
            lens->SetValue(D2D1_DISPLACEMENTMAP_PROP_SCALE,
                           2.0f * g_tuning.maxDisplacement * piece.dpiScale);
            lens->SetValue(D2D1_DISPLACEMENTMAP_PROP_X_CHANNEL_SELECT,
                           D2D1_CHANNEL_SELECTOR_R);
            lens->SetValue(D2D1_DISPLACEMENTMAP_PROP_Y_CHANNEL_SELECT,
                           D2D1_CHANNEL_SELECTOR_G);

            // Bound it again. The clamp above makes the image infinite, and an
            // infinite output rect reaching DrawImage is a question nobody here
            // can answer on real hardware. This piece is the only part that ever
            // shows.
            ComPtr<ID2D1Effect> bound;
            dc->CreateEffect(CLSID_D2D1Crop, bound.Put());
            if (!bound) return lens;

            bound->SetInputEffect(0, lens.Get());
            bound->SetValue(D2D1_CROP_PROP_RECT,
                            D2D1::Vector4F(0.0f, 0.0f, width, height));
            return bound;
        };

        // Two taps. The interior is the soft one; the bezel is a much sharper
        // one masked to the curved band and drawn over it.
        //
        // Blur and refraction are different effects, and a lens bending an image
        // that has already been through a sigma of 8 produces a smear rather
        // than a bend. This is what makes the edge read as glass instead of as
        // frost. GlassRimTap=0 in settings.ini turns it off if it goes wrong on
        // some driver, and everything else about the material stays.
        const float sigma    = g_tuning.blurSigma;
        const float rimSigma = g_tuning.rimBlurSigma;

        // An effect is not an image, so the downscale's result has to be asked
        // for as one. The returned image holds a reference back to the effect
        // that produces it, so the chain stays alive on its own.
        ComPtr<ID2D1Image> scaled;
        if (scale) scale->GetOutput(scaled.Put());

        ComPtr<ID2D1Effect> interior =
            scaled ? buildTap(scaled.Get(), sigma, kBlurDownscale)
                   : ComPtr<ID2D1Effect>{};
        if (interior) dc->DrawImage(interior.Get(), D2D1_INTERPOLATION_MODE_LINEAR);

        if (interior && map && config::Current().glassRimTap) {
            // Full resolution, straight off the captured bitmap: no Scale on
            // this path, which is the point of it.
            ComPtr<ID2D1Effect> rim = buildTap(captured.Get(), rimSigma, 1.0f);
            ComPtr<ID2D1Bitmap1> band =
                UploadBitmap(dc, BuildBezelMask(glassSurface));

            ComPtr<ID2D1Effect> masked;
            if (rim && band) dc->CreateEffect(CLSID_D2D1AlphaMask, masked.Put());

            if (masked) {
                masked->SetInputEffect(0, rim.Get());
                masked->SetInput(1, band.Get());
                dc->DrawImage(masked.Get(), D2D1_INTERPOLATION_MODE_LINEAR);
            }
        }
    }

    const D2D1_RECT_F area = D2D1::RectF(0.0f, 0.0f, width, height);

    // No backdrop at all: lay down a nearly opaque base coat first.
    //
    // The tint on its own is 10%, which over a sharp live desktop leaves
    // everything drawn on top floating on essentially nothing. The machines that
    // reach this path (a wedged GPU, a remote session, a capture that missed its
    // deadline) are exactly the ones nobody tests on.
    if (!haveBackdrop) {
        const D2D1_COLOR_F tint = TintColour(material);
        ComPtr<ID2D1SolidColorBrush> base;
        if (SUCCEEDED(dc->CreateSolidColorBrush(
                D2D1::ColorF(tint.r, tint.g, tint.b, material.fallbackAlpha),
                base.Put()))) {
            dc->FillRectangle(area, base.Get());
        }
    }

    ComPtr<ID2D1SolidColorBrush> tintBrush;
    if (SUCCEEDED(dc->CreateSolidColorBrush(TintColour(material), tintBrush.Put())))
        dc->FillRectangle(area, tintBrush.Get());

    dc->PopLayer();

    // Both outside the clip layer. The stroke is drawn on the antialiased
    // boundary itself, and the lit edge carries the shape's coverage in its own
    // alpha, so neither needs one.
    DrawOuterStroke(piece, toSurface, width, height, radius, material);

    // What the rim reflects: the sharp backdrop under this shape, not the
    // treated one. A highlight is a reflection of the scene, and the scene is
    // what was captured, before this material took 29% of its contrast off.
    LumaField env;
    if (haveBackdrop) {
        env = BuildLumaField(piece.frame->pixels,
                             screenRect.left - piece.frame->bounds.left,
                             screenRect.top  - piece.frame->bounds.top,
                             static_cast<int>(width),
                             static_cast<int>(height),
                             static_cast<int>(16.0f * piece.dpiScale));
    }
    DrawEdgeLight(piece, glassSurface, material, env.Empty() ? nullptr : &env);

    dc->SetTransform(D2D1::Matrix3x2F::Identity());
}

} // namespace mactab::glass
