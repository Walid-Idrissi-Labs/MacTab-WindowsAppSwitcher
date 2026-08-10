// Icon pipeline preview.
//
// MacTab is developed on a machine that cannot run it, which means the single
// most subjective part of the project, whether the squircle geometry actually
// reads as macOS, would otherwise be entirely unverified until it shipped.
//
// image.cpp and squircle.cpp are deliberately free of windows.h, so they build
// and run natively. This harness feeds them synthetic icons covering the shapes
// Windows actually hands out, and writes the results as PNGs that can be looked
// at directly.
//
//   ./tools/preview/build.sh && ./build-preview/preview out/
//
// The synthetic inputs are chosen to exercise the branch that matters most:
//   full-bleed square  -> should get visibly squircled corners
//   circle             -> should pass through untouched (Chrome is a circle on
//                         macOS too; forcing it into a squircle would be wrong)
//   small glyph        -> should gain a generated gradient tile
//   wide/short art     -> should not be distorted
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <zlib.h>

#include "image.h"
#include "squircle.h"
#include "glass.h"
#include "glass_map.h"
#include "glass_tune.h"
#include "panel_layout.h"

using namespace mactab;

namespace {

// The material this run is using. The shipped values unless --set says
// otherwise, which is how a number that looked right on Windows gets replayed
// and measured here: settings.ini and this share one table of names, so
//
//     GlassDarkGain=0.74      in settings.ini
//     --set dark.gain=0.74    here
//
// mean the same thing by construction rather than by anybody remembering to
// keep two lists in step.
glass::Params g_dark  = glass::kDark;
glass::Params g_light = glass::kLight;

// --- minimal PNG writer ----------------------------------------------------

void AppendBigEndian32(std::vector<unsigned char>& out, uint32_t value) {
    out.push_back(static_cast<unsigned char>((value >> 24) & 0xFF));
    out.push_back(static_cast<unsigned char>((value >> 16) & 0xFF));
    out.push_back(static_cast<unsigned char>((value >>  8) & 0xFF));
    out.push_back(static_cast<unsigned char>( value        & 0xFF));
}

void AppendChunk(std::vector<unsigned char>& out, const char tag[4],
                 const std::vector<unsigned char>& data) {
    AppendBigEndian32(out, static_cast<uint32_t>(data.size()));

    std::vector<unsigned char> tagged(tag, tag + 4);
    tagged.insert(tagged.end(), data.begin(), data.end());

    out.insert(out.end(), tagged.begin(), tagged.end());
    AppendBigEndian32(out, static_cast<uint32_t>(
        crc32(0, tagged.data(), static_cast<uInt>(tagged.size()))));
}

bool WritePng(const std::string& path, const Bitmap& bitmap) {
    if (bitmap.Empty()) return false;

    // Raw scanlines, filter byte 0 (None) per row, RGBA order.
    std::vector<unsigned char> raw;
    raw.reserve(static_cast<size_t>(bitmap.height) * (1 + bitmap.width * 4));
    for (int y = 0; y < bitmap.height; ++y) {
        raw.push_back(0);
        for (int x = 0; x < bitmap.width; ++x) {
            const uint32_t p = bitmap.At(x, y);
            raw.push_back(RedOf(p));
            raw.push_back(GreenOf(p));
            raw.push_back(BlueOf(p));
            raw.push_back(AlphaOf(p));
        }
    }

    uLongf compressedSize = compressBound(static_cast<uLong>(raw.size()));
    std::vector<unsigned char> compressed(compressedSize);
    if (compress2(compressed.data(), &compressedSize, raw.data(),
                  static_cast<uLong>(raw.size()), 9) != Z_OK) {
        return false;
    }
    compressed.resize(compressedSize);

    std::vector<unsigned char> png{ 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };

    std::vector<unsigned char> ihdr;
    AppendBigEndian32(ihdr, static_cast<uint32_t>(bitmap.width));
    AppendBigEndian32(ihdr, static_cast<uint32_t>(bitmap.height));
    ihdr.push_back(8);   // bit depth
    ihdr.push_back(6);   // colour type: RGBA
    ihdr.push_back(0);   // deflate
    ihdr.push_back(0);   // adaptive filtering
    ihdr.push_back(0);   // no interlace
    AppendChunk(png, "IHDR", ihdr);
    AppendChunk(png, "IDAT", compressed);
    AppendChunk(png, "IEND", {});

    FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) return false;
    const size_t written = std::fwrite(png.data(), 1, png.size(), file);
    std::fclose(file);
    return written == png.size();
}

// --- synthetic test icons --------------------------------------------------

Bitmap MakeFullBleedSquare(int size) {
    Bitmap bitmap = Bitmap::Create(size, size);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            // Diagonal gradient so the corners are obvious when clipped.
            const int r = 40 + 180 * x / size;
            const int g = 60 + 120 * y / size;
            bitmap.At(x, y) = MakePixel(static_cast<uint8_t>(r),
                                        static_cast<uint8_t>(g), 220, 255);
        }
    }
    return bitmap;
}

Bitmap MakeCircle(int size) {
    Bitmap bitmap = Bitmap::Create(size, size);
    const double centre = size / 2.0;
    const double radius = size * 0.48;

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const double dx = x + 0.5 - centre;
            const double dy = y + 0.5 - centre;
            const double d  = std::sqrt(dx * dx + dy * dy);

            // One-pixel analytic feather so the edge is not stair-stepped.
            const double coverage = std::clamp(radius - d + 0.5, 0.0, 1.0);
            if (coverage <= 0.0) continue;

            bitmap.At(x, y) = MakePixel(232, 72, 60,
                                        static_cast<uint8_t>(coverage * 255));
        }
    }
    return bitmap;
}

Bitmap MakeSmallGlyph(int size) {
    // A dark monochrome mark on transparency, occupying a small share of the
    // canvas, the case that should acquire a generated tile.
    Bitmap bitmap = Bitmap::Create(size, size);
    const int x0 = size * 36 / 100, x1 = size * 64 / 100;
    const int y0 = size * 30 / 100, y1 = size * 70 / 100;

    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const bool stem = (x < x0 + (x1 - x0) / 3);
            const bool bar  = (y < y0 + (y1 - y0) / 4);
            if (stem || bar)
                bitmap.At(x, y) = MakePixel(30, 90, 190, 255);
        }
    }
    return bitmap;
}

Bitmap MakeWideArt(int size) {
    // Deliberately non-square content, to confirm nothing stretches.
    Bitmap bitmap = Bitmap::Create(size, size);
    const int y0 = size * 35 / 100, y1 = size * 65 / 100;
    for (int y = y0; y < y1; ++y) {
        for (int x = 0; x < size; ++x)
            bitmap.At(x, y) = MakePixel(250, 190, 40, 255);
    }
    return bitmap;
}

// Composite onto a checkerboard so transparency is visible in the PNG.
Bitmap OnCheckerboard(const Bitmap& source) {
    Bitmap out = Bitmap::Create(source.width, source.height);
    constexpr int kCell = 8;
    for (int y = 0; y < out.height; ++y) {
        for (int x = 0; x < out.width; ++x) {
            const bool light = ((x / kCell) + (y / kCell)) % 2 == 0;
            const uint8_t v = light ? 0xDD : 0xBB;
            out.At(x, y) = MakePixel(v, v, v, 255);
        }
    }
    CompositeOver(out, source, 0, 0);
    return out;
}

struct Case {
    const char* name;
    Bitmap (*make)(int);
};

// --- the glass material ----------------------------------------------------
//
// panel.cpp builds this out of Direct2D effects, which cannot run here. What
// CAN run here is the maths, and the maths is where the judgement calls are:
// saturation, gain, bias, tint alpha and the two rim alphas are seven numbers
// picked by eye, on a machine that cannot show them. So they live in glass.h,
// free of every Windows type, and both sides read the same constants and the
// same matrix. The pixel loops differ. The numbers cannot.
//
// What is NOT shared, and therefore what this does not prove: that the D2D
// plumbing is correct. CI and the user's machine cover that. This covers the
// numbers.

// Corner coverage for a superellipse of extent `r` and exponent `n`.
//
// SquircleMask is no use for the panel: it rasterises a superellipse spanning
// the WHOLE bitmap at n = 5, which is right for an icon tile whose corner
// extent is half its own width, and wrong for a 900x200 panel with a 62px
// corner. Same reason CreateSquircleGeometry takes an exponent.
std::vector<uint8_t> CornerMask(int r, float exponent) {
    std::vector<uint8_t> mask(static_cast<size_t>(r) * r, 0);
    constexpr int kSupersample = 4;

    for (int y = 0; y < r; ++y) {
        for (int x = 0; x < r; ++x) {
            int hits = 0;
            for (int sy = 0; sy < kSupersample; ++sy) {
                for (int sx = 0; sx < kSupersample; ++sx) {
                    const double u = (r - (x + (sx + 0.5) / kSupersample)) / r;
                    const double v = (r - (y + (sy + 0.5) / kSupersample)) / r;
                    if (std::pow(u, exponent) + std::pow(v, exponent) <= 1.0) ++hits;
                }
            }
            mask[static_cast<size_t>(y) * r + x] = static_cast<uint8_t>(
                (hits * 255 + (kSupersample * kSupersample) / 2) /
                (kSupersample * kSupersample));
        }
    }
    return mask;
}

// Three box passes stand in for the Gaussian.
//
// Equivalent full box width is sqrt(4*sigma^2 + 1), which is the standard
// approximation; three passes of it are perceptually indistinguishable from a
// true Gaussian, and certainly indistinguishable underneath a 0.42 tint. The
// input is an opaque wallpaper, so there is no premultiplication to worry about.
void BoxPass(Bitmap& image, int radius, bool horizontal) {
    if (radius < 1) return;

    const int major = horizontal ? image.width  : image.height;
    const int minor = horizontal ? image.height : image.width;
    const int window = radius * 2 + 1;

    std::vector<uint32_t> line(static_cast<size_t>(major));

    for (int j = 0; j < minor; ++j) {
        auto at = [&](int i) -> uint32_t& {
            return horizontal ? image.At(i, j) : image.At(j, i);
        };
        for (int i = 0; i < major; ++i) line[static_cast<size_t>(i)] = at(i);

        auto sample = [&](int i) {
            const int c = i < 0 ? 0 : (i >= major ? major - 1 : i);
            return line[static_cast<size_t>(c)];
        };

        int sr = 0, sg = 0, sb = 0;
        for (int i = -radius; i <= radius; ++i) {
            const uint32_t px = sample(i);
            sr += RedOf(px); sg += GreenOf(px); sb += BlueOf(px);
        }
        for (int i = 0; i < major; ++i) {
            at(i) = MakePixel(static_cast<uint8_t>(sr / window),
                              static_cast<uint8_t>(sg / window),
                              static_cast<uint8_t>(sb / window), 255);
            const uint32_t out = sample(i - radius);
            const uint32_t in  = sample(i + radius + 1);
            sr += RedOf(in)   - RedOf(out);
            sg += GreenOf(in) - GreenOf(out);
            sb += BlueOf(in)  - BlueOf(out);
        }
    }
}

