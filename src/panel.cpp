#include "winrt_pch.h"

#include <d2d1_1.h>
#include <d2d1effects_2.h>
#include <d3d11.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <shellscalingapi.h>

#include <future>
#include <thread>

#include <DispatcherQueue.h>
#include <windows.ui.composition.interop.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Composition.Desktop.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.h>

#include "panel.h"
#include "capture.h"
#include "com.h"
#include "common.h"
#include "diag.h"
#include "geometry.h"
#include "config.h"
#include "glass.h"
#include "glass_map.h"
#include "panel_layout.h"

namespace WUC = winrt::Windows::UI::Composition;
namespace WUI = winrt::Windows::UI;
namespace WFN = winrt::Windows::Foundation::Numerics;
namespace WUCABI = ABI::Windows::UI::Composition;

namespace mactab {
namespace {

constexpr wchar_t kPanelClass[] = L"MacTabPanelWindow";

// Tile size, gap, padding, corner radius and the shrink-to-fit rule live in
// panel_layout.h, which is free of windows.h so tools/preview can render the
// real geometry natively. Only the rendering-specific constants stay here.
// kBlurSigma and kBlurDownscale live in glass.h with the rest of the material,
// so tools/preview reads the same numbers instead of a comment claiming it does.
using glass::kBlurDownscale;
using glass::kBlurSigma;

// Extra desktop captured around the panel so the blur has real pixels to pull
// from instead of clamping at the edge under D2D1_BORDER_MODE_HARD.
//
// Derived from sigma rather than fixed, and scaled with the DPI. It was 48 raw
// pixels, which was already thin against a 34px sigma and would have smeared
// visibly on a 150% display.
//
// It also has to cover the refraction, which pulls in from up to
// kMaxDisplacement outside the panel outline. 1.5 sigma is 45 logical pixels
// against a ceiling of 16, so that is covered with room to spare, but the two
// numbers are now coupled and moving sigma down far enough would break it.
inline float BlurMarginPx(float dpiScale) {
    return std::ceil(kBlurSigma * dpiScale * 1.5f);
}

bool SystemUsesLightTheme() {
    DWORD value = 0, size = sizeof(value);
    if (::RegGetValueW(HKEY_CURRENT_USER,
                       L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                       L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS)
        return value != 0;
    return false;   // Windows defaults to dark for apps when the key is absent
}

// "auto" follows the system app theme; "dark" and "light" pin it. Reading the
// setting here rather than only at startup means a change takes effect on the
// next gesture instead of the next launch.
bool ResolveLightTheme() {
    const std::wstring& choice = config::Current().theme;
    if (choice == L"light") return true;
    if (choice == L"dark")  return false;
    return SystemUsesLightTheme();
}

struct Theme {
    glass::Params material;     // saturation, gain, bias, tint, rim
    D2D1_COLOR_F  selection;
    WUI::Color    label;
};

Theme MakeTheme(bool light) {
    Theme theme{};
    theme.material  = light ? glass::kLight : glass::kDark;
    theme.selection = light ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.10f)
                            : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.18f);
    theme.label     = light ? WUI::Color{ 255, 20, 20, 22 }
                            : WUI::Color{ 255, 245, 245, 247 };
    return theme;
}

D2D1_COLOR_F TintColour(const glass::Params& p) {
    return D2D1::ColorF(p.tint[0], p.tint[1], p.tint[2], p.tint[3]);
}

// glass::BuildMatrix is constexpr and D2D-free, so it lives with the numbers it
// belongs to and tools/preview evaluates the identical coefficients. This just
// unpacks it into the type Direct2D wants. Written out longhand rather than
// memcpy'd over the union: the layout does match, but nothing checks that it
// still does, and this file is the one the syntax checker cannot see.
D2D1_MATRIX_5X4_F ToD2D(const glass::Matrix5x4& g) {
    return D2D1::Matrix5x4F(
        g.m[0][0], g.m[0][1], g.m[0][2], g.m[0][3],
        g.m[1][0], g.m[1][1], g.m[1][2], g.m[1][3],
        g.m[2][0], g.m[2][1], g.m[2][2], g.m[2][3],
        g.m[3][0], g.m[3][1], g.m[3][2], g.m[3][3],
        g.m[4][0], g.m[4][1], g.m[4][2], g.m[4][3]);
}

} // namespace

// ---------------------------------------------------------------------------

struct Panel::Impl {
    HINSTANCE instance = nullptr;
    HWND      hwnd         = nullptr;
    HWND      notifyWindow = nullptr;
    UINT      hoverMessage = 0;
    UINT      clickMessage = 0;
    int       hoveredIndex = -1;
    bool      visible  = false;
    bool      ready    = false;

    // Composition
    winrt::Windows::System::DispatcherQueueController dispatcher{ nullptr };
    WUC::Compositor                                   compositor{ nullptr };
    WUC::Desktop::DesktopWindowTarget                 target{ nullptr };
    WUC::CompositionGraphicsDevice                    graphics{ nullptr };

    WUC::ContainerVisual root{ nullptr };
    WUC::ContainerVisual content{ nullptr };
    WUC::SpriteVisual    backdropVisual{ nullptr };
    WUC::SpriteVisual    selectionVisual{ nullptr };
    WUC::ContainerVisual tileLayer{ nullptr };
    WUC::SpriteVisual    labelVisual{ nullptr };

    WUC::CompositionDrawingSurface backdropSurface{ nullptr };
    WUC::CompositionDrawingSurface labelSurface{ nullptr };

    std::vector<WUC::SpriteVisual> tileVisuals;

    // Direct2D / DirectWrite
    ComPtr<ID3D11Device>       d3dDevice;
    ComPtr<ID2D1Factory1>      d2dFactory;
    ComPtr<ID2D1Device>        d2dDevice;
    ComPtr<IDWriteFactory>     dwriteFactory;

    // State
    std::vector<PanelItem> items;
    int    selected  = 0;
    float  dpiScale  = 1.0f;
    float  tilePx    = layout::kTileSize;
    RECT   panelRect{};        // screen coords
    Theme  theme        = MakeTheme(false);
    bool   themeIsLight = false;

    // theme.material bent to suit the captured backdrop. Everything that draws
    // glass reads THIS, not theme.material: the panel, its rim and glow, and the
    // app name's capsule all have to be the same piece of material.
    glass::Params material = glass::kDark;
    HMONITOR monitor = nullptr;
    int      laidOutCount   = -1;   // skips a redundant second Layout per gesture

    // The panel's corner extent in physical pixels, after the clamp in
    // layout::Compute. Everything tracing the panel outline reads this rather
    // than scaling layout::kPanelRadius itself, so two surfaces cannot clamp
    // independently and disagree.
    float    panelRadiusPx = layout::kPanelRadius;

    // Height of the strip below the glass that holds the app name. The window
    // is taller than panelRect by exactly this much.
    float    labelBandPx   = 0.0f;

    bool CreateDevices();
    bool CreatePanelWindow();
    void RecoverDevices();
    bool CreateVisualTree();

    // The captured desktop frame, in flight. Started when the gesture begins and
    // consumed when the panel is actually revealed.
    std::future<capture::Frame> pendingCapture;
    capture::Frame              lastFrame;   // reused when relaying out while visible

    void Layout(int count);
    void BakeSelection();
    void StartCapture();
    void CollectFrame();
    void DrawGlass(ID2D1DeviceContext* dc, POINT surfaceOffset,
                   const RECT& screenRect, float radius);
    void BakeBackdrop();
    void DrawOuterStroke(ID2D1DeviceContext* dc, const D2D1_MATRIX_3X2_F& toSurface,
                         float width, float height, float radius);
    void DrawEdgeLight(ID2D1DeviceContext* dc, const glass::Surface& surface);
    void BakeLabel();
    float CapsuleLuma(const RECT& screenRect) const;
    void PositionTiles(bool animate);
    void UploadIcon(size_t index);
    int  HitTestScreen(POINT screenPoint) const;

    float Scaled(float logical) const { return logical * dpiScale; }
};

namespace {

// Fill a CompositionDrawingSurface via its D2D interop, returning the device
// context positioned at the surface's offset inside its atlas.
struct SurfaceDraw {
    ComPtr<ID2D1DeviceContext> dc;
    POINT                      offset{};
    winrt::com_ptr<WUCABI::ICompositionDrawingSurfaceInterop> interop;
    bool ok = false;

    explicit SurfaceDraw(const WUC::CompositionDrawingSurface& surface) {
        interop = surface.try_as<WUCABI::ICompositionDrawingSurfaceInterop>();
        if (!interop) return;
        ok = SUCCEEDED(interop->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext),
                                          dc.PutVoid(), &offset));
    }
    ~SurfaceDraw() {
        if (ok && interop) interop->EndDraw();
    }
};

bool IsDeviceLost(HRESULT hr) {
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET ||
           hr == D2DERR_RECREATE_TARGET;
}

} // namespace

