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

#include "glass.h"
#include "panel_layout.h"

using namespace mactab;

D2D1_MATRIX_5X4_F ToD2D(const glass::Matrix5x4& g) {
    return D2D1::Matrix5x4F(
        g.m[0][0], g.m[0][1], g.m[0][2], g.m[0][3],
        g.m[1][0], g.m[1][1], g.m[1][2], g.m[1][3],
        g.m[2][0], g.m[2][1], g.m[2][2], g.m[2][3],
        g.m[3][0], g.m[3][1], g.m[3][2], g.m[3][3],
        g.m[4][0], g.m[4][1], g.m[4][2], g.m[4][3]);
}

void CheckMaterial(ID2D1DeviceContext* dc, ID2D1Bitmap1* captured) {
    ID2D1Effect *scale = nullptr, *blur = nullptr, *material = nullptr;
    dc->CreateEffect(CLSID_D2D1Scale, &scale);
    dc->CreateEffect(CLSID_D2D1GaussianBlur, &blur);
    dc->CreateEffect(CLSID_D2D1ColorMatrix, &material);

    scale->SetInput(0, captured);
    scale->SetValue(D2D1_SCALE_PROP_SCALE, D2D1::Vector2F(0.25f, 0.25f));
    blur->SetInputEffect(0, scale);
    blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, 13.0f);
    blur->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD);

    material->SetInputEffect(0, blur);
    material->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX,
                       ToD2D(glass::BuildMatrix(glass::kDark)));
    material->SetValue(D2D1_COLORMATRIX_PROP_CLAMP_OUTPUT, TRUE);
    dc->DrawImage(material, D2D1_INTERPOLATION_MODE_LINEAR);
}

void CheckRim(ID2D1DeviceContext* dc, const D2D1_MATRIX_3X2_F& toSurface, float height) {
    const glass::Params& m = glass::kDark;

    ID2D1SolidColorBrush* dark = nullptr;
    dc->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, m.rimOuterDark), &dark);

    const D2D1_GRADIENT_STOP stops[] = {
        { 0.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, m.rimTop) },
        { 1.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, m.rimBottom) },
    };
    ID2D1GradientStopCollection* collection = nullptr;
    dc->CreateGradientStopCollection(stops, ARRAYSIZE(stops), &collection);

    ID2D1LinearGradientBrush* rim = nullptr;
    dc->CreateLinearGradientBrush(
        D2D1::LinearGradientBrushProperties(D2D1::Point2F(0.0f, 0.0f),
                                            D2D1::Point2F(0.0f, height)),
        collection, &rim);

    dc->SetTransform(D2D1::Matrix3x2F::Translation(2.0f, 2.0f) * toSurface);

    // Additive rim. The stop colours are the amount to ADD, so alpha 1 on them
    // means "add all of this" rather than "cover with this".
    dc->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_ADD);
    dc->DrawGeometry(nullptr, rim, 1.0f);
    dc->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);

    // Inner glow: four-stop vertical gradient filled over the whole panel.
    const D2D1_GRADIENT_STOP glow[] = {
        { 0.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, m.glowTop) },
        { 0.1f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.0f) },
        { 0.9f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.0f) },
        { 1.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, m.glowBottom) },
    };
    ID2D1GradientStopCollection* glowStops = nullptr;
    dc->CreateGradientStopCollection(glow, ARRAYSIZE(glow), &glowStops);
    dc->FillRectangle(D2D1::RectF(0.0f, 0.0f, 100.0f, height), rim);

    dc->SetTransform(toSurface);
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