void Blur(Bitmap& image, float sigma) {
    const int radius = static_cast<int>(std::round(std::sqrt(4.0 * sigma * sigma + 1.0) * 0.5));
    for (int pass = 0; pass < 3; ++pass) {
        BoxPass(image, radius, true);
        BoxPass(image, radius, false);
    }
}

// The whole material for one pixel now lives in glass.h, because panel.cpp and
// this file both need exactly it. All that is left here is the loop.
void ApplyMaterial(Bitmap& image, const glass::Params& p) {
    auto to255 = [](float v) {
        const float c = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        return static_cast<uint8_t>(c * 255.0f + 0.5f);
    };

    for (uint32_t& px : image.pixels) {
        const float in[3] = { RedOf(px) / 255.0f, GreenOf(px) / 255.0f,
                              BlueOf(px) / 255.0f };
        float out[3];
        glass::Apply(p, in, out);
        px = MakePixel(to255(out[0]), to255(out[1]), to255(out[2]), 255);
    }
}

// The no-capture path: an opaque base coat at fallbackAlpha, then the ordinary
// tint over it, which is exactly the order BakeBackdrop draws them in.
void ApplyFallback(Bitmap& image, const glass::Params& p) {
    auto over = [](float dst, float src, float a) { return dst * (1.0f - a) + src * a; };
    auto to255 = [](float v) {
        const float c = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        return static_cast<uint8_t>(c * 255.0f + 0.5f);
    };

    for (uint32_t& px : image.pixels) {
        float c[3] = { RedOf(px) / 255.0f, GreenOf(px) / 255.0f, BlueOf(px) / 255.0f };
        for (int i = 0; i < 3; ++i) {
            c[i] = over(c[i], p.tint[i], p.fallbackAlpha);
            c[i] = over(c[i], p.tint[i], p.tint[3]);
        }
        px = MakePixel(to255(c[0]), to255(c[1]), to255(c[2]), 255);
    }
}

// Mean sRGB luma of a bitmap, which is what Adapt() steers on.
float MeanLuma(const Bitmap& image) {
    if (image.Empty()) return 0.5;
    double total = 0.0;
    for (uint32_t px : image.pixels)
        total += glass::Luma(RedOf(px) / 255.0f, GreenOf(px) / 255.0f, BlueOf(px) / 255.0f);
    return static_cast<float>(total / image.pixels.size());
}

// Mean relative saturation, (max - min) / max. Used to check the material is
// boosting chroma by about what the reference does, rather than by eye.
float MeanSaturation(const Bitmap& image) {
    if (image.Empty()) return 0.0f;
    double total = 0.0;
    for (uint32_t px : image.pixels) {
        const int mx = (std::max)({ RedOf(px), GreenOf(px), BlueOf(px) });
        const int mn = (std::min)({ RedOf(px), GreenOf(px), BlueOf(px) });
        if (mx > 0) total += static_cast<double>(mx - mn) / mx;
    }
    return static_cast<float>(total / image.pixels.size());
}

// How far outside the panel the refraction can reach, plus slack. The crop that
// feeds ApplyRefraction has to carry at least this much margin or the rim pulls
// in pixels that are not there.
// Margin the refraction needs around a shape, in physical pixels. A function
// rather than a constant because --set can raise the displacement ceiling, and a
// pad frozen at the shipped value would then let the lens sample past the crop.
int RefractPad() {
    return static_cast<int>(glass::g_tuning.maxDisplacement) + 2;
}

// Zero the decoded displacement, so the same shot can be rendered with and
// without the lens. That comparison is the whole reason the bars wallpaper
// exists, and it is what caught the single-tap version doing nothing at all.
bool g_noRefract = false;

// Refraction, by resampling the treated backdrop through the same encoded map
// panel.cpp hands to D2D1DisplacementMap.
//
// Decoding the 8-bit map rather than calling Displacement() directly is
// deliberate. The quantisation is part of what ships, so it should be part of
// what is measured, and the encoding is the piece most likely to be wrong in a
// way that only shows on a machine nobody here has.
//
// `source` is the treated backdrop with RefractPad() pixels of margin on every
// side, because the whole point is that the rim pulls in content from outside
// the panel.
Bitmap ApplyRefraction(const Bitmap& source, const glass::Surface& surface,
                       float* peakOut = nullptr) {
    const int w = static_cast<int>(surface.width);
    const int h = static_cast<int>(surface.height);

    const Bitmap map   = glass::BuildDisplacementMap(surface);
    const float  scale = 2.0f * glass::OpticsFor(surface).maxDisplacement;

    // Bilinear, on opaque pixels, so there is no premultiply to worry about.
    auto sample = [&](float fx, float fy) {
        fx = std::min(std::max(fx, 0.0f), static_cast<float>(source.width  - 1));
        fy = std::min(std::max(fy, 0.0f), static_cast<float>(source.height - 1));

        const int   x0 = static_cast<int>(fx), y0 = static_cast<int>(fy);
        const int   x1 = std::min(x0 + 1, source.width  - 1);
        const int   y1 = std::min(y0 + 1, source.height - 1);
        const float tx = fx - x0, ty = fy - y0;

        auto mix = [&](int shift) {
            const float a = static_cast<float>((source.At(x0, y0) >> shift) & 0xFF);
            const float b = static_cast<float>((source.At(x1, y0) >> shift) & 0xFF);
            const float c = static_cast<float>((source.At(x0, y1) >> shift) & 0xFF);
            const float d = static_cast<float>((source.At(x1, y1) >> shift) & 0xFF);
            const float top    = a + (b - a) * tx;
            const float bottom = c + (d - c) * tx;
            return static_cast<uint8_t>(top + (bottom - top) * ty + 0.5f);
        };
        return MakePixel(mix(16), mix(8), mix(0), 255);
    };

    Bitmap out = Bitmap::Create(w, h);
    float peak = 0.0f;

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const uint32_t encoded = map.At(x, y);
            float dx = (RedOf(encoded)   / 255.0f - 0.5f) * scale;
            float dy = (GreenOf(encoded) / 255.0f - 0.5f) * scale;
            if (g_noRefract) { dx = 0.0f; dy = 0.0f; }

            peak = std::max(peak, std::sqrt(dx * dx + dy * dy));
            out.At(x, y) = sample(static_cast<float>(RefractPad() + x) + dx,
                                  static_cast<float>(RefractPad() + y) + dy);
        }
    }

    if (peakOut) *peakOut = peak;
    return out;
}

// Crop wider than the shape, treat that, then refract inward out of it.
//
// The wider crop is because the rim samples from outside the outline: without it
// the band the lens pulls in would be raw wallpaper sitting against treated
// glass.
//
// `originX/originY` locate the shape inside the canvas, which has to carry at
// least RefractPad() of margin around it on every side.
Bitmap RefractTap(const Bitmap& frostedCanvas, const glass::Surface& surface,
                  const glass::Params& material, int originX, int originY,
                  float* peakOut = nullptr) {
    const int w = static_cast<int>(surface.width);
    const int h = static_cast<int>(surface.height);

    Bitmap wide = Bitmap::Create(w + RefractPad() * 2, h + RefractPad() * 2);
    for (int y = 0; y < wide.height; ++y)
        for (int x = 0; x < wide.width; ++x)
            wide.At(x, y) = frostedCanvas.At(originX + x - RefractPad(),
                                             originY + y - RefractPad());

    ApplyMaterial(wide, material);
    return ApplyRefraction(wide, surface, peakOut);
}

bool g_noRimTap = false;

// The two taps, blended by the bezel mask.
//
// The interior comes off the frosted canvas and the bezel off a much sharper
// one, because blur and refraction are different effects and bending an image
// that has already been softened to nothing is not refraction, it is a smear.
// The mask fades one into the other over the bezel width.
//
// This is the CPU model of what panel.cpp builds out of D2D effects. Having it
// here is the whole reason the second tap can ship at all: the D2D graph is the
// one part of the material nothing off Windows can execute, so the material
// itself gets settled here and only the plumbing is left to be wrong.
Bitmap RefractTwoTap(const Bitmap& frosted, const Bitmap& clear,
                     const glass::Surface& surface, const glass::Params& material,
                     int originX, int originY, float* peakOut = nullptr) {
    Bitmap body = RefractTap(frosted, surface, material, originX, originY, peakOut);
    if (g_noRimTap) return body;

    const Bitmap mask = glass::BuildBezelMask(surface);
    if (mask.Empty()) return body;

    const Bitmap rim = RefractTap(clear, surface, material, originX, originY);

    for (int y = 0; y < body.height; ++y)
        for (int x = 0; x < body.width; ++x) {
            const float a = AlphaOf(mask.At(x, y)) / 255.0f;
            if (a <= 0.0f) continue;
            const uint32_t lo = body.At(x, y), hi = rim.At(x, y);
            auto mix = [&](uint8_t p, uint8_t q) {
                return static_cast<uint8_t>(p * (1.0f - a) + q * a + 0.5f);
            };
            body.At(x, y) = MakePixel(mix(RedOf(lo),   RedOf(hi)),
                                      mix(GreenOf(lo), GreenOf(hi)),
                                      mix(BlueOf(lo),  BlueOf(hi)),
                                      AlphaOf(lo));
        }
    return body;
}

// The lit edge, from the generator panel.cpp uses. The alpha carries the amount
// to add, so applying it is one loop.
void ApplyEdgeLight(Bitmap& body, const glass::Surface& surface,
                    const glass::Params& p, const glass::LumaField* env) {
    const Bitmap light = glass::BuildEdgeLight(surface, p, env);
    if (light.Empty()) return;

    auto plus = [](uint8_t c, float amount) {
        const int v = static_cast<int>(c + amount * 255.0f + 0.5f);
        return static_cast<uint8_t>(v > 255 ? 255 : v);
    };

    const int w = std::min(body.width,  light.width);
    const int h = std::min(body.height, light.height);

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float a = AlphaOf(light.At(x, y)) / 255.0f;
            if (a <= 0.0f) continue;
            uint32_t& px = body.At(x, y);
            px = MakePixel(plus(RedOf(px), a), plus(GreenOf(px), a),
                           plus(BlueOf(px), a), AlphaOf(px));
        }
    }
}

// --- self-checks -----------------------------------------------------------
//
// The pure layer is the only part of MacTab that can be executed off Windows,
// so it is the only part that can have real regression cover. These assert the
// properties that are easy to break silently while tuning constants.

int g_checkFailures = 0;

void Check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "CHECK FAILED: %s\n", what);
        ++g_checkFailures;
    }
}

