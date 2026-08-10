// Type-check the Direct2D and DirectWrite calls src/panel.cpp makes.
//
// panel.cpp is the one source tools/syntax-check.sh cannot touch, because
// C++/WinRT ships only with the Windows SDK. Every review round so far has found
// that its defects concentrate there, and in 0.1.0 it would not have compiled at
// all. But the WinRT projection is only half of what that file does: the other
// half is plain Direct2D and DirectWrite, and mingw-w64 has headers for both.
//
// So this mirrors the D2D and DWrite call shapes panel.cpp uses, with no WinRT
// anywhere, and syntax-check.sh compiles it. It catches the things that actually
// go wrong at this boundary: a CLSID that does not exist under that name, a
// helper constructor with a different arity, a method that lives on a later
// interface than the one being held.
//
// What it is NOT: a copy of panel.cpp, and nothing enforces that the two agree.
// It can drift. It is worth having anyway, because "verified once and re-checked
// whenever someone remembers" beats "never compiled by anything".
//
// Not part of the product. It is never linked into MacTab.

#include <windows.h>   // syntax-check.sh already defines WIN32_LEAN_AND_MEAN and NOMINMAX
#include <d2d1_1.h>
#include <d2d1effects_2.h>
#include <dwrite.h>
#include <cmath>
#include <string>

// mingw-w64's d2d1effects.h is missing some property enums MSVC has.
#ifndef D2D1_SCALE_PROP_SCALE
#define D2D1_SCALE_PROP_SCALE ((D2D1_PROPERTY)0)
#endif

#include "com.h"
#include "glass.h"
#include "glass_map.h"
#include "image.h"
#include "panel_layout.h"

using namespace mactab;

// Mirrors panel.cpp's UploadBitmap, and uses ComPtr rather than raw pointers on
// purpose.
//
// The rest of this file holds raw interface pointers, which is fine for checking
// call shapes but misses a whole class of mistake: ComPtr in src/com.h is
// hand-rolled and deliberately small, so it has no assignment from nullptr and
// no implicit conversion to bool. Writing `ptr = nullptr` compiles against every
// other smart pointer in the world and fails here, which is exactly what got
// through to CI once.
ComPtr<ID2D1Bitmap1> CheckUpload(ID2D1DeviceContext* dc, Bitmap image) {
    ComPtr<ID2D1Bitmap1> result;
    if (image.Empty()) return result;

    PremultiplyInPlace(image);

    const D2D1_SIZE_U size{ static_cast<UINT32>(image.width),
                            static_cast<UINT32>(image.height) };
    D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));

    if (FAILED(dc->CreateBitmap(size, image.pixels.data(),
                                static_cast<UINT32>(image.width * 4),
                                &props, result.Put()))) {
        result.Reset();
    }
    return result;
}

D2D1_MATRIX_5X4_F ToD2D(const glass::Matrix5x4& g) {
    return D2D1::Matrix5x4F(
        g.m[0][0], g.m[0][1], g.m[0][2], g.m[0][3],
        g.m[1][0], g.m[1][1], g.m[1][2], g.m[1][3],
        g.m[2][0], g.m[2][1], g.m[2][2], g.m[2][3],
        g.m[3][0], g.m[3][1], g.m[3][2], g.m[3][3],
        g.m[4][0], g.m[4][1], g.m[4][2], g.m[4][3]);
}

void CheckMaterial(ID2D1DeviceContext* dc, ID2D1Bitmap1* captured, ID2D1Bitmap1* map) {
    ID2D1Effect *scale = nullptr, *blur = nullptr, *material = nullptr;
    ID2D1Effect *place = nullptr, *lens = nullptr;
    dc->CreateEffect(CLSID_D2D1Scale, &scale);
    dc->CreateEffect(CLSID_D2D1GaussianBlur, &blur);
    dc->CreateEffect(CLSID_D2D1ColorMatrix, &material);
    dc->CreateEffect(CLSID_D2D12DAffineTransform, &place);
    dc->CreateEffect(CLSID_D2D1DisplacementMap, &lens);

    scale->SetInput(0, captured);
    scale->SetValue(D2D1_SCALE_PROP_SCALE, D2D1::Vector2F(0.25f, 0.25f));
    blur->SetInputEffect(0, scale);
    blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, 7.5f);
    blur->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD);

    material->SetInputEffect(0, blur);
    material->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX,
                       ToD2D(glass::BuildMatrix(glass::kDark)));
    material->SetValue(D2D1_COLORMATRIX_PROP_CLAMP_OUTPUT, TRUE);

    // The upscale and the panel-local placement, inside the graph rather than on
    // the device context, because the displacement map is a second input and
    // both inputs have to share a coordinate space.
    place->SetInputEffect(0, material);
    place->SetValue(D2D1_2DAFFINETRANSFORM_PROP_TRANSFORM_MATRIX,
                    D2D1::Matrix3x2F::Scale(4.0f, 4.0f) *
                    D2D1::Matrix3x2F::Translation(-45.0f, -45.0f));
    place->SetValue(D2D1_2DAFFINETRANSFORM_PROP_INTERPOLATION_MODE,
                    D2D1_2DAFFINETRANSFORM_INTERPOLATION_MODE_LINEAR);

    lens->SetInputEffect(0, place);
    lens->SetInput(1, map);
    lens->SetValue(D2D1_DISPLACEMENTMAP_PROP_SCALE,
                   2.0f * glass::kMaxDisplacement);
    lens->SetValue(D2D1_DISPLACEMENTMAP_PROP_X_CHANNEL_SELECT,
                   D2D1_CHANNEL_SELECTOR_R);
    lens->SetValue(D2D1_DISPLACEMENTMAP_PROP_Y_CHANNEL_SELECT,
                   D2D1_CHANNEL_SELECTOR_G);

    dc->DrawImage(lens, D2D1_INTERPOLATION_MODE_LINEAR);
}