// Every C++/WinRT call throws on failure, and these run inside a window
// procedure, an escaping hresult_error would unwind straight out of wWinMain
// into std::terminate. Device loss after a driver update or a resume is not
// hypothetical for a process that lives for weeks in a tray, so the panel has
// to survive it rather than take the app down with it.
template <typename F>
static bool GuardPanel(Panel::Impl& impl, const char* what, F&& fn) {
    if (!impl.ready) return false;
    try {
        fn();
        return true;
    } catch (const winrt::hresult_error& e) {
        const HRESULT hr = e.code();
        MACTAB_FAIL("panel: %s threw 0x%08lX", what, static_cast<unsigned long>(hr));
        if (IsDeviceLost(hr)) {
            // Recovery creates WinRT objects too, so it can throw, and a throw
            // from inside a catch handler lands right back in std::terminate,
            // which is the exact path this guard exists to close.
            try {
                impl.RecoverDevices();
            } catch (...) {
                MACTAB_FAIL("panel: device recovery failed; panel disabled");
                impl.ready = false;
            }
        }
        return false;
    } catch (...) {
        MACTAB_FAIL("panel: %s threw a non-WinRT exception", what);
        return false;
    }
}

// ---------------------------------------------------------------------------

Panel::Panel() : m_impl(std::make_unique<Impl>()) {}
Panel::~Panel() { Shutdown(); }

bool Panel::Impl::CreateDevices() {
    // D3D11 device shared by D2D and the composition graphics device.
    // BGRA support is mandatory for D2D interop.
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL level{};

    HRESULT hr = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                     nullptr, 0, D3D11_SDK_VERSION,
                                     d3dDevice.Put(), &level, nullptr);
    if (FAILED(hr)) {
        // Machines with no usable GPU (some VMs, Basic Display driver) still
        // run DWM, so WARP keeps the panel working rather than failing outright.
        MACTAB_WARN("panel: hardware D3D device failed (0x%08lX), trying WARP",
                    static_cast<unsigned long>(hr));
        hr = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
                                 nullptr, 0, D3D11_SDK_VERSION,
                                 d3dDevice.Put(), &level, nullptr);
    }
    if (FAILED(hr)) {
        MACTAB_FAIL("panel: no D3D11 device available (0x%08lX)",
                    static_cast<unsigned long>(hr));
        return false;
    }

    D2D1_FACTORY_OPTIONS options{};
    if (FAILED(::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                   __uuidof(ID2D1Factory1), &options,
                                   d2dFactory.PutVoid()))) {
        MACTAB_FAIL("panel: D2D1CreateFactory failed");
        return false;
    }

    ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(d3dDevice->QueryInterface(IID_PPV_ARGS(dxgiDevice.Put()))) ||
        FAILED(d2dFactory->CreateDevice(dxgiDevice.Get(), d2dDevice.Put()))) {
        MACTAB_FAIL("panel: could not create the D2D device");
        return false;
    }

    if (FAILED(::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                     reinterpret_cast<IUnknown**>(dwriteFactory.Put())))) {
        MACTAB_FAIL("panel: DWriteCreateFactory failed");
        return false;
    }

    // Hand the D2D device to the compositor.
    auto interop = compositor.as<WUCABI::ICompositorInterop>();
    winrt::com_ptr<WUCABI::ICompositionGraphicsDevice> abiGraphics;
    if (FAILED(interop->CreateGraphicsDevice(d2dDevice.Get(), abiGraphics.put()))) {
        MACTAB_FAIL("panel: CreateGraphicsDevice failed");
        return false;
    }
    graphics = abiGraphics.as<WUC::CompositionGraphicsDevice>();
    return true;
}

// Mouse handling.
//
// WS_EX_NOREDIRECTIONBITMAP means the window has no surface to hit-test
// against, so the whole rect receives input and the tile under the pointer has
// to be resolved against the layout. That is the design, not a workaround.
LRESULT CALLBACK PanelWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* impl = reinterpret_cast<Panel::Impl*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!impl)
        return ::DefWindowProcW(hwnd, msg, wParam, lParam);

    if (msg == WM_MOUSEMOVE || msg == WM_LBUTTONUP) {
        POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ::ClientToScreen(hwnd, &point);
        const int index = impl->HitTestScreen(point);

        if (msg == WM_LBUTTONUP) {
            if (index >= 0 && impl->notifyWindow)
                ::PostMessageW(impl->notifyWindow, impl->clickMessage,
                               static_cast<WPARAM>(index), 0);
        } else if (index != impl->hoveredIndex) {
            impl->hoveredIndex = index;
            if (impl->notifyWindow)
                ::PostMessageW(impl->notifyWindow, impl->hoverMessage,
                               static_cast<WPARAM>(static_cast<INT_PTR>(index)), 0);
        }
        return 0;
    }

    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool Panel::Impl::CreatePanelWindow() {
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = PanelWndProc;
    wc.hInstance     = instance;
    wc.lpszClassName = kPanelClass;
    wc.hCursor       = ::LoadCursorW(nullptr, IDC_ARROW);
    ::RegisterClassExW(&wc);

    // WS_EX_NOREDIRECTIONBITMAP, deliberately NOT WS_EX_LAYERED.
    //
    // It removes the GDI redirection surface so DWM composes our visual tree
    // directly. That kills the blank/flashing first frame a layered window
    // gives you, and avoids UpdateLayeredWindow entirely. The documented
    // consequence is that there is no per-pixel hit testing; the whole window
    // rect takes mouse input, which for a switcher is exactly what we want,
    // and is why HitTest() is a plain layout test.
    //
    // WS_EX_NOACTIVATE keeps the panel from stealing foreground, so the
    // activation logic in activate.cpp stays honest.
    hwnd = ::CreateWindowExW(
        WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kPanelClass, L"MacTab", WS_POPUP,
        0, 0, 1, 1, nullptr, nullptr, instance, nullptr);

    if (!hwnd) {
        MACTAB_FAIL("panel: CreateWindowEx failed (err %lu)", ::GetLastError());
        return false;
    }

    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    return true;
}

bool Panel::Impl::CreateVisualTree() {
    auto interop = compositor.as<ABI::Windows::UI::Composition::Desktop::ICompositorDesktopInterop>();
    if (FAILED(interop->CreateDesktopWindowTarget(
            hwnd, false,
            reinterpret_cast<ABI::Windows::UI::Composition::Desktop::IDesktopWindowTarget**>(
                winrt::put_abi(target))))) {
        MACTAB_FAIL("panel: CreateDesktopWindowTarget failed");
        return false;
    }

    root = compositor.CreateContainerVisual();
    root.RelativeSizeAdjustment({ 1.0f, 1.0f });
    target.Root(root);

    // Order matters: the glass, then the selection highlight, then tiles, then
    // the label.
    content         = compositor.CreateContainerVisual();
    backdropVisual  = compositor.CreateSpriteVisual();
    selectionVisual = compositor.CreateSpriteVisual();
    tileLayer       = compositor.CreateContainerVisual();
    labelVisual     = compositor.CreateSpriteVisual();

    // Everything that belongs to the panel proper hangs off `content`, so every
    // layer positions in panel-local space and only one visual carries the
    // window offset.
    root.Children().InsertAtTop(content);

    auto contentChildren = content.Children();
    contentChildren.InsertAtTop(backdropVisual);
    contentChildren.InsertAtTop(selectionVisual);
    contentChildren.InsertAtTop(tileLayer);
    contentChildren.InsertAtTop(labelVisual);

    // The whole panel fades and scales as one unit.
    root.Opacity(0.0f);
    return true;
}

bool Panel::Initialize(HINSTANCE instance, HWND notifyWindow,
                       UINT hoverMessage, UINT clickMessage) {
    Impl& impl = *m_impl;
    impl.instance     = instance;
    impl.notifyWindow = notifyWindow;
    impl.hoverMessage = hoverMessage;
    impl.clickMessage = clickMessage;

    // Explicit STA.
    //
    // Microsoft's own Win32 Composition sample passes DQTAT_COM_ASTA here, but
    // that field is documented as relevant only when threadType is
    // DQTYPE_THREAD_DEDICATED, with DQTYPE_THREAD_CURRENT it is ignored and
    // initialises nothing. A Compositor has thread affinity and WinRT
    // activation on an uninitialised thread silently lands in the MTA, so the
    // apartment has to be established here.
    try {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
    } catch (const winrt::hresult_error& e) {
        MACTAB_FAIL("panel: init_apartment failed (0x%08lX)",
                    static_cast<unsigned long>(e.code()));
        return false;
    }

    DispatcherQueueOptions options{ sizeof(DispatcherQueueOptions),
                                    DQTYPE_THREAD_CURRENT, DQTAT_COM_NONE };
    if (FAILED(::CreateDispatcherQueueController(
            options,
            reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(
                winrt::put_abi(impl.dispatcher))))) {
        MACTAB_FAIL("panel: CreateDispatcherQueueController failed");
        return false;
    }

    try {
        impl.compositor = WUC::Compositor();
    } catch (const winrt::hresult_error& e) {
        MACTAB_FAIL("panel: Compositor construction failed (0x%08lX)",
                    static_cast<unsigned long>(e.code()));
        return false;
    }

    impl.themeIsLight = ResolveLightTheme();
    impl.theme        = MakeTheme(impl.themeIsLight);

    if (!impl.CreatePanelWindow()) return false;
    if (!impl.CreateDevices())     return false;
    if (!impl.CreateVisualTree())  return false;

    impl.ready = true;
    MACTAB_DIAG("panel: initialised and pre-warmed");
    return true;
}

