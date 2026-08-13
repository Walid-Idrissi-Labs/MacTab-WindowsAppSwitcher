#include "pch.h"
#include <shobjidl.h>
#include <wincodec.h>

#include "wallpaper.h"
#include "com.h"
#include "common.h"
#include "diag.h"

namespace mactab::wallpaper {
namespace {

// One cached decode. There is exactly one wallpaper and one Mission Control
// size per monitor, so a single entry per monitor is the whole cache.
struct Entry {
    HMONITOR    monitor = nullptr;
    int         width   = 0;    // the screen the picture was laid out for
    int         height  = 0;
    RECT        region{};       // the part of it that was kept
    std::wstring path;
    Bitmap      pixels;
};

bool SameRect(const RECT& a, const RECT& b) {
    return a.left == b.left && a.top == b.top &&
           a.right == b.right && a.bottom == b.bottom;
}

std::mutex         g_mutex;
std::vector<Entry> g_cache;

// Which file is on which monitor.
//
// SPI_GETDESKWALLPAPER returns one path for the whole desktop, which is wrong
// the moment a second monitor has a different picture, and wrong again for a
// slideshow, where it returns a stale transcoded copy. IDesktopWallpaper is the
// per-monitor answer and has been there since Windows 8.
//
// It identifies monitors by device path, not by HMONITOR, so the mapping goes
// through the rectangles: ask it for each monitor's rect and take the one whose
// rect matches the HMONITOR's. That is exact, because both come from the same
// display configuration.
std::wstring PathForMonitor(HMONITOR monitor) {
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    const bool haveRect = ::GetMonitorInfoW(monitor, &info) != FALSE;

    ComPtr<IDesktopWallpaper> wallpaper;
    if (haveRect &&
        SUCCEEDED(::CoCreateInstance(CLSID_DesktopWallpaper, nullptr, CLSCTX_ALL,
                                     IID_PPV_ARGS(wallpaper.Put())))) {
        UINT count = 0;
        if (SUCCEEDED(wallpaper->GetMonitorDevicePathCount(&count))) {
            for (UINT i = 0; i < count; ++i) {
                LPWSTR id = nullptr;
                if (FAILED(wallpaper->GetMonitorDevicePathAt(i, &id)) || !id)
                    continue;

                RECT rect{};
                const bool match =
                    SUCCEEDED(wallpaper->GetMonitorRECT(id, &rect)) &&
                    rect.left   == info.rcMonitor.left &&
                    rect.top    == info.rcMonitor.top &&
                    rect.right  == info.rcMonitor.right &&
                    rect.bottom == info.rcMonitor.bottom;

                std::wstring path;
                if (match) {
                    LPWSTR file = nullptr;
                    if (SUCCEEDED(wallpaper->GetWallpaper(id, &file)) && file) {
                        path = file;
                        ::CoTaskMemFree(file);
                    }
                }

                ::CoTaskMemFree(id);
                if (match) return path;
            }
        }
    }

    // Either the interface is unavailable or no monitor matched, which happens
    // if the display configuration changed between the two calls.
    wchar_t buffer[MAX_PATH] = L"";
    if (::SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, buffer, 0) && buffer[0])
        return buffer;
    return {};
}

// The copy Windows keeps for itself.
//
// Whenever the wallpaper is set, the shell writes a decoded copy here, and it
// is the only source that survives the cases the two APIs above do not cover:
// a picture that has been deleted or is on a network share that is not mounted,
// a wallpaper that came from a theme pack, and a slideshow, where the reported
// path is whichever file was picked at some point in the past.
//
// It has no extension and it is a JPEG, which the decoder works out for itself.
std::wstring TranscodedPath() {
    wchar_t* roaming = nullptr;
    if (FAILED(::SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roaming)) ||
        !roaming)
        return {};

    std::wstring path = roaming;
    ::CoTaskMemFree(roaming);

    path += L"\\Microsoft\\Windows\\Themes\\TranscodedWallpaper";
    return path;
}

