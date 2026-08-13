#include "pch.h"
#include <d3d11.h>
#include <dxgi1_2.h>

#include "capture.h"
#include "com.h"
#include "common.h"
#include "diag.h"

namespace mactab::capture {
namespace {

// Total time we are willing to spend waiting for a duplicated frame.
//
// Twenty milliseconds, not forty-eight and not eight. AcquireNextFrame hands
// over a frame when the desktop has CHANGED since the duplication was opened,
// and returns DXGI_ERROR_WAIT_TIMEOUT when it has not. On a still desktop, which
// is most of the time somebody presses Alt+Tab, nothing is ever going to arrive,
// so six attempts were 48 ms spent proving a negative and then going the slow
// way round through BitBlt anyway.
//
// One attempt at 8 ms, which 0.9.0 cut it to, went too far the other way. A
// screen that IS presenting does so every 16.7 ms at 60 Hz, so a single 8 ms
// window catches it about half the time: that is the coin toss behind a panel
// that is glass in fullscreen only SOMETIMES. Two attempts at 10 ms clears a
// frame interval and turns that into nearly always, and the cost when nothing
// arrives is 20 ms on a path that no longer loses the reveal when it overruns.
//
// That 40 ms was not free. The panel is revealed after RevealDelayMs, 180 by
// default, and a grab that misses that shows the near-opaque fallback coat
// instead of glass. Add an uncached D3D11CreateDevice, a DuplicateOutput and a
// CAPTUREBLT blit, which forces a DWM sync, and a still desktop could go over
// the line while a moving one came back inside 8 ms. Glass while something
// animates behind the panel and a grey slab the rest of the time is exactly what
// that looks like to the person using it.
//
// The reveal is no longer lost when this does overrun: Panel::WaitForCapture
// takes the frame whenever it lands and re-bakes.
constexpr DWORD kAcquireTimeoutMs = 10;
constexpr int   kAcquireAttempts  = 2;

bool InRemoteSession() {
    // Explicit check rather than trying to detect black frames: desktop
    // duplication is documented to misbehave over RDP (black frames on some
    // builds, outright blocked under Server RemoteApp), and a heuristic on
    // pixel content would misfire on a genuinely black desktop.
    return ::GetSystemMetrics(SM_REMOTESESSION) != 0;
}

// Find the DXGI output covering `rect`, and the adapter that owns it.
//
// The adapter matters: on hybrid-GPU laptops, calling DuplicateOutput with a
// device created on a different adapter than the one driving the output fails
// with DXGI_ERROR_UNSUPPORTED. So the D3D device must be built from the adapter
// that enumerated this output, never from D3D_DRIVER_TYPE_HARDWARE.
bool FindOutputForRect(const RECT& rect,
                       ComPtr<IDXGIAdapter1>& adapterOut,
                       ComPtr<IDXGIOutput1>& outputOut,
                       RECT& outputBoundsOut) {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(::CreateDXGIFactory1(IID_PPV_ARGS(factory.Put()))))
        return false;

    const POINT centre{ (rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2 };

    for (UINT adapterIndex = 0;; ++adapterIndex) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(adapterIndex, adapter.Put()) == DXGI_ERROR_NOT_FOUND)
            break;

        for (UINT outputIndex = 0;; ++outputIndex) {
            ComPtr<IDXGIOutput> output;
            if (adapter->EnumOutputs(outputIndex, output.Put()) == DXGI_ERROR_NOT_FOUND)
                break;

            DXGI_OUTPUT_DESC desc{};
            if (FAILED(output->GetDesc(&desc)))
                continue;

            if (!::PtInRect(&desc.DesktopCoordinates, centre))
                continue;

            ComPtr<IDXGIOutput1> output1;
            if (FAILED(output->QueryInterface(IID_PPV_ARGS(output1.Put()))))
                return false;

            adapterOut      = adapter;
            outputOut       = output1;
            outputBoundsOut = desc.DesktopCoordinates;
            return true;
        }
    }

    return false;
}