void Panel::Shutdown() {
    Impl& impl = *m_impl;
    if (!impl.ready && !impl.hwnd) return;

    impl.ready = false;

    // B4: a capture may still be running on a detached thread inside DXGI/GDI.
    // Let it finish before COM and the CRT start tearing down underneath it.
    if (impl.pendingCapture.valid()) {
        impl.pendingCapture.wait_for(std::chrono::milliseconds(500));
        impl.pendingCapture = {};
    }

    // Release the Composition graph HERE, not from Impl's destructor.
    //
    // g_app is a global, so its destructor runs during CRT exit; after
    // wWinMain has returned and this thread has stopped pumping its dispatcher
    // queue. Releasing thread-affine Composition objects and a live
    // DispatcherQueueController at that point is a well-known exit hang/crash.
    try {
        if (impl.target) impl.target.Root(nullptr);

        impl.tileVisuals.clear();
        impl.labelVisual     = nullptr;
        impl.tileLayer       = nullptr;
        impl.selectionVisual = nullptr;
        impl.backdropVisual  = nullptr;
        impl.content         = nullptr;
        impl.root            = nullptr;

        impl.backdropSurface = nullptr;
        impl.labelSurface    = nullptr;

        impl.target     = nullptr;
        impl.graphics    = nullptr;
        impl.compositor  = nullptr;
        impl.dispatcher  = nullptr;
    } catch (const winrt::hresult_error& e) {
        MACTAB_WARN("panel: teardown threw 0x%08lX",
                    static_cast<unsigned long>(e.code()));
    }

    impl.dwriteFactory.Reset();
    impl.d2dDevice.Reset();
    impl.d2dFactory.Reset();
    impl.d3dDevice.Reset();

    if (impl.hwnd) {
        ::DestroyWindow(impl.hwnd);
        impl.hwnd = nullptr;
    }
}

void Panel::PrepareLayout(int itemCount) {
    Impl& impl = *m_impl;
    GuardPanel(impl, "PrepareLayout", [&] { impl.Layout(itemCount); });
}

bool Panel::Ready() const   { return m_impl->ready; }
bool Panel::Visible() const { return m_impl->visible; }
HWND Panel::Hwnd() const    { return m_impl->hwnd; }
int  Panel::TileSizePx() const {
    return static_cast<int>(std::lround(m_impl->tilePx));
}

// Rebuild the graphics stack after device loss.
//
// The compositor, the desktop target and the visual tree all survive a lost
// adapter, only the D3D/D2D devices and anything drawn through them do not.
// So this recreates the devices and the surfaces baked at startup; per-gesture
// surfaces are rebuilt by the next SetItems anyway.
void Panel::Impl::RecoverDevices() {
    MACTAB_WARN("panel: graphics device lost, rebuilding");

    graphics = nullptr;
    dwriteFactory.Reset();
    d2dDevice.Reset();
    d2dFactory.Reset();
    d3dDevice.Reset();

    backdropSurface = nullptr;
    labelSurface    = nullptr;

    if (!CreateDevices()) {
        MACTAB_FAIL("panel: could not rebuild the graphics device; panel disabled");
        ready = false;
        return;
    }

    try {
        BakeSelection();
    } catch (const winrt::hresult_error& e) {
        MACTAB_FAIL("panel: re-baking after device loss threw 0x%08lX",
                    static_cast<unsigned long>(e.code()));
        ready = false;
        return;
    }

    MACTAB_DIAG("panel: graphics device rebuilt");
}

// Kick the desktop grab off the UI thread.
//
// This is started at gesture BEGIN and consumed at reveal, which matters for
// two reasons. Capturing synchronously would put a full screen grab plus a
// Gaussian blur on the reveal path and blow the one-frame budget outright. Worse,
// BEGIN fires on the first Tab, before the hold delay has decided whether the
// panel will appear at all, so a synchronous capture would make every quick
// Alt+Tab, the single most common operation, pay for a backdrop nobody ever sees.
//
// A packaged_task rather than std::async: a future from std::async blocks in its
// own destructor until the task completes, which would reintroduce exactly the
// stall this exists to avoid whenever a capture is abandoned.
void Panel::Impl::StartCapture() {
    const int margin = static_cast<int>(BlurMarginPx(dpiScale));

    // Asymmetric at the bottom, because the app name's capsule lives down there
    // and is glass too. It sits labelGap + labelHeight below panelRect and needs
    // its own blur reach underneath that, so a symmetric margin leaves the
    // capsule's lower half blurred against a clamped edge, and a relayout that
    // grows the panel can push the capsule out of the captured region entirely.
    RECT captureRect = panelRect;
    ::InflateRect(&captureRect, margin, margin);
    captureRect.bottom += static_cast<int>(std::ceil(labelBandPx));

    auto task = std::make_shared<std::packaged_task<capture::Frame()>>(
        [captureRect] { return capture::GrabRegion(captureRect); });

    pendingCapture = task->get_future();
    std::thread([task] { (*task)(); }).detach();
}

// Selection highlight.
//
// A SpriteVisual with a colour brush is a hard-edged rectangle, which next to
// squircle icons reads immediately as wrong. The highlight needs the same
// rounded shape, and it has to resize as the tile size changes, so it is baked
// once as a squircle and stretched with a nine-grid.
//
// Same corner as the backdrop, literally: same extent, same exponent.
//
// It used to be 0.22 of the tile, which came out at 28px against the panel's 62
// and read as a noticeably tighter, squarer shape sitting inside a rounder one.
// The highlight is a smaller rectangle nested in the panel, so the two corners
// are seen together and any difference between them reads as a mistake rather
// than as a hierarchy.
//
// Clamped to half the highlight's own size for the shrink-to-fit case: at
// kMinTileSize the box is only 45 across and a 62 extent has no meaning there.
void Panel::Impl::BakeSelection() {
    const float tile = (tilePx > 0.0f) ? tilePx : Scaled(layout::kTileSize);
    const float box  = tile * (1.0f + 2.0f * layout::kSelectionInset);

    const float radius = (std::min)(panelRadiusPx, box * 0.5f);
    const int   cell   = static_cast<int>(std::ceil(radius)) + 2;
    const int   size   = cell * 2 + 4;

    auto surface = graphics.CreateDrawingSurface(
        { static_cast<float>(size), static_cast<float>(size) },
        winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
        winrt::Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);

    {
        SurfaceDraw draw(surface);
        if (!draw.ok) {
            // Same reason BakeLabel clears first: called from RecoverDevices,
            // a bail-out here leaves the highlight pointed at a surface from
            // the device that was just destroyed.
            MACTAB_WARN("panel: selection BeginDraw failed");
            selectionVisual.Brush(nullptr);
            return;
        }

        draw.dc->Clear(D2D1::ColorF(0, 0, 0, 0));

        auto geometry = CreateSquircleGeometry(d2dFactory.Get(),
                                               static_cast<float>(size),
                                               static_cast<float>(size), radius,
                                               layout::kPanelCornerExponent);
        if (!geometry) {
            selectionVisual.Brush(nullptr);
            return;
        }

        ComPtr<ID2D1SolidColorBrush> brush;
        draw.dc->CreateSolidColorBrush(theme.selection, brush.Put());

        draw.dc->SetTransform(D2D1::Matrix3x2F::Translation(
            static_cast<float>(draw.offset.x), static_cast<float>(draw.offset.y)));
        draw.dc->FillGeometry(geometry.Get(), brush.Get());
        draw.dc->SetTransform(D2D1::Matrix3x2F::Identity());
    }

    auto nine = compositor.CreateNineGridBrush();
    nine.Source(compositor.CreateSurfaceBrush(surface));
    nine.SetInsets(static_cast<float>(cell));

    // Belt and braces against the degenerate nine-grid case: two insets must
    // leave a middle to stretch, or the brush stops rendering.
    nine.SetInsetScales((std::min)(1.0f, box * 0.98f / (2.0f * cell)));

    selectionVisual.Brush(nine);
}