// Wallpapers chosen to break the material if the numbers are wrong.
//
// Gradient is a stand-in for a real desktop and is where the saturation and the
// transfer slope get judged. Black and white are the two ends the adaptive bias
// exists for: with a fixed transfer the panel washes out over one and goes to a
// slab over the other, and the app name stops being readable over at least one.
// The six surfaces the brief asks the material to survive, plus the three that
// were already here for the numeric assertions.
enum class Wallpaper {
    Gradient,   // stand-in desktop: where saturation and the transfer are judged
    Black,      // bottom of the range
    White,      // top of the range
    Bars,       // diagonal high-contrast, for the lens
    Red,        // saturated red everywhere: does the panel pick up the hue
    Split,      // black left, white right: does the rim reflect what is behind it
    Solids,     // red / blue / green / black / white bands
    Shapes,     // large saturated circles and rectangles
    Text,       // black and white lettering
    Detail,     // bar target, decreasing width, for measuring what survives
    Ramp,       // strong colour gradients
    Photo,      // synthetic photograph, structured like the reference shot
};

const char* WallpaperName(Wallpaper kind) {
    switch (kind) {
        case Wallpaper::Black:  return "black";
        case Wallpaper::White:  return "white";
        case Wallpaper::Bars:   return "bars";
        case Wallpaper::Red:    return "red";
        case Wallpaper::Split:  return "split";
        case Wallpaper::Solids: return "solids";
        case Wallpaper::Shapes: return "shapes";
        case Wallpaper::Text:   return "text";
        case Wallpaper::Detail: return "detail";
        case Wallpaper::Ramp:   return "ramp";
        case Wallpaper::Photo:  return "photo";
        default:                return "gradient";
    }
}

// The bar target's layout, shared by the wallpaper that draws it and the code
// that measures it, because a copy of these numbers in the measurement would
// silently read the wrong columns.
inline constexpr int kBarGroups     = 5;
inline constexpr int kBarPeriods[kBarGroups] = { 96, 64, 48, 32, 16 };

int BarGroupStart(int width, int group) {
    const int left = width * 12 / 100;
    const int span = width * 76 / 100;
    return left + span * group / kBarGroups;
}

// --- drawing primitives for the test surfaces -------------------------------

void FillRect(Bitmap& b, int x0, int y0, int w, int h, uint32_t colour) {
    for (int y = (std::max)(0, y0); y < (std::min)(b.height, y0 + h); ++y)
        for (int x = (std::max)(0, x0); x < (std::min)(b.width, x0 + w); ++x)
            b.At(x, y) = colour;
}

void FillDisc(Bitmap& b, float cx, float cy, float r, uint32_t colour) {
    const int x0 = (std::max)(0, static_cast<int>(cx - r) - 1);
    const int x1 = (std::min)(b.width,  static_cast<int>(cx + r) + 2);
    const int y0 = (std::max)(0, static_cast<int>(cy - r) - 1);
    const int y1 = (std::min)(b.height, static_cast<int>(cy + r) + 2);
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            const float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            if (dx * dx + dy * dy <= r * r) b.At(x, y) = colour;
        }
}

// 5x7 uppercase, digits and a few marks. Enough to put readable words behind
// the glass without pulling in a font library, and blocky letterforms are the
// harder case anyway: they have no antialiasing to hide behind.
const char* GlyphRows(char c) {
    switch (c) {
        case 'A': return "01110""10001""10001""11111""10001""10001""10001";
        case 'B': return "11110""10001""11110""10001""10001""10001""11110";
        case 'C': return "01111""10000""10000""10000""10000""10000""01111";
        case 'D': return "11110""10001""10001""10001""10001""10001""11110";
        case 'E': return "11111""10000""11110""10000""10000""10000""11111";
        case 'F': return "11111""10000""11110""10000""10000""10000""10000";
        case 'G': return "01111""10000""10000""10011""10001""10001""01111";
        case 'H': return "10001""10001""11111""10001""10001""10001""10001";
        case 'I': return "11111""00100""00100""00100""00100""00100""11111";
        case 'K': return "10001""10010""11100""10010""10001""10001""10001";
        case 'L': return "10000""10000""10000""10000""10000""10000""11111";
        case 'M': return "10001""11011""10101""10001""10001""10001""10001";
        case 'N': return "10001""11001""10101""10011""10001""10001""10001";
        case 'O': return "01110""10001""10001""10001""10001""10001""01110";
        case 'P': return "11110""10001""11110""10000""10000""10000""10000";
        case 'R': return "11110""10001""11110""10100""10010""10001""10001";
        case 'S': return "01111""10000""01110""00001""00001""10001""01110";
        case 'T': return "11111""00100""00100""00100""00100""00100""00100";
        case 'U': return "10001""10001""10001""10001""10001""10001""01110";
        case 'V': return "10001""10001""10001""10001""01010""01010""00100";
        case 'W': return "10001""10001""10001""10101""10101""11011""10001";
        case 'X': return "10001""01010""00100""00100""00100""01010""10001";
        case 'Y': return "10001""01010""00100""00100""00100""00100""00100";
        case '0': return "01110""10011""10101""10101""10101""11001""01110";
        case '1': return "00100""01100""00100""00100""00100""00100""01110";
        case '2': return "01110""10001""00001""00110""01000""10000""11111";
        case '5': return "11111""10000""11110""00001""00001""10001""01110";
        case '.': return "00000""00000""00000""00000""00000""01100""01100";
        default:  return "00000""00000""00000""00000""00000""00000""00000";
    }
}

// `scale` is the size of one font pixel, so a scale of 8 gives 56px capitals.
void DrawWord(Bitmap& b, const char* text, int x0, int y0, int scale,
              uint32_t colour) {
    int pen = x0;
    for (const char* c = text; *c; ++c) {
        if (*c != ' ') {
            const char* rows = GlyphRows(*c);
            for (int r = 0; r < 7; ++r)
                for (int k = 0; k < 5; ++k)
                    if (rows[r * 5 + k] == '1')
                        FillRect(b, pen + k * scale, y0 + r * scale,
                                 scale, scale, colour);
        }
        pen += 6 * scale;
    }
}

Bitmap MakeWallpaper(int width, int height, Wallpaper kind) {
    Bitmap out = Bitmap::Create(width, height);

    if (kind == Wallpaper::Red) {
        // Not quite pure red. A dead 255/0/0 field has no structure at all, and
        // the point is to watch the hue survive the material, which needs the
        // material to have something to work on.
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x) {
                const bool dim = ((x / 60) + (y / 60)) % 2 == 0;
                out.At(x, y) = dim ? MakePixel(214, 16, 20, 255)
                                   : MakePixel(238, 34, 30, 255);
            }
        return out;
    }

    if (kind == Wallpaper::Split) {
        // The rim's own test surface. One edge of the panel runs across both
        // halves, so a rim that reflects its environment comes out visibly
        // brighter on the right, and a rim that is a painted-on border comes out
        // the same on both. Nothing else here can tell those apart.
        FillRect(out, 0, 0, width / 2, height, MakePixel(6, 6, 8, 255));
        FillRect(out, width / 2, 0, width - width / 2, height,
                 MakePixel(248, 248, 246, 255));
        return out;
    }

    if (kind == Wallpaper::Solids) {
        // Five vertical bands, so one render covers every solid the brief lists
        // and the panel spans all of them at once. That is the harder case than
        // five separate renders: the material has to hold together across a hard
        // colour boundary sitting under its middle.
        const uint32_t bands[5] = {
            MakePixel(220,  30,  30, 255), MakePixel( 30,  70, 220, 255),
            MakePixel( 20, 170,  70, 255), MakePixel(  0,   0,   0, 255),
            MakePixel(255, 255, 255, 255),
        };
        for (int x = 0; x < width; ++x) {
            const int band = (std::min)(4, x * 5 / (std::max)(1, width));
            for (int y = 0; y < height; ++y) out.At(x, y) = bands[band];
        }
        return out;
    }

    if (kind == Wallpaper::Shapes) {
        FillRect(out, 0, 0, width, height, MakePixel(28, 28, 34, 255));
        FillDisc(out, width * 0.22f, height * 0.42f, height * 0.30f,
                 MakePixel(228, 40, 48, 255));
        FillDisc(out, width * 0.74f, height * 0.34f, height * 0.26f,
                 MakePixel(36, 96, 235, 255));
        FillRect(out, static_cast<int>(width * 0.38f),
                 static_cast<int>(height * 0.52f),
                 static_cast<int>(width * 0.30f),
                 static_cast<int>(height * 0.40f),
                 MakePixel(24, 190, 96, 255));
        return out;
    }

    if (kind == Wallpaper::Text) {
        // Black lettering on the left half, white on the right, so both
        // polarities sit under the panel in one render.
        const int scale = (std::max)(3, height / 46);
        FillRect(out, 0, 0, width / 2, height, MakePixel(238, 238, 238, 255));
        FillRect(out, width / 2, 0, width - width / 2, height,
                 MakePixel(18, 18, 18, 255));
        DrawWord(out, "GLASS",  width / 24, height / 6, scale,
                 MakePixel(0, 0, 0, 255));
        DrawWord(out, "NOT",    width / 24, height / 2, scale,
                 MakePixel(0, 0, 0, 255));
        DrawWord(out, "FROSTED", width / 2 + width / 24, height / 6, scale,
                 MakePixel(255, 255, 255, 255));
        DrawWord(out, "PLASTIC", width / 2 + width / 24, height / 2, scale,
                 MakePixel(255, 255, 255, 255));
        return out;
    }

    if (kind == Wallpaper::Detail) {
        // A bar target: groups of vertical bars of decreasing period. Amplitude
        // measured through the glass at each period is the objective form of
        // "the background stays recognisable", and it is the one surface here
        // whose purpose is a number rather than a look.
        //
        // The groups are inset to the middle 76% so that all five sit inside the
        // panel with room to spare, and the widest is sized to fit one and a
        // half cycles in its group, which is all max-minus-min needs.
        FillRect(out, 0, 0, width, height, MakePixel(128, 128, 128, 255));
        for (int g = 0; g < kBarGroups; ++g) {
            const int p  = kBarPeriods[g];
            const int x0 = BarGroupStart(width, g);
            const int x1 = BarGroupStart(width, g + 1);
            for (int x = (std::max)(0, x0); x < (std::min)(width, x1); ++x) {
                const bool on = ((x - x0) / (p / 2)) % 2 == 0;
                const uint8_t v = on ? 245 : 12;
                for (int y = 0; y < height; ++y)
                    out.At(x, y) = MakePixel(v, v, v, 255);
            }
        }
        return out;
    }

    if (kind == Wallpaper::Ramp) {
        for (int y = 0; y < height; ++y) {
            const float v = static_cast<float>(y) / (std::max)(1, height - 1);
            for (int x = 0; x < width; ++x) {
                const float u = static_cast<float>(x) / (std::max)(1, width - 1);
                const float r = 255.0f * u;
                const float g = 255.0f * (1.0f - u) * (1.0f - v);
                const float b = 255.0f * v;
                out.At(x, y) = MakePixel(static_cast<uint8_t>(r),
                                         static_cast<uint8_t>(g),
                                         static_cast<uint8_t>(b), 255);
            }
        }
        return out;
    }

    if (kind == Wallpaper::Photo) {
        // Built to the same shape as the reference screenshot: sky, sun, and a
        // building whose windows give structure at exactly the scale the
        // measurement says has to survive the blur. In the reference you can
        // count the floors through the panel, so this is the surface where that
        // claim can be checked rather than asserted.
        for (int y = 0; y < height; ++y) {
            const float v = static_cast<float>(y) / (std::max)(1, height - 1);
            for (int x = 0; x < width; ++x) {
                out.At(x, y) = MakePixel(
                    static_cast<uint8_t>( 60.0f + 150.0f * v),
                    static_cast<uint8_t>(110.0f + 110.0f * v),
                    static_cast<uint8_t>(190.0f -  40.0f * v), 255);
            }
        }
        FillDisc(out, width * 0.80f, height * 0.18f, height * 0.09f,
                 MakePixel(255, 244, 214, 255));

        const int bx = static_cast<int>(width * 0.08f);
        const int bw = static_cast<int>(width * 0.56f);
        const int by = static_cast<int>(height * 0.22f);
        FillRect(out, bx, by, bw, height - by, MakePixel(86, 78, 72, 255));

        // Floors and columns, 9px windows on a 16px pitch. Under a sigma of 8
        // these stay countable; under the 30 we shipped in 0.4.0 they did not.
        for (int wy = by + 10; wy + 9 < height; wy += 16)
            for (int wx = bx + 10; wx + 9 < bx + bw; wx += 16) {
                const bool lit = ((wx / 16) * 7 + (wy / 16) * 3) % 5 != 0;
                FillRect(out, wx, wy, 9, 9,
                         lit ? MakePixel(238, 226, 186, 255)
                             : MakePixel(44, 40, 38, 255));
            }
        return out;
    }

    if (kind == Wallpaper::Bars) {
        // High-contrast diagonal bars, wide enough to survive the 30px blur, so
        // the rim has real structure to bend. Diagonal on purpose: horizontal or
        // vertical bars would only exercise one axis of the displacement.
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x) {
                const bool lightBar = ((x + y) / 75) % 2 == 0;
                const uint8_t v = lightBar ? 230 : 25;
                out.At(x, y) = MakePixel(v, v, v, 255);
            }
        return out;
    }

    if (kind != Wallpaper::Gradient) {
        // Not perfectly flat. A dead-flat field would hide a blur that is too
        // weak, and the point of these two is the ENDS of the range, not the
        // texture, so a few percent of structure is enough.
        // Genuinely the ends of the range. A 250/236 checker means the "white"
        // case never actually reaches white, and the adaptive step's clamp can
        // then bind without any assertion noticing.
        const uint8_t base = (kind == Wallpaper::White) ? 255 : 0;
        const uint8_t alt  = (kind == Wallpaper::White) ? 246 : 9;
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x) {
                const uint8_t v = ((x / 40) + (y / 40)) % 2 ? alt : base;
                out.At(x, y) = MakePixel(v, v, v, 255);
            }
        return out;
    }

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float u = static_cast<float>(x) / width;
            const float v = static_cast<float>(y) / height;
            float r = 30.0f + 200.0f * u * u;
            float g = 60.0f + 90.0f * v;
            float b = 210.0f - 150.0f * u;

            // Fine structure: if the blur is too weak this survives as legible
            // detail through the glass, which is exactly the giveaway that the
            // backdrop is a screenshot rather than a material.
            if ((y / 7) % 2 == 0) { r *= 0.86f; g *= 0.86f; b *= 0.86f; }
            if ((x / 31) == 0)    { r *= 0.70f; g *= 0.70f; b *= 0.70f; }

            out.At(x, y) = MakePixel(static_cast<uint8_t>(r),
                                     static_cast<uint8_t>(g),
                                     static_cast<uint8_t>(b), 255);
        }
    }

    // A blown-out block and a near-black one, so the adaptive step has real
    // extremes inside a single frame rather than only across frames.
    auto block = [&](int x0, int y0, int w, int h, uint8_t v) {
        for (int y = y0; y < y0 + h && y < height; ++y)
            for (int x = x0; x < x0 + w && x < width; ++x)
                out.At(x, y) = MakePixel(v, v, v, 255);
    };
    block(width / 8,     height / 5, width / 5, height / 2, 250);
    block(width * 5 / 8, height / 5, width / 5, height / 2, 6);

    return out;
}