Bitmap ReadStagingTexture(ID3D11DeviceContext* context, ID3D11Texture2D* staging,
                          int width, int height) {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)))
        return {};

    Bitmap out = Bitmap::Create(width, height);
    const auto* src = static_cast<const uint8_t*>(mapped.pData);

    for (int y = 0; y < height; ++y) {
        std::memcpy(&out.pixels[static_cast<size_t>(y) * width],
                    src + static_cast<size_t>(y) * mapped.RowPitch,
                    static_cast<size_t>(width) * 4);
    }

    context->Unmap(staging, 0);

    // The desktop has no meaningful alpha; force opaque so the blur does not
    // pull transparency in from whatever the driver left in that channel.
    for (uint32_t& pixel : out.pixels)
        pixel |= 0xFF000000u;

    return out;
}

Frame GrabWithDuplication(const RECT& rect) {
    Frame frame;

    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput1>  output;
    RECT outputBounds{};
    if (!FindOutputForRect(rect, adapter, output, outputBounds)) {
        MACTAB_WARN("capture: no DXGI output covers the panel rect");
        return frame;
    }

    ComPtr<ID3D11Device>        device;
    ComPtr<ID3D11DeviceContext> context;
    const HRESULT deviceHr = ::D3D11CreateDevice(
        adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
        device.Put(), nullptr, context.Put());
    if (FAILED(deviceHr)) {
        MACTAB_WARN("capture: D3D11CreateDevice failed (hr 0x%08lX)",
                    static_cast<unsigned long>(deviceHr));
        return frame;
    }

    ComPtr<IDXGIOutputDuplication> duplication;
    const HRESULT dupHr = output->DuplicateOutput(device.Get(), duplication.Put());
    if (FAILED(dupHr)) {
        // DXGI_ERROR_UNSUPPORTED here is the classic hybrid-GPU case;
        // E_ACCESSDENIED means the secure desktop is up.
        MACTAB_WARN("capture: DuplicateOutput failed (hr 0x%08lX)",
                    static_cast<unsigned long>(dupHr));
        return frame;
    }

    ComPtr<IDXGIResource>       resource;
    DXGI_OUTDUPL_FRAME_INFO     info{};
    HRESULT acquireHr = DXGI_ERROR_WAIT_TIMEOUT;

    for (int attempt = 0; attempt < kAcquireAttempts; ++attempt) {
        acquireHr = duplication->AcquireNextFrame(kAcquireTimeoutMs, &info, resource.Put());
        if (SUCCEEDED(acquireHr)) break;
        if (acquireHr != DXGI_ERROR_WAIT_TIMEOUT) break;
    }

    if (FAILED(acquireHr)) {
        // DXGI_ERROR_WAIT_TIMEOUT (0x887A0027) here is the ordinary answer on a
        // desktop that has not changed, not a fault: BitBlt picks it up and the
        // panel is none the wiser. Anything else is worth reading.
        MACTAB_WARN("capture: AcquireNextFrame failed (hr 0x%08lX)",
                    static_cast<unsigned long>(acquireHr));
        return frame;
    }

    // Everything below must release the frame, so keep the scope tight.
    ComPtr<ID3D11Texture2D> desktop;
    const bool haveTexture = SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(desktop.Put())));

    if (haveTexture) {
        D3D11_TEXTURE2D_DESC desktopDesc{};
        desktop->GetDesc(&desktopDesc);

        // With HDR enabled the duplicated frame arrives as scRGB float rather
        // than BGRA, and DuplicateOutput does not convert. Tone mapping that
        // properly is a job for the bake pass; refusing here means we fall back
        // to BitBlt, which gives SDR pixels, rather than showing garbage.
        if (desktopDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
            MACTAB_WARN("capture: unexpected duplication format %u (HDR?), falling back",
                        static_cast<unsigned>(desktopDesc.Format));
        } else {
            // Region is in virtual-screen coordinates; the texture is in
            // output-local ones.
            const int width  = static_cast<int>(rect.right  - rect.left);
            const int height = static_cast<int>(rect.bottom - rect.top);

            D3D11_BOX box{};
            box.left   = static_cast<UINT>((std::max)(0L, rect.left - outputBounds.left));
            box.top    = static_cast<UINT>((std::max)(0L, rect.top  - outputBounds.top));
            box.right  = (std::min)(box.left + static_cast<UINT>(width),  desktopDesc.Width);
            box.bottom = (std::min)(box.top  + static_cast<UINT>(height), desktopDesc.Height);
            box.front  = 0;
            box.back   = 1;

            if (box.right > box.left && box.bottom > box.top) {
                D3D11_TEXTURE2D_DESC stagingDesc{};
                stagingDesc.Width              = box.right - box.left;
                stagingDesc.Height             = box.bottom - box.top;
                stagingDesc.MipLevels          = 1;
                stagingDesc.ArraySize          = 1;
                stagingDesc.Format             = DXGI_FORMAT_B8G8R8A8_UNORM;
                stagingDesc.SampleDesc.Count   = 1;
                stagingDesc.Usage              = D3D11_USAGE_STAGING;
                stagingDesc.CPUAccessFlags     = D3D11_CPU_ACCESS_READ;

                ComPtr<ID3D11Texture2D> staging;
                if (SUCCEEDED(device->CreateTexture2D(&stagingDesc, nullptr, staging.Put()))) {
                    context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0,
                                                   desktop.Get(), 0, &box);
                    frame.pixels = ReadStagingTexture(context.Get(), staging.Get(),
                                                      static_cast<int>(stagingDesc.Width),
                                                      static_cast<int>(stagingDesc.Height));
                    if (!frame.pixels.Empty()) {
                        frame.source = Source::DesktopDuplication;
                        // Report the clamped region, not the requested one.
                        frame.bounds.left   = outputBounds.left + static_cast<LONG>(box.left);
                        frame.bounds.top    = outputBounds.top  + static_cast<LONG>(box.top);
                        frame.bounds.right  = frame.bounds.left + frame.pixels.width;
                        frame.bounds.bottom = frame.bounds.top  + frame.pixels.height;
                    }
                }
            }
        }
    }

    duplication->ReleaseFrame();
    return frame;
}

