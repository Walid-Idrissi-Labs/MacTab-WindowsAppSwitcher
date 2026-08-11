#include "pch.h"
#include "geometry.h"
#include "diag.h"

namespace mactab {
namespace {

constexpr int kSegmentsPerCorner = 24;

// Point on a superellipse quadrant of radius `radius`, at parameter `t`.
//
// x = sign(cos t) * |cos t|^(2/n), y likewise with sin. At n = 2 this is a
// circle; as n grows the curve pushes out toward the corner of its bounding
// box.
D2D1_POINT_2F SuperellipsePoint(D2D1_POINT_2F centre, float radius, float t, float exponent) {
    const float c = std::cos(t);
    const float s = std::sin(t);
    const float power = 2.0f / exponent;

    const float x = std::copysign(std::pow(std::fabs(c), power), c);
    const float y = std::copysign(std::pow(std::fabs(s), power), s);

    return D2D1::Point2F(centre.x + radius * x, centre.y + radius * y);
}

void AddCorner(ID2D1GeometrySink* sink, D2D1_POINT_2F centre, float radius,
               float startAngle, float exponent) {
    constexpr float kQuarterTurn = 1.57079632679f;

    // Skip i = 0: the caller has already positioned the sink at that point,
    // either via BeginFigure or the preceding AddLine.
    for (int i = 1; i <= kSegmentsPerCorner; ++i) {
        const float t = startAngle +
                        (static_cast<float>(i) / kSegmentsPerCorner) * kQuarterTurn;
        sink->AddLine(SuperellipsePoint(centre, radius, t, exponent));
    }
}

} // namespace

ComPtr<ID2D1Bitmap1> UploadBitmap(ID2D1DeviceContext* dc, Bitmap image) {
    ComPtr<ID2D1Bitmap1> result;
    if (!dc || image.Empty()) return result;

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

ComPtr<ID2D1PathGeometry> CreateSquircleGeometry(ID2D1Factory* factory,
                                                 float width, float height, float radius,
                                                 float exponent) {
    ComPtr<ID2D1PathGeometry> geometry;
    if (!factory || width <= 0.0f || height <= 0.0f)
        return geometry;

    // n < 2 is a concave star, not a corner. Nothing should be asking for it,
    // but a bad config value must not produce a shape that self-intersects.
    if (!(exponent >= 2.0f)) exponent = 2.0f;

    // A radius above half the shorter side has no meaning; clamp rather than
    // producing a self-intersecting path.
    radius = (std::min)(radius, (std::min)(width, height) * 0.5f);
    if (radius < 0.0f) radius = 0.0f;

    if (FAILED(factory->CreatePathGeometry(geometry.Put()))) {
        MACTAB_FAIL("geometry: CreatePathGeometry failed");
        return {};
    }

    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(geometry->Open(sink.Put()))) {
        MACTAB_FAIL("geometry: could not open geometry sink");
        return {};
    }

    const float left = 0.0f, top = 0.0f, right = width, bottom = height;

    const D2D1_POINT_2F topLeft     = D2D1::Point2F(left  + radius, top    + radius);
    const D2D1_POINT_2F topRight    = D2D1::Point2F(right - radius, top    + radius);
    const D2D1_POINT_2F bottomRight = D2D1::Point2F(right - radius, bottom - radius);
    const D2D1_POINT_2F bottomLeft  = D2D1::Point2F(left  + radius, bottom - radius);

    constexpr float kPi = 3.14159265359f;
    constexpr float kHalfPi = kPi * 0.5f;

    // Clockwise from the top edge.
    sink->BeginFigure(D2D1::Point2F(left + radius, top), D2D1_FIGURE_BEGIN_FILLED);

    sink->AddLine(D2D1::Point2F(right - radius, top));
    AddCorner(sink.Get(), topRight, radius, -kHalfPi, exponent);       // top -> right

    sink->AddLine(D2D1::Point2F(right, bottom - radius));
    AddCorner(sink.Get(), bottomRight, radius, 0.0f, exponent);        // right -> bottom

    sink->AddLine(D2D1::Point2F(left + radius, bottom));
    AddCorner(sink.Get(), bottomLeft, radius, kHalfPi, exponent);      // bottom -> left

    sink->AddLine(D2D1::Point2F(left, top + radius));
    AddCorner(sink.Get(), topLeft, radius, kPi, exponent);             // left -> top

    sink->EndFigure(D2D1_FIGURE_END_CLOSED);

    if (FAILED(sink->Close())) {
        MACTAB_FAIL("geometry: geometry sink Close failed");
        return {};
    }

    return geometry;
}

} // namespace mactab
