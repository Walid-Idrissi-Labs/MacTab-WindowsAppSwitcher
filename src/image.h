#pragma once

// Deliberately free of windows.h.
//
// Everything here is pure computation, which means it can be compiled and RUN
// natively on a development machine that is not Windows, see tools/preview/,
// which renders the icon pipeline's output to PNG so the squircle geometry can
// actually be looked at rather than guessed at. Win32 interop (HBITMAP/HICON
// conversion) lives in icon_source.h instead, precisely to keep this header
// portable.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace mactab {

// A plain 32-bit image.
//
// Layout is 0xAARRGGBB per pixel, which in little-endian memory is B,G,R,A,
// the same byte order as a Win32 32-bit DIB and as DXGI_FORMAT_B8G8R8A8_UNORM.
// That means a finished bitmap can be handed to both the shell and Direct2D
// without a channel shuffle.
//
// Alpha is STRAIGHT (not premultiplied) everywhere in this type. Premultiplied
// data is only produced at the boundary, by PremultiplyInPlace, immediately
// before upload. Keeping intermediate stages straight avoids the progressive
// darkening you get from repeatedly compositing premultiplied pixels.
struct Bitmap {
    int                   width  = 0;
    int                   height = 0;
    std::vector<uint32_t> pixels;

    bool Empty() const { return width <= 0 || height <= 0 || pixels.empty(); }

    uint32_t At(int x, int y) const {
        return pixels[static_cast<size_t>(y) * static_cast<size_t>(width) +
                      static_cast<size_t>(x)];
    }
    uint32_t& At(int x, int y) {
        return pixels[static_cast<size_t>(y) * static_cast<size_t>(width) +
                      static_cast<size_t>(x)];
    }

    static Bitmap Create(int width, int height, uint32_t fill = 0) {
        Bitmap bitmap;
        bitmap.width  = width;
        bitmap.height = height;
        bitmap.pixels.assign(static_cast<size_t>(width) * static_cast<size_t>(height), fill);
        return bitmap;
    }
};

constexpr uint8_t AlphaOf(uint32_t p) { return static_cast<uint8_t>((p >> 24) & 0xFF); }
constexpr uint8_t RedOf(uint32_t p)   { return static_cast<uint8_t>((p >> 16) & 0xFF); }
constexpr uint8_t GreenOf(uint32_t p) { return static_cast<uint8_t>((p >>  8) & 0xFF); }
constexpr uint8_t BlueOf(uint32_t p)  { return static_cast<uint8_t>( p        & 0xFF); }

constexpr uint32_t MakePixel(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(r) << 16) |
           (static_cast<uint32_t>(g) <<  8) |  static_cast<uint32_t>(b);
}

// Resize with correct alpha handling.
//
// Resampling straight-alpha pixels bleeds the colour of fully transparent
// pixels into visible ones; the classic dark or white halo around a scaled
// icon. So this premultiplies, resamples, then unpremultiplies.
//
// Downscaling uses a box filter (area average) rather than bilinear: icons are
// frequently reduced 256 -> 64, where bilinear point-samples and aliases badly.
//
// Upscaling uses Catmull-Rom rather than bilinear. Plenty of apps ship nothing
// larger than a 48px icon, so a 4x enlargement is a normal case rather than an
// unlucky one, and bilinear at 4x is visibly mushy: it interpolates linearly
// between two samples and reproduces none of the edge that was there. A cubic
// with a slight negative lobe keeps the edge crisp. It can overshoot around a
// hard boundary, which is clamped away on the way out.
Bitmap Resize(const Bitmap& source, int width, int height);

// In-place conversion for upload to Direct2D / Composition surfaces, which
// expect premultiplied alpha.
void PremultiplyInPlace(Bitmap& bitmap);

// Straight-alpha source-over composite of `src` onto `dst` at (offsetX, offsetY).
void CompositeOver(Bitmap& dst, const Bitmap& src, int offsetX, int offsetY);

// Fraction of pixels with alpha above `threshold`, in 0..1. Used to decide
// whether an icon is full-bleed artwork or a small glyph on transparency.
double OpaqueCoverage(const Bitmap& bitmap, uint8_t threshold = 128);

struct Bounds {
    int left = 0, top = 0, right = 0, bottom = 0;   // right/bottom exclusive

    int Width()  const { return right - left; }
    int Height() const { return bottom - top; }
    bool Empty() const { return Width() <= 0 || Height() <= 0; }
};

// Tightest box containing pixels above `threshold` alpha.
//
// Needed because plenty of Windows icons are a small mark floating in a mostly
// empty 256x256 canvas. Scaling such a canvas as a whole produces a tile with a
// dot in the middle of it; scaling the *content* is what makes it look like a
// real app icon.
Bounds OpaqueBounds(const Bitmap& bitmap, uint8_t threshold = 8);

// Mean colour over a rectangle, clipped to the bitmap, alpha ignored.
//
// Used to decide the app name's colour: the label sits below the glass, on
// whatever the desktop happens to be, so a fixed per-theme colour is wrong over
// half of all wallpapers. Sampled from the captured frame that the backdrop was
// built from, so it costs nothing extra and needs no readback. Returns opaque
// mid-grey for an empty rectangle, which is the least-wrong default.
uint32_t MeanColourIn(const Bitmap& bitmap, int left, int top, int right, int bottom);

Bitmap Crop(const Bitmap& source, const Bounds& bounds);

// A flat background colour that reaches the edge of the image.
struct BorderFill {
    bool     found   = false;
    uint32_t colour  = 0;     // opaque; meaningless when !found
    double   removed = 0.0;   // fraction of the canvas it covered
};

// Turn a flat background that touches the border into transparency, and report
// the colour that was taken out.
//
// Two different sources hand us an icon with its background baked in, and both
// end up looking like the same defect: a small mark adrift in a big coloured
// square.
//
//   The shell regularly returns a 32-bit icon bitmap with every alpha byte
//   zero. Read literally that is an invisible icon, so icon_source.cpp forces
//   it opaque, which also makes the black padding around a small icon opaque.
//
//   A packaged app's icon comes out of the Apps folder already composited onto
//   the background colour from its manifest. Windows' own taskbar does not show
//   it that way; it uses the unplated asset.
//
// The removal is a flood fill inward from the border rather than a colour key
// over the whole image, because a colour key would also punch out any part of
// the artwork that happens to match, which for the common case of black padding
// means every dark pixel in the icon. Padding reaches the border; the dark half
// of a logo does not. Pixels near the tolerance edge come out part-transparent,
// so an antialiased mark keeps its antialiasing.
//
// Leaves the bitmap untouched, and reports `found = false`, when the border is
// not one flat colour, when the border is already transparent (the icon has a
// working alpha channel and there is nothing to do), or when the fill would
// swallow almost the entire image.
BorderFill RemoveBorderFill(Bitmap& bitmap);

// Scale to fit inside boxWidth x boxHeight PRESERVING ASPECT RATIO, then centre
// the result on a transparent canvas of exactly that size. Non-square artwork
// stays un-stretched.
Bitmap FitInto(const Bitmap& source, int boxWidth, int boxHeight);

} // namespace mactab