Frame GrabWithBitBlt(const RECT& rect, bool captureBlt) {
    Frame frame;

    const int width  = static_cast<int>(rect.right  - rect.left);
    const int height = static_cast<int>(rect.bottom - rect.top);
    if (width <= 0 || height <= 0) return frame;

    // Every failure below is reported. None of them were, which is most of why
    // a panel with no backdrop went five releases without anybody being able to
    // say why: the only trace of a failed blit was the absence of a frame.
    const HDC screen = ::GetDC(nullptr);
    if (!screen) {
        MACTAB_WARN("capture: GetDC(screen) failed (err %lu)", ::GetLastError());
        return frame;
    }

    const HDC memory = ::CreateCompatibleDC(screen);
    if (!memory) {
        MACTAB_WARN("capture: CreateCompatibleDC failed (err %lu)", ::GetLastError());
        ::ReleaseDC(nullptr, screen);
        return frame;
    }

    BITMAPINFO info{};
    info.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth       = width;
    info.bmiHeader.biHeight      = -height;   // top-down
    info.bmiHeader.biPlanes      = 1;
    info.bmiHeader.biBitCount    = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    const HBITMAP dib = ::CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);

    if (!dib || !bits)
        MACTAB_WARN("capture: CreateDIBSection %dx%d failed (err %lu)", width, height,
                    ::GetLastError());

    if (dib && bits) {
        const HGDIOBJ previous = ::SelectObject(memory, dib);

        // CAPTUREBLT picks up layered windows above the desktop, which was the
        // right call before Windows 8. Under DWM every window is already
        // composited into the surface a plain SRCCOPY reads, so it buys nothing
        // there, and it is the flag most likely to be why a blit comes back
        // black: it forces a synchronous compositor flush and takes a different
        // path inside it. GrabRegion tries the plain one first for that reason.
        const DWORD rop = captureBlt ? (SRCCOPY | CAPTUREBLT) : SRCCOPY;

        if (!::BitBlt(memory, 0, 0, width, height, screen, rect.left, rect.top, rop)) {
            MACTAB_WARN("capture: BitBlt%s failed (err %lu)",
                        captureBlt ? " with CAPTUREBLT" : "", ::GetLastError());
        } else {
            frame.pixels = Bitmap::Create(width, height);
            std::memcpy(frame.pixels.pixels.data(), bits,
                        static_cast<size_t>(width) * height * 4);
            for (uint32_t& pixel : frame.pixels.pixels)
                pixel |= 0xFF000000u;
            frame.source = captureBlt ? Source::GdiBitBlt : Source::GdiPlain;
            frame.bounds = rect;   // BitBlt reads the virtual screen, no clamping
        }

        ::SelectObject(memory, previous);
    }

    // Outside the guard above, which tests the bits pointer as well as the
    // bitmap. CreateDIBSection is documented to fill both or neither, so this
    // should not be reachable, but a bitmap that exists and is never deleted is
    // a leak on a path that runs once per gesture.
    if (dib) ::DeleteObject(dib);

    ::DeleteDC(memory);
    ::ReleaseDC(nullptr, screen);
    return frame;
}