// The dark outer stroke, on the outermost opaque pixel, matching the inset
// scheme panel.cpp strokes with. All that is left of the old rim: the bright
// part of it is now the lit edge, which comes off the surface normal instead of
// a vertical gradient.
//
// Shared by the panel and the app name's capsule, because panel.cpp draws it on
// both. A preview that left it off the capsule was showing a shape the build
// does not produce.
void ApplyOuterStroke(Bitmap& body, const glass::Params& material) {
    const int w = body.width, h = body.height;

    auto darken = [&](int x, int y) {
        if (x < 0 || y < 0 || x >= w || y >= h) return;
        const float a = material.rimOuterDark;
        uint32_t& px = body.At(x, y);
        px = MakePixel(static_cast<uint8_t>(RedOf(px)   * (1.0f - a)),
                       static_cast<uint8_t>(GreenOf(px) * (1.0f - a)),
                       static_cast<uint8_t>(BlueOf(px)  * (1.0f - a)),
                       AlphaOf(px));
    };

    for (int y = 0; y < h; ++y) {
        int first = -1, last = -1;
        for (int x = 0; x < w; ++x)
            if (AlphaOf(body.At(x, y)) >= 200) { first = x; break; }
        for (int x = w - 1; x >= 0; --x)
            if (AlphaOf(body.At(x, y)) >= 200) { last = x; break; }
        if (first < 0) continue;

        darken(first, y);
        darken(last,  y);
    }
    for (int x = 0; x < w; ++x) {
        int first = -1, last = -1;
        for (int y = 0; y < h; ++y)
            if (AlphaOf(body.At(x, y)) >= 200) { first = y; break; }
        for (int y = h - 1; y >= 0; --y)
            if (AlphaOf(body.At(x, y)) >= 200) { last = y; break; }
        if (first < 0) continue;

        darken(x, first);
        darken(x, last);
    }
}

// Nearest-neighbour crop and magnify. The rim band is about 14 pixels wide, so
// at 1:1 it is a smudge; this is what makes the lensing and the filament
// something that can be judged by eye rather than only asserted on.
Bitmap Zoom(const Bitmap& source, int x0, int y0, int w, int h, int factor) {
    Bitmap out = Bitmap::Create(w * factor, h * factor);
    for (int y = 0; y < out.height; ++y) {
        for (int x = 0; x < out.width; ++x) {
            const int sx = std::min(source.width  - 1, x0 + x / factor);
            const int sy = std::min(source.height - 1, y0 + y / factor);
            out.At(x, y) = source.At(std::max(0, sx), std::max(0, sy));
        }
    }
    return out;
}

// What the render measured, so the numbers can be diffed against the reference
// table instead of squinted at.
// Mean lit-edge amount along the TOP run, over a horizontal slice of the shape
// given as fractions of its width.
//
// The point of it is the split wallpaper: with the rim reflecting its
// environment the two ends of the same edge have to come out different, and by
// roughly the ratio Params::rimEnvFloor and rimEnvGain say they should. Nothing
// else here can tell a reflection from a painted line.
float MeanRimAlphaOver(const glass::Surface& s, const glass::Params& p,
                       const glass::LumaField& env, float u0, float u1) {
    const Bitmap light = glass::BuildEdgeLight(s, p, &env);
    if (light.Empty()) return 0.0f;

    const int x0 = static_cast<int>(u0 * light.width);
    const int x1 = static_cast<int>(u1 * light.width);
    const int rows = std::min(light.height, static_cast<int>(glass::kRimSpan));

    double sum = 0.0;
    int    n   = 0;
    for (int y = 0; y < rows; ++y)
        for (int x = x0; x < x1 && x < light.width; ++x) {
            sum += AlphaOf(light.At(x, y)) / 255.0;
            ++n;
        }
    return (n > 0) ? static_cast<float>(sum / n) : 0.0f;
}

struct PanelStats {
    float backdropLuma = 0.0f;   // mean of the blurred wallpaper under the panel
    float panelLuma    = 0.0f;   // mean of the finished glass
    float satBefore    = 0.0f;
    float satAfter     = 0.0f;
    float adaptedBias  = 0.0f;
    float labelContrast = 0.0f;
    float peakDisplacement = 0.0f;   // largest rim displacement, physical px
    float labelShadowContrast = 0.0f;
    bool  labelShadowed = false;
    float rimLeft = 0.0f, rimRight = 0.0f;   // mean top-rim amount at each end
    float panelChroma = 0.0f;                // mean (max - min) channel spread
};

// How much colour is left in the finished glass, as the mean spread between the
// brightest and dimmest channel. Zero is grey.
float MeanChroma(const Bitmap& image) {
    if (image.Empty()) return 0.0f;
    double sum = 0.0;
    for (int y = 0; y < image.height; ++y)
        for (int x = 0; x < image.width; ++x) {
            const uint32_t px = image.At(x, y);
            const int hi = std::max({ RedOf(px), GreenOf(px), BlueOf(px) });
            const int lo = std::min({ RedOf(px), GreenOf(px), BlueOf(px) });
            sum += (hi - lo) / 255.0;
        }
    return static_cast<float>(sum / (static_cast<double>(image.width) * image.height));
}

