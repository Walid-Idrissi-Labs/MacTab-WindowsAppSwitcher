#include "squircle.h"

#include <unordered_map>

namespace mactab {
namespace {

// Superellipse exponent. 5 is the usual approximation of Apple's continuous
// corner; 4 reads noticeably rounder, 6 noticeably boxier.
constexpr double kSquircleExponent = 5.0;

// macOS icon grid: an 824px shape on a 1024px canvas.
constexpr double kShapeRatio = 824.0 / 1024.0;

// How much of the shape a synthesised glyph occupies, leaving a margin so the
// glyph does not crowd the tile's edges.
constexpr double kGlyphRatio = 0.70;

// Supersampling for the mask edge. 4x4 = 16 samples per pixel, which is past
// the point of visible improvement at icon sizes.
constexpr int kSupersample = 4;

// How full the mark's own bounding box has to be, and how square that box has
// to be, for the mark to stand on its own without a tile behind it.
//
// A circle fills 0.785 of its box, a rounded square nearly all of it, a
// letterform or a thin logo mark around a third. The aspect test is what keeps
// a wide banner-shaped mark off the artwork path: it would fill its box
// completely and then float in the middle of the tile with empty air above and
// below it, which reads worse than a tile does.
constexpr double kArtworkFill   = 0.62;
constexpr double kArtworkAspect = 0.72;

// How far a mark may be enlarged before enlarging it stops helping.
//
// Plenty of apps ship nothing above a 48px frame, and blown up to fill a 206px
// shape on a 200% display that is a smear whatever the filter. Past this the
// mark is drawn smaller instead: a small sharp icon on a tile reads as an icon,
// a large soft one reads as a mistake. It also demotes such an icon off the
// artwork path, since artwork has to fill the shape and there is no version of
// that which is not an enlargement.
constexpr double kMaxEnlargement = 2.5;

// A stripped background is only worth reusing as the tile colour inside this
// luma range. Outside it the fill was padding rather than a brand colour: the
// shell pads with black, and a few handlers pad with white.
constexpr int kPlateMinLuma = 24;
constexpr int kPlateMaxLuma = 236;

std::vector<uint8_t> BuildMask(int size) {
    std::vector<uint8_t> mask(static_cast<size_t>(size) * static_cast<size_t>(size), 0);

    const double centre = size / 2.0;
    const double halfExtent = centre;
    const int half = (size + 1) / 2;

    // The shape is symmetric about both axes, so rasterise one quadrant and
    // mirror it. Four times less work, and it guarantees the result is exactly
    // symmetric rather than almost symmetric.
    for (int y = 0; y < half; ++y) {
        for (int x = 0; x < half; ++x) {
            int hits = 0;

            for (int sy = 0; sy < kSupersample; ++sy) {
                for (int sx = 0; sx < kSupersample; ++sx) {
                    const double px = x + (sx + 0.5) / kSupersample - centre;
                    const double py = y + (sy + 0.5) / kSupersample - centre;

                    const double v = std::pow(std::fabs(px) / halfExtent, kSquircleExponent) +
                                     std::pow(std::fabs(py) / halfExtent, kSquircleExponent);
                    if (v <= 1.0) ++hits;
                }
            }

            const auto coverage = static_cast<uint8_t>(
                (hits * 255 + (kSupersample * kSupersample) / 2) /
                (kSupersample * kSupersample));

            const int mx = size - 1 - x;
            const int my = size - 1 - y;

            mask[static_cast<size_t>(y)  * size + x]  = coverage;
            mask[static_cast<size_t>(y)  * size + mx] = coverage;
            mask[static_cast<size_t>(my) * size + x]  = coverage;
            mask[static_cast<size_t>(my) * size + mx] = coverage;
        }
    }

    return mask;
}

uint8_t ClampByte(int v) {
    return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
}

uint32_t Lighten(uint32_t colour, double amount) {
    return MakePixel(ClampByte(static_cast<int>(RedOf(colour)   + (255 - RedOf(colour))   * amount)),
                     ClampByte(static_cast<int>(GreenOf(colour) + (255 - GreenOf(colour)) * amount)),
                     ClampByte(static_cast<int>(BlueOf(colour)  + (255 - BlueOf(colour))  * amount)),
                     255);
}

uint32_t Darken(uint32_t colour, double amount) {
    return MakePixel(ClampByte(static_cast<int>(RedOf(colour)   * (1.0 - amount))),
                     ClampByte(static_cast<int>(GreenOf(colour) * (1.0 - amount))),
                     ClampByte(static_cast<int>(BlueOf(colour)  * (1.0 - amount))),
                     255);
}

int Luma(uint32_t colour) {
    // Rec. 601 weights; good enough for deciding "is this too dark to read".
    return (RedOf(colour) * 299 + GreenOf(colour) * 587 + BlueOf(colour) * 114) / 1000;
}

// Push a colour to roughly `targetLuma`, keeping its hue.
//
// Blending toward white/black rather than scaling the channels: scaling a
// saturated colour up clips channels one at a time and shifts the hue, which
// turns a blue into a cyan on its way to being light.
uint32_t AdjustToLuma(uint32_t colour, int targetLuma) {
    const int current = Luma(colour);
    if (current == targetLuma) return colour;

    if (targetLuma > current) {
        const int headroom = 255 - current;
        if (headroom <= 0) return colour;
        return Lighten(colour, static_cast<double>(targetLuma - current) / headroom);
    }
    if (current <= 0) return colour;
    return Darken(colour, 1.0 - static_cast<double>(targetLuma) / current);
}

// Mean colour of the pixels that will actually be drawn on the tile.
uint32_t MeanOpaqueColour(const Bitmap& bitmap) {
    uint64_t r = 0, g = 0, b = 0, count = 0;
    for (uint32_t pixel : bitmap.pixels) {
        if (AlphaOf(pixel) < 128) continue;
        r += RedOf(pixel); g += GreenOf(pixel); b += BlueOf(pixel);
        ++count;
    }
    if (count == 0) return MakePixel(128, 128, 128, 255);
    return MakePixel(static_cast<uint8_t>(r / count),
                     static_cast<uint8_t>(g / count),
                     static_cast<uint8_t>(b / count), 255);
}

// The icon's representative colour, used to build a tile that feels like it
// belongs to the app.
//
// Median-cut or k-means would be more principled, but a coarse 3D histogram
// picks the same colour in practice for artwork this small, and costs a
// fraction as much.
uint32_t DominantColour(const Bitmap& icon) {
    constexpr int kBins = 4;   // per channel, so 64 buckets total
    constexpr uint32_t kNeutral = 0xFF6E7681u;

    // Working at 32x32 both bounds the cost and averages away single-pixel
    // outliers such as antialiased edge fringes.
    // Not named `small`: rpcndr.h defines that as a macro for `char`.
    const Bitmap thumbnail = Resize(icon, 32, 32);
    if (thumbnail.Empty()) return kNeutral;

    struct Bucket { uint64_t r = 0, g = 0, b = 0, count = 0; };
    std::vector<Bucket> buckets(kBins * kBins * kBins);

    for (uint32_t pixel : thumbnail.pixels) {
        if (AlphaOf(pixel) < 128) continue;   // ignore transparent and near-transparent

        const int r = RedOf(pixel), g = GreenOf(pixel), b = BlueOf(pixel);

        // Skip near-white and near-black: they are almost always background or
        // outline, and produce washed-out or muddy tiles if they win.
        const int luma = Luma(pixel);
        if (luma > 240 || luma < 18) continue;

        const size_t index = static_cast<size_t>(r * kBins / 256) * kBins * kBins +
                             static_cast<size_t>(g * kBins / 256) * kBins +
                             static_cast<size_t>(b * kBins / 256);
        buckets[index].r += static_cast<uint64_t>(r);
        buckets[index].g += static_cast<uint64_t>(g);
        buckets[index].b += static_cast<uint64_t>(b);
        buckets[index].count += 1;
    }

    const auto winner = std::max_element(
        buckets.begin(), buckets.end(),
        [](const Bucket& a, const Bucket& b) { return a.count < b.count; });

    if (winner == buckets.end() || winner->count == 0)
        return kNeutral;

    uint32_t colour = MakePixel(static_cast<uint8_t>(winner->r / winner->count),
                                static_cast<uint8_t>(winner->g / winner->count),
                                static_cast<uint8_t>(winner->b / winner->count),
                                255);

    // Keep the tile in a range where a glyph of either polarity stays legible.
    const int luma = Luma(colour);
    if (luma < 70)       colour = Lighten(colour, 0.35);
    else if (luma > 200) colour = Darken(colour, 0.30);

    return colour;
}

// Multiply an image's alpha by the squircle coverage mask, clipping anything
// that extends past the shape.
void ApplyMask(Bitmap& bitmap, const std::vector<uint8_t>& mask) {
    if (mask.size() != bitmap.pixels.size()) return;

    for (size_t i = 0; i < bitmap.pixels.size(); ++i) {
        const uint32_t pixel = bitmap.pixels[i];
        const uint32_t alpha = AlphaOf(pixel) * mask[i] / 255;
        bitmap.pixels[i] = (pixel & 0x00FFFFFFu) | (alpha << 24);
    }
}

} // namespace

const std::vector<uint8_t>& SquircleMask(int size) {
    static std::unordered_map<int, std::vector<uint8_t>> cache;

    const auto found = cache.find(size);
    if (found != cache.end())
        return found->second;

    const auto [inserted, ok] = cache.emplace(size, BuildMask(size));
    (void)ok;
    return inserted->second;
}

IconPrep PrepareIcon(const Bitmap& source) {
    IconPrep prep;
    if (source.Empty()) return prep;

    // Take out anything painted behind the mark before measuring it. Left in,
    // it is opaque everywhere and the icon measures as full-bleed artwork, so
    // the tile ends up being the background with the real icon small in the
    // middle of it.
    Bitmap cleaned = source;
    prep.fill = RemoveBorderFill(cleaned);

    const Bounds bounds = OpaqueBounds(cleaned);
    prep.content = bounds.Empty() ? std::move(cleaned) : Crop(cleaned, bounds);
    if (prep.content.Empty()) return prep;

    const double longest  = (std::max)(prep.content.width, prep.content.height);
    const double shortest = (std::min)(prep.content.width, prep.content.height);
    const double aspect   = (longest > 0.0) ? shortest / longest : 0.0;

    prep.analysis.artwork = OpaqueCoverage(prep.content) >= kArtworkFill &&
                            aspect >= kArtworkAspect;

    // The tint is worked out either way. MakeIconTile can still demote a mark
    // off the artwork path once it knows the tile size, and it has no way to
    // derive these itself.
    //
    // A background we stripped is the app's own colour and is exactly what
    // Windows shows behind the same mark, so it makes a better tile than
    // anything we could derive from the mark. Padding is not, and is filtered
    // out by luma.
    const bool usePlate = prep.fill.found &&
                          Luma(prep.fill.colour) >= kPlateMinLuma &&
                          Luma(prep.fill.colour) <= kPlateMaxLuma;

    uint32_t base = usePlate ? prep.fill.colour : DominantColour(prep.content);

    // Derived from the glyph's own colours, the tile lands at the same luma as
    // the glyph and the glyph disappears into it. Drive it to the opposite side
    // of the glyph's brightness, so a light mark gets a dark tile and vice
    // versa, which is also what macOS-style icons do.
    const int glyphLuma = Luma(MeanOpaqueColour(prep.content));
    constexpr int kMinLumaSeparation = 70;

    const int target = (glyphLuma > 140) ? 52 : 196;
    if (std::abs(Luma(base) - glyphLuma) < kMinLumaSeparation)
        base = AdjustToLuma(base, target);

    prep.analysis.tintTop    = Lighten(base, 0.12);
    prep.analysis.tintBottom = Darken(base, 0.12);

    return prep;
}

Bitmap MakeIconTile(const Bitmap& source, int size) {
    if (source.Empty() || size <= 0) return {};

    const int shapeSize = (std::max)(1, static_cast<int>(std::lround(size * kShapeRatio)));
    const std::vector<uint8_t>& mask = SquircleMask(shapeSize);

    const IconPrep prep = PrepareIcon(source);
    const Bitmap& content = prep.content;
    if (content.Empty()) return {};

    const int longest = (std::max)(content.width, content.height);

    // Filling the shape with a mark this small would be an enlargement past the
    // point where it buys anything, so put it on a tile at its own scale.
    const bool artwork = prep.analysis.artwork &&
                         longest * kMaxEnlargement >= shapeSize;

    Bitmap shape;
    if (artwork) {
        // Fit the artwork to the macOS shape size and clip it. Icons already
        // inside the squircle (circles, for instance) come through untouched.
        shape = FitInto(content, shapeSize, shapeSize);
        ApplyMask(shape, mask);
    } else {
        // Build a tile from the icon's own colours, then sit the glyph on it.
        const IconAnalysis& analysis = prep.analysis;

        shape = Bitmap::Create(shapeSize, shapeSize);
        for (int y = 0; y < shapeSize; ++y) {
            const double t = (shapeSize > 1) ? static_cast<double>(y) / (shapeSize - 1) : 0.0;
            const uint8_t r = static_cast<uint8_t>(
                RedOf(analysis.tintTop)   + (RedOf(analysis.tintBottom)   - static_cast<int>(RedOf(analysis.tintTop)))   * t);
            const uint8_t g = static_cast<uint8_t>(
                GreenOf(analysis.tintTop) + (GreenOf(analysis.tintBottom) - static_cast<int>(GreenOf(analysis.tintTop))) * t);
            const uint8_t b = static_cast<uint8_t>(
                BlueOf(analysis.tintTop)  + (BlueOf(analysis.tintBottom)  - static_cast<int>(BlueOf(analysis.tintTop)))  * t);

            const uint32_t row = MakePixel(r, g, b, 255);
            for (int x = 0; x < shapeSize; ++x)
                shape.At(x, y) = row;
        }
        ApplyMask(shape, mask);

        int glyphSize = (std::max)(1, static_cast<int>(std::lround(shapeSize * kGlyphRatio)));

        // Same cap as above: a 32px mark drawn at 144px is mush, and a mark
        // that is merely smaller than its neighbours is not.
        glyphSize = (std::min)(glyphSize,
                              (std::max)(1, static_cast<int>(std::lround(longest * kMaxEnlargement))));

        const Bitmap glyph = FitInto(content, glyphSize, glyphSize);
        const int glyphOffset = (shapeSize - glyphSize) / 2;
        CompositeOver(shape, glyph, glyphOffset, glyphOffset);
    }

    // Centre the shape on the full tile, leaving transparent padding.
    Bitmap tile = Bitmap::Create(size, size);
    const int offset = (size - shapeSize) / 2;
    CompositeOver(tile, shape, offset, offset);

    return tile;
}

} // namespace mactab
