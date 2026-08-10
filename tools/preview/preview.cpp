// Icon pipeline preview.
//
// MacTab is developed on a machine that cannot run it, which means the single
// most subjective part of the project — whether the squircle geometry actually
// reads as macOS — would otherwise be entirely unverified until it shipped.
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
    // canvas — the case that should acquire a generated tile.
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

    // A mock of the whole panel at the real layout metrics.
    //
    // The tile size, gap, padding and corner radius were all chosen without
    // being able to see them. This renders the actual shared layout code — not
    // a copy of it — so the proportions can be judged rather than assumed.
    {
        const int count = 6;
        const layout::Metrics m = layout::Compute(count, 2400.0f, 1.0f);

        const int margin = 40;
        Bitmap panel = Bitmap::Create(static_cast<int>(m.panelWidth) + margin * 2,
                                      static_cast<int>(m.panelHeight) + margin * 2);

        // Stand-in for the blurred desktop: a soft gradient, so the glass has
        // something to sit on and the panel edge is visible.
        for (int y = 0; y < panel.height; ++y) {
            for (int x = 0; x < panel.width; ++x) {
                const int v = 90 + 70 * x / panel.width - 30 * y / panel.height;
                panel.At(x, y) = MakePixel(static_cast<uint8_t>(v * 0.55),
                                           static_cast<uint8_t>(v * 0.62),
                                           static_cast<uint8_t>(v * 0.80), 255);
            }
        }

        // The panel body, superellipse-cornered like the real thing.
        Bitmap body = Bitmap::Create(static_cast<int>(m.panelWidth),
                                     static_cast<int>(m.panelHeight));
        for (int y = 0; y < body.height; ++y) {
            for (int x = 0; x < body.width; ++x)
                body.At(x, y) = MakePixel(23, 23, 26, 210);
        }
        // Approximate the corner treatment by masking with a squircle whose
        // radius matches; enough to judge whether 24px reads as macOS.
        {
            const int r = static_cast<int>(m.radius);
            const std::vector<uint8_t>& corner = SquircleMask(r * 2);
            for (int cy = 0; cy < r; ++cy) {
                for (int cx = 0; cx < r; ++cx) {
                    const uint8_t a = corner[static_cast<size_t>(cy) * (r * 2) + cx];
                    auto apply = [&](int x, int y) {
                        uint32_t& px = body.At(x, y);
                        px = (px & 0x00FFFFFFu) | (static_cast<uint32_t>(AlphaOf(px) * a / 255) << 24);
                    };
                    apply(cx, cy);
                    apply(body.width - 1 - cx, cy);
                    apply(cx, body.height - 1 - cy);
                    apply(body.width - 1 - cx, body.height - 1 - cy);
                }
            }
        }
        CompositeOver(panel, body, margin, margin);

        // Selection highlight behind tile 1, then the tiles themselves.
        const int selected = 1;
        const int inset = static_cast<int>(m.tileSize * layout::kSelectionInset);
        const int hlSize = static_cast<int>(m.tileSize) + inset * 2;
        Bitmap highlight = Bitmap::Create(hlSize, hlSize);
        for (uint32_t& px : highlight.pixels) px = MakePixel(255, 255, 255, 46);
        // Rounded, matching BakeSelection in panel.cpp — a hard-edged rectangle
        // next to squircle icons reads as wrong immediately.
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
        CompositeOver(panel, highlight,
                      margin + static_cast<int>(m.TileX(selected)) - inset,
                      margin + static_cast<int>(m.padding) - inset);

        for (int i = 0; i < count; ++i) {
            const Bitmap tile = MakeIconTile(cases[i % 4].make(kSourceSize),
                                             static_cast<int>(m.tileSize));
            CompositeOver(panel, tile,
                          margin + static_cast<int>(m.TileX(i)),
                          margin + static_cast<int>(m.padding));
        }

        if (!WritePng(outDir + "/panel.png", panel)) {
            std::fprintf(stderr, "failed to write panel.png\n");
            ++failures;
        }
        std::printf("\npanel: %d tiles, tile %.0f, panel %.0fx%.0f, radius %.0f\n",
                    count, m.tileSize, m.panelWidth, m.panelHeight, m.radius);
    }

    std::printf("\nwrote PNGs to %s/\n", outDir.c_str());
    return failures == 0 ? 0 : 1;
}