// What a grab actually came back with.
//
// Nothing here ever looked at the pixels, and that turned out to be the whole
// problem: a blit that "succeeds" and hands back a rectangle of black is
// indistinguishable, to every line of code downstream, from a genuinely black
// desktop. The panel then blurs black, tints it, and the adaptive bias lifts it
// to a flat mid-dark grey, which is precisely the slab this material has been
// accused of being for five releases.
//
// Sampled rather than exhaustive: every fourth pixel is tens of thousands of
// samples on a panel-sized grab, which is far more than enough to tell a picture
// from a blank, and it runs on the capture worker where the time is free.
struct Content {
    double mean   = 0.0;   // mean luma, 0..1
    double spread = 0.0;   // mean absolute deviation from that, 0..1
};

Content Assess(const Bitmap& pixels) {
    Content content;
    if (pixels.pixels.empty()) return content;

    constexpr size_t kStride = 4;

    double  total = 0.0;
    size_t  count = 0;
    for (size_t i = 0; i < pixels.pixels.size(); i += kStride) {
        const uint32_t px = pixels.pixels[i];
        total += (RedOf(px) * 0.2126 + GreenOf(px) * 0.7152 + BlueOf(px) * 0.0722) / 255.0;
        ++count;
    }
    if (!count) return content;

    content.mean = total / static_cast<double>(count);

    double deviation = 0.0;
    for (size_t i = 0; i < pixels.pixels.size(); i += kStride) {
        const uint32_t px = pixels.pixels[i];
        const double luma =
            (RedOf(px) * 0.2126 + GreenOf(px) * 0.7152 + BlueOf(px) * 0.0722) / 255.0;
        deviation += (luma > content.mean) ? (luma - content.mean) : (content.mean - luma);
    }
    content.spread = deviation / static_cast<double>(count);
    return content;
}

// A grab worth putting behind the panel.
//
// Rejects a black rectangle with nothing in it, which is what a failed GDI
// capture of a composited desktop looks like. Deliberately NOT a general
// "uniform" test: a plain dark wallpaper is flat too, and it is a real desktop
// that the material handles correctly, so only near-black AND featureless is
// treated as a failure. That pairs with the order in GrabRegion, where a frame
// that fails this is kept as a last resort rather than thrown away: a real black
// desktop still ends up drawn, just after the other paths have been tried.
bool Blank(const Content& content) {
    return content.mean < 0.012 && content.spread < 0.004;
}

Source g_forced = Source::None;

} // namespace

const char* SourceName(Source source) {
    switch (source) {
    case Source::DesktopDuplication: return "desktop-duplication";
    case Source::GdiBitBlt:          return "gdi-bitblt";
    case Source::GdiPlain:           return "gdi-plain";
    case Source::None:               break;
    }
    return "none";
}

