#include "pch.h"

#include "thumbnail.h"
#include "common.h"
#include "diag.h"

namespace mactab::thumbnail {
namespace {

// Renders the window's full contents even where they are occluded, and is the
// only way to get anything at all out of a Chromium window. Documented since
// Windows 8.1 but missing from some SDK headers.
#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

// The undocumented pair.
//
// Ordinals rather than names, because these are exported by ordinal only. The
// signatures are the shape every shipping user of them agrees on; there is no
// header anywhere that declares them.
//
// dwThumbnailFlags is the one parameter with no consensus explanation. Every
// working example passes 2. Passing it and then checking the result, rather
// than trusting it, is why Probe() creates a real visual instead of only
// resolving the export.
using CreateSharedThumbnailVisualFn = HRESULT(WINAPI*)(
    HWND destination, HWND source, DWORD thumbnailFlags,
    DWM_THUMBNAIL_PROPERTIES* properties, void* dcompDevice,
    void** visual, HTHUMBNAIL* thumbnailId);

using QueryThumbnailSourceSizeFn = HRESULT(WINAPI*)(
    HWND source, BOOL clientAreaOnly, SIZE* size);

CreateSharedThumbnailVisualFn g_createShared = nullptr;
QueryThumbnailSourceSizeFn    g_querySize    = nullptr;

Tier g_tier   = Tier::None;
Tier g_forced = Tier::None;
bool g_probed = false;

bool ResolveExports() {
    static bool resolved = false;
    if (resolved) return g_createShared != nullptr;
    resolved = true;

    // Already loaded; the process links dwmapi. GetModuleHandle rather than
    // LoadLibrary so this cannot be the thing that pulls it in.
    HMODULE dwm = ::GetModuleHandleW(L"dwmapi.dll");
    if (!dwm) dwm = ::LoadLibraryW(L"dwmapi.dll");
    if (!dwm) {
        MACTAB_WARN("thumbnail: dwmapi.dll unavailable");
        return false;
    }

    g_createShared = reinterpret_cast<CreateSharedThumbnailVisualFn>(
        reinterpret_cast<void*>(::GetProcAddress(dwm, MAKEINTRESOURCEA(147))));
    g_querySize = reinterpret_cast<QueryThumbnailSourceSizeFn>(
        reinterpret_cast<void*>(::GetProcAddress(dwm, MAKEINTRESOURCEA(162))));

    if (!g_createShared)
        MACTAB_DIAG("thumbnail: dwmapi ordinal 147 not exported on this build");

    return g_createShared != nullptr;
}

// Does this window answer messages?
//
// PrintWindow drives the target's own WM_PRINT and WM_PAINT handling, which
// means a hung application hangs whoever called it, with no timeout of its own.
// The ping is the only guard there is, and it is what every production user of
// PrintWindow does.
bool Responsive(HWND hwnd) {
    DWORD_PTR result = 0;
    return ::SendMessageTimeoutW(hwnd, WM_NULL, 0, 0,
                                 SMTO_ABORTIFHUNG | SMTO_BLOCK, 50, &result) != 0;
}

} // namespace

const char* TierName(Tier tier) {
    switch (tier) {
        case Tier::SharedVisual: return "shared visual (dwmapi 147)";
        case Tier::Snapshot:     return "PrintWindow snapshot";
        case Tier::IconOnly:     return "icon cards";
        default:                 return "none";
    }
}

Tier Current() {
    return (g_forced != Tier::None) ? g_forced : g_tier;
}

void Force(Tier tier) {
    g_forced = tier;
    if (tier != Tier::None)
        MACTAB_DIAG("thumbnail: tier forced to %s", TierName(tier));
}

Tier Probe(HWND destination, HWND source) {
    if (g_probed) return Current();
    g_probed = true;

    g_tier = Tier::Snapshot;   // the floor that always works

    if (!source) source = destination;

    if (!destination || !ResolveExports()) {
        MACTAB_DIAG("thumbnail: tier is %s", TierName(Current()));
        return Current();
    }

    // Resolving the export is not evidence. Create one for real, against our own
    // window, and throw it away.
    //
    // A null device is deliberate here: this asks only whether DWM will accept
    // the call at all. Creating a visual needs a composition device, which the
    // overlay owns and which has not necessarily been built yet at probe time.
    // A build where the export is present but the call is rejected outright is
    // exactly what this catches, and it is the failure that would otherwise
    // reach a user.
    DWM_THUMBNAIL_PROPERTIES properties{};
    properties.dwFlags               = DWM_TNP_VISIBLE | DWM_TNP_OPACITY |
                                       DWM_TNP_SOURCECLIENTAREAONLY;
    properties.fVisible              = TRUE;
    properties.opacity               = 255;
    properties.fSourceClientAreaOnly = FALSE;

    void*      visual = nullptr;
    HTHUMBNAIL handle = nullptr;
    const HRESULT hr = g_createShared(destination, source, 2, &properties,
                                      nullptr, &visual, &handle);

    // E_INVALIDARG is the expected answer to a null device on a build where the
    // export works: it got far enough to validate arguments. Anything that
    // looks like "no such thing" means the export is not what we think it is.
    const bool usable = SUCCEEDED(hr) || hr == E_INVALIDARG || hr == E_POINTER;

    if (visual) reinterpret_cast<IUnknown*>(visual)->Release();
    if (handle) ::DwmUnregisterThumbnail(handle);

    if (usable) g_tier = Tier::SharedVisual;

    MACTAB_DIAG("thumbnail: probe returned 0x%08lX, tier is %s",
                static_cast<unsigned long>(hr), TierName(Current()));
    return Current();
}

bool SourceSize(HWND source, SIZE& out) {
    out.cx = 0;
    out.cy = 0;
    if (!source) return false;

    if (ResolveExports() && g_querySize &&
        SUCCEEDED(g_querySize(source, FALSE, &out)) && out.cx > 0 && out.cy > 0)
        return true;

    RECT bounds{};
    if (SUCCEEDED(::DwmGetWindowAttribute(source, DWMWA_EXTENDED_FRAME_BOUNDS,
                                          &bounds, sizeof(bounds))) ||
        ::GetWindowRect(source, &bounds)) {
        out.cx = bounds.right - bounds.left;
        out.cy = bounds.bottom - bounds.top;
    }
    return out.cx > 0 && out.cy > 0;
}

bool SourceGeometry(HWND source, RECT& window, RECT& frame) {
    window = RECT{};
    frame  = RECT{};
    if (!source || !::GetWindowRect(source, &window)) return false;

    if (FAILED(::DwmGetWindowAttribute(source, DWMWA_EXTENDED_FRAME_BOUNDS,
                                       &frame, sizeof(frame))) ||
        frame.right <= frame.left || frame.bottom <= frame.top) {
        frame = window;
    }

    // A window can report a frame larger than itself when it is maximised on a
    // scaled display, and a negative inset would push the thumbnail the wrong
    // way, so the frame is held inside the window rect.
    frame.left   = (std::max)(frame.left,   window.left);
    frame.top    = (std::max)(frame.top,    window.top);
    frame.right  = (std::min)(frame.right,  window.right);
    frame.bottom = (std::min)(frame.bottom, window.bottom);

    return frame.right > frame.left && frame.bottom > frame.top;
}

bool CreateSharedVisual(void* device, HWND destination, HWND source, SIZE render,
                        void** outVisual, HTHUMBNAIL* outHandle) {
    if (outVisual) *outVisual = nullptr;
    if (outHandle) *outHandle = nullptr;

    if (Current() != Tier::SharedVisual || !device || !destination || !source)
        return false;
    if (!ResolveExports() || !outVisual || !outHandle)
        return false;

    if (render.cx <= 0 || render.cy <= 0) {
        SIZE fallback{};
        if (!SourceSize(source, fallback)) return false;
        render = fallback;
    }

    DWM_THUMBNAIL_PROPERTIES properties{};
    properties.dwFlags               = DWM_TNP_VISIBLE | DWM_TNP_OPACITY |
                                       DWM_TNP_SOURCECLIENTAREAONLY |
                                       DWM_TNP_RECTDESTINATION;
    properties.fVisible              = TRUE;
    properties.opacity               = 255;
    properties.fSourceClientAreaOnly = FALSE;

    // The destination is what DWM sizes its render to. Asked for the source's
    // own size, so the thumbnail carries every pixel the window has and the
    // only scaling that ever happens is downward, in the compositor.
    properties.rcDestination = RECT{ 0, 0, render.cx, render.cy };

    const HRESULT hr = g_createShared(destination, source, 2, &properties,
                                      device, outVisual, outHandle);
    if (FAILED(hr) || !*outVisual) {
        // Expected for a window that has gone away between enumeration and
        // here, and for anything living on another virtual desktop, which DWM
        // does not compose through this path.
        MACTAB_DIAG("thumbnail: no shared visual for %p (0x%08lX)",
                    static_cast<void*>(source), static_cast<unsigned long>(hr));
        if (*outHandle) { ::DwmUnregisterThumbnail(*outHandle); *outHandle = nullptr; }
        return false;
    }
    return true;
}

void ReleaseSharedVisual(HTHUMBNAIL handle) {
    if (handle) ::DwmUnregisterThumbnail(handle);
}

Bitmap Snapshot(HWND source, int maxWidth, int maxHeight) {
    if (!source || maxWidth <= 0 || maxHeight <= 0) return {};
    if (!::IsWindow(source) || ::IsIconic(source)) return {};
    if (!Responsive(source)) {
        MACTAB_DIAG("thumbnail: %p did not answer in 50 ms, skipping",
                    static_cast<void*>(source));
        return {};
    }

    RECT bounds{};
    if (FAILED(::DwmGetWindowAttribute(source, DWMWA_EXTENDED_FRAME_BOUNDS,
                                       &bounds, sizeof(bounds))))
        ::GetWindowRect(source, &bounds);

    const int width  = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;
    if (width <= 0 || height <= 0) return {};

    // Refuse the absurd rather than allocating it. A window can legitimately be
    // enormous when it spans several 4K monitors, and this runs once per window.
    if (static_cast<long long>(width) * height > 64ll * 1024 * 1024) return {};

    HDC screen = ::GetDC(nullptr);
    if (!screen) return {};

    BITMAPINFO info{};
    info.bmiHeader.biSize        = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth       = width;
    info.bmiHeader.biHeight      = -height;   // top-down
    info.bmiHeader.biPlanes      = 1;
    info.bmiHeader.biBitCount    = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void*   bits   = nullptr;
    HBITMAP dib    = ::CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    HDC     memory = ::CreateCompatibleDC(screen);

    Bitmap out;

    if (dib && memory && bits) {
        HGDIOBJ previous = ::SelectObject(memory, dib);

        // PW_RENDERFULLCONTENT is what makes this work on anything modern.
        // Without it a Chromium window comes back solid black, which covers a
        // large share of what is actually open on a real desktop.
        if (::PrintWindow(source, memory, PW_RENDERFULLCONTENT)) {
            Bitmap full = Bitmap::Create(width, height);
            std::memcpy(full.pixels.data(), bits,
                        static_cast<size_t>(width) * height * 4);

            // GDI leaves alpha at zero on plenty of windows, and a bitmap that
            // is entirely transparent composites as nothing at all. The window
            // is opaque, so say so.
            for (uint32_t& pixel : full.pixels) pixel |= 0xFF000000u;

            const double scale = (std::min)(static_cast<double>(maxWidth)  / width,
                                            static_cast<double>(maxHeight) / height);
            if (scale < 1.0) {
                out = Resize(full,
                             (std::max)(1, static_cast<int>(width  * scale + 0.5)),
                             (std::max)(1, static_cast<int>(height * scale + 0.5)));
            } else {
                out = std::move(full);
            }
        }

        ::SelectObject(memory, previous);
    }

    if (memory) ::DeleteDC(memory);
    if (dib)    ::DeleteObject(dib);
    ::ReleaseDC(nullptr, screen);

    return out;
}

} // namespace mactab::thumbnail