// The whole panel at the real layout metrics, over a real blurred wallpaper,
// with the real material applied.
//
// Every constant in here comes from panel_layout.h or glass.h, not from a copy,
// so the proportions and the material are the ones that ship.
Bitmap RenderPanel(const glass::Params& base, const Case* cases, int caseCount,
                   int selected, bool haveCapture, Wallpaper wallpaper,
                   PanelStats* stats, int count = 6) {
    const layout::Metrics m = layout::Compute(count, 2400.0f, 1.0f);

    const int panelW = static_cast<int>(m.panelWidth);
    const int panelH = static_cast<int>(m.panelHeight);
    const int margin = 60;
    const int band   = static_cast<int>(std::ceil(m.labelGap + m.labelHeight));

    Bitmap canvas = MakeWallpaper(panelW + margin * 2, panelH + band + margin * 2, wallpaper);

    // Crop from the BLURRED wallpaper only when there is a capture to blur. The
    // fallback path has no captured frame at all, so its coat goes over the
    // sharp live desktop, and that sharpness is the entire reason the coat has
    // to be as opaque as it is.
    Bitmap source = canvas;
    Bitmap clear  = canvas;
    if (haveCapture) {
        Blur(source, glass::g_tuning.blurSigma);
        Blur(clear,  glass::g_tuning.rimBlurSigma);
    }

    const glass::Surface surface{ static_cast<float>(panelW),
                                  static_cast<float>(panelH),
                                  m.radius, 1.0f };

    Bitmap body = Bitmap::Create(panelW, panelH);
    for (int y = 0; y < panelH; ++y)
        for (int x = 0; x < panelW; ++x)
            body.At(x, y) = source.At(x + margin, y + margin);

    glass::Params material = base;

    if (haveCapture) {
        // Adapt on the mean luma of what is actually behind the panel, which is
        // the whole point of holding a frozen frame.
        const float backdrop = MeanLuma(body);
        material = glass::Adapt(base, backdrop);

        if (stats) {
            stats->backdropLuma = backdrop;
            stats->satBefore    = MeanSaturation(body);
            stats->adaptedBias  = material.bias;
        }

        body = RefractTwoTap(source, clear, surface, material, margin, margin,
                             stats ? &stats->peakDisplacement : nullptr);
    } else {
        ApplyFallback(body, material);
    }

    if (stats) {
        stats->panelLuma   = MeanLuma(body);
        stats->satAfter    = MeanSaturation(body);
        stats->panelChroma = MeanChroma(body);
    }

    // Corners.
    {
        const int r = static_cast<int>(m.radius);
        const std::vector<uint8_t> corner = CornerMask(r, layout::kPanelCornerExponent);
        for (int cy = 0; cy < r; ++cy) {
            for (int cx = 0; cx < r; ++cx) {
                const uint8_t a = corner[static_cast<size_t>(cy) * r + cx];
                auto apply = [&](int x, int y) {
                    uint32_t& px = body.At(x, y);
                    px = (px & 0x00FFFFFFu) |
                         (static_cast<uint32_t>(AlphaOf(px) * a / 255) << 24);
                };
                apply(cx, cy);
                apply(panelW - 1 - cx, cy);
                apply(cx, panelH - 1 - cy);
                apply(panelW - 1 - cx, panelH - 1 - cy);
            }
        }
    }

    ApplyOuterStroke(body, material);
    {
        // The rim reflects the sharp desktop, not the treated backdrop. Same
        // input panel.cpp hands it.
        const glass::LumaField env =
            glass::BuildLumaField(canvas, margin, margin, panelW, panelH);
        ApplyEdgeLight(body, surface, material, &env);
        if (stats) {
            stats->rimLeft  = MeanRimAlphaOver(surface, material, env, 0.0f, 0.25f);
            stats->rimRight = MeanRimAlphaOver(surface, material, env, 0.75f, 1.0f);
        }
    }

    CompositeOver(canvas, body, margin, margin);

    // Selection highlight, then the tiles themselves.
    const int inset  = static_cast<int>(m.tileSize * layout::kSelectionInset);
    const int hlSize = static_cast<int>(m.tileSize) + inset * 2;

    Bitmap highlight = Bitmap::Create(hlSize, hlSize);
    const uint8_t hlAlpha = (material.tint[0] > 0.5f) ? 26 : 46;
    const uint8_t hlValue = (material.tint[0] > 0.5f) ? 0 : 255;
    for (uint32_t& px : highlight.pixels) px = MakePixel(hlValue, hlValue, hlValue, hlAlpha);
    // Rounded, matching BakeSelection in panel.cpp: the SAME corner as the
    // backdrop, same extent and same exponent, clamped to half the highlight's
    // own size. A tighter corner nested inside a rounder one reads as a mistake.
    {
        const int r = static_cast<int>(std::min(m.radius, hlSize * 0.5f));
        const std::vector<uint8_t> corner = CornerMask(r, layout::kPanelCornerExponent);
        for (int cy = 0; cy < r; ++cy) {
            for (int cx = 0; cx < r; ++cx) {
                const uint8_t a = corner[static_cast<size_t>(cy) * r + cx];
                auto apply = [&](int x, int y) {
                    uint32_t& px = highlight.At(x, y);
                    px = (px & 0x00FFFFFFu) |
                         (static_cast<uint32_t>(AlphaOf(px) * a / 255) << 24);
                };
                apply(cx, cy);
                apply(hlSize - 1 - cx, cy);
                apply(cx, hlSize - 1 - cy);
                apply(hlSize - 1 - cx, hlSize - 1 - cy);
            }
        }
    }
    CompositeOver(canvas, highlight,
                  margin + static_cast<int>(m.TileX(selected)) - inset,
                  margin + static_cast<int>(m.padding) - inset);

    for (int i = 0; i < count; ++i) {
        const Bitmap tile = MakeIconTile(cases[i % caseCount].make(256),
                                         static_cast<int>(m.tileSize));
        CompositeOver(canvas, tile,
                      margin + static_cast<int>(m.TileX(i)),
                      margin + static_cast<int>(m.padding));
    }

    // The app name's capsule, below the glass. There is no DirectWrite here, so
    // the text itself is a bar of the width a name like "Visual Studio Code"
    // occupies; the capsule and its placement are what need looking at.
    {
        // Same cap and same clamp BakeLabel uses. They had drifted, which made
        // panel-label-end.png, the render whose whole job is to show the clamp,
        // show a position up to 22px away from the one that ships.
        const float labelWidth = std::min(2.0f * (m.tileSize + m.gap), m.panelWidth);
        const float centre = m.TileCentreX(selected);
        const float x = std::max(0.0f,
                                 std::min(centre - labelWidth * 0.5f,
                                          m.panelWidth - labelWidth));

        const int pillW = static_cast<int>(labelWidth);
        const int pillH = static_cast<int>(m.labelHeight);
        const int pillX = margin + static_cast<int>(x);
        const int pillY = margin + static_cast<int>(m.panelHeight + m.labelGap);

        // A capsule: corner extent is half the height.
        const glass::Surface pillSurface{ static_cast<float>(pillW),
                                          static_cast<float>(pillH),
                                          static_cast<float>(pillH) * 0.5f, 1.0f };

        Bitmap pill = Bitmap::Create(pillW, pillH);
        for (int y = 0; y < pillH; ++y)
            for (int xx = 0; xx < pillW; ++xx)
                pill.At(xx, y) = source.At(pillX + xx, pillY + y);

        // The capsule adapts on ITS OWN backdrop, not the panel's.
        //
        // It used to inherit the panel's, which is wrong whenever the two sit
        // over different content, and the text surface is what caught it: half
        // the wallpaper white and half black puts the panel's mean at 0.53 while
        // the capsule sits entirely over the white half, so the capsule landed
        // far outside its band and the app name came out at 1.7:1.
        //
        // They are still the same material. They are just two pieces of it in
        // two different places, which is what a material means.
        glass::Params pillMaterial = material;
        if (haveCapture) {
            pillMaterial = glass::Adapt(base, MeanLuma(pill));
            pill = RefractTwoTap(source, clear, pillSurface, pillMaterial,
                                 pillX, pillY);
        } else {
            ApplyFallback(pill, pillMaterial);
        }

        {
            const int r = pillH / 2;
            const std::vector<uint8_t> corner = CornerMask(r, layout::kPanelCornerExponent);
            for (int cy = 0; cy < r; ++cy)
                for (int cx = 0; cx < r; ++cx) {
                    const uint8_t a = corner[static_cast<size_t>(cy) * r + cx];
                    auto apply = [&](int xx, int yy) {
                        uint32_t& px = pill.At(xx, yy);
                        px = (px & 0x00FFFFFFu) |
                             (static_cast<uint32_t>(AlphaOf(px) * a / 255) << 24);
                    };
                    apply(cx, cy);
                    apply(pillW - 1 - cx, cy);
                    apply(cx, pillH - 1 - cy);
                    apply(pillW - 1 - cx, pillH - 1 - cy);
                }
        }

        ApplyOuterStroke(pill, pillMaterial);
        {
            const glass::LumaField env =
                glass::BuildLumaField(canvas, pillX, pillY, pillW, pillH, 8);
            ApplyEdgeLight(pill, pillSurface, pillMaterial, &env);
        }

        if (stats) {
            const float pillLuma = MeanLuma(pill);
            const bool  lightText = pillMaterial.tint[0] <= 0.5f;
            const float text = lightText ? (245.0f / 255.0f) : (20.0f / 255.0f);

            stats->labelContrast = glass::ContrastRatio(pillLuma, text);

            // What BakeLabel does when that is not enough: black at 0.30 under
            // light text, white at 0.35 under dark text, drawn one pixel down.
            // Modelled as the local background the glyph actually sits on, which
            // is what decides whether it reads.
            const float shadowed = lightText ? pillLuma * (1.0f - 0.30f)
                                             : pillLuma * (1.0f - 0.35f) + 0.35f;
            stats->labelShadowContrast = glass::ContrastRatio(shadowed, text);
            stats->labelShadowed = stats->labelContrast < glass::kMinTextContrast;
        }

        Bitmap bar = Bitmap::Create(pillW - pillH, pillH / 3);
        const uint8_t v = (material.tint[0] > 0.5f) ? 20 : 245;
        for (uint32_t& px : bar.pixels) px = MakePixel(v, v, v, 235);

        CompositeOver(canvas, pill, pillX, pillY);
        CompositeOver(canvas, bar, pillX + pillH / 2, pillY + pillH / 3);
    }

    return canvas;
}