// Upload a CPU bitmap for use as a D2D effect input or a DrawBitmap source.
//
// Takes the bitmap by value because it has to premultiply it, and the generated
// maps are built fresh per gesture anyway.
ComPtr<ID2D1Bitmap1> UploadBitmap(ID2D1DeviceContext* dc, Bitmap image) {
    ComPtr<ID2D1Bitmap1> result;
    if (image.Empty()) return result;

    PremultiplyInPlace(image);

    const D2D1_SIZE_U size{ static_cast<UINT32>(image.width),
                            static_cast<UINT32>(image.height) };
    D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));

    if (FAILED(dc->CreateBitmap(size, image.pixels.data(),
                                static_cast<UINT32>(image.width * 4),
                                &props, result.Put()))) {
        result = nullptr;
    }
    return result;
}

// The dark outer stroke.
//
// All that is left of the old rim. The bright half of it is now the lit edge,
// which comes off the surface normal instead of a vertical gradient, so it lands
// on the corners correctly instead of reading the same all the way down the
// sides.
//
// This one still has to be a stroke rather than part of the generated map: it is
// drawn OUTSIDE the clip layer, on the antialiased boundary itself, and a CPU
// bitmap composited source-over would double the coverage there.
//
// INSET by half its own width rather than centred on the panel outline. D2D
// centres strokes on the path, the surface is exactly panel-sized with no
// margin, and the surface edge is a rectangle while the panel outline is not: a
// centred stroke therefore loses its outer half along the straight runs and
// keeps almost all of it through the corners, so it would visibly thicken at the
// corners at anything above 100% DPI. Inset, the whole stroke stays inside the
// surface and the width is uniform.
//
// The radius is inset by the same amount, which is what keeps the inner outline
// concentric with the outer one. Reusing the outer radius pinches the corners.
void Panel::Impl::DrawOuterStroke(ID2D1DeviceContext* dc,
                                  const D2D1_MATRIX_3X2_F& toSurface,
                                  float width, float height, float radius) {
    if (material.rimOuterDark <= 0.0f) return;

    const float sw    = Scaled(1.0f);
    const float inset = sw * 0.5f;

    auto geometry = CreateSquircleGeometry(d2dFactory.Get(),
                                           width  - inset * 2.0f,
                                           height - inset * 2.0f,
                                           radius - inset,
                                           layout::kPanelCornerExponent);
    if (!geometry) return;

    ComPtr<ID2D1SolidColorBrush> dark;
    if (FAILED(dc->CreateSolidColorBrush(
            D2D1::ColorF(0.0f, 0.0f, 0.0f, material.rimOuterDark), dark.Put()))) {
        return;
    }

    dc->SetTransform(D2D1::Matrix3x2F::Translation(inset, inset) * toSurface);
    dc->DrawGeometry(geometry.Get(), dark.Get(), sw);
    dc->SetTransform(toSurface);
}

// The lit edge: ambient over the whole rim, a lobe where the surface faces up,
// and a filament along the top run. See glass_map.h for what each is and where
// the amounts came from.
//
// Generated on the CPU and added, rather than stroked. The whole point is that
// the amount depends on which way the surface faces, and there is no brush that
// varies with a normal.
//
// ADD, not source-over. The bitmap is premultiplied white with the amount in its
// alpha, so adding it adds exactly that amount. It carries the shape's own
// antialiased coverage, which is why this can be drawn after the clip layer is
// popped and does not need a second one.
void Panel::Impl::DrawEdgeLight(ID2D1DeviceContext* dc, const glass::Surface& surface) {
    ComPtr<ID2D1Bitmap1> light =
        UploadBitmap(dc, glass::BuildEdgeLight(surface, material));
    if (!light) return;

    dc->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_ADD);
    dc->DrawBitmap(light.Get(),
                   D2D1::RectF(0.0f, 0.0f, surface.width, surface.height),
                   1.0f, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
    dc->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
}

// ---------------------------------------------------------------------------
// Backdrop
//
// Captured desktop -> downscale -> Gaussian blur -> colour matrix -> upscale ->
// tint -> rim, all clipped to the squircle by a D2D layer. Because the source is
// a single frozen frame, this is a draw-time operation rather than a live effect
// graph, which is what lets us avoid Win2D and a hand-rolled
// IGraphicsEffectD2D1Interop entirely.
//
// The colour matrix sits AFTER the blur, at quarter resolution. It is per-pixel
// linear and the blur is spatially linear, so the two commute and this is the
// cheaper of the two orderings by a factor of sixteen. See glass.h for what the
// matrix is doing and why it is not a saturation effect.
// Collect the desktop grab and work out the material from it.
//
// Separate from BakeBackdrop because the app name's capsule is a second piece of
// glass cut from the same frame, and it is baked before the backdrop is. Both
// have to agree about which frame they are drawing and which parameters they are
// drawing it with, so both of those are decided exactly once, here.
void Panel::Impl::CollectFrame() {
    // The capture was started at gesture begin and has had the whole hold delay
    // to finish, so in practice this never waits. It is bounded anyway, because
    // a wedged GPU must degrade to a flat fallback rather than stall the reveal.
    if (pendingCapture.valid()) {
        if (pendingCapture.wait_for(std::chrono::milliseconds(10)) ==
            std::future_status::ready) {
            lastFrame = pendingCapture.get();
        } else {
            // Do NOT fall through to the previous lastFrame: after the first
            // gesture that holds a stale desktop, and a visibly outdated blur
            // reads worse than no blur at all.
            MACTAB_WARN("panel: capture not ready at reveal, using the flat fallback");
            lastFrame = {};
        }
        pendingCapture = {};
    }

    // A relayout while the panel is already up reuses the frame captured at
    // gesture begin, and that frame only covers the panel as it was then.
    // Expanding an app with more windows than there are apps makes the panel
    // WIDER, and because the blur runs with D2D1_BORDER_MODE_HARD the effect's
    // output rect is exactly its input rect: it does not extend. The strips
    // beyond the old capture would get no backdrop at all, just tint over
    // nothing, visibly two-toned against the blurred middle.
    //
    // Re-capturing is not an option, our own panel is on screen by then, so
    // drop to the flat fallback instead. Shrinking is fine and stays blurred.
    //
    // The test covers the capsule as well as the panel: it is the piece that
    // hangs lowest, so it is the one that leaves the frame first.
    if (!lastFrame.pixels.Empty() &&
        (panelRect.left   < lastFrame.bounds.left  ||
         panelRect.top    < lastFrame.bounds.top   ||
         panelRect.right  > lastFrame.bounds.right ||
         panelRect.bottom + std::ceil(labelBandPx) > lastFrame.bounds.bottom)) {
        MACTAB_WARN("panel: relayout grew past the captured region, using the flat fallback");
        lastFrame = {};
    }

    // Bend the material to what is actually behind the panel.
    //
    // This is the whole reason a frozen frame is an advantage rather than a
    // limitation: the mean luma of the backdrop is known before a single pixel
    // is drawn, so the operating point can move to suit it. A fixed transfer
    // washes out over a white wallpaper and turns into a slab over a black one,
    // which is what 0.2.0 did and what the user reported.
    //
    // With no frame there is no mean, so the base parameters stand.
    if (lastFrame.pixels.Empty()) {
        material = theme.material;
        return;
    }

    const uint32_t mean = MeanColourIn(lastFrame.pixels,
                                       panelRect.left - lastFrame.bounds.left,
                                       panelRect.top  - lastFrame.bounds.top,
                                       panelRect.right  - lastFrame.bounds.left,
                                       panelRect.bottom - lastFrame.bounds.top);

    const float backdropLuma = glass::Luma(RedOf(mean)   / 255.0f,
                                           GreenOf(mean) / 255.0f,
                                           BlueOf(mean)  / 255.0f);
    material = glass::Adapt(theme.material, backdropLuma);

    const glass::Optics optics =
        glass::OpticsFor({ static_cast<float>(panelRect.right - panelRect.left),
                           static_cast<float>(panelRect.bottom - panelRect.top),
                           panelRadiusPx, dpiScale });

    MACTAB_DIAG("panel: backdrop luma %.3f, bias %.3f -> %.3f, panel lands at %.3f, "
                "bezel %.1f px, peak lens %.1f px, refraction %s",
                backdropLuma, theme.material.bias, material.bias,
                glass::PanelLuma(material, backdropLuma),
                optics.bezel, glass::Displacement(optics.bezel * 0.05f, optics),
                config::Current().glassRefraction ? "on" : "off");
}

