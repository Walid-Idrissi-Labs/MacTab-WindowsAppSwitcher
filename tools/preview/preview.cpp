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
#include "panel_layout.h"

using namespace mactab;

namespace {

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

// The colour matrix from glass.h, then the tint over the top. Exactly the two
// operations panel.cpp performs, in the same order, with the same coefficients.
void ApplyMaterial(Bitmap& image, const glass::Params& p) {
    const glass::Matrix5x4 m = glass::BuildMatrix(p);

    auto clamp255 = [](float v) {
        return static_cast<uint8_t>(v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v));
    };

    for (uint32_t& px : image.pixels) {
        const float r = RedOf(px)   / 255.0f;
        const float g = GreenOf(px) / 255.0f;
        const float b = BlueOf(px)  / 255.0f;

        // CLAMP_OUTPUT is on in panel.cpp, so clamp here too: saturation above
        // 1 drives strongly coloured pixels negative in one channel.
        const float r2 = r * m.m[0][0] + g * m.m[1][0] + b * m.m[2][0] + m.m[4][0];
        const float g2 = r * m.m[0][1] + g * m.m[1][1] + b * m.m[2][1] + m.m[4][1];
        const float b2 = r * m.m[0][2] + g * m.m[1][2] + b * m.m[2][2] + m.m[4][2];

        // Straight "over" with the tint, which is what FillRectangle does on top
        // of the treated backdrop.
        const float a = p.tint[3];
        px = MakePixel(clamp255((r2 * (1.0f - a) + p.tint[0] * a) * 255.0f),
                       clamp255((g2 * (1.0f - a) + p.tint[1] * a) * 255.0f),
                       clamp255((b2 * (1.0f - a) + p.tint[2] * a) * 255.0f),
                       255);
    }
}

