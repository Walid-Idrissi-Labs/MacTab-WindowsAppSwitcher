#include "pch.h"
#include <wincodec.h>

#include "icon_source.h"
#include "com.h"
#include "common.h"
#include "diag.h"

namespace mactab {

Bitmap FromHBitmap(HBITMAP source, bool* alphaMissing) {
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
    if (alphaMissing) *alphaMissing = !anyAlpha;

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

namespace {

// The RT_GROUP_ICON payload: a directory of every frame the icon holds, each
// pointing at the RT_ICON resource with the pixels. Not in any SDK header, and
// packed to 2 because it is a file format that predates alignment mattering.
#pragma pack(push, 2)
struct GroupIconEntry {
    BYTE  width;        // 0 means 256
    BYTE  height;       // 0 means 256
    BYTE  colours;
    BYTE  reserved;
    WORD  planes;
    WORD  bitCount;
    DWORD bytesInRes;
    WORD  id;           // the RT_ICON resource holding the pixels
};
struct GroupIconDir {
    WORD reserved;
    WORD type;          // 1 for icons
    WORD count;
    // GroupIconEntry[count] follows
};
#pragma pack(pop)

int FrameSize(BYTE stored) { return stored == 0 ? 256 : stored; }

bool LooksLikePng(const BYTE* data, DWORD bytes) {
    static const BYTE kSignature[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    return bytes >= sizeof(kSignature) && std::memcmp(data, kSignature, sizeof(kSignature)) == 0;
}

// A resource name, which is either a string or a small integer wearing a
// pointer's clothes.
struct ResourceName {
    bool         found = false;
    bool         integral = false;
    UINT_PTR     id = 0;
    std::wstring text;

    LPCWSTR Get() const {
        return integral ? MAKEINTRESOURCEW(id) : text.c_str();
    }
};

// The first icon group in the module, which is the one the shell draws for
// index 0 and therefore the one the taskbar shows. Resource directories are
// stored sorted, so "first enumerated" and "lowest id" are the same thing.
BOOL CALLBACK FirstGroupProc(HMODULE, LPCWSTR, LPWSTR name, LONG_PTR param) {
    auto& out = *reinterpret_cast<ResourceName*>(param);
    out.found = true;

    if (IS_INTRESOURCE(name)) {
        out.integral = true;
        out.id = reinterpret_cast<UINT_PTR>(name);
    } else {
        out.text = name;
    }

    return FALSE;   // stop at the first
}

Bitmap DecodeThroughWic(IWICBitmapDecoder* decoder, IWICImagingFactory* factory) {
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, frame.Put())))
        return {};

    // Straight-alpha BGRA to match our Bitmap contract; the pipeline
    // premultiplies only at upload.
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(converter.Put())) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom)))
        return {};

    UINT width = 0, height = 0;
    if (FAILED(converter->GetSize(&width, &height)) || width == 0 || height == 0)
        return {};
    if (width > 2048 || height > 2048)
        return {};

    Bitmap out = Bitmap::Create(static_cast<int>(width), static_cast<int>(height));
    const UINT stride = width * 4;
    if (FAILED(converter->CopyPixels(nullptr, stride, stride * height,
                                     reinterpret_cast<BYTE*>(out.pixels.data()))))
        return {};

    return out;
}

} // namespace

Bitmap DecodeImageFile(const std::wstring& path) {
    ComApartment apartment(COINIT_APARTMENTTHREADED);

    ComPtr<IWICImagingFactory> factory;
    if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(factory.Put()))))
        return {};

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                  WICDecodeMetadataCacheOnLoad,
                                                  decoder.Put())))
        return {};

    return DecodeThroughWic(decoder.Get(), factory.Get());
}