// One piece of glass, cut from the captured frame at `screenRect` and drawn into
// the current surface with its top-left at the origin.
//
// Shared by the panel and by the app name's capsule, which are two pieces of the
// same material and have to stay that way. `radius` is the corner extent;
// passing half the height gives a capsule.
void Panel::Impl::DrawGlass(ID2D1DeviceContext* dc, POINT surfaceOffset,
                            const RECT& screenRect, float radius) {
    const float width  = static_cast<float>(screenRect.right  - screenRect.left);
    const float height = static_cast<float>(screenRect.bottom - screenRect.top);
    if (width <= 0.0f || height <= 0.0f) return;

    auto geometry = CreateSquircleGeometry(d2dFactory.Get(), width, height, radius,
                                           layout::kPanelCornerExponent);
    if (!geometry) return;

    const D2D1_MATRIX_3X2_F toSurface =
        D2D1::Matrix3x2F::Translation(static_cast<float>(surfaceOffset.x),
                                      static_cast<float>(surfaceOffset.y));
    dc->SetTransform(toSurface);

    ComPtr<ID2D1Layer> layer;
    dc->CreateLayer(nullptr, layer.Put());
    dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), geometry.Get(),
                                         D2D1_ANTIALIAS_MODE_PER_PRIMITIVE),
                  layer.Get());

    const capture::Frame& frame = lastFrame;
    const glass::Surface  glassSurface{ width, height, radius, dpiScale };

    if (!frame.pixels.Empty()) {
        ComPtr<ID2D1Bitmap1> captured = UploadBitmap(dc, frame.pixels);

        // The downscale is shared by both taps, which is most of what makes the
        // second one nearly free: it blurs a quarter-resolution image with a
        // sigma of two.
        ComPtr<ID2D1Effect> scale;
        if (captured) dc->CreateEffect(CLSID_D2D1Scale, scale.Put());
        if (scale) {
            scale->SetInput(0, captured.Get());
            scale->SetValue(D2D1_SCALE_PROP_SCALE,
                            D2D1::Vector2F(kBlurDownscale, kBlurDownscale));
        }

        // Where the capture ACTUALLY landed, not the margin we asked for: near a
        // screen edge the grab is clamped to the monitor and comes back shifted,
        // and assuming the requested origin slides the blur sideways.
        const float dx = static_cast<float>(frame.bounds.left - screenRect.left);
        const float dy = static_cast<float>(frame.bounds.top  - screenRect.top);

        ComPtr<ID2D1Bitmap1> map;
        if (scale && config::Current().glassRefraction)
            map = UploadBitmap(dc, glass::BuildDisplacementMap(glassSurface));

        // One tap: blur, treat, place at this piece's origin, then bend.
        //
        // A lambda because the two taps differ in exactly one number. Written out
        // twice they drift, and a rim built from a different material than the
        // interior it sits in reads as a colour seam rather than as a bug.
        auto buildTap = [&](float sigma) -> ComPtr<ID2D1Effect> {
            ComPtr<ID2D1Effect> blur, matrix, place;
            dc->CreateEffect(CLSID_D2D1GaussianBlur, blur.Put());
            dc->CreateEffect(CLSID_D2D1ColorMatrix, matrix.Put());
            dc->CreateEffect(CLSID_D2D12DAffineTransform, place.Put());
            if (!blur || !matrix || !place) return {};

            blur->SetInputEffect(0, scale.Get());
            blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION,
                           Scaled(sigma) * kBlurDownscale);
            // HARD border mode: SOFT would fade the blur toward transparent at
            // the capture edges and halo the panel.
            blur->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE,
                           D2D1_BORDER_MODE_HARD);

            matrix->SetInputEffect(0, blur.Get());
            matrix->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX,
                             ToD2D(glass::BuildMatrix(material)));
            // The desktop grab is opaque everywhere, so premultiplied and
            // straight are numerically the same here and the bias needs no
            // division. Left at the default rather than set, so that stays true
            // by construction if the input ever gains an alpha channel and
            // somebody has to think about it.
            //
            // Clamping is NOT optional: a saturation of 2 drives strongly
            // coloured pixels well out of range in both directions, and those
            // want clipping at the effect rather than wherever the upscale
            // interpolator meets them next.
            matrix->SetValue(D2D1_COLORMATRIX_PROP_CLAMP_OUTPUT, TRUE);

            // Undo the downscale and shift so this piece's own area sits at the
            // origin. This used to be done with the device context transform,
            // which cannot work any more: the displacement map is a second input
            // and both inputs have to share a coordinate space, so the placement
            // has to happen inside the graph.
            place->SetInputEffect(0, matrix.Get());
            place->SetValue(D2D1_2DAFFINETRANSFORM_PROP_TRANSFORM_MATRIX,
                            D2D1::Matrix3x2F::Scale(1.0f / kBlurDownscale,
                                                    1.0f / kBlurDownscale) *
                            D2D1::Matrix3x2F::Translation(dx, dy));
            place->SetValue(D2D1_2DAFFINETRANSFORM_PROP_INTERPOLATION_MODE,
                            D2D1_2DAFFINETRANSFORM_INTERPOLATION_MODE_LINEAR);

            if (!map) return place;

            // Refraction. The map is panel-local and starts at the origin, which
            // is exactly where the step above puts the backdrop, so the two line
            // up by construction rather than by arithmetic.
            //
            // The effect samples at p + scale * (channel - 0.5), so a scale of
            // twice the ceiling makes the encoding in glass_map.h exact.
            ComPtr<ID2D1Effect> lens;
            dc->CreateEffect(CLSID_D2D1DisplacementMap, lens.Put());
            if (!lens) return place;

            lens->SetInputEffect(0, place.Get());
            lens->SetInput(1, map.Get());
            lens->SetValue(D2D1_DISPLACEMENTMAP_PROP_SCALE,
                           2.0f * Scaled(glass::kMaxDisplacement));
            lens->SetValue(D2D1_DISPLACEMENTMAP_PROP_X_CHANNEL_SELECT,
                           D2D1_CHANNEL_SELECTOR_R);
            lens->SetValue(D2D1_DISPLACEMENTMAP_PROP_Y_CHANNEL_SELECT,
                           D2D1_CHANNEL_SELECTOR_G);
            return lens;
        };

        ComPtr<ID2D1Effect> frosted = scale ? buildTap(kBlurSigma) : ComPtr<ID2D1Effect>{};
        if (frosted) {
            dc->DrawImage(frosted.Get(), D2D1_INTERPOLATION_MODE_LINEAR);

            // The bezel band, refracted out of a much lighter blur and masked to
            // the rim.
            //
            // This is what makes the lens visible. With the frosted tap alone the
            // panel renders pixel for pixel the same with the displacement on and
            // with it off, because a 30px sigma leaves no edges to bend. Only
            // built when refraction is on: a clear band that bends nothing is
            // just a hole in the frosting.
            ComPtr<ID2D1Bitmap1> rimMask;
            if (map) rimMask = UploadBitmap(dc, glass::BuildRimMask(glassSurface));

            ComPtr<ID2D1Effect> clear, masked;
            if (rimMask) clear = buildTap(glass::kRimTapSigma);
            if (clear)   dc->CreateEffect(CLSID_D2D1AlphaMask, masked.Put());

            if (masked) {
                masked->SetInputEffect(0, clear.Get());
                masked->SetInput(1, rimMask.Get());
                dc->DrawImage(masked.Get(), D2D1_INTERPOLATION_MODE_LINEAR);
            }
        }
    }

    const D2D1_RECT_F area = D2D1::RectF(0.0f, 0.0f, width, height);

    // No backdrop at all: lay down a nearly opaque base coat first.
    //
    // The tint on its own is 14%, which over a sharp live desktop leaves the
    // tiles and the label floating on essentially nothing. 0.1.0 never needed
    // this because its tint was 0.55 and stood up alone. The machines that reach
    // this path (a wedged GPU, a remote session, a capture that missed its
    // deadline) are exactly the ones nobody tests on.
    if (frame.pixels.Empty()) {
        const D2D1_COLOR_F tint = TintColour(material);
        ComPtr<ID2D1SolidColorBrush> base;
        if (SUCCEEDED(dc->CreateSolidColorBrush(
                D2D1::ColorF(tint.r, tint.g, tint.b, material.fallbackAlpha), base.Put()))) {
            dc->FillRectangle(area, base.Get());
        }
    }

    ComPtr<ID2D1SolidColorBrush> tintBrush;
    if (SUCCEEDED(dc->CreateSolidColorBrush(TintColour(material), tintBrush.Put())))
        dc->FillRectangle(area, tintBrush.Get());

    dc->PopLayer();

    // Both outside the clip layer. The stroke is drawn on the antialiased
    // boundary itself, and the lit edge carries the shape's coverage in its own
    // alpha, so neither needs one.
    DrawOuterStroke(dc, toSurface, width, height, radius);
    DrawEdgeLight(dc, glassSurface);

    dc->SetTransform(D2D1::Matrix3x2F::Identity());
}