// A wallpaper deliberately built to break the material if the numbers are
// wrong: a saturated colour field so the saturation boost is visible, a
// near-white block and a near-black block so the luma compression can be judged
// at both ends, and fine detail so an under-strength blur shows up as legible
// structure through the glass.
Bitmap MakeWallpaper(int width, int height) {
    Bitmap out = Bitmap::Create(width, height);
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

    auto block = [&](int x0, int y0, int w, int h, uint8_t v) {
        for (int y = y0; y < y0 + h && y < height; ++y)
            for (int x = x0; x < x0 + w && x < width; ++x)
                out.At(x, y) = MakePixel(v, v, v, 255);
    };
    block(width / 8,     height / 5, width / 5, height / 2, 250);   // blown-out white
    block(width * 5 / 8, height / 5, width / 5, height / 2, 6);     // near black

    return out;
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

// The whole panel at the real layout metrics, over a real blurred wallpaper,
// with the real material applied.
//
// The tile size, gap, padding, corner radius and every glass coefficient were
// chosen without being able to see them. This runs the actual shared layout and
// glass code, not a copy of it, so the proportions and the material can be
// judged rather than assumed.
Bitmap RenderPanel(const glass::Params& material, const Case* cases, int caseCount) {
    const int count = 6;
    const layout::Metrics m = layout::Compute(count, 2400.0f, 1.0f);

    const int panelW = static_cast<int>(m.panelWidth);
    const int panelH = static_cast<int>(m.panelHeight);
    const int margin = 60;

    Bitmap canvas = MakeWallpaper(panelW + margin * 2, panelH + margin * 2);

    // Backdrop: blur a copy of the wallpaper, crop the panel's own rect out of
    // it, run the material over it. Blurring the whole canvas and cropping is
    // equivalent to capturing panel-plus-margin and blurring that, which is
    // what StartCapture does, and avoids reimplementing the margin logic.
    Bitmap blurred = canvas;
    Blur(blurred, 52.0f);   // kBlurSigma

    Bitmap body = Bitmap::Create(panelW, panelH);
    for (int y = 0; y < panelH; ++y)
        for (int x = 0; x < panelW; ++x)
            body.At(x, y) = blurred.At(x + margin, y + margin);

    ApplyMaterial(body, material);

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

    // Rim. The real one is a stroked geometry inset by half its width; this
    // approximates it by lightening the outermost opaque pixel of each column
    // and row, which is enough to judge the two alphas against each other.
    {
        auto lighten = [&](int x, int y, float alpha, bool dark) {
            if (x < 0 || y < 0 || x >= panelW || y >= panelH) return;
            uint32_t& px = body.At(x, y);
            if (AlphaOf(px) < 200) return;
            const float target = dark ? 0.0f : 255.0f;
            px = MakePixel(static_cast<uint8_t>(RedOf(px)   * (1 - alpha) + target * alpha),
                           static_cast<uint8_t>(GreenOf(px) * (1 - alpha) + target * alpha),
                           static_cast<uint8_t>(BlueOf(px)  * (1 - alpha) + target * alpha),
                           AlphaOf(px));
        };

        for (int y = 0; y < panelH; ++y) {
            const float t = static_cast<float>(y) / (panelH - 1);
            const float a = material.rimTop + (material.rimBottom - material.rimTop) * t;

            // Walk in from each side to the first opaque pixel, which follows
            // the curve through the corners.
            for (int x = 0; x < panelW; ++x)
                if (AlphaOf(body.At(x, y)) >= 200) { lighten(x, y, a, false); break; }
            for (int x = panelW - 1; x >= 0; --x)
                if (AlphaOf(body.At(x, y)) >= 200) { lighten(x, y, a, false); break; }
        }
        for (int x = 0; x < panelW; ++x) {
            for (int y = 0; y < panelH; ++y)
                if (AlphaOf(body.At(x, y)) >= 200) { lighten(x, y, material.rimTop, false); break; }
            for (int y = panelH - 1; y >= 0; --y)
                if (AlphaOf(body.At(x, y)) >= 200) { lighten(x, y, material.rimBottom, false); break; }
        }
    }

    CompositeOver(canvas, body, margin, margin);

    // Selection highlight behind tile 1, then the tiles themselves.
    const int selected = 1;
    const int inset  = static_cast<int>(m.tileSize * layout::kSelectionInset);
    const int hlSize = static_cast<int>(m.tileSize) + inset * 2;

    Bitmap highlight = Bitmap::Create(hlSize, hlSize);
    const uint8_t hlAlpha = (material.tint[0] > 0.5f) ? 26 : 46;   // light vs dark theme
    const uint8_t hlValue = (material.tint[0] > 0.5f) ? 0 : 255;
    for (uint32_t& px : highlight.pixels) px = MakePixel(hlValue, hlValue, hlValue, hlAlpha);
    // Rounded, matching BakeSelection in panel.cpp. A hard-edged rectangle next
    // to squircle icons reads as wrong immediately. This one keeps n = 5,
    // because it belongs to the icons' shape language rather than the panel's.
    {
        const int r = static_cast<int>(m.tileSize * 0.22f);
        const std::vector<uint8_t>& corner = SquircleMask(r * 2);
        for (int cy = 0; cy < r; ++cy) {
            for (int cx = 0; cx < r; ++cx) {
                const uint8_t a = corner[static_cast<size_t>(cy) * (r * 2) + cx];
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

    // Stand-in for the label. There is no DirectWrite here, so this is a bar of
    // the width a name like "Visual Studio Code" occupies, placed by the same
    // rule BakeLabel uses: centred on the selected tile, clamped inside the
    // panel's padding. It is the placement that needs looking at, not the text.
    {
        const float labelWidth =
            std::min(2.0f * (m.tileSize + m.gap),
                     std::min(m.panelWidth - m.padding * 2.0f, 150.0f));
        const float centre = m.TileCentreX(selected);
        const float x = std::max(m.padding,
                                 std::min(centre - labelWidth * 0.5f,
                                          m.panelWidth - m.padding - labelWidth));

        Bitmap bar = Bitmap::Create(static_cast<int>(labelWidth),
                                    static_cast<int>(m.labelHeight * 0.5f));
        const uint8_t v = (material.tint[0] > 0.5f) ? 20 : 245;
        for (uint32_t& px : bar.pixels) px = MakePixel(v, v, v, 90);

        CompositeOver(canvas, bar, margin + static_cast<int>(x),
                      margin + static_cast<int>(m.padding + m.tileSize + 4.0f));
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
    for (const glass::Params* p : { &glass::kDark, &glass::kLight }) {
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

        // Saturation above 1 must actually push colours apart, not clamp to
        // identity. This is the failure mode CLSID_D2D1Saturation has.
        Check(p->saturation > 1.0f, "the material boosts saturation");
        Check(m.m[0][0] > glass::ColumnSum(m, 0),
              "a channel contributes more to itself than the whole column sums to");
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

} // namespace

int main(int argc, char** argv) {
    const std::string outDir = (argc > 1) ? argv[1] : "preview-out";

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

    // The panel, dark and light, over a blurred wallpaper with the real glass
    // material applied. This is the only place the material's numbers can
    // actually be looked at before they reach a Windows machine.
    {
        const layout::Metrics m = layout::Compute(6, 2400.0f, 1.0f);
        std::printf("\npanel: 6 tiles, tile %.0f, panel %.0fx%.0f, radius %.0f, n %.2f\n",
                    m.tileSize, m.panelWidth, m.panelHeight, m.radius,
                    static_cast<double>(layout::kPanelCornerExponent));

        struct { const char* file; const glass::Params& material; } themes[] = {
            { "/panel-dark.png",  glass::kDark  },
            { "/panel-light.png", glass::kLight },
        };

        for (const auto& theme : themes) {
            const Bitmap panel = RenderPanel(theme.material, cases,
                                             static_cast<int>(std::size(cases)));
            if (!WritePng(outDir + theme.file, panel)) {
                std::fprintf(stderr, "failed to write %s\n", theme.file);
                ++failures;
            }
        }
    }

    std::printf("\nwrote PNGs to %s/\n", outDir.c_str());
    return failures == 0 ? 0 : 1;
}