// The tap builder, in ComPtr terms. Same reason as CheckUpload: this is where
// the returns-an-empty-ComPtr and the ternary-on-a-ComPtr live, and both of
// those are hand-rolled behaviour rather than anything the standard guarantees.
ComPtr<ID2D1Effect> CheckTap(ID2D1DeviceContext* dc, ID2D1Image* src,
                             ID2D1Bitmap1* map, float sigma, float srcScale) {
    ComPtr<ID2D1Effect> blur, matrix, place;
    dc->CreateEffect(CLSID_D2D1GaussianBlur, blur.Put());
    dc->CreateEffect(CLSID_D2D1ColorMatrix, matrix.Put());
    dc->CreateEffect(CLSID_D2D12DAffineTransform, place.Put());
    if (!blur || !matrix || !place || !src) return {};

    blur->SetInput(0, src);
    place->SetValue(D2D1_2DAFFINETRANSFORM_PROP_TRANSFORM_MATRIX,
                    D2D1::Matrix3x2F::Scale(1.0f / srcScale, 1.0f / srcScale) *
                    D2D1::Matrix3x2F::Translation(0.0f, 0.0f));
    blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, sigma * 0.25f);
    blur->SetValue(D2D1_PROPERTY_CACHED, TRUE);
    matrix->SetInputEffect(0, blur.Get());
    place->SetInputEffect(0, matrix.Get());
    if (!map) return place;

    ComPtr<ID2D1Effect> extend, lens, bound;
    dc->CreateEffect(CLSID_D2D1Border, extend.Put());
    dc->CreateEffect(CLSID_D2D1DisplacementMap, lens.Put());
    dc->CreateEffect(CLSID_D2D1Crop, bound.Put());
    if (!extend || !lens || !bound) return place;

    extend->SetInputEffect(0, place.Get());
    extend->SetValue(D2D1_BORDER_PROP_EDGE_MODE_X, D2D1_BORDER_EDGE_MODE_CLAMP);
    extend->SetValue(D2D1_BORDER_PROP_EDGE_MODE_Y, D2D1_BORDER_EDGE_MODE_CLAMP);

    lens->SetInputEffect(0, extend.Get());
    lens->SetInput(1, map);

    bound->SetInputEffect(0, lens.Get());
    bound->SetValue(D2D1_CROP_PROP_RECT, D2D1::Vector4F(0.0f, 0.0f, 100.0f, 50.0f));
    return bound;
}

// Both taps and both draws, in the shape and the order panel.cpp uses them.
//
// The interior comes off the quarter-resolution Scale; the rim comes off the
// full-resolution capture, which is the difference that made the second tap
// worth having.
void CheckTapChoice(ID2D1DeviceContext* dc, ID2D1Effect* scale,
                    ID2D1Bitmap1* captured, ID2D1Bitmap1* map,
                    ID2D1Bitmap1* bezelMask) {
    // An effect is not an image. This is the exact shape panel.cpp needs and
    // the exact mistake this file exists to catch: passing the Scale effect
    // straight into SetInput compiles nowhere and was written that way first.
    ComPtr<ID2D1Image> scaled;
    if (scale) scale->GetOutput(scaled.Put());

    ComPtr<ID2D1Effect> frosted =
        scaled ? CheckTap(dc, scaled.Get(), map, glass::g_tuning.blurSigma, 0.25f)
               : ComPtr<ID2D1Effect>{};
    if (frosted) dc->DrawImage(frosted.Get(), D2D1_INTERPOLATION_MODE_LINEAR);

    if (!frosted || !map || !bezelMask) return;

    ComPtr<ID2D1Effect> rim =
        CheckTap(dc, captured, map, glass::g_tuning.rimBlurSigma, 1.0f);

    ComPtr<ID2D1Effect> masked;
    if (rim) dc->CreateEffect(CLSID_D2D1AlphaMask, masked.Put());
    if (!masked) return;

    masked->SetInputEffect(0, rim.Get());
    masked->SetInput(1, bezelMask);
    dc->DrawImage(masked.Get(), D2D1_INTERPOLATION_MODE_LINEAR);
}