void RunSelfChecks() {
    // Shrink-to-fit: a single strip, never wrapping, never below the floor.
    {
        const layout::Metrics few = layout::Compute(3, 2000.0f, 1.0f);
        Check(few.tileSize == layout::kTileSize,
              "few apps keep the full tile size");

        const layout::Metrics many = layout::Compute(40, 1200.0f, 1.0f);
        Check(many.tileSize < layout::kTileSize, "many apps shrink the tiles");
        Check(many.tileSize >= layout::kMinTileSize, "tiles never go below the floor");
        Check(many.panelWidth <= 1200.0f + 0.5f, "panel never exceeds the space given");

        // Tiles must not overlap at any count.
        const layout::Metrics m = layout::Compute(12, 900.0f, 1.0f);
        for (int i = 1; i < 12; ++i)
            Check(m.TileX(i) >= m.TileX(i - 1) + m.tileSize, "tiles do not overlap");

        // DPI scaling must be uniform, not applied twice anywhere.
        const layout::Metrics at200 = layout::Compute(3, 4000.0f, 2.0f);
        Check(std::fabs(at200.tileSize - few.tileSize * 2.0f) < 0.01f,
              "tile size scales linearly with DPI");
    }

    // Resize preserves aspect ratio and never invents opacity.
    {
        Bitmap wide = Bitmap::Create(200, 50);
        for (uint32_t& px : wide.pixels) px = MakePixel(255, 0, 0, 255);

        const Bitmap fitted = FitInto(wide, 100, 100);
        Check(fitted.width == 100 && fitted.height == 100, "FitInto returns the box size");

        const Bounds content = OpaqueBounds(fitted);
        Check(content.Width() > content.Height(), "wide art stays wide after fitting");
        Check(std::abs(content.Width() - 100) <= 2, "wide art fills the constrained axis");

        const Bitmap empty = Bitmap::Create(64, 64);
        Check(OpaqueBounds(empty).Empty(), "fully transparent art has empty bounds");
        Check(OpaqueCoverage(empty) == 0.0, "fully transparent art has zero coverage");
    }

    // The squircle mask is symmetric and actually clips the corners.
    {
        const int size = 64;
        const std::vector<uint8_t>& mask = SquircleMask(size);
        Check(mask.size() == static_cast<size_t>(size) * size, "mask is the requested size");
        Check(mask[0] == 0, "mask corner is fully transparent");
        Check(mask[static_cast<size_t>(size / 2) * size + size / 2] == 255,
              "mask centre is fully opaque");
        for (int y = 0; y < size; ++y) {
            Check(mask[static_cast<size_t>(y) * size] ==
                  mask[static_cast<size_t>(y) * size + size - 1],
                  "mask is horizontally symmetric");
        }
    }

    // The glass matrix.
    //
    // Two invariants, and they are the only things that can rot silently: a
    // transposition, or a slip in one of the six coefficients. Both survive
    // compilation and both are invisible without a screenshot.
    for (const glass::Params* p : { &g_dark, &g_light }) {
        const glass::Matrix5x4 m = glass::BuildMatrix(*p);

        // Grey in, grey out at the gain: every colour column sums to g.
        for (int c = 0; c < 3; ++c) {
            Check(std::fabs(glass::ColumnSum(m, c) - p->gain) < 1e-3f,
                  "glass matrix column sums to the gain");
        }

        // White maps to gain + bias, which is where the compression puts the
        // white point and therefore what a blown-out wallpaper becomes.
        for (int c = 0; c < 3; ++c) {
            const float white = m.m[0][c] + m.m[1][c] + m.m[2][c] + m.m[4][c];
            Check(std::fabs(white - (p->gain + p->bias)) < 1e-3f,
                  "glass matrix maps white to gain + bias");
        }

        // Not symmetric. If it is, someone has transposed it, and the
        // saturation will be applied along the wrong axis.
        Check(std::fabs(m.m[0][1] - m.m[1][0]) > 1e-3f,
              "glass matrix is not symmetric (a transposition would be silent)");

        // Alpha passes through untouched.
        Check(m.m[3][3] == 1.0f && m.m[3][0] == 0.0f, "glass matrix leaves alpha alone");

        // Params is initialised positionally, so inserting a member in the
        // middle silently shifts every value after it and still compiles.
        // These three orderings hold for both themes and nothing else in the
        // struct has that shape, so a shifted field trips at least one.
        Check(p->fallbackAlpha > p->tint[3],
              "the no-capture base coat is more opaque than the tint");
        Check(p->rimAmbient > p->rimLobe && p->specLine > p->rimLobe,
              "the rim amplitudes keep their order");
        Check(p->gain > 0.0f && p->gain < 1.0f, "gain compresses rather than expands");
        Check(p->bias >= 0.0f && p->bias < 0.5f, "bias lifts the black point, not the whole image");
        Check(p->rimEnvGain > p->rimEnvFloor && p->rimEnvFloor > 0.0f,
              "the rim's reflection has a floor below its gain");
        Check(p->targetMin < p->targetMax, "the adaptive band is the right way round");
        Check(p->kneeBelow > 0.0f && p->kneeBelow <= 1.0f &&
              p->kneeAbove >= 0.0f && p->kneeAbove <= 1.0f,
              "the knees are fractions");

        // A mid backdrop has to leave the rim amounts meaning what they were
        // measured to mean, or every number above drifts with the reflection.
        Check(std::fabs(glass::RimReflection(*p, 0.5f) - 1.0f) < 0.01f,
              "a mid backdrop reflects at unity");

        // The knee has to keep the direction of an excursion, in both
        // directions, or the panel is back to being a fixed colour.
        Check(glass::LandingPoint(*p, p->targetMin - 0.2f) < p->targetMin,
              "a dark desktop lands the panel below its floor");
        Check(glass::LandingPoint(*p, p->targetMin - 0.2f) > p->targetMin - 0.2f,
              "the floor still pulls, it just no longer clamps");

        // The user's verdict on 0.3 was that the glass reads opaque. This is
        // the number that was wrong, so this is the assertion that stops it
        // coming back: at least half the desktop's contrast reaches the screen.
        Check(glass::EndGain(*p) >= glass::kMinEndGain,
              "the material lets enough of the desktop through to read as glass");

        // Saturation above 1 must actually push colours apart, not clamp to
        // identity. This is the failure mode CLSID_D2D1Saturation has.
        Check(p->saturation > 1.0f, "the material boosts saturation");
        Check(m.m[0][0] > glass::ColumnSum(m, 0),
              "a channel contributes more to itself than the whole column sums to");
    }

    // The optics.
    //
    // None of this can be looked at on a Windows machine before it ships, and
    // the D2D side of it (channel selection, the scale property, whether input 1
    // lines up with input 0) cannot be checked here at all. What CAN be checked
    // is that the map being handed over is the right map, so this checks it
    // hard.
    {
        const layout::Metrics m = layout::Compute(6, 2400.0f, 1.0f);
        const glass::Surface surface{ m.panelWidth, m.panelHeight, m.radius, 1.0f };
        const glass::Optics  optics = glass::OpticsFor(surface);

        const Bitmap map   = glass::BuildDisplacementMap(surface);
        const Bitmap light = glass::BuildEdgeLight(surface, g_dark);
        const float  scale = 2.0f * optics.maxDisplacement;

        // The interior is flat glass. Anything else there means the bezel is
        // reaching all the way in, and the whole panel is a lens.
        const int cx = map.width / 2, cy = map.height / 2;
        Check(RedOf(map.At(cx, cy)) == 128 && GreenOf(map.At(cx, cy)) == 128,
              "the middle of the panel displaces nothing");
        Check(AlphaOf(map.At(cx, cy)) == 255,
              "the map is opaque, so premultiplying cannot bend it");
        Check(AlphaOf(light.At(cx, cy)) == 0, "the middle of the panel is not lit");

        // Peak displacement. The physics puts it at 12.5 against a ceiling of
        // 16; well under means a retune has quietly flattened the lens, and at
        // the ceiling means it is clipping and the profile no longer has its
        // shape.
        float peak = 0.0f, peakAt = 0.0f;
        for (float s = 0.05f; s < optics.bezel; s += 0.05f) {
            const float d = glass::Displacement(s, optics);
            if (d > peak) { peak = d; peakAt = s; }
        }
        Check(peak >= 0.6f * optics.maxDisplacement &&
              peak <= 1.0f * optics.maxDisplacement,
              "peak rim displacement sits between 0.6 and 1.0 of the ceiling");
        Check(glass::Displacement(0.0f, optics) == 0.0f &&
              glass::Displacement(optics.bezel, optics) == 0.0f,
              "displacement is zero at both ends of the bezel");

        // Past the peak it has to fall off monotonically. A sign slip in the
        // profile's derivative gives a lens that gets stronger toward the
        // middle, which would look like a smear rather than an edge.
        bool monotone = true;
        for (float s = peakAt + 0.5f; s + 0.25f < optics.bezel; s += 0.25f) {
            if (glass::Displacement(s + 0.25f, optics) > glass::Displacement(s, optics) + 1e-4f)
                monotone = false;
        }
        Check(monotone, "displacement falls off monotonically past its peak");

        // The 8-bit round trip. This is what D2D actually reads, so an encoding
        // that loses more than one step is a visible band at the rim.
        float worst = 0.0f;
        for (int x = 0; x < map.width; ++x) {
            float nx = 0.0f, ny = 0.0f;
            const float d = glass::SignedDistance(surface, x + 0.5f, 0.5f, nx, ny);
            if (d >= 0.0f) continue;
            const float want = glass::Displacement(-d, optics) * ny;
            const float got  = (GreenOf(map.At(x, 0)) / 255.0f - 0.5f) * scale;
            worst = std::max(worst, std::fabs(want - got));
        }
        Check(worst <= 2.0f * optics.maxDisplacement / 255.0f,
              "the map round trips within one quantisation step");
        std::printf("optics: bezel %.1f  depth %.1f  peak %.1f at %.1f in  "
                    "round trip %.3f px\n",
                    static_cast<double>(optics.bezel),
                    static_cast<double>(optics.depth),
                    static_cast<double>(peak), static_cast<double>(peakAt),
                    static_cast<double>(worst));

        // The app name's capsule, which is the shape that breaks these.
        //
        // 28 logical pixels tall against a 14px bezel, so OpticsFor has to scale
        // the whole lens down or the capsule is bezel all the way through and
        // the app name sits on nothing but distortion. Checked at both DPIs
        // because the clamp is a ratio and the constants are not.
        for (float dpi : { 1.0f, 2.0f }) {
            const glass::Surface capsule{ 268.0f * dpi, 28.0f * dpi,
                                          14.0f * dpi, dpi };
            const glass::Optics co = glass::OpticsFor(capsule);
            Check(co.bezel < capsule.height * 0.5f,
                  "the capsule keeps an unbent core");
            Check(glass::Displacement(co.bezel * 0.05f, co) <= co.maxDisplacement,
                  "the capsule's lens stays under its ceiling");

            const Bitmap capsuleMap = glass::BuildDisplacementMap(capsule);
            const int mid = capsuleMap.height / 2;
            Check(RedOf(capsuleMap.At(capsuleMap.width / 2, mid)) == 128 &&
                  GreenOf(capsuleMap.At(capsuleMap.width / 2, mid)) == 128,
                  "the middle of the capsule displaces nothing");
        }

        // The overhead lobe. Sampled four pixels in, past the filament, so this
        // measures the field alone. The reference has the top rim at +33 luma
        // against +22 at the sides.
        for (const glass::Params* p : { &g_dark, &g_light }) {
            const Bitmap lit = glass::BuildEdgeLight(surface, *p);
            const float top  = AlphaOf(lit.At(lit.width / 2, 4));
            const float side = AlphaOf(lit.At(4, lit.height / 2));
            Check(side > 0.0f && top / side >= 1.4f && top / side <= 1.7f,
                  "the top of the rim is lit about 1.5 times the sides");
        }
    }

    // Glyph tiles must stay legible: the generated background has to contrast
    // with the mark sitting on it. This is the bug the preview caught by eye.
    {
        const Bitmap glyph = MakeSmallGlyph(256);
        const IconAnalysis analysis = AnalyzeIcon(glyph);
        Check(!analysis.artwork, "a small mark is treated as a glyph");

        auto luma = [](uint32_t c) {
            return (RedOf(c) * 299 + GreenOf(c) * 587 + BlueOf(c) * 114) / 1000;
        };
        // Mean colour of the mark itself.
        uint64_t r = 0, g = 0, b = 0, n = 0;
        for (uint32_t px : glyph.pixels) {
            if (AlphaOf(px) < 128) continue;
            r += RedOf(px); g += GreenOf(px); b += BlueOf(px); ++n;
        }
        Check(n > 0, "the test glyph has opaque pixels");
        const int glyphLuma = luma(MakePixel(static_cast<uint8_t>(r / n),
                                             static_cast<uint8_t>(g / n),
                                             static_cast<uint8_t>(b / n), 255));
        const int tileLuma = luma(analysis.tintTop);
        Check(std::abs(tileLuma - glyphLuma) >= 50,
              "generated tile contrasts with the glyph on it");
    }
}

