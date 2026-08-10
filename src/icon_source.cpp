#include "pch.h"
#include "icon_source.h"
#include "diag.h"

namespace mactab {

Bitmap FromHBitmap(HBITMAP source) {
    if (!source) return {};

    BITMAP info{};
    if (::GetObjectW(source, sizeof(info), &info) == 0)
        return {};
    if (info.bmWidth <= 0 || info.bmHeight <= 0)
        return {};

    const HDC screen = ::GetDC(nullptr);
    if (!screen) return {};

    BITMAPINFO request{};
    request.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    request.bmiHeader.biWidth       = info.bmWidth;
    request.bmiHeader.biHeight      = -info.bmHeight;   // negative = top-down
    request.bmiHeader.biPlanes      = 1;
    request.bmiHeader.biBitCount    = 32;
    request.bmiHeader.biCompression = BI_RGB;

    Bitmap out = Bitmap::Create(info.bmWidth, info.bmHeight);
    const int copied = ::GetDIBits(screen, source, 0, static_cast<UINT>(info.bmHeight),
                                   out.pixels.data(), &request, DIB_RGB_COLORS);
    ::ReleaseDC(nullptr, screen);

    if (copied == 0) {
        MACTAB_WARN("icon_source: GetDIBits failed");
        return {};
    }

    // Shell icons regularly come back 32-bit with every alpha byte zero, which
    // read literally is a fully invisible icon. If nothing is opaque anywhere,
    // treat the image as having no alpha channel rather than as empty.
    bool anyAlpha = false;
    for (uint32_t pixel : out.pixels) {
        if (AlphaOf(pixel) != 0) { anyAlpha = true; break; }
    }
    if (!anyAlpha) {
        for (uint32_t& pixel : out.pixels)
            pixel |= 0xFF000000u;
    }

    return out;
}

Bitmap FromHIcon(HICON icon) {
    if (!icon) return {};

    ICONINFO iconInfo{};
    if (!::GetIconInfo(icon, &iconInfo))
        return {};

    // GetIconInfo hands back bitmaps we own and must free on every path.
    struct Cleanup {
        HBITMAP colour, mask;
        ~Cleanup() {
            if (colour) ::DeleteObject(colour);
            if (mask)   ::DeleteObject(mask);
        }
    } cleanup{ iconInfo.hbmColor, iconInfo.hbmMask };

    Bitmap out = FromHBitmap(iconInfo.hbmColor);
    if (out.Empty())
        return {};

    // A 1-bit icon has no colour alpha; transparency lives in the AND mask,
    // where a set bit means "transparent".
    const Bitmap maskBitmap = FromHBitmap(iconInfo.hbmMask);
    if (!maskBitmap.Empty() &&
        maskBitmap.width == out.width && maskBitmap.height == out.height) {

        bool anyPartialAlpha = false;
        for (uint32_t pixel : out.pixels) {
            const uint8_t a = AlphaOf(pixel);
            if (a != 0 && a != 255) { anyPartialAlpha = true; break; }
        }

        // Only fall back to the mask when the colour bitmap has no real alpha
        // gradient of its own, otherwise we would throw away good data.
        if (!anyPartialAlpha) {
            for (int y = 0; y < out.height; ++y) {
                for (int x = 0; x < out.width; ++x) {
                    const bool transparent = (BlueOf(maskBitmap.At(x, y)) != 0);
                    uint32_t& pixel = out.At(x, y);
                    pixel = transparent ? (pixel & 0x00FFFFFFu) : (pixel | 0xFF000000u);
                }
            }
        }
    }

    return out;
}

} // namespace mactab