// The CPU-generated maps, called exactly as panel.cpp calls them.
//
// panel.cpp is the one file nothing off Windows compiles, so a signature it uses
// and nobody else does is unchecked until CI. These four are the ones that
// changed when the rim started reflecting its backdrop.
void CheckMaps(const Bitmap& frame, const glass::Surface& surface,
               const RECT& rect, const RECT& bounds, float dpiScale) {
    const glass::Params piece =
        glass::Adapt(glass::kDark, glass::Luma(0.4f, 0.4f, 0.4f));

    glass::LumaField env =
        glass::BuildLumaField(frame, rect.left - bounds.left,
                              rect.top - bounds.top,
                              static_cast<int>(surface.width),
                              static_cast<int>(surface.height),
                              static_cast<int>(dpiScale * 16.0f));

    Bitmap light = glass::BuildEdgeLight(surface, piece,
                                         env.Empty() ? nullptr : &env);
    Bitmap map   = glass::BuildDisplacementMap(surface);
    Bitmap bezel = glass::BuildBezelMask(surface);

    const float rim = glass::MeanRimAlpha(piece, surface.height, dpiScale,
                                          env.At(0.0f, 0.0f));
    (void)light; (void)map; (void)bezel; (void)rim;
}

void CheckRim(ID2D1DeviceContext* dc, const D2D1_MATRIX_3X2_F& toSurface,
              ID2D1Bitmap1* edgeLight, float width, float height) {
    const glass::Params& m = glass::kDark;

    // Dark outer stroke, source-over, inset by half its own width.
    ID2D1SolidColorBrush* dark = nullptr;
    dc->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, m.rimOuterDark), &dark);

    dc->SetTransform(D2D1::Matrix3x2F::Translation(0.5f, 0.5f) * toSurface);
    dc->DrawGeometry(nullptr, dark, 1.0f);
    dc->SetTransform(toSurface);

    // The lit edge: a generated bitmap, premultiplied white with the amount to
    // add in its alpha, blended ADD rather than over.
    dc->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_ADD);
    dc->DrawBitmap(edgeLight, D2D1::RectF(0.0f, 0.0f, width, height), 1.0f,
                   D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
    dc->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
}

void CheckLabel(IDWriteFactory* dwrite, ID2D1DeviceContext* dc, IDWriteTextFormat* format,
                const std::wstring& text, float maxWidth, int height,
                ID2D1SolidColorBrush* brush) {
    IDWriteTextLayout* textLayout = nullptr;
    dwrite->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
                             format, maxWidth, static_cast<float>(height), &textLayout);

    IDWriteInlineObject* ellipsis = nullptr;
    dwrite->CreateEllipsisTrimmingSign(format, &ellipsis);
    DWRITE_TRIMMING trimming{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
    textLayout->SetTrimming(&trimming, ellipsis);

    DWRITE_TEXT_METRICS metrics{};
    textLayout->GetMetrics(&metrics);
    textLayout->SetMaxWidth(metrics.width);

    dc->DrawTextLayout(D2D1::Point2F(0.0f, 0.0f), textLayout, brush);
}

void CheckMenu(HMENU m) {
    ::CheckMenuRadioItem(m, 210, 212, 211, MF_BYCOMMAND);
    ::AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(m), L"Settings");
}

// The bitmap brush the space chips are filled with.
//
// Here because MSVC rejected the obvious spelling of this and nothing on this
// side of the build noticed. ID2D1DeviceContext declares its own overloads of
// CreateBitmapBrush, which hide every one ID2D1RenderTarget had, so the plain
// D2D1_BITMAP_BRUSH_PROPERTIES form does not resolve at all.
void CheckBitmapBrush(ID2D1DeviceContext* dc, ID2D1Bitmap1* bitmap) {
    D2D1_BITMAP_BRUSH_PROPERTIES1 properties{};
    properties.extendModeX       = D2D1_EXTEND_MODE_CLAMP;
    properties.extendModeY       = D2D1_EXTEND_MODE_CLAMP;
    properties.interpolationMode = D2D1_INTERPOLATION_MODE_LINEAR;

    ID2D1BitmapBrush1* brush = nullptr;
    if (FAILED(dc->CreateBitmapBrush(bitmap, &properties, nullptr, &brush)) || !brush)
        return;

    brush->SetTransform(D2D1::Matrix3x2F::Translation(12.0f, 8.0f));
    brush->Release();
}

// The rounded capsule behind the window title, and the geometry stroke around
// a space chip.
void CheckMissionChrome(ID2D1DeviceContext* dc, ID2D1PathGeometry* geometry,
                        ID2D1SolidColorBrush* brush) {
    const D2D1_ROUNDED_RECT capsule{ D2D1::RectF(0.0f, 0.0f, 200.0f, 30.0f),
                                     15.0f, 15.0f };
    dc->FillRoundedRectangle(capsule, brush);
    dc->DrawGeometry(geometry, brush, 2.5f);
}