// Decode, scale to cover, crop to centre.
//
// Cover rather than fit, because that is what Windows' default "Fill" style
// does and it is what the overwhelming majority of desktops are set to. Getting
// every one of the six placement styles exactly right would mean reading
// WallpaperStyle and TileWallpaper out of the registry and reproducing each
// one, for a backdrop that is about to be blurred past recognition.
Bitmap Decode(const std::wstring& path, int width, int height, const RECT& region) {
    const int keepW = region.right - region.left;
    const int keepH = region.bottom - region.top;
    if (path.empty() || width <= 0 || height <= 0 || keepW <= 0 || keepH <= 0)
        return {};

    // The apartment belongs to Region, which is the only caller and needs one
    // before this is reached anyway.
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(factory.Put()))))
        return {};

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                  WICDecodeMetadataCacheOnDemand,
                                                  decoder.Put())))
        return {};

    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, frame.Put())))
        return {};

    UINT sourceW = 0, sourceH = 0;
    if (FAILED(frame->GetSize(&sourceW, &sourceH)) || sourceW == 0 || sourceH == 0)
        return {};

    // Scale so the short side covers, then crop the long side to centre.
    const double scale = (std::max)(static_cast<double>(width)  / sourceW,
                                    static_cast<double>(height) / sourceH);
    const UINT scaledW = (std::max)(static_cast<UINT>(width),
                                    static_cast<UINT>(sourceW * scale + 0.5));
    const UINT scaledH = (std::max)(static_cast<UINT>(height),
                                    static_cast<UINT>(sourceH * scale + 0.5));

    ComPtr<IWICBitmapScaler> scaler;
    if (FAILED(factory->CreateBitmapScaler(scaler.Put())) ||
        FAILED(scaler->Initialize(frame.Get(), scaledW, scaledH,
                                  WICBitmapInterpolationModeFant)))
        return {};

    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(converter.Put())) ||
        FAILED(converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppBGRA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom)))
        return {};

    // The centre crop that turns the covered picture into the screen, plus the
    // part of the screen the caller actually asked for.
    //
    // Only that part is copied out, and WIC is pull-based: the format converter
    // asks the scaler for the rows it needs and the scaler asks the decoder for
    // the rows IT needs, so a request for the top strip of a 4K wallpaper does
    // not scale the other two thousand rows. That is what makes it affordable to
    // cut the glass under the spaces bar from the full-resolution picture rather
    // than from the quarter-resolution copy the backdrop uses.
    WICRect crop{};
    crop.X      = static_cast<INT>((scaledW - static_cast<UINT>(width))  / 2) + region.left;
    crop.Y      = static_cast<INT>((scaledH - static_cast<UINT>(height)) / 2) + region.top;
    crop.Width  = keepW;
    crop.Height = keepH;

    Bitmap out = Bitmap::Create(keepW, keepH);
    const UINT stride = static_cast<UINT>(keepW) * 4;
    const UINT bytes  = stride * static_cast<UINT>(keepH);

    if (FAILED(converter->CopyPixels(&crop, stride, bytes,
                                     reinterpret_cast<BYTE*>(out.pixels.data()))))
        return {};

    // A wallpaper has no transparency, but the decoder reports whatever the file
    // claims, and a PNG with an alpha channel would otherwise composite as a
    // hole. The desktop is opaque by definition.
    for (uint32_t& pixel : out.pixels) pixel |= 0xFF000000u;

    return out;
}

} // namespace

uint32_t SolidColour() {
    const COLORREF colour = ::GetSysColor(COLOR_DESKTOP);
    return MakePixel(GetRValue(colour), GetGValue(colour), GetBValue(colour), 255);
}

Bitmap ForMonitor(HMONITOR monitor, int width, int height) {
    return Region(monitor, width, height, RECT{ 0, 0, width, height });
}