// Captured desktop -> downscale -> blur -> colour matrix -> upscale -> refract ->
// tint, all clipped to the squircle by a D2D layer, then the outer stroke and
// the lit edge over the top of it. Because the source is a single frozen frame
// this is a draw-time operation rather than a live effect graph, which is what
// lets us avoid Win2D and a hand-rolled IGraphicsEffectD2D1Interop entirely.
//
// The colour matrix sits AFTER the blur, at quarter resolution. It is per-pixel
// linear and the blur is spatially linear, so the two commute and this is the
// cheaper of the two orderings by a factor of sixteen. See glass.h for what the
// matrix is doing and why it is not a saturation effect.
void Panel::Impl::BakeBackdrop() {
    const int width  = panelRect.right  - panelRect.left;
    const int height = panelRect.bottom - panelRect.top;
    if (width <= 0 || height <= 0) return;

    backdropSurface = graphics.CreateDrawingSurface(
        { static_cast<float>(width), static_cast<float>(height) },
        winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
        winrt::Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);

    {
        SurfaceDraw draw(backdropSurface);
        if (!draw.ok) {
            MACTAB_WARN("panel: backdrop BeginDraw failed");
            return;
        }

        draw.dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        DrawGlass(draw.dc.Get(), draw.offset, panelRect, panelRadiusPx);
    }

    backdropVisual.Brush(compositor.CreateSurfaceBrush(backdropSurface));
    backdropVisual.Size({ static_cast<float>(width), static_cast<float>(height) });

    // The material's numbers, in the log, because every report about how the
    // panel looks arrives as a screenshot from a machine I do not have. Knowing
    // which theme won, what radius was used, where the adaptive step put the
    // bias and whether the capture succeeded turns "the glass looks wrong" into
    // something actionable.
    MACTAB_DIAG("panel: backdrop baked %dx%d radius %.0f n %.2f, %s theme "
                "(sat %.2f gain %.2f bias %.3f tint a %.2f), capture %s",
                width, height, panelRadiusPx,
                static_cast<double>(layout::kPanelCornerExponent),
                themeIsLight ? "light" : "dark",
                material.saturation, material.gain,
                material.bias, material.tint[3],
                capture::SourceName(lastFrame.source));
}

// ---------------------------------------------------------------------------

// The app name, centred under the SELECTED tile and BELOW the glass, on a
// capsule of the same material.
//
// It is outside the panel now, not inside it. The reference has uniform padding
// on all four sides, so there is no taller bottom band to put text in, and
// inventing one is the single most obvious way the panel stops looking like the
// thing it is copying.
//
// macOS anchors nothing here, because macOS shows no app name at all. This is a
// useful affordance that Apple does not have, so it gets Apple's material rather
// than an invented one: a small capsule cut from the same frozen frame, twelve
// pixels below the panel, ellipsised at two tile pitches and slid inward at
// either end of the row so it never overhangs the panel's width.
void Panel::Impl::BakeLabel() {
    // Clear first, restore on success.
    //
    // Everything below can bail: an empty name, a font that will not create, a
    // surface that will not open. Every one of those paths used to leave the
    // PREVIOUS app's name on screen, under the newly selected tile, at the
    // previous tile's position. Showing the wrong app's name is worse than
    // showing none, and this is the only guard that covers all of them.
    //
    // Brush(nullptr) rather than IsVisible(false): Visual.IsVisible arrived in
    // 10.0.17763 and MacTab supports 17134.
    labelVisual.Brush(nullptr);

    if (items.empty()) return;

    const int index = (std::max)(0, (std::min)(selected, static_cast<int>(items.size()) - 1));
    const std::wstring& text = items[static_cast<size_t>(index)].label;

    const float panelWidth  = static_cast<float>(panelRect.right  - panelRect.left);
    const float panelHeight = static_cast<float>(panelRect.bottom - panelRect.top);
    const float padding     = Scaled(layout::kPanelPadding);
    const float gap         = Scaled(layout::kTileGap);
    const int   height      = static_cast<int>(std::ceil(Scaled(layout::kLabelHeight)));

    if (panelWidth <= 0.0f || height <= 0 || text.empty()) return;

    // Segoe UI Variable on Windows 11, Segoe UI before it. Shipping SF Pro would
    // be a licence violation, so the system UI font is the honest choice.
    const wchar_t* family = (WindowsBuildNumber() >= 22000) ? L"Segoe UI Variable Display"
                                                            : L"Segoe UI";
    ComPtr<IDWriteTextFormat> format;
    if (FAILED(dwriteFactory->CreateTextFormat(family, nullptr,
                                               DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                               DWRITE_FONT_STYLE_NORMAL,
                                               DWRITE_FONT_STRETCH_NORMAL,
                                               Scaled(14.0f), L"", format.Put()))) {
        return;
    }
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    // The capsule is padded by half its own height at each end, which is what
    // makes the rounded ends read as deliberate rather than as a clipped box.
    const float capsulePad = static_cast<float>(height) * 0.5f;
    const float maxTextWidth =
        (std::min)(2.0f * (tilePx + gap), panelWidth) - capsulePad * 2.0f;
    if (maxTextWidth <= 1.0f) return;

    ComPtr<IDWriteTextLayout> textLayout;
    if (FAILED(dwriteFactory->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
                                               format.Get(), maxTextWidth,
                                               static_cast<float>(height),
                                               textLayout.Put()))) {
        return;
    }

    ComPtr<IDWriteInlineObject> ellipsis;
    if (SUCCEEDED(dwriteFactory->CreateEllipsisTrimmingSign(format.Get(), ellipsis.Put()))) {
        DWRITE_TRIMMING trimming{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
        textLayout->SetTrimming(&trimming, ellipsis.Get());
    }

    // Size the capsule to the text, not to the panel. Clamped because with
    // trimming in play the reported width is the untrimmed line for some
    // scripts, and a capsule wider than the cap would defeat the point of it.
    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(textLayout->GetMetrics(&metrics))) return;

    const float textWidth = (std::min)(maxTextWidth, std::ceil(metrics.width) + Scaled(2.0f));
    if (textWidth <= 1.0f) return;

    textLayout->SetMaxWidth(textWidth);

    const int surfaceWidth = static_cast<int>(std::ceil(textWidth + capsulePad * 2.0f));

    // Anchor on the selected tile's centre, then slide inward so the capsule
    // never overhangs the panel's own width at either end of the row.
    const float tileCentre =
        padding + static_cast<float>(index) * (tilePx + gap) + tilePx * 0.5f;
    const float x = (std::max)(0.0f,
                               (std::min)(tileCentre - surfaceWidth * 0.5f,
                                          panelWidth - surfaceWidth));
    const float y = panelHeight + Scaled(layout::kLabelGap);

    labelSurface = graphics.CreateDrawingSurface(
        { static_cast<float>(surfaceWidth), static_cast<float>(height) },
        winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
        winrt::Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);

    {
        SurfaceDraw draw(labelSurface);
        if (!draw.ok) return;

        draw.dc->Clear(D2D1::ColorF(0, 0, 0, 0));

        // A capsule of the same glass, cut from the same captured frame.
        //
        // Bare shadowed text floating on the wallpaper is what a Windows app
        // does. The reference has no app-name label at all, so there is no 1:1
        // answer here; a second piece of the same material is the version that
        // still looks like it belongs to the panel, and it solves legibility
        // structurally rather than by piling on a text shadow.
        const RECT capsule{ panelRect.left + static_cast<int>(x),
                            panelRect.top  + static_cast<int>(y),
                            panelRect.left + static_cast<int>(x) + surfaceWidth,
                            panelRect.top  + static_cast<int>(y) + height };

        DrawGlass(draw.dc.Get(), draw.offset, capsule, static_cast<float>(height) * 0.5f);

        const D2D1_POINT_2F origin =
            D2D1::Point2F(static_cast<float>(draw.offset.x) + capsulePad,
                          static_cast<float>(draw.offset.y));

        // The capsule puts the text on a known material rather than on the
        // wallpaper, and Adapt() holds that material inside a band chosen so a
        // fixed per-theme text colour clears 4.5:1 at both ends of it. The
        // shadow is a backstop for the case the band's MEAN misses: a wallpaper
        // that is half white and half black averages to the middle, adapts by
        // nothing, and can still be dark under the capsule specifically.
        const float capsuleLuma = CapsuleLuma(capsule);
        const float textLuma    = glass::Luma(theme.label.R / 255.0f,
                                              theme.label.G / 255.0f,
                                              theme.label.B / 255.0f);

        if (glass::ContrastRatio(capsuleLuma, textLuma) < glass::kMinTextContrast) {
            const bool lightText = theme.label.G > 128;
            ComPtr<ID2D1SolidColorBrush> shadow;
            if (SUCCEEDED(draw.dc->CreateSolidColorBrush(
                    lightText ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.30f)
                              : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.35f),
                    shadow.Put()))) {
                draw.dc->DrawTextLayout(D2D1::Point2F(origin.x, origin.y + Scaled(1.0f)),
                                        textLayout.Get(), shadow.Get());
            }
        }

        ComPtr<ID2D1SolidColorBrush> brush;
        if (FAILED(draw.dc->CreateSolidColorBrush(
                D2D1::ColorF(theme.label.R / 255.0f, theme.label.G / 255.0f,
                             theme.label.B / 255.0f, 1.0f),
                brush.Put()))) {
            return;
        }

        draw.dc->DrawTextLayout(origin, textLayout.Get(), brush.Get());
    }

    labelVisual.Brush(compositor.CreateSurfaceBrush(labelSurface));
    labelVisual.Size({ static_cast<float>(surfaceWidth), static_cast<float>(height) });
    labelVisual.Offset({ x, y, 0.0f });
}

