#include "pch.h"
#include <d3d11.h>
#include <dxgi1_2.h>

#include "capture.h"
#include "com.h"
#include "common.h"
#include "diag.h"

namespace mactab::capture {
namespace {

// Total time we are willing to spend waiting for a duplicated frame. The reveal
// is never blocked on this — the panel shows with a flat tint and crossfades the
// blur in if it arrives late — so this only bounds the worker.
constexpr DWORD kAcquireTimeoutMs = 8;
constexpr int   kAcquireAttempts  = 6;

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

Frame GrabWithBitBlt(const RECT& rect) {
    Frame frame;

    const int width  = static_cast<int>(rect.right  - rect.left);
    const int height = static_cast<int>(rect.bottom - rect.top);
    if (width <= 0 || height <= 0) return frame;

    const HDC screen = ::GetDC(nullptr);
    if (!screen) return frame;

    const HDC memory = ::CreateCompatibleDC(screen);
    if (!memory) {
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

    if (dib && bits) {
        const HGDIOBJ previous = ::SelectObject(memory, dib);

        // CAPTUREBLT so layered windows above the desktop are included; without
        // it the backdrop would be missing exactly the translucent surfaces
        // that make a blur look convincing.
        if (::BitBlt(memory, 0, 0, width, height, screen, rect.left, rect.top,
                     SRCCOPY | CAPTUREBLT)) {
            frame.pixels = Bitmap::Create(width, height);
            std::memcpy(frame.pixels.pixels.data(), bits,
                        static_cast<size_t>(width) * height * 4);
            for (uint32_t& pixel : frame.pixels.pixels)
                pixel |= 0xFF000000u;
            frame.source = Source::GdiBitBlt;
            frame.bounds = rect;   // BitBlt reads the virtual screen, no clamping
        }

        ::SelectObject(memory, previous);
        ::DeleteObject(dib);
    }

    ::DeleteDC(memory);
    ::ReleaseDC(nullptr, screen);
    return frame;
}

} // namespace

const char* SourceName(Source source) {
    switch (source) {
    case Source::DesktopDuplication: return "desktop-duplication";
    case Source::GdiBitBlt:          return "gdi-bitblt";
    case Source::None:               break;
    }
    return "none";
}

Frame GrabRegion(const RECT& rect) {
    const double started = NowMs();

    Frame frame;

    if (InRemoteSession()) {
        MACTAB_DIAG("capture: remote session, skipping desktop duplication");
    } else {
        frame = GrabWithDuplication(rect);
    }

    if (frame.source == Source::None)
        frame = GrabWithBitBlt(rect);

    MACTAB_DIAG("capture: %s, %dx%d at (%ld,%ld) in %.2f ms",
                SourceName(frame.source),
                frame.pixels.width, frame.pixels.height,
                frame.bounds.left, frame.bounds.top, NowMs() - started);

    return frame;
}

void ReleaseCachedResources() {
    // Nothing cached yet: devices and the duplication object are created and
    // destroyed per grab, on purpose. If profiling later shows device creation
    // dominating, cache the device but never the duplication.
}

} // namespace mactab::capture
