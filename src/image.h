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

// Scale to fit inside boxWidth x boxHeight PRESERVING ASPECT RATIO, then centre
// the result on a transparent canvas of exactly that size. Non-square artwork
// stays un-stretched.
Bitmap FitInto(const Bitmap& source, int boxWidth, int boxHeight);

} // namespace mactab