// What the capsule's glass will read at, so BakeLabel can decide whether the app
// name needs a shadow.
//
// Runs the same material over the mean of the captured pixels under the capsule,
// using the shared arithmetic in glass.h rather than reading the surface back.
// Returns the theme's own tint luma when there is no frame, which is what the
// nearly-opaque fallback coat produces.
float Panel::Impl::CapsuleLuma(const RECT& screenRect) const {
    if (lastFrame.pixels.Empty())
        return glass::TintLuma(material);

    // Sample the blur's NEIGHBOURHOOD, not the capsule's own strip.
    //
    // What ends up under the capsule is a 30px-sigma blur, so each pixel there is
    // an average over roughly a sigma in every direction, and the capsule is
    // only 28 logical pixels tall. Measuring the strip alone gets this badly
    // wrong in exactly the case the shadow exists for: a hard bright/dark
    // boundary just above the capsule. A dark strip under a bright neighbourhood
    // measures 0.12 and skips the shadow, while what renders is nearer 0.50 and
    // needs it.
    const int reach = static_cast<int>(Scaled(kBlurSigma));

    const uint32_t mean = MeanColourIn(lastFrame.pixels,
                                       screenRect.left   - lastFrame.bounds.left - reach,
                                       screenRect.top    - lastFrame.bounds.top  - reach,
                                       screenRect.right  - lastFrame.bounds.left + reach,
                                       screenRect.bottom - lastFrame.bounds.top  + reach);

    const float in[3]  = { RedOf(mean) / 255.0f, GreenOf(mean) / 255.0f,
                           BlueOf(mean) / 255.0f };
    float out[3];
    glass::Apply(material, in, out);

    // Include the lit edge. The capsule is short enough that the rim covers most
    // of it, so leaving it out makes this estimate systematically too dark and
    // switches the text shadow on when nothing needs it.
    //
    // Blurring preserves a region's mean, so running the material over the mean
    // of the RAW capture gives the same answer as averaging the blurred and
    // treated result, up to the matrix's clamp. Refraction preserves it too, near
    // enough: it moves pixels around inside the capsule and pulls a little in
    // from just outside, which the inflated sampling region above already covers.
    const float height = static_cast<float>(screenRect.bottom - screenRect.top);
    return glass::LitBy(glass::Luma(out[0], out[1], out[2]),
                        glass::MeanRimAlpha(material, height, dpiScale));
}

// ---------------------------------------------------------------------------

namespace {

// Pick the display the panel opens on.
//
// Sampled once per layout, and never re-sampled at reveal: the desktop grab for
// the backdrop is started at gesture begin against whatever rect Layout chose,
// so re-deciding later would let a mouse flicked across displays during the hold
// delay put the panel on one monitor and its backdrop on another.
HMONITOR ChooseMonitor(HWND fallbackWindow) {
    switch (config::Current().panelDisplay) {
        case config::PanelDisplay::Mouse: {
            POINT cursor{};
            // GetCursorPos fails across a secure-desktop transition, which a
            // gesture can genuinely race. Fall back rather than trusting an
            // uninitialised point.
            if (::GetCursorPos(&cursor))
                return ::MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
            MACTAB_WARN("panel: GetCursorPos failed (err %lu), using the main display",
                        ::GetLastError());
            return ::MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
        }

        case config::PanelDisplay::Primary:
            // The primary display always owns the virtual desktop's origin.
            return ::MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);

        case config::PanelDisplay::ActiveWindow:
        default: {
            const HWND foreground = ::GetForegroundWindow();
            return ::MonitorFromWindow(foreground ? foreground : fallbackWindow,
                                       MONITOR_DEFAULTTOPRIMARY);
        }
    }
}

} // namespace

void Panel::Impl::Layout(int count) {
    // Pick the display once per gesture, not once per layout. Layout runs again
    // mid-gesture whenever the item count changes, which is every window
    // expansion and every app quit, and with PanelDisplay=mouse a cursor that
    // has since crossed to another monitor would teleport a panel that is
    // already on screen.
    if (!visible || !monitor)
        monitor = ChooseMonitor(hwnd);

    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    ::GetMonitorInfoW(monitor, &monitorInfo);

    UINT dpiX = 96, dpiY = 96;
    ::GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
    dpiScale = static_cast<float>(dpiX) / 96.0f;

    const float monitorWidth =
        static_cast<float>(monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left);
    const float maxPanelWidth = monitorWidth - Scaled(80.0f);

    // macOS shrinks tiles to fit rather than wrapping to a second row, so the
    // panel is always a single strip.
    const layout::Metrics metrics = layout::Compute(
        count, maxPanelWidth, dpiScale,
        static_cast<float>(config::Current().tileSize));
    tilePx        = metrics.tileSize;
    panelRadiusPx = metrics.radius;

    const float panelWidth  = metrics.panelWidth;
    const float panelHeight = metrics.panelHeight;

    const int width  = static_cast<int>(std::lround(panelWidth));
    const int height = static_cast<int>(std::lround(panelHeight));

    // Centre on the MONITOR, not on the work area.
    //
    // macOS centres the switcher on the display; the Dock and the menu bar do
    // not push it around. Centring on rcWork instead would lift the panel by
    // half the taskbar's height, which is small and, once you have noticed it,
    // permanently visible.
    const int centreX = (monitorInfo.rcMonitor.left + monitorInfo.rcMonitor.right) / 2;
    const int centreY = (monitorInfo.rcMonitor.top + monitorInfo.rcMonitor.bottom) / 2;

    panelRect.left   = centreX - width / 2;
    panelRect.top    = centreY - height / 2;
    panelRect.right  = panelRect.left + width;
    panelRect.bottom = panelRect.top + height;

    // The window is taller than the glass, because the app name is drawn below
    // the panel rather than inside it. There is no shadow gutter any more, so
    // the glass sits at the window's origin and the label hangs off the bottom.
    labelBandPx = metrics.labelGap + metrics.labelHeight;

    ::SetWindowPos(hwnd, HWND_TOPMOST,
                   panelRect.left, panelRect.top,
                   width, height + static_cast<int>(std::ceil(labelBandPx)),
                   SWP_NOACTIVATE | SWP_NOREDRAW);

    content.Offset({ 0.0f, 0.0f, 0.0f });
    content.Size({ panelWidth, panelHeight + labelBandPx });

    // Last line on purpose. Everything above can throw (device loss), and
    // recording the count earlier would let SetItems skip the re-layout after a
    // recovery; rendering the gesture against half-updated geometry.
    laidOutCount = count;
}

void Panel::Impl::UploadIcon(size_t index) {
    if (index >= items.size() || index >= tileVisuals.size()) return;

    const Bitmap& icon = items[index].icon;
    auto visual = tileVisuals[index];

    if (icon.Empty()) {
        // Neutral placeholder until the worker delivers. Never leave a hole,
        // an empty slot reads as a bug, a grey tile reads as loading.
        visual.Brush(compositor.CreateColorBrush(WUI::Color{ 60, 255, 255, 255 }));
        return;
    }

    Bitmap upload = icon;
    PremultiplyInPlace(upload);

    auto surface = graphics.CreateDrawingSurface(
        { static_cast<float>(upload.width), static_cast<float>(upload.height) },
        winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
        winrt::Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);

    SurfaceDraw draw(surface);
    if (!draw.ok) return;

    draw.dc->Clear(D2D1::ColorF(0, 0, 0, 0));

    ComPtr<ID2D1Bitmap1> bitmap;
    const D2D1_SIZE_U pixelSize{ static_cast<UINT32>(upload.width),
                                 static_cast<UINT32>(upload.height) };
    D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));

    if (SUCCEEDED(draw.dc->CreateBitmap(pixelSize, upload.pixels.data(),
                                        static_cast<UINT32>(upload.width * 4),
                                        &props, bitmap.Put()))) {
        draw.dc->DrawBitmap(bitmap.Get(),
                            D2D1::RectF(static_cast<float>(draw.offset.x),
                                        static_cast<float>(draw.offset.y),
                                        static_cast<float>(draw.offset.x + upload.width),
                                        static_cast<float>(draw.offset.y + upload.height)));
    }

    visual.Brush(compositor.CreateSurfaceBrush(surface));
}

