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
ComPtr<ID2D1Effect> CheckTap(ID2D1DeviceContext* dc, ID2D1Effect* scale,
                             ID2D1Bitmap1* map, float sigma) {
    ComPtr<ID2D1Effect> blur, matrix, place;
    dc->CreateEffect(CLSID_D2D1GaussianBlur, blur.Put());
    dc->CreateEffect(CLSID_D2D1ColorMatrix, matrix.Put());
    dc->CreateEffect(CLSID_D2D12DAffineTransform, place.Put());
    if (!blur || !matrix || !place) return {};

    blur->SetInputEffect(0, scale);
    blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, sigma * 0.25f);
    matrix->SetInputEffect(0, blur.Get());
    place->SetInputEffect(0, matrix.Get());
    if (!map) return place;

    ComPtr<ID2D1Effect> lens;
    dc->CreateEffect(CLSID_D2D1DisplacementMap, lens.Put());
    if (!lens) return place;

    lens->SetInputEffect(0, place.Get());
    lens->SetInput(1, map);
    return lens;
}

void CheckTapChoice(ID2D1DeviceContext* dc, ID2D1Effect* scale, ID2D1Bitmap1* map) {
    ComPtr<ID2D1Effect> frosted =
        scale ? CheckTap(dc, scale, map, glass::kBlurSigma) : ComPtr<ID2D1Effect>{};
    if (frosted) dc->DrawImage(frosted.Get(), D2D1_INTERPOLATION_MODE_LINEAR);
}

// The second tap: the same graph off a lighter blur, masked to the bezel band
// and drawn over the frosted interior.
void CheckRimTap(ID2D1DeviceContext* dc, ID2D1Effect* clear, ID2D1Bitmap1* rimMask) {
    ID2D1Effect* masked = nullptr;
    dc->CreateEffect(CLSID_D2D1AlphaMask, &masked);

    masked->SetInputEffect(0, clear);
    masked->SetInput(1, rimMask);
    dc->DrawImage(masked, D2D1_INTERPOLATION_MODE_LINEAR);
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
