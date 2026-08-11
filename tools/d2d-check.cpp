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
// The glass material used to be mirrored here, all of it: the upload, the two
// taps, the effect chain, the maps, the rim. It is not any more, and that is an
// improvement rather than a loss. It lives in src/glass_draw.cpp now, which has
// no C++/WinRT in it, so syntax-check.sh compiles the REAL code instead of a
// copy of it that nothing kept honest.
//
// What is left below is the Direct2D and DirectWrite that panel.cpp and
// mission.cpp still do inline, where a mirror is the only check there is.



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
