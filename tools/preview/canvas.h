#pragma once

// The drawing floor shared by every preview harness.
//
// There is more than one thing in MacTab that can only be judged by looking at
// it, and every one of them needs the same three things: a way to put pixels in
// a buffer, a way to put readable words in it, and a way to write it out as a
// PNG. This is that, and it lives here rather than in one harness so the second
// harness does not arrive with its own slightly different copy.
//
// Native only. Nothing here touches windows.h, which is the whole point.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <zlib.h>

#include "image.h"

namespace mactab::preview {

// --- minimal PNG writer ----------------------------------------------------

inline void AppendBigEndian32(std::vector<unsigned char>& out, uint32_t value) {
    out.push_back(static_cast<unsigned char>((value >> 24) & 0xFF));
    out.push_back(static_cast<unsigned char>((value >> 16) & 0xFF));
    out.push_back(static_cast<unsigned char>((value >>  8) & 0xFF));
    out.push_back(static_cast<unsigned char>( value        & 0xFF));
}

inline void AppendChunk(std::vector<unsigned char>& out, const char tag[4],
                        const std::vector<unsigned char>& data) {
    AppendBigEndian32(out, static_cast<uint32_t>(data.size()));

    std::vector<unsigned char> tagged(tag, tag + 4);
    tagged.insert(tagged.end(), data.begin(), data.end());

    out.insert(out.end(), tagged.begin(), tagged.end());
    AppendBigEndian32(out, static_cast<uint32_t>(
        crc32(0, tagged.data(), static_cast<uInt>(tagged.size()))));
}

inline bool WritePng(const std::string& path, const Bitmap& bitmap) {
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

// --- primitives -------------------------------------------------------------

inline void FillRect(Bitmap& b, int x0, int y0, int w, int h, uint32_t colour) {
    for (int y = (std::max)(0, y0); y < (std::min)(b.height, y0 + h); ++y)
        for (int x = (std::max)(0, x0); x < (std::min)(b.width, x0 + w); ++x)
            b.At(x, y) = colour;
}

inline void FillDisc(Bitmap& b, float cx, float cy, float r, uint32_t colour) {
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
inline const char* GlyphRows(char c) {
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
        case 'J': return "00111""00010""00010""00010""00010""10010""01100";
        case 'K': return "10001""10010""11100""10010""10001""10001""10001";
        case 'L': return "10000""10000""10000""10000""10000""10000""11111";
        case 'M': return "10001""11011""10101""10001""10001""10001""10001";
        case 'N': return "10001""11001""10101""10011""10001""10001""10001";
        case 'O': return "01110""10001""10001""10001""10001""10001""01110";
        case 'P': return "11110""10001""11110""10000""10000""10000""10000";
        case 'Q': return "01110""10001""10001""10001""10101""10010""01101";
        case 'R': return "11110""10001""11110""10100""10010""10001""10001";
        case 'S': return "01111""10000""01110""00001""00001""10001""01110";
        case 'T': return "11111""00100""00100""00100""00100""00100""00100";
        case 'U': return "10001""10001""10001""10001""10001""10001""01110";
        case 'V': return "10001""10001""10001""10001""01010""01010""00100";
        case 'W': return "10001""10001""10001""10101""10101""11011""10001";
        case 'X': return "10001""01010""00100""00100""00100""01010""10001";
        case 'Y': return "10001""01010""00100""00100""00100""00100""00100";
        case 'Z': return "11111""00001""00010""00100""01000""10000""11111";
        case '0': return "01110""10011""10101""10101""10101""11001""01110";
        case '1': return "00100""01100""00100""00100""00100""00100""01110";
        case '2': return "01110""10001""00001""00110""01000""10000""11111";
        case '3': return "11110""00001""00001""01110""00001""00001""11110";
        case '4': return "00010""00110""01010""10010""11111""00010""00010";
        case '5': return "11111""10000""11110""00001""00001""10001""01110";
        case '6': return "01110""10000""11110""10001""10001""10001""01110";
        case '7': return "11111""00001""00010""00100""01000""01000""01000";
        case '8': return "01110""10001""01110""10001""10001""10001""01110";
        case '9': return "01110""10001""10001""01111""00001""00001""01110";
        case '.': return "00000""00000""00000""00000""00000""01100""01100";
        case '-': return "00000""00000""00000""11111""00000""00000""00000";
        default:  return "00000""00000""00000""00000""00000""00000""00000";
    }
}

// `scale` is the size of one font pixel, so a scale of 8 gives 56px capitals.
inline void DrawWord(Bitmap& b, const char* text, int x0, int y0, int scale,
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

inline int WordWidth(const char* text, int scale) {
    int n = 0;
    for (const char* c = text; *c; ++c) ++n;
    return n > 0 ? (n * 6 - 1) * scale : 0;
}

// Nearest-neighbour crop and magnify. Fine detail is a smudge at 1:1, and this
// is what makes it something that can be judged by eye rather than only
// asserted on.
inline Bitmap Zoom(const Bitmap& source, int x0, int y0, int w, int h, int factor) {
    Bitmap out = Bitmap::Create(w * factor, h * factor);
    for (int y = 0; y < out.height; ++y) {
        for (int x = 0; x < out.width; ++x) {
            const int sx = (std::min)(source.width  - 1, x0 + x / factor);
            const int sy = (std::min)(source.height - 1, y0 + y / factor);
            out.At(x, y) = source.At((std::max)(0, sx), (std::max)(0, sy));
        }
    }
    return out;
}

} // namespace mactab::preview