void Force(Source source) {
    g_forced = source;
    MACTAB_DIAG("capture: source forced to %s", SourceName(source));
}

Source ParseSource(const wchar_t* keyword) {
    if (!keyword) return Source::None;
    if (::lstrcmpiW(keyword, L"duplication") == 0) return Source::DesktopDuplication;
    if (::lstrcmpiW(keyword, L"bitblt")      == 0) return Source::GdiBitBlt;
    if (::lstrcmpiW(keyword, L"plain")       == 0) return Source::GdiPlain;
    return Source::None;   // "auto", and anything unrecognised
}

Frame GrabRegion(const RECT& rect) {
    const double started = NowMs();

    // In preference order. Duplication first because it is the only one that
    // reads the composed desktop from the compositor rather than through GDI;
    // the two blits after it are there because on some machines it never
    // delivers, and on a still desktop it CANNOT: it has nothing to hand over
    // until something on screen changes.
    // The plain blit before the CAPTUREBLT one, which is the opposite of the
    // order this shipped in for five releases. CAPTUREBLT was here to pick up
    // layered windows, which was the right call before Windows 8; under DWM
    // every window is already composited into the surface a plain SRCCOPY
    // reads, so the flag buys nothing and it is the one most likely to be the
    // reason a blit comes back black, since it forces a synchronous compositor
    // flush and takes a different path inside it. It stays as the third try
    // rather than being deleted, because a machine where it is the one that
    // works is exactly the kind of thing nobody here can rule out.
    const Source order[] = { Source::DesktopDuplication,
                             Source::GdiPlain,
                             Source::GdiBitBlt };

    const bool remote = InRemoteSession();
    if (remote)
        MACTAB_DIAG("capture: remote session, skipping desktop duplication");

    // Read once, and let go of a forced source that cannot run here.
    //
    // CaptureSource=duplication in a remote session used to skip every path in
    // the loop: duplication because the session is remote, and the two GDI ones
    // because they are not the forced source. Nothing ran, the panel fell back
    // to the flat coat, and the only thing said about it was that no path
    // returned a usable frame, which reads like all three were tried and failed.
    Source forced = g_forced;
    if (forced == Source::DesktopDuplication && remote) {
        MACTAB_WARN("capture: CaptureSource=duplication cannot run in a remote "
                    "session; using the other paths instead");
        forced = Source::None;
    }

    Frame best;   // the last thing that came back at all, blank or not

    for (const Source source : order) {
        if (forced != Source::None && source != forced) continue;
        if (source == Source::DesktopDuplication && remote) continue;

        Frame frame;
        switch (source) {
        case Source::DesktopDuplication: frame = GrabWithDuplication(rect);   break;
        case Source::GdiBitBlt:          frame = GrabWithBitBlt(rect, true);  break;
        case Source::GdiPlain:           frame = GrabWithBitBlt(rect, false); break;
        case Source::None:               break;
        }

        if (frame.source == Source::None || frame.pixels.Empty())
            continue;

        const Content content = Assess(frame.pixels);
        MACTAB_DIAG("capture: %s came back %dx%d, mean luma %.3f, spread %.3f%s",
                    SourceName(frame.source), frame.pixels.width, frame.pixels.height,
                    content.mean, content.spread,
                    Blank(content) ? " (blank, trying the next path)" : "");

        if (!Blank(content)) {
            frame.blank = false;
            MACTAB_DIAG("capture: %s, %dx%d at (%ld,%ld) in %.2f ms",
                        SourceName(frame.source),
                        frame.pixels.width, frame.pixels.height,
                        frame.bounds.left, frame.bounds.top, NowMs() - started);
            return frame;
        }

        best = std::move(frame);
    }

    // Everything available came back blank. Hand back the last one anyway: a
    // desktop that really is black is a case the material handles, and a black
    // backdrop drawn honestly is no worse than the flat coat that replaces it.
    MACTAB_WARN("capture: no path returned a usable frame (%s after %.2f ms)",
                SourceName(best.source), NowMs() - started);
    return best;
}

} // namespace mactab::capture
