#include "image.h"

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

} // namespace

Bitmap Resize(const Bitmap& source, int width, int height) {
    if (source.Empty() || width <= 0 || height <= 0)
        return {};

    if (source.width == width && source.height == height)
        return source;

    const std::vector<PremultipliedF> src = ToPremultipliedFloat(source);
    Bitmap out = Bitmap::Create(width, height);

    const double scaleX = static_cast<double>(source.width)  / width;
    const double scaleY = static_cast<double>(source.height) / height;

    const bool downscaling = (width < source.width) || (height < source.height);

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            PremultipliedF acc;

            if (downscaling) {
                // Average every source pixel falling inside this destination
                // pixel's footprint. Bilinear would point-sample and alias.
                const int x0 = static_cast<int>(x * scaleX);
                const int y0 = static_cast<int>(y * scaleY);
                int x1 = static_cast<int>((x + 1) * scaleX);
                int y1 = static_cast<int>((y + 1) * scaleY);
                x1 = (std::max)(x1, x0 + 1);
                y1 = (std::max)(y1, y0 + 1);
                x1 = (std::min)(x1, source.width);
                y1 = (std::min)(y1, source.height);

                float count = 0;
                for (int sy = y0; sy < y1; ++sy) {
                    for (int sx = x0; sx < x1; ++sx) {
                        const PremultipliedF& p =
                            src[static_cast<size_t>(sy) * source.width + sx];
                        acc.r += p.r; acc.g += p.g; acc.b += p.b; acc.a += p.a;
                        count += 1.0f;
                    }
                }
                if (count > 0) {
                    acc.r /= count; acc.g /= count; acc.b /= count; acc.a /= count;
                }
            } else {
                // Bilinear, sampling at pixel centres.
                const double fx = (x + 0.5) * scaleX - 0.5;
                const double fy = (y + 0.5) * scaleY - 0.5;

                int x0 = static_cast<int>(std::floor(fx));
                int y0 = static_cast<int>(std::floor(fy));
                const float tx = static_cast<float>(fx - x0);
                const float ty = static_cast<float>(fy - y0);

                const int x1 = (std::min)(x0 + 1, source.width  - 1);
                const int y1 = (std::min)(y0 + 1, source.height - 1);
                x0 = (std::max)(x0, 0);
                y0 = (std::max)(y0, 0);

                const PremultipliedF& p00 = src[static_cast<size_t>(y0) * source.width + x0];
                const PremultipliedF& p10 = src[static_cast<size_t>(y0) * source.width + x1];
                const PremultipliedF& p01 = src[static_cast<size_t>(y1) * source.width + x0];
                const PremultipliedF& p11 = src[static_cast<size_t>(y1) * source.width + x1];

                auto mix = [&](float a, float b, float c, float d) {
                    return (a * (1 - tx) + b * tx) * (1 - ty) +
                           (c * (1 - tx) + d * tx) * ty;
                };
                acc.r = mix(p00.r, p10.r, p01.r, p11.r);
                acc.g = mix(p00.g, p10.g, p01.g, p11.g);
                acc.b = mix(p00.b, p10.b, p01.b, p11.b);
                acc.a = mix(p00.a, p10.a, p01.a, p11.a);
            }

            out.At(x, y) = FromPremultiplied(acc);
        }
    }

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

double OpaqueCoverage(const Bitmap& bitmap, uint8_t threshold) {
    if (bitmap.Empty()) return 0.0;

    size_t opaque = 0;
    for (uint32_t pixel : bitmap.pixels) {
        if (AlphaOf(pixel) > threshold) ++opaque;
    }
    return static_cast<double>(opaque) / static_cast<double>(bitmap.pixels.size());
}

} // namespace mactab