Bitmap Region(HMONITOR monitor, int width, int height, RECT region) {
    if (width <= 0 || height <= 0) return {};

    // Clipped rather than trusted. A caller inflating a rect by a blur margin
    // walks off the top of the screen as a matter of course, and a negative
    // origin would be read as a crop into the picture rather than as nothing.
    region.left   = (std::max)(0L, region.left);
    region.top    = (std::max)(0L, region.top);
    region.right  = (std::min)(static_cast<LONG>(width),  region.right);
    region.bottom = (std::min)(static_cast<LONG>(height), region.bottom);
    if (region.right <= region.left || region.bottom <= region.top) return {};

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (const Entry& entry : g_cache) {
            if (entry.monitor != monitor || entry.width != width ||
                entry.height != height)
                continue;

            if (SameRect(entry.region, region)) return entry.pixels;

            // A bigger piece of the same picture is already decoded, so cut this
            // one out of it rather than reading the file again.
            //
            // This is the ordinary case rather than a lucky one: with no blur
            // the backdrop is baked at the screen's own size, and the strip the
            // spaces bar is cut from is the top of exactly that. Without it,
            // turning the blur off means decoding a 4K photograph twice and
            // holding both.
            if (entry.region.left  <= region.left  && entry.region.top    <= region.top &&
                entry.region.right >= region.right && entry.region.bottom >= region.bottom) {
                Bounds within;
                within.left   = region.left   - entry.region.left;
                within.top    = region.top    - entry.region.top;
                within.right  = region.right  - entry.region.left;
                within.bottom = region.bottom - entry.region.top;
                return Crop(entry.pixels, within);
            }
        }
    }

    // Covers the lookup as well as the decode. It used to sit inside Decode,
    // which left PathForMonitor running outside any apartment: harmless on the
    // UI thread, which the panel has already put in an STA, but the first
    // caller here is Mission's prewarm thread, which initialises nothing. There
    // CoCreateInstance returned CO_E_NOTINITIALIZED and the per-monitor lookup
    // fell through to SPI_GETDESKWALLPAPER every time, which is exactly the two
    // cases IDesktopWallpaper is here for: one picture per monitor, and a
    // slideshow, whose reported path is whichever file was current some time
    // ago. The wrong pixels then went into the cache under a key that does not
    // mention the path, so the UI thread could never correct it later.
    ComApartment apartment(COINIT_APARTMENTTHREADED);

    // Decoding outside the lock. It reads a file and can take tens of
    // milliseconds on a 4K JPEG, and holding the lock across that would make a
    // second monitor's bake wait on the first for no reason.
    std::wstring path = PathForMonitor(monitor);

    const double started = NowMs();
    Bitmap pixels = Decode(path, width, height, region);

    if (pixels.Empty()) {
        // The reported picture could not be read. That is not unusual: it may
        // have been deleted, it may be on a share that is not mounted, it may
        // have come from a theme, or this may be a slideshow whose reported
        // path is whichever file was current some time ago.
        const std::wstring transcoded = TranscodedPath();
        pixels = Decode(transcoded, width, height, region);
        if (!pixels.Empty()) {
            MACTAB_DIAG("wallpaper: \"%s\" was unreadable, used the shell's own copy",
                        ToUtf8(path).c_str());
            path = transcoded;
        }
    }

    if (pixels.Empty()) {
        MACTAB_DIAG("wallpaper: no picture for monitor %p (\"%s\"), using the desktop colour",
                    static_cast<void*>(monitor), ToUtf8(path).c_str());
    } else {
        MACTAB_DIAG("wallpaper: %ldx%ld of %dx%d from \"%s\" in %.1f ms",
                    region.right - region.left, region.bottom - region.top,
                    width, height, ToUtf8(path).c_str(), NowMs() - started);
    }

    {
        std::lock_guard<std::mutex> lock(g_mutex);

        // Another thread may have finished the same decode while this one was
        // reading the file. Whichever landed first wins; they are identical.
        for (const Entry& entry : g_cache)
            if (entry.monitor == monitor && entry.width == width &&
                entry.height == height && SameRect(entry.region, region))
                return entry.pixels;

        Entry entry;
        entry.monitor = monitor;
        entry.width   = width;
        entry.height  = height;
        entry.region  = region;
        entry.path    = path;
        entry.pixels  = pixels;
        g_cache.push_back(std::move(entry));
    }

    return pixels;
}

void Invalidate() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_cache.empty()) return;
    g_cache.clear();
    MACTAB_DIAG("wallpaper: cache dropped");
}

} // namespace mactab::wallpaper