void Panel::Impl::PositionTiles(bool animate) {
    const float padding = Scaled(layout::kPanelPadding);
    const float gap     = Scaled(layout::kTileGap);

    for (size_t i = 0; i < tileVisuals.size(); ++i) {
        const float x = padding + static_cast<float>(i) * (tilePx + gap);
        tileVisuals[i].Size({ tilePx, tilePx });
        tileVisuals[i].Offset({ x, padding, 0.0f });
    }

    if (selected >= 0 && selected < static_cast<int>(tileVisuals.size())) {
        const float inset = tilePx * layout::kSelectionInset;
        const float x = padding + static_cast<float>(selected) * (tilePx + gap) - inset;

        selectionVisual.Size({ tilePx + inset * 2, tilePx + inset * 2 });

        const WFN::float3 destination{ x, padding - inset, 0.0f };

        if (animate) {
            // A spring rather than a keyframe curve: the highlight overshoots
            // very slightly and settles, which is what makes macOS's selection
            // feel physical instead of mechanical.
            auto spring = compositor.CreateSpringVector3Animation();
            spring.DampingRatio(0.80f);
            spring.Period(std::chrono::milliseconds(50));
            spring.FinalValue(destination);
            selectionVisual.StartAnimation(L"Offset", spring);
        } else {
            selectionVisual.Offset(destination);
        }
    }
}

void Panel::SetItems(std::vector<PanelItem> items, int selectedIndex) {
    Impl& impl = *m_impl;
    GuardPanel(impl, "SetItems", [&] {
    MACTAB_DIAG_TIMER("panel: SetItems");

    impl.items    = std::move(items);
    impl.selected = selectedIndex;

    // Re-resolve the theme once per gesture, so switching Windows between light
    // and dark, or picking a theme from the tray, takes effect on the next
    // Alt+Tab rather than on the next launch. Everything else on the panel is
    // baked per gesture anyway; the selection highlight is the one surface that
    // is not, so it is the one that has to be rebuilt here.
    if (const bool light = ResolveLightTheme(); light != impl.themeIsLight) {
        impl.theme        = MakeTheme(light);
        impl.themeIsLight = light;
        MACTAB_DIAG("panel: theme is now %s", light ? "light" : "dark");
    }

    // PrepareLayout has usually just run for this exact count; laying out
    // again would mean a second SetWindowPos per gesture for nothing.
    if (impl.laidOutCount != static_cast<int>(impl.items.size()))
        impl.Layout(static_cast<int>(impl.items.size()));

    // Reuse existing tile visuals; only create or trim the difference. Rebuilding
    // the whole tree per invocation would blow the one-frame budget.
    auto children = impl.tileLayer.Children();
    while (impl.tileVisuals.size() < impl.items.size()) {
        auto visual = impl.compositor.CreateSpriteVisual();
        children.InsertAtTop(visual);
        impl.tileVisuals.push_back(visual);
    }
    while (impl.tileVisuals.size() > impl.items.size()) {
        children.Remove(impl.tileVisuals.back());
        impl.tileVisuals.pop_back();
    }

    for (size_t i = 0; i < impl.items.size(); ++i)
        impl.UploadIcon(i);

    impl.BakeSelection();
    impl.PositionTiles(false);

    // A relayout while the panel is already up (window expansion, an app
    // quitting) resizes panelRect, so the glass has to be re-baked at the new
    // size or it keeps the previous one. Reuse the frame already captured rather
    // than grabbing the desktop again with our own panel on it.
    //
    // The label comes last because its capsule is glass too: it needs the frame
    // collected and the material adapted, and both of those happen in
    // CollectFrame. At gesture begin there is no frame yet, so the capsule is
    // baked from the fallback and Show() re-bakes it once the capture lands.
    if (impl.visible) {
        impl.CollectFrame();
        impl.BakeBackdrop();
    } else {
        // No frame yet, so nothing to adapt to. Reset the material rather than
        // leaving the previous gesture's adapted copy, which after a theme flip
        // is the wrong theme entirely. Show() re-adapts and re-bakes before any
        // of this reaches the screen; this only keeps the invariant true.
        impl.material = impl.theme.material;
        impl.StartCapture();
    }

    impl.BakeLabel();
    });
}

void Panel::SetSelection(int index) {
    Impl& impl = *m_impl;
    if (impl.items.empty()) return;

    GuardPanel(impl, "SetSelection", [&] {
        impl.selected =
            (std::max)(0, (std::min)(index, static_cast<int>(impl.items.size()) - 1));
        impl.PositionTiles(true);
        impl.BakeLabel();
    });
}

void Panel::UpdateIcon(const std::wstring& key, const Bitmap& icon) {
    Impl& impl = *m_impl;

    GuardPanel(impl, "UpdateIcon", [&] {
        // Every matching item, not just the first. In window mode all tiles
        // share the expanded app's key, and with GroupByApp=0 every window of
        // an app does too, returning early left all but one as placeholders.
        for (size_t i = 0; i < impl.items.size(); ++i) {
            if (impl.items[i].key != key) continue;
            impl.items[i].icon = icon;
            impl.UploadIcon(i);
        }
    });
}

void Panel::Show() {
    Impl& impl = *m_impl;
    if (impl.visible) return;

    const bool ok = GuardPanel(impl, "Show", [&] {
    // Compose the glass now, from the frame captured when the gesture started.
    //
    // The label is re-baked after it, not before: its capsule is glass cut from
    // the same frame, and at gesture begin that frame had not arrived yet, so
    // the version baked then is the fallback one.
    impl.CollectFrame();
    impl.BakeBackdrop();
    impl.BakeLabel();

    // SW_SHOWNA: show without activating, so the panel never becomes the
    // foreground window and never competes with the window we are about to
    // switch to.
    ::ShowWindow(impl.hwnd, SW_SHOWNA);
    ::SetWindowPos(impl.hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);

    auto fade = impl.compositor.CreateScalarKeyFrameAnimation();
    fade.InsertKeyFrame(0.0f, 0.0f);
    fade.InsertKeyFrame(1.0f, 1.0f);
    fade.Duration(std::chrono::milliseconds(120));
    impl.root.StartAnimation(L"Opacity", fade);

    // Scale from the panel's centre, not its corner.
    const float w = static_cast<float>(impl.panelRect.right - impl.panelRect.left);
    const float h = static_cast<float>(impl.panelRect.bottom - impl.panelRect.top);
    impl.root.CenterPoint({ w * 0.5f, h * 0.5f, 0.0f });

    auto grow = impl.compositor.CreateVector3KeyFrameAnimation();
    grow.InsertKeyFrame(0.0f, { 0.96f, 0.96f, 1.0f });
    grow.InsertKeyFrame(1.0f, { 1.0f, 1.0f, 1.0f });
    grow.Duration(std::chrono::milliseconds(120));
    impl.root.StartAnimation(L"Scale", grow);
    });

    if (!ok) {
        // The window was already shown before the throw, so it is sitting there
        // topmost at zero opacity, swallowing every click over the middle of the
        // screen, and Hide() would never run, because `visible` is still false.
        ::ShowWindow(impl.hwnd, SW_HIDE);
        return;
    }
    impl.visible = true;
}

void Panel::Hide() {
    Impl& impl = *m_impl;
    if (!impl.visible) return;

    GuardPanel(impl, "Hide", [&] {
    // Hidden immediately rather than fading out: the eye is already following
    // the window being activated, and a lingering panel reads as lag.
    impl.root.Opacity(0.0f);
    ::ShowWindow(impl.hwnd, SW_HIDE);
    });

    // The window must end up hidden even if the compositor threw.
    ::ShowWindow(impl.hwnd, SW_HIDE);
    impl.visible      = false;
    impl.hoveredIndex = -1;

    // Abandon any capture that was still in flight. The detached thread
    // finishes harmlessly; dropping the future here keeps a stale frame from
    // being used by the next gesture.
    if (impl.pendingCapture.valid())
        impl.pendingCapture = {};

    // The next gesture captures its own desktop; keeping this would risk a
    // stale blur if that capture is late.
    impl.lastFrame = {};
}

int Panel::Impl::HitTestScreen(POINT screenPoint) const {
    if (!visible || items.empty()) return -1;

    const float padding = Scaled(layout::kPanelPadding);
    const float gap     = Scaled(layout::kTileGap);

    const float x = static_cast<float>(screenPoint.x - panelRect.left);
    const float y = static_cast<float>(screenPoint.y - panelRect.top);

    if (y < padding || y > padding + tilePx) return -1;

    for (size_t i = 0; i < items.size(); ++i) {
        const float left = padding + static_cast<float>(i) * (tilePx + gap);
        if (x >= left && x <= left + tilePx)
            return static_cast<int>(i);
    }
    return -1;
}

int Panel::HitTest(POINT screenPoint) const {
    return m_impl->HitTestScreen(screenPoint);
}

} // namespace mactab
