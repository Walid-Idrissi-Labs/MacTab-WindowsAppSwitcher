#include "image.h"

#include <unordered_map>

namespace mactab {
namespace {

struct PremultipliedF {
    float r = 0, g = 0, b = 0, a = 0;
};

std::vector<PremultipliedF> ToPremultipliedFloat(const Bitmap& source) {
    std::vector<PremultipliedF> out(source.pixels.size());
    for (size_t i = 0; i < source.pixels.size(); ++i) {
        const uint32_t p = source.pixels[i];
        const float a = AlphaOf(p) / 255.0f;
        out[i].r = (RedOf(p)   / 255.0f) * a;
        out[i].g = (GreenOf(p) / 255.0f) * a;
        out[i].b = (BlueOf(p)  / 255.0f) * a;
        out[i].a = a;
    }
    return out;
}

uint8_t ToByte(float v) {
    const float scaled = v * 255.0f + 0.5f;
    if (scaled <= 0.0f)   return 0;
    if (scaled >= 255.0f) return 255;
    return static_cast<uint8_t>(scaled);
}

uint32_t FromPremultiplied(const PremultipliedF& p) {
    if (p.a <= 0.0001f)
        return 0;   // fully transparent; colour is meaningless

    // Unpremultiply, clamping so rounding cannot push a channel above alpha.
    const float inv = 1.0f / p.a;
    return MakePixel(ToByte((std::min)(p.r * inv, 1.0f)),
                     ToByte((std::min)(p.g * inv, 1.0f)),
                     ToByte((std::min)(p.b * inv, 1.0f)),
                     ToByte(p.a));
}

int Clamp(int v, int low, int high) {
    return v < low ? low : (v > high ? high : v);
}

// Catmull-Rom, the Mitchell-Netravali family at B = 0, C = 0.5. Interpolating,
// so an unscaled pixel comes back exactly, with a small negative lobe either
// side of the peak that keeps edges from washing out.
float CatmullRom(float t) {
    t = std::fabs(t);
    if (t < 1.0f) return ((1.5f * t - 2.5f) * t) * t + 1.0f;
    if (t < 2.0f) return ((-0.5f * t + 2.5f) * t - 4.0f) * t + 2.0f;
    return 0.0f;
}

// Largest per-channel difference between two colours, alpha ignored.
int ColourDistance(uint32_t a, uint32_t b) {
    const int dr = std::abs(static_cast<int>(RedOf(a))   - static_cast<int>(RedOf(b)));
    const int dg = std::abs(static_cast<int>(GreenOf(a)) - static_cast<int>(GreenOf(b)));
    const int db = std::abs(static_cast<int>(BlueOf(a))  - static_cast<int>(BlueOf(b)));
    return (std::max)(dr, (std::max)(dg, db));
}

} // namespace

Bitmap Resize(const Bitmap& source, int width, int height) {
    if (source.Empty() || width <= 0 || height <= 0)
        return {};

    if (source.width == width && source.height == height)
        return source;

    // Two separable passes rather than one two-dimensional filter, because the
    // two axes do not always want the same filter. Cropping to a mark's bounding
    // box produces plenty of non-square images, and any resize of one to a
    // square is shrinking on one axis while growing on the other. A single
    // filter choice for both then runs a box average over an axis that is
    // growing, where the footprint of a destination pixel is less than one
    // source pixel and the average degenerates to point sampling.
    //
    // Both filters are separable, so for the ordinary case where both axes go
    // the same way this produces the identical result for less work: 4 + 4 taps
    // per pixel instead of 16.
    std::vector<PremultipliedF> rows = ToPremultipliedFloat(source);

    auto pass = [](const std::vector<PremultipliedF>& in, int inLength,
                   int outLength, int lines, bool horizontal) {
        std::vector<PremultipliedF> out(static_cast<size_t>(outLength) * lines);

        const double scale = static_cast<double>(inLength) / outLength;
        const bool shrinking = outLength < inLength;

        // Row-major either way: `line` walks the axis being left alone.
        const size_t inStride  = horizontal ? static_cast<size_t>(inLength)  : 1;
        const size_t inStep    = horizontal ? 1 : static_cast<size_t>(lines);
        const size_t outStride = horizontal ? static_cast<size_t>(outLength) : 1;
        const size_t outStep   = horizontal ? 1 : static_cast<size_t>(lines);

        for (int line = 0; line < lines; ++line) {
            const PremultipliedF* src = in.data() + static_cast<size_t>(line) * inStride;
            PremultipliedF* dst = out.data() + static_cast<size_t>(line) * outStride;

            for (int i = 0; i < outLength; ++i) {
                PremultipliedF acc;

                if (shrinking) {
                    // Average every source pixel falling inside this destination
                    // pixel's footprint. Bilinear would point-sample and alias.
                    const int from = static_cast<int>(i * scale);
                    int to = static_cast<int>((i + 1) * scale);
                    to = (std::min)((std::max)(to, from + 1), inLength);

                    float count = 0;
                    for (int s = from; s < to; ++s) {
                        const PremultipliedF& p = src[static_cast<size_t>(s) * inStep];
                        acc.r += p.r; acc.g += p.g; acc.b += p.b; acc.a += p.a;
                        count += 1.0f;
                    }
                    if (count > 0) {
                        acc.r /= count; acc.g /= count; acc.b /= count; acc.a /= count;
                    }
                } else {
                    // Catmull-Rom, sampling at pixel centres.
                    const double centre = (i + 0.5) * scale - 0.5;
                    const int base = static_cast<int>(std::floor(centre));

                    for (int tap = -1; tap <= 2; ++tap) {
                        const float w = CatmullRom(static_cast<float>(centre - (base + tap)));
                        if (w == 0.0f) continue;

                        const int s = Clamp(base + tap, 0, inLength - 1);
                        const PremultipliedF& p = src[static_cast<size_t>(s) * inStep];
                        acc.r += p.r * w; acc.g += p.g * w;
                        acc.b += p.b * w; acc.a += p.a * w;
                    }

                    // The negative lobe can push alpha past either end. Left
                    // alone, an alpha above 1 divides back out to a colour
                    // darker than the source, which shows up as a dark rim on a
                    // light mark.
                    acc.a = (std::min)((std::max)(acc.a, 0.0f), 1.0f);
                }

                dst[static_cast<size_t>(i) * outStep] = acc;
            }
        }

        return out;
    };

    if (width != source.width)
        rows = pass(rows, source.width, width, source.height, true);
    if (height != source.height)
        rows = pass(rows, source.height, height, width, false);

    Bitmap out = Bitmap::Create(width, height);
    for (size_t i = 0; i < out.pixels.size(); ++i)
        out.pixels[i] = FromPremultiplied(rows[i]);

    return out;
}

void PremultiplyInPlace(Bitmap& bitmap) {
    for (uint32_t& pixel : bitmap.pixels) {
        const uint32_t a = AlphaOf(pixel);
        if (a == 255) continue;
        if (a == 0) { pixel = 0; continue; }

        // +127 rounds to nearest rather than truncating, which otherwise
        // darkens semi-transparent edges by up to one level per channel.
        const uint32_t r = (RedOf(pixel)   * a + 127) / 255;
        const uint32_t g = (GreenOf(pixel) * a + 127) / 255;
        const uint32_t b = (BlueOf(pixel)  * a + 127) / 255;
        pixel = MakePixel(static_cast<uint8_t>(r), static_cast<uint8_t>(g),
                          static_cast<uint8_t>(b), static_cast<uint8_t>(a));
    }
}

void CompositeOver(Bitmap& dst, const Bitmap& src, int offsetX, int offsetY) {
    for (int y = 0; y < src.height; ++y) {
        const int dy = y + offsetY;
        if (dy < 0 || dy >= dst.height) continue;

        for (int x = 0; x < src.width; ++x) {
            const int dx = x + offsetX;
            if (dx < 0 || dx >= dst.width) continue;

            const uint32_t s  = src.At(x, y);
            const uint32_t sa = AlphaOf(s);
            if (sa == 0) continue;

            uint32_t& d = dst.At(dx, dy);
            if (sa == 255) { d = s; continue; }

            const uint32_t da   = AlphaOf(d);
            const uint32_t outA = sa + da * (255 - sa) / 255;
            if (outA == 0) { d = 0; continue; }

            auto blend = [&](uint32_t sc, uint32_t dc) {
                // Composite in premultiplied space, then divide back out.
                const uint32_t num = sc * sa + dc * da * (255 - sa) / 255;
                return static_cast<uint8_t>(num / outA);
            };

            d = MakePixel(blend(RedOf(s),   RedOf(d)),
                          blend(GreenOf(s), GreenOf(d)),
                          blend(BlueOf(s),  BlueOf(d)),
                          static_cast<uint8_t>(outA));
        }
    }
}

Bounds OpaqueBounds(const Bitmap& bitmap, uint8_t threshold) {
    Bounds bounds;
    if (bitmap.Empty()) return bounds;

    int left = bitmap.width, top = bitmap.height, right = -1, bottom = -1;

    for (int y = 0; y < bitmap.height; ++y) {
        for (int x = 0; x < bitmap.width; ++x) {
            if (AlphaOf(bitmap.At(x, y)) <= threshold) continue;
            if (x < left)   left = x;
            if (x > right)  right = x;
            if (y < top)    top = y;
            if (y > bottom) bottom = y;
        }
    }

    if (right < 0) return bounds;   // nothing opaque at all

    bounds.left   = left;
    bounds.top    = top;
    bounds.right  = right + 1;
    bounds.bottom = bottom + 1;
    return bounds;
}

uint32_t MeanColourIn(const Bitmap& bitmap, int left, int top, int right, int bottom) {
    if (bitmap.Empty()) return MakePixel(128, 128, 128, 255);

    left   = (std::max)(0, left);
    top    = (std::max)(0, top);
    right  = (std::min)(bitmap.width,  right);
    bottom = (std::min)(bitmap.height, bottom);
    if (right <= left || bottom <= top) return MakePixel(128, 128, 128, 255);

    // Step rather than visit every pixel. This is a strip a few hundred pixels
    // wide sampled to pick one of two text colours; a sixteenth of it decides
    // the same way, and this runs on the gesture path.
    const int step = (std::max)(1, (right - left) / 64);

    uint64_t r = 0, g = 0, b = 0, n = 0;
    for (int y = top; y < bottom; y += step) {
        for (int x = left; x < right; x += step) {
            const uint32_t px = bitmap.At(x, y);
            r += RedOf(px); g += GreenOf(px); b += BlueOf(px); ++n;
        }
    }
    if (n == 0) return MakePixel(128, 128, 128, 255);

    return MakePixel(static_cast<uint8_t>(r / n), static_cast<uint8_t>(g / n),
                     static_cast<uint8_t>(b / n), 255);
}

Bitmap Crop(const Bitmap& source, const Bounds& bounds) {
    if (source.Empty() || bounds.Empty()) return {};

    const int left   = (std::max)(0, bounds.left);
    const int top    = (std::max)(0, bounds.top);
    const int right  = (std::min)(source.width,  bounds.right);
    const int bottom = (std::min)(source.height, bounds.bottom);
    if (right <= left || bottom <= top) return {};

    Bitmap out = Bitmap::Create(right - left, bottom - top);
    for (int y = 0; y < out.height; ++y) {
        for (int x = 0; x < out.width; ++x)
            out.At(x, y) = source.At(left + x, top + y);
    }
    return out;
}

Bitmap FitInto(const Bitmap& source, int boxWidth, int boxHeight) {
    if (source.Empty() || boxWidth <= 0 || boxHeight <= 0) return {};

    // Scale by whichever axis is the tighter constraint.
    const double scale = (std::min)(static_cast<double>(boxWidth)  / source.width,
                                    static_cast<double>(boxHeight) / source.height);

    const int scaledWidth  = (std::max)(1, static_cast<int>(std::lround(source.width  * scale)));
    const int scaledHeight = (std::max)(1, static_cast<int>(std::lround(source.height * scale)));

    const Bitmap scaled = Resize(source, scaledWidth, scaledHeight);

    Bitmap out = Bitmap::Create(boxWidth, boxHeight);
    CompositeOver(out, scaled, (boxWidth - scaledWidth) / 2, (boxHeight - scaledHeight) / 2);
    return out;
}

BorderFill RemoveBorderFill(Bitmap& bitmap) {
    BorderFill result;

    // Below this there is not enough border to judge anything from, and the
    // icon is too small for a plate to be the problem anyway.
    if (bitmap.width < 16 || bitmap.height < 16) return result;

    // Two pixels are the same flat colour within this. Nominally-flat fills
    // are not always exactly flat: a plate that has been through a resize, or
    // a gradient shallow enough to read as flat, both land a few levels apart.
    constexpr int kFlat = 10;

    // ...and this far out they are unrelated. Between the two, alpha ramps,
    // which is what keeps an antialiased mark from coming out with a hard
    // stair-stepped edge where it met the background.
    constexpr int kSoft = 48;

    constexpr double kMinRingAgreement = 0.90;   // of the border, one colour
    constexpr double kMaxRingAlphaHoles = 0.10;  // above this it already has alpha
    constexpr double kMinRemoved = 0.06;         // below this there was no plate

    // A mark can legitimately be a very small part of a very padded canvas, so
    // there is no upper bound on how much may go. What there is a bound on is
    // how little may be left: an icon that is one flat colour end to end floods
    // completely, and an empty tile is worse than the flat one it replaced.
    constexpr double kMinSurviving = 0.001;      // 8x8 out of 256x256

    const int w = bitmap.width, h = bitmap.height;

    // --- the border ring ----------------------------------------------------
    std::vector<int> ring;
    ring.reserve(static_cast<size_t>(2 * (w + h)));
    for (int x = 0; x < w; ++x) {
        ring.push_back(x);
        ring.push_back((h - 1) * w + x);
    }
    for (int y = 1; y < h - 1; ++y) {
        ring.push_back(y * w);
        ring.push_back(y * w + w - 1);
    }

    size_t holes = 0;
    std::unordered_map<uint32_t, size_t> tally;
    for (int index : ring) {
        const uint32_t pixel = bitmap.pixels[static_cast<size_t>(index)];
        if (AlphaOf(pixel) < 128) { ++holes; continue; }
        ++tally[pixel & 0x00FFFFFFu];
    }

    // Already transparent at the edges, so the alpha channel is doing its job
    // and there is nothing baked in to take out.
    if (static_cast<double>(holes) / ring.size() > kMaxRingAlphaHoles)
        return result;
    if (tally.empty()) return result;

    const auto modal = std::max_element(
        tally.begin(), tally.end(),
        [](const auto& a, const auto& b) { return a.second < b.second; });

    const uint32_t key = modal->first | 0xFF000000u;

    size_t agreeing = 0;
    for (int index : ring) {
        const uint32_t pixel = bitmap.pixels[static_cast<size_t>(index)];
        if (AlphaOf(pixel) >= 128 && ColourDistance(pixel, key) <= kFlat)
            ++agreeing;
    }
    if (static_cast<double>(agreeing) / ring.size() < kMinRingAgreement)
        return result;

    // --- flood inward -------------------------------------------------------
    //
    // Alpha is written to a side buffer so the whole thing can be abandoned if
    // the result turns out not to be a background after all.
    std::vector<uint8_t> alpha(bitmap.pixels.size(), 0xFF);
    std::vector<uint8_t> seen(bitmap.pixels.size(), 0);
    std::vector<int> stack;
    stack.reserve(bitmap.pixels.size() / 4);

    auto consider = [&](int index) {
        if (seen[static_cast<size_t>(index)]) return;

        const uint32_t pixel = bitmap.pixels[static_cast<size_t>(index)];
        if (AlphaOf(pixel) < 128) {
            // Transparent already: nothing to remove, and nothing to spread
            // through either. Letting the fill cross transparency would break
            // the one argument this whole approach rests on, that a background
            // is what reaches the border. It would jump a transparent gap into
            // an enclosed region of the same colour and eat artwork that is not
            // connected to the border at all.
            seen[static_cast<size_t>(index)] = 1;
            alpha[static_cast<size_t>(index)] = AlphaOf(pixel);
            return;
        }

        const int distance = ColourDistance(pixel, key);
        if (distance >= kSoft) return;   // artwork, stop here

        const double ramp = (distance <= kFlat)
            ? 0.0
            : static_cast<double>(distance - kFlat) / (kSoft - kFlat);

        seen[static_cast<size_t>(index)] = 1;
        alpha[static_cast<size_t>(index)] =
            static_cast<uint8_t>(AlphaOf(pixel) * ramp + 0.5);
        stack.push_back(index);
    };

    for (int index : ring) consider(index);

    double removedWeight = 0.0;
    while (!stack.empty()) {
        const int index = stack.back();
        stack.pop_back();

        const uint32_t original = bitmap.pixels[static_cast<size_t>(index)];
        if (AlphaOf(original) >= 128)
            removedWeight += 1.0 - alpha[static_cast<size_t>(index)] / 255.0;

        const int x = index % w, y = index / w;
        if (x > 0)     consider(index - 1);
        if (x < w - 1) consider(index + 1);
        if (y > 0)     consider(index - w);
        if (y < h - 1) consider(index + w);
    }

    const double total = static_cast<double>(bitmap.pixels.size());
    const double removed = removedWeight / total;
    if (removed < kMinRemoved) return result;

    size_t surviving = 0;
    for (size_t i = 0; i < bitmap.pixels.size(); ++i) {
        const uint8_t left = seen[i] ? alpha[i] : AlphaOf(bitmap.pixels[i]);
        if (left >= 128) ++surviving;
    }
    if (static_cast<double>(surviving) / total < kMinSurviving)
        return result;

    for (size_t i = 0; i < bitmap.pixels.size(); ++i) {
        if (!seen[i]) continue;
        bitmap.pixels[i] = (bitmap.pixels[i] & 0x00FFFFFFu) |
                           (static_cast<uint32_t>(alpha[i]) << 24);
    }

    result.found   = true;
    result.colour  = key;
    result.removed = removed;
    return result;
}

double OpaqueCoverage(const Bitmap& bitmap, uint8_t threshold) {
    if (bitmap.Empty()) return 0.0;

    size_t opaque = 0;
    for (uint32_t pixel : bitmap.pixels) {
        if (AlphaOf(pixel) > threshold) ++opaque;
    }
    return static_cast<double>(opaque) / static_cast<double>(bitmap.pixels.size());
}

} // namespace mactab