Bitmap DecodeImageMemory(const void* data, size_t bytes) {
    if (!data || bytes == 0) return {};

    ComApartment apartment(COINIT_APARTMENTTHREADED);

    ComPtr<IWICImagingFactory> factory;
    if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(factory.Put()))))
        return {};

    // InitializeFromMemory does not copy, so the buffer has to outlive the
    // decode. Every caller here is holding a mapped resource open across it.
    ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(stream.Put())) ||
        FAILED(stream->InitializeFromMemory(
                   const_cast<BYTE*>(static_cast<const BYTE*>(data)),
                   static_cast<DWORD>(bytes))))
        return {};

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr,
                                                WICDecodeMetadataCacheOnLoad,
                                                decoder.Put())))
        return {};

    return DecodeThroughWic(decoder.Get(), factory.Get());
}

Bitmap FromExecutableResource(const std::wstring& path, int* nativePixels) {
    if (nativePixels) *nativePixels = 0;
    if (path.empty()) return {};

    // AS_DATAFILE|AS_IMAGE_RESOURCE maps the file for resource reads only: no
    // relocation, no DllMain, no code from the target ever runs in this process.
    const HMODULE module = ::LoadLibraryExW(
        path.c_str(), nullptr,
        LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!module) return {};

    struct Unload {
        HMODULE module;
        ~Unload() { if (module) ::FreeLibrary(module); }
    } unload{ module };

    ResourceName group;
    ::EnumResourceNamesW(module, RT_GROUP_ICON, FirstGroupProc,
                         reinterpret_cast<LONG_PTR>(&group));
    if (!group.found) return {};   // no icon in the file at all

    const HRSRC groupResource = ::FindResourceW(module, group.Get(), RT_GROUP_ICON);
    if (!groupResource) return {};

    const DWORD groupBytes = ::SizeofResource(module, groupResource);
    const HGLOBAL groupHandle = ::LoadResource(module, groupResource);
    if (!groupHandle || groupBytes < sizeof(GroupIconDir)) return {};

    const auto* directory = static_cast<const GroupIconDir*>(::LockResource(groupHandle));
    if (!directory || directory->count == 0) return {};

    // Guard against a truncated or hostile resource before indexing into it.
    const size_t needed = sizeof(GroupIconDir) +
                          static_cast<size_t>(directory->count) * sizeof(GroupIconEntry);
    if (groupBytes < needed) return {};

    const auto* entries = reinterpret_cast<const GroupIconEntry*>(directory + 1);

    // Largest frame wins; at equal size, the deeper one. A 256 frame and a 48
    // frame are not two qualities of the same picture, they are usually drawn
    // differently, and the big one is the one that was drawn for this.
    const GroupIconEntry* best = nullptr;
    for (WORD i = 0; i < directory->count; ++i) {
        const GroupIconEntry& entry = entries[i];
        if (!best ||
            FrameSize(entry.width) > FrameSize(best->width) ||
            (FrameSize(entry.width) == FrameSize(best->width) &&
             entry.bitCount > best->bitCount))
            best = &entry;
    }
    if (!best) return {};

    const HRSRC iconResource = ::FindResourceW(module, MAKEINTRESOURCEW(best->id), RT_ICON);
    if (!iconResource) return {};

    const DWORD iconBytes = ::SizeofResource(module, iconResource);
    const HGLOBAL iconHandle = ::LoadResource(module, iconResource);
    if (!iconHandle || iconBytes == 0) return {};

    auto* pixels = static_cast<BYTE*>(::LockResource(iconHandle));
    if (!pixels) return {};

    Bitmap out;
    if (LooksLikePng(pixels, iconBytes)) {
        // Vista onward a 256 frame is a whole PNG rather than a DIB.
        out = DecodeImageMemory(pixels, iconBytes);
    } else {
        // 0x00030000 is the icon format version, the only value that has ever
        // been valid. TRUE asks for an icon rather than a cursor.
        const HICON icon = ::CreateIconFromResourceEx(
            pixels, iconBytes, TRUE, 0x00030000, 0, 0, LR_DEFAULTCOLOR);
        if (!icon) return {};

        out = FromHIcon(icon);
        ::DestroyIcon(icon);
    }

    if (out.Empty()) return {};

    if (nativePixels)
        *nativePixels = (std::max)(out.width, out.height);

    MACTAB_DIAG("icon_source: %s carries a %dx%d frame",
                ToUtf8(path).c_str(), out.width, out.height);
    return out;
}

} // namespace mactab