// --set dark.gain=0.74, --set light.tinta=0.06, --set blursigma=5
//
// The same names settings.ini uses, minus the Glass prefix, so a value that
// looked right on the Windows side can be pasted in here and measured. Anything
// unrecognised is an error rather than a shrug: a silently ignored knob is how
// somebody spends an afternoon tuning a number that was never being read.
bool ApplySetting(const std::string& arg) {
    const size_t eq = arg.find('=');
    if (eq == std::string::npos) return false;

    std::string name  = arg.substr(0, eq);
    const float value = std::strtof(arg.c_str() + eq + 1, nullptr);

    glass::Params* target = nullptr;
    const size_t dot = name.find('.');
    if (dot != std::string::npos) {
        const std::string which = name.substr(0, dot);
        if      (which == "dark")  target = &g_dark;
        else if (which == "light") target = &g_light;
        else return false;
        name = name.substr(dot + 1);
    }

    if (target) return glass::SetField(*target, name.c_str(), value);
    return glass::SetOptic(glass::g_tuning, name.c_str(), value);
}

} // namespace

int main(int argc, char** argv) {
    std::string outDir = "preview-out";
    bool haveOutDir = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--set") {
            if (i + 1 >= argc || !ApplySetting(argv[i + 1])) {
                std::fprintf(stderr, "--set wants name=value, for example "
                                     "dark.gain=0.74 or blursigma=5\n");
                return 2;
            }
            ++i;
        } else if (!haveOutDir) {
            outDir = arg;
            haveOutDir = true;
        }
    }

    const Case cases[] = {
        { "full-bleed-square", MakeFullBleedSquare },
        { "circle",            MakeCircle },
        { "small-glyph",       MakeSmallGlyph },
        { "wide-art",          MakeWideArt },
    };

    constexpr int kSourceSize = 256;
    constexpr int kTileSize   = 128;

    int failures = 0;

    RunSelfChecks();
    failures += g_checkFailures;
    std::printf("self-checks: %s\n\n",
                g_checkFailures == 0 ? "all passed" : "FAILURES (see above)");

    for (const Case& testCase : cases) {
        const Bitmap source = testCase.make(kSourceSize);
        const double coverage = OpaqueCoverage(source);
        const IconAnalysis analysis = AnalyzeIcon(source);
        const Bitmap tile = MakeIconTile(source, kTileSize);

        std::printf("%-18s coverage %.3f  -> %s\n",
                    testCase.name, coverage,
                    analysis.artwork ? "artwork (mask only)" : "glyph (generated tile)");

        const std::string base = outDir + "/" + testCase.name;
        if (!WritePng(base + "-in.png",  OnCheckerboard(source)) ||
            !WritePng(base + "-out.png", OnCheckerboard(tile))) {
            std::fprintf(stderr, "failed to write PNGs for %s\n", testCase.name);
            ++failures;
        }
    }

    // A strip of all four tiles side by side, which is how they will actually
    // be seen in the panel and the only way to judge whether they look
    // consistent with each other.
    constexpr int kGap = 16;
    const int stripWidth = kTileSize * 4 + kGap * 5;
    Bitmap strip = Bitmap::Create(stripWidth, kTileSize + kGap * 2);
    for (int y = 0; y < strip.height; ++y) {
        for (int x = 0; x < strip.width; ++x)
            strip.At(x, y) = MakePixel(0x2A, 0x2A, 0x2E, 255);   // dark panel-ish
    }
    for (int i = 0; i < 4; ++i) {
        const Bitmap tile = MakeIconTile(cases[i].make(kSourceSize), kTileSize);
        CompositeOver(strip, tile, kGap + i * (kTileSize + kGap), kGap);
    }
    if (!WritePng(outDir + "/strip.png", strip)) {
        std::fprintf(stderr, "failed to write strip.png\n");
        ++failures;
    }

    // The panel over three wallpapers in both themes, plus the no-capture
    // fallback, with the real material applied.
    //
    // Black and white are not decoration: the adaptive bias exists precisely so
    // one material serves both ends, and the numbers printed underneath are the
    // acceptance test for it. The reference measured 0.44 * L + 0.22 for the
    // light material, and this table is the only place that claim gets checked
    // before it reaches a Windows machine.
    {
        const layout::Metrics m = layout::Compute(6, 2400.0f, 1.0f);
        std::printf("\npanel: 6 tiles, tile %.0f, glass %.0fx%.0f, radius %.0f, n %.2f\n",
                    m.tileSize, m.panelWidth, m.panelHeight, m.radius,
                    static_cast<double>(layout::kPanelCornerExponent));
        std::printf("       transfer  dark %.2f*L + %.2f   light %.2f*L + %.2f\n",
                    static_cast<double>(glass::EndGain(g_dark)),
                    static_cast<double>(glass::EndBias(g_dark)),
                    static_cast<double>(glass::EndGain(g_light)),
                    static_cast<double>(glass::EndBias(g_light)));

        struct Shot {
            const char* file;
            const glass::Params& material;
            const char* theme;
            Wallpaper wallpaper;
            int selected;
            bool capture;
            int count;
        };
        const Shot shots[] = {
            { "/panel-dark-gradient.png",  g_dark,  "dark",  Wallpaper::Gradient, 1, true,  6 },
            { "/panel-dark-black.png",     g_dark,  "dark",  Wallpaper::Black,    1, true,  6 },
            { "/panel-dark-white.png",     g_dark,  "dark",  Wallpaper::White,    1, true,  6 },
            { "/panel-light-gradient.png", g_light, "light", Wallpaper::Gradient, 1, true,  6 },
            { "/panel-light-black.png",    g_light, "light", Wallpaper::Black,    1, true,  6 },
            { "/panel-light-white.png",    g_light, "light", Wallpaper::White,    1, true,  6 },
            { "/panel-label-end.png",      g_dark,  "dark",  Wallpaper::Gradient, 5, true,  6 },
            { "/panel-no-capture.png",     g_dark,  "dark",  Wallpaper::Gradient, 1, false, 6 },
            // One app is a real case now: the switcher shows a panel for it
            // rather than suppressing itself. The panel goes square, which is
            // the geometry least likely to have been thought about.
            { "/panel-single-app.png",     g_dark,  "dark",  Wallpaper::Gradient, 0, true,  1 },

            // The surfaces from the brief. These exist to be looked at rather
            // than to be measured, with one exception: `detail` is measured
            // below, because "you can still see what is behind it" is only an
            // opinion until somebody puts a number on it.
            { "/surface-dark-red.png",     g_dark,  "dark",  Wallpaper::Red,    1, true, 6 },
            { "/surface-dark-split.png",   g_dark,  "dark",  Wallpaper::Split,  1, true, 6 },
            { "/surface-dark-solids.png",  g_dark,  "dark",  Wallpaper::Solids, 1, true, 6 },
            { "/surface-dark-shapes.png",  g_dark,  "dark",  Wallpaper::Shapes, 1, true, 6 },
            { "/surface-dark-text.png",    g_dark,  "dark",  Wallpaper::Text,   1, true, 6 },
            { "/surface-dark-detail.png",  g_dark,  "dark",  Wallpaper::Detail, 1, true, 6 },
            { "/surface-dark-ramp.png",    g_dark,  "dark",  Wallpaper::Ramp,   1, true, 6 },
            { "/surface-dark-photo.png",   g_dark,  "dark",  Wallpaper::Photo,  1, true, 6 },
            { "/surface-light-red.png",    g_light, "light", Wallpaper::Red,    1, true, 6 },
            { "/surface-light-split.png",  g_light, "light", Wallpaper::Split,  1, true, 6 },
            { "/surface-light-solids.png", g_light, "light", Wallpaper::Solids, 1, true, 6 },
            { "/surface-light-shapes.png", g_light, "light", Wallpaper::Shapes, 1, true, 6 },
            { "/surface-light-text.png",   g_light, "light", Wallpaper::Text,   1, true, 6 },
            { "/surface-light-detail.png", g_light, "light", Wallpaper::Detail, 1, true, 6 },
            { "/surface-light-ramp.png",   g_light, "light", Wallpaper::Ramp,   1, true, 6 },
            { "/surface-light-photo.png",  g_light, "light", Wallpaper::Photo,  1, true, 6 },
        };

        std::printf("\n%-9s %-9s %8s %8s %8s %7s %7s %8s\n",
                    "theme", "wallpaper", "backdrop", "panel", "target",
                    "sat in", "sat out", "label");
        for (const Shot& shot : shots) {
            PanelStats stats;
            const Bitmap panel = RenderPanel(shot.material, cases,
                                             static_cast<int>(std::size(cases)),
                                             shot.selected, shot.capture,
                                             shot.wallpaper, &stats, shot.count);
            if (!WritePng(outDir + shot.file, panel)) {
                std::fprintf(stderr, "failed to write %s\n", shot.file);
                ++failures;
            }
            if (!shot.capture) continue;

            std::printf("%-9s %-9s %8.3f %8.3f  %.2f-%.2f %7.3f %7.3f %7.1f:1\n",
                        shot.theme, WallpaperName(shot.wallpaper),
                        static_cast<double>(stats.backdropLuma),
                        static_cast<double>(stats.panelLuma),
                        static_cast<double>(shot.material.targetMin),
                        static_cast<double>(shot.material.targetMax),
                        static_cast<double>(stats.satBefore),
                        static_cast<double>(stats.satAfter),
                        static_cast<double>(stats.labelContrast));
            if (stats.labelShadowed)
                std::printf("%-9s %-9s   label needs its shadow: %.1f:1 bare, "
                            "%.1f:1 shadowed\n", "", "",
                            static_cast<double>(stats.labelContrast),
                            static_cast<double>(stats.labelShadowContrast));

            // The acceptance test.
            //
            // This used to be "the panel lands inside the band", which stopped
            // being the right assertion the moment the band stopped being a hard
            // clamp. Adapt() is shared code, so the stronger form is available:
            // predict the exact landing point and check the render hit it. That
            // catches a drifting material the band never could, and it survives
            // any future retune of the knee without being rewritten.
            //
            // PanelLuma is a grey-world model: it puts the mean luma through an
            // affine transfer. On strongly coloured content that is not the
            // whole story, because a saturation of 1.7 drives the dominant
            // channel past 1 and the clamp takes the overshoot off, which lowers
            // the finished luma. That is a real effect and not an error, so on
            // chromatic surfaces the check becomes directional: clipping can
            // only pull the panel down, never lift it.
            const float predicted =
                glass::PanelLuma(glass::Adapt(shot.material, stats.backdropLuma),
                                 stats.backdropLuma);
            const float drift = predicted - stats.panelLuma;
            Check(stats.satBefore > 0.70f ? (drift >= -0.045f && drift <= 0.09f)
                                          : (std::fabs(drift) < 0.045f),
                  "the panel lands where Adapt says it will");

            // And the material still has to react. A knee of zero would pass the
            // check above and would be the fixed-colour panel all over again, so
            // the direction is asserted separately at the two ends.
            if (shot.wallpaper == Wallpaper::White || shot.wallpaper == Wallpaper::Black) {
                const float flat = glass::PanelLuma(shot.material, 0.5f);
                const bool brighter = stats.backdropLuma > 0.5f;
                Check(brighter ? (stats.panelLuma > flat) : (stats.panelLuma < flat),
                      "a brighter desktop gives a brighter panel and a darker one a darker panel");
            }
            // The app name has to read, with the shadow BakeLabel adds when the
            // bare capsule is not enough.
            //
            // Bare 4.5:1 everywhere was the old assertion, and it was the thing
            // holding the dark band down at 0.38 when the reference measures
            // 0.459. Apple runs its own label at about 4.6:1 and puts a shadow
            // under it, so requiring more than Apple of a panel meant to look
            // like Apple's was the wrong constraint.
            Check(stats.labelContrast >= glass::kMinTextContrast ||
                  stats.labelShadowContrast >= glass::kMinTextContrast,
                  "app name clears 4.5:1, with its shadow where it needs one");

            // Vibrancy, against the reference rather than against taste. The
            // macOS switcher measures relative saturation 0.737 outside the
            // panel and 0.642 inside, a ratio of 0.87. Only the light material
            // has a reference, and only a wallpaper with real chroma can measure
            // it, so this is the one shot that can check it.
            if (&shot.material == &g_light &&
                shot.wallpaper == Wallpaper::Gradient && stats.satBefore > 0.0f) {
                const float ratio = stats.satAfter / stats.satBefore;
                Check(ratio >= 0.84f && ratio <= 0.90f,
                      "the light material lands on the reference saturation ratio");
            }

            // The rim is a reflection, so the two ends of one edge over a
            // black/white split must not match. The amounts come from
            // rimEnvFloor 0.30 and rimEnvGain 1.40, which put a white backdrop
            // at 1.70 against black at 0.30, so the ratio should be near six.
            // Anything under three is a rim that has stopped reacting.
            if (shot.wallpaper == Wallpaper::Split && stats.rimLeft > 0.0f) {
                const float ratio = stats.rimRight / stats.rimLeft;
                std::printf("%-9s %-9s   rim reflects: %.3f dark end, %.3f bright "
                            "end, %.1fx\n", "", "",
                            static_cast<double>(stats.rimLeft),
                            static_cast<double>(stats.rimRight),
                            static_cast<double>(ratio));
                Check(ratio >= 3.0f,
                      "the rim reflects its backdrop instead of being a painted line");
            }

            // The environment's hue has to reach the panel. Red desktop, red
            // panel: the transmission path carries it at the end gain times the
            // saturation boost, and if this fails the material has gone neutral
            // and the glass has become a coloured card.
            if (shot.wallpaper == Wallpaper::Red) {
                Check(stats.panelChroma >= 0.15f,
                      "a red desktop gives a red panel");
            }
        }

        // How much of the desktop actually survives, as a number.
        //
        // "It looks too opaque" is a judgement nobody on this side of the build
        // can check. This turns it into one: a bar target of halving period sits
        // under the panel, and for each period the amplitude inside the panel is
        // divided by the amplitude of the same bars outside it. The bars run
        // vertically and are constant down the image, so a row through the panel
        // and a row above it see the same pattern and the comparison needs no
        // alignment.
        //
        // Two things are in that ratio and both belong there: the blur, which
        // takes the fine periods out, and the material's gain, which takes a
        // flat 29% off everything. What is left is the contrast a person sees.
        {
            const layout::Metrics dm = layout::Compute(6, 2400.0f, 1.0f);
            const int panelW = static_cast<int>(dm.panelWidth);
            const int panelH = static_cast<int>(dm.panelHeight);
            const int margin = 60;

            const Bitmap shot = RenderPanel(g_dark, cases,
                                            static_cast<int>(std::size(cases)),
                                            1, true, Wallpaper::Detail, nullptr, 6);

            auto amplitude = [&](int row, int x0, int x1) {
                float lo = 1.0f, hi = 0.0f;
                for (int x = x0; x < x1; ++x) {
                    const uint32_t px = shot.At(x, row);
                    const float l = glass::Luma(RedOf(px)   / 255.0f,
                                                GreenOf(px) / 255.0f,
                                                BlueOf(px)  / 255.0f);
                    lo = (std::min)(lo, l);
                    hi = (std::max)(hi, l);
                }
                return hi - lo;
            };

            const int outsideRow = margin / 2;   // sharp wallpaper above the panel
            // Inside the panel, above the icons. The tiles start at the panel
            // padding, 22 logical pixels down, and the lit edge is done by 13,
            // so this is the only band of pure glass across the full width.
            const int insideRow = margin + 18;

            std::printf("\nbar target, amplitude through the glass\n");
            std::printf("%8s %9s %9s %8s\n", "period", "outside", "inside", "kept");

            float keptWidest = 0.0f;
            for (int g = 0; g < kBarGroups; ++g) {
                const int gs = BarGroupStart(shot.width, g);
                const int ge = BarGroupStart(shot.width, g + 1);
                if (gs < margin + 20 || ge > margin + panelW - 20) continue;

                const float out = amplitude(outsideRow, gs, ge);
                const float in  = amplitude(insideRow,  gs, ge);
                const float kept = (out > 1e-4f) ? in / out : 0.0f;
                if (g == 0) keptWidest = kept;

                std::printf("%8d %9.3f %9.3f %7.0f%%\n", kBarPeriods[g],
                            static_cast<double>(out), static_cast<double>(in),
                            static_cast<double>(kept * 100.0f));
            }

            // The widest bars are 48px of light against 48px of dark, roughly
            // the scale of a window on the building in the reference shot.
            // Losing those means the backdrop is gone and the panel is a card.
            //
            // The ceiling is the material's own gain, 0.71, and there is no
            // getting past it, so this only has a floor. At sigma 8 the blur
            // takes another 13% off that period, which lands it near 0.62.
            Check(keptWidest >= 0.50f,
                  "coarse structure stays visible through the glass");
        }

        // The rim over structure that survives the blur, with the lens on and
        // off, top edge and left edge, both at 4x.
        //
        // This pair is the only honest test of the refraction. Every other
        // wallpaper here is smooth enough that bending it changes nothing you
        // can see, which is exactly how the first version of this shipped a lens
        // that did nothing: bars-top-4x.png and bars-top-flat-4x.png came out
        // identical.
        for (int off = 0; off < 2; ++off) {
            g_noRefract = (off == 1);
            const Bitmap panel = RenderPanel(g_dark, cases,
                                             static_cast<int>(std::size(cases)),
                                             1, true, Wallpaper::Bars, nullptr, 6);
            const char* topName  = off ? "/bars-top-flat-4x.png"  : "/bars-top-4x.png";
            const char* leftName = off ? "/bars-left-flat-4x.png" : "/bars-left-4x.png";
            if (!WritePng(outDir + topName,  Zoom(panel, 330, 40, 150, 80, 4)) ||
                !WritePng(outDir + leftName, Zoom(panel, 40, 90, 110, 100, 4)) ||
                !WritePng(outDir + (off ? "/bars-flat.png" : "/bars.png"), panel)) {
                std::fprintf(stderr, "failed to write bars crops\n");
                ++failures;
            }
        }
        g_noRefract = false;

        // The rim tap, on and off, over text.
        //
        // Text is the right surface for it: the whole claim of the second tap is
        // that the bezel bends recognisable content rather than mush, and a
        // letterform either survives being bent or it does not. It is also the
        // exact screenshot to take on Windows, so it is worth having the two
        // sides of it side by side before anyone is asked to judge.
        //
        // The assertion is the one that caught the lens doing nothing at all in
        // 0.4.0, when the on and off renders came out identical: two taps that
        // agree everywhere are one tap and some wasted work.
        {
            Bitmap withTap, withoutTap;
            for (int off = 0; off < 2; ++off) {
                g_noRimTap = (off == 1);
                Bitmap panel = RenderPanel(g_dark, cases,
                                           static_cast<int>(std::size(cases)),
                                           1, true, Wallpaper::Text, nullptr, 6);
                const char* name = off ? "/rimtap-off-4x.png" : "/rimtap-on-4x.png";
                if (!WritePng(outDir + name, Zoom(panel, 70, 42, 180, 78, 4))) {
                    std::fprintf(stderr, "failed to write %s\n", name);
                    ++failures;
                }
                (off ? withoutTap : withTap) = std::move(panel);
            }
            g_noRimTap = false;

            long long differing = 0;
            if (withTap.width == withoutTap.width &&
                withTap.height == withoutTap.height) {
                for (int y = 0; y < withTap.height; ++y)
                    for (int x = 0; x < withTap.width; ++x)
                        if (withTap.At(x, y) != withoutTap.At(x, y)) ++differing;
            }
            std::printf("\nrim tap changes %lld pixels\n", differing);
            Check(differing > 2000,
                  "the rim's sharper tap actually reaches the picture");
        }

        // The top-left corner at 4x, both themes. The numbers above say the map
        // is right; this says whether the result looks like glass, which is the
        // question that started all of this and the one no assertion answers.
        for (int i = 0; i < 2; ++i) {
            const glass::Params& p = i ? g_light : g_dark;
            const Bitmap panel = RenderPanel(p, cases,
                                             static_cast<int>(std::size(cases)),
                                             1, true, Wallpaper::Gradient, nullptr, 6);
            const Bitmap rim = Zoom(panel, 40, 40, 110, 90, 4);
            if (!WritePng(outDir + (i ? "/rim-light-4x.png" : "/rim-dark-4x.png"), rim)) {
                std::fprintf(stderr, "failed to write the rim crop\n");
                ++failures;
            }
        }
    }

    std::printf("\nwrote PNGs to %s/\n", outDir.c_str());
    return failures == 0 ? 0 : 1;
}
