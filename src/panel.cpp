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
constexpr float kShadowSigma = 22.0f;
constexpr float kBlurSigma   = 34.0f;

// Extra desktop captured around the panel so the blur has real pixels to pull
// from instead of clamping at the edge.
constexpr int kBlurMarginPx = 48;

// Downsample factor before blurring. A 34px sigma at quarter resolution costs
// what an 8.5px sigma costs, and after the matching upscale the difference is
// invisible under a tint.
constexpr float kBlurDownscale = 0.25f;

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
    D2D1_COLOR_F tint;
    D2D1_COLOR_F border;
    D2D1_COLOR_F selection;
    WUI::Color   label;
};

Theme MakeTheme(bool light) {
    Theme theme;
    if (light) {
        theme.tint      = D2D1::ColorF(0.96f, 0.96f, 0.97f, 0.62f);
        theme.border    = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.10f);
        theme.selection = D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.10f);
        theme.label     = WUI::Color{ 255, 20, 20, 22 };
    } else {
        theme.tint      = D2D1::ColorF(0.09f, 0.09f, 0.10f, 0.55f);
        theme.border    = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.10f);
        theme.selection = D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.18f);
        theme.label     = WUI::Color{ 255, 245, 245, 247 };
    }
    return theme;
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
    WUC::SpriteVisual    shadowVisual{ nullptr };
    WUC::SpriteVisual    backdropVisual{ nullptr };
    WUC::SpriteVisual    selectionVisual{ nullptr };
    WUC::ContainerVisual tileLayer{ nullptr };
    WUC::SpriteVisual    labelVisual{ nullptr };

    WUC::CompositionDrawingSurface backdropSurface{ nullptr };
    WUC::CompositionDrawingSurface labelSurface{ nullptr };
    WUC::CompositionSurfaceBrush   shadowBrush{ nullptr };

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
    Theme  theme     = MakeTheme(false);
    HMONITOR monitor = nullptr;
    float    shadowMarginPx = 0.0f;
    float    bakedShadowScale = 0.0f;
    int      laidOutCount     = -1;   // skips a redundant second Layout per gesture

    bool CreateDevices();
    bool CreatePanelWindow();
    void RecoverDevices();
    bool CreateVisualTree();

    // The captured desktop frame, in flight. Started when the gesture begins and
    // consumed when the panel is actually revealed.
    std::future<capture::Frame> pendingCapture;
    capture::Frame              lastFrame;   // reused when relaying out while visible

    void Layout(int count);
    void BakeShadow();
    void BakeSelection();
    void StartCapture();
    void BakeBackdrop();
    void BakeLabel();
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

    // Order matters: shadow underneath, then the glass, then the selection
    // highlight, then tiles, then the label.
    shadowVisual    = compositor.CreateSpriteVisual();
    content         = compositor.CreateContainerVisual();
    backdropVisual  = compositor.CreateSpriteVisual();
    selectionVisual = compositor.CreateSpriteVisual();
    tileLayer       = compositor.CreateContainerVisual();
    labelVisual     = compositor.CreateSpriteVisual();

    // The window is inflated by the shadow margin, so everything that belongs
    // to the panel proper hangs off `content`, which carries that offset once.
    // Positioning each layer against the window origin instead is how the
    // selection highlight and label ended up drawn in the shadow gutter.
    auto children = root.Children();
    children.InsertAtTop(shadowVisual);
    children.InsertAtTop(content);

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

    impl.theme = MakeTheme(ResolveLightTheme());

    if (!impl.CreatePanelWindow()) return false;
    if (!impl.CreateDevices())     return false;
    if (!impl.CreateVisualTree())  return false;

    // Pre-bake at 100% so the common case is warm; Layout re-bakes if the
    // panel lands on a monitor with a different scale.
    impl.BakeShadow();
    impl.bakedShadowScale = impl.dpiScale;

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
        impl.shadowVisual    = nullptr;
        impl.content         = nullptr;
        impl.root            = nullptr;

        impl.backdropSurface = nullptr;
        impl.labelSurface    = nullptr;
        impl.shadowBrush     = nullptr;

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
    shadowBrush     = nullptr;

    if (!CreateDevices()) {
        MACTAB_FAIL("panel: could not rebuild the graphics device; panel disabled");
        ready = false;
        return;
    }

    try {
        BakeShadow();
        BakeSelection();
    } catch (const winrt::hresult_error& e) {
        MACTAB_FAIL("panel: re-baking after device loss threw 0x%08lX",
                    static_cast<unsigned long>(e.code()));
        ready = false;
        return;
    }

    MACTAB_DIAG("panel: graphics device rebuilt");
}

// ---------------------------------------------------------------------------
// Shadow
//
// A nine-grid rather than a Composition DropShadow. The silhouette never
// changes, only the panel's size does, so the shadow can be rendered once at
// startup and stretched. A DropShadow with a 40px blur is recomputed by the
// compositor whenever the shadow's size changes, at a cost we cannot see or
// bound. This is one textured quad per frame instead.
void Panel::Impl::BakeShadow() {
    const float sigma  = Scaled(kShadowSigma);
    const float radius = Scaled(layout::kPanelRadius);
    const int   cell   = static_cast<int>(std::ceil(radius + 3.0f * sigma));
    const int   size   = cell * 2 + 4;

    WFN::float2 surfaceSize{ static_cast<float>(size), static_cast<float>(size) };
    auto surface = graphics.CreateDrawingSurface(
        { surfaceSize.x, surfaceSize.y },
        winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
        winrt::Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);

    {
        SurfaceDraw draw(surface);
        if (!draw.ok) {
            MACTAB_WARN("panel: shadow BeginDraw failed");
            return;
        }

        draw.dc->Clear(D2D1::ColorF(0, 0, 0, 0));

        // Render the silhouette through a PRIVATE device context, not by
        // retargeting the interop one.
        //
        // The DC handed back by BeginDraw carries an update clip in atlas
        // coordinates, and D2D's clip stack is context state rather than target
        // state, so drawing to an offscreen through it can be clipped against
        // wherever this surface happens to sit in the atlas, and leaves the
        // context in a state EndDraw did not expect.
        ComPtr<ID2D1DeviceContext> silhouetteDc;
        if (FAILED(d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
                                                  silhouetteDc.Put()))) {
            MACTAB_WARN("panel: could not create a device context for the shadow");
            return;
        }

        ComPtr<ID2D1Bitmap1> silhouette;
        const D2D1_SIZE_U pixelSize{ static_cast<UINT32>(size), static_cast<UINT32>(size) };
        D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));

        if (FAILED(silhouetteDc->CreateBitmap(pixelSize, nullptr, 0, &props, silhouette.Put())))
            return;

        auto geometry = CreateSquircleGeometry(d2dFactory.Get(),
                                               static_cast<float>(size - 4),
                                               static_cast<float>(size - 4), radius);
        if (!geometry) return;

        silhouetteDc->SetTarget(silhouette.Get());
        silhouetteDc->BeginDraw();
        silhouetteDc->Clear(D2D1::ColorF(0, 0, 0, 0));

        ComPtr<ID2D1SolidColorBrush> black;
        silhouetteDc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.42f), black.Put());
        silhouetteDc->SetTransform(D2D1::Matrix3x2F::Translation(2.0f, 2.0f));
        silhouetteDc->FillGeometry(geometry.Get(), black.Get());
        silhouetteDc->SetTransform(D2D1::Matrix3x2F::Identity());

        if (FAILED(silhouetteDc->EndDraw())) {
            MACTAB_WARN("panel: shadow silhouette EndDraw failed");
            return;
        }
        silhouetteDc->SetTarget(nullptr);

        // Only the finished blur touches the interop context.
        ComPtr<ID2D1Effect> blur;
        if (SUCCEEDED(draw.dc->CreateEffect(CLSID_D2D1GaussianBlur, blur.Put()))) {
            blur->SetInput(0, silhouette.Get());
            blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, sigma);
            draw.dc->DrawImage(blur.Get(),
                               D2D1::Point2F(static_cast<float>(draw.offset.x),
                                             static_cast<float>(draw.offset.y)));
        }
    }

    shadowBrush = compositor.CreateSurfaceBrush(surface);

    auto nine = compositor.CreateNineGridBrush();
    nine.Source(shadowBrush);
    nine.SetInsets(static_cast<float>(cell));
    shadowVisual.Brush(nine);
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
    RECT captureRect = panelRect;
    ::InflateRect(&captureRect, kBlurMarginPx, kBlurMarginPx);

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
// once as a squircle and stretched with a nine-grid, exactly like the shadow.
void Panel::Impl::BakeSelection() {
    const float radius = Scaled(layout::kTileSize) * 0.22f;
    const int   cell   = static_cast<int>(std::ceil(radius)) + 2;
    const int   size   = cell * 2 + 4;

    auto surface = graphics.CreateDrawingSurface(
        { static_cast<float>(size), static_cast<float>(size) },
        winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
        winrt::Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);

    {
        SurfaceDraw draw(surface);
        if (!draw.ok) {
            MACTAB_WARN("panel: selection BeginDraw failed");
            return;
        }

        draw.dc->Clear(D2D1::ColorF(0, 0, 0, 0));

        auto geometry = CreateSquircleGeometry(d2dFactory.Get(),
                                               static_cast<float>(size),
                                               static_cast<float>(size), radius);
        if (!geometry) return;

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
    selectionVisual.Brush(nine);
}

// ---------------------------------------------------------------------------
// Backdrop
//
// Captured desktop -> downscale -> Gaussian blur -> upscale -> tint -> border,
// all clipped to the squircle by a D2D layer. Because the source is a single
// frozen frame, this is a draw-time operation rather than a live effect graph,
// which is what lets us avoid Win2D and a hand-rolled IGraphicsEffectD2D1Interop
// entirely.
void Panel::Impl::BakeBackdrop() {
    const int width  = panelRect.right  - panelRect.left;
    const int height = panelRect.bottom - panelRect.top;
    if (width <= 0 || height <= 0) return;

    backdropSurface = graphics.CreateDrawingSurface(
        { static_cast<float>(width), static_cast<float>(height) },
        winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
        winrt::Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);

    // Collect the frame started at gesture begin. It has had the whole hold
    // delay to finish, so in practice this never waits, but it is bounded
    // anyway, because a wedged GPU must degrade to a flat tint rather than
    // stall the reveal.
    if (pendingCapture.valid()) {
        if (pendingCapture.wait_for(std::chrono::milliseconds(10)) ==
            std::future_status::ready) {
            lastFrame = pendingCapture.get();
        } else {
            // Do NOT fall through to lastFrame here: after the first gesture
            // that holds a previous desktop, and a visibly outdated blur reads
            // worse than no blur at all.
            MACTAB_WARN("panel: capture not ready at reveal, using flat tint");
            lastFrame = {};
        }
        pendingCapture = {};
    }
    const capture::Frame& frame = lastFrame;

    {
        SurfaceDraw draw(backdropSurface);
        if (!draw.ok) {
            MACTAB_WARN("panel: backdrop BeginDraw failed");
            return;
        }

        draw.dc->Clear(D2D1::ColorF(0, 0, 0, 0));

        auto geometry = CreateSquircleGeometry(d2dFactory.Get(),
                                               static_cast<float>(width),
                                               static_cast<float>(height),
                                               Scaled(layout::kPanelRadius));
        if (!geometry) return;

        const D2D1_MATRIX_3X2_F toSurface =
            D2D1::Matrix3x2F::Translation(static_cast<float>(draw.offset.x),
                                          static_cast<float>(draw.offset.y));
        draw.dc->SetTransform(toSurface);

        ComPtr<ID2D1Layer> layer;
        draw.dc->CreateLayer(nullptr, layer.Put());
        draw.dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), geometry.Get(),
                                                  D2D1_ANTIALIAS_MODE_PER_PRIMITIVE),
                           layer.Get());

        if (!frame.pixels.Empty()) {
            Bitmap source = frame.pixels;
            PremultiplyInPlace(source);

            ComPtr<ID2D1Bitmap1> captured;
            const D2D1_SIZE_U pixelSize{ static_cast<UINT32>(source.width),
                                         static_cast<UINT32>(source.height) };
            D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_NONE,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));

            if (SUCCEEDED(draw.dc->CreateBitmap(pixelSize, source.pixels.data(),
                                                static_cast<UINT32>(source.width * 4),
                                                &props, captured.Put()))) {
                ComPtr<ID2D1Effect> scale, blur;
                draw.dc->CreateEffect(CLSID_D2D1Scale, scale.Put());
                draw.dc->CreateEffect(CLSID_D2D1GaussianBlur, blur.Put());

                if (scale && blur) {
                    scale->SetInput(0, captured.Get());
                    scale->SetValue(D2D1_SCALE_PROP_SCALE,
                                    D2D1::Vector2F(kBlurDownscale, kBlurDownscale));

                    blur->SetInputEffect(0, scale.Get());
                    blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION,
                                   Scaled(kBlurSigma) * kBlurDownscale);
                    // HARD border mode: SOFT would fade the blur toward
                    // transparent at the capture edges and halo the panel.
                    blur->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE,
                                   D2D1_BORDER_MODE_HARD);

                    // Undo the downscale on the way out, and shift so the
                    // panel's own area lands at the surface origin.
                    //
                    // Offset comes from where the capture ACTUALLY landed, not
                    // from the margin we asked for: near a screen edge the grab
                    // is clamped to the monitor and comes back shifted, and
                    // assuming the requested origin slides the blur sideways.
                    const float dx = static_cast<float>(frame.bounds.left - panelRect.left);
                    const float dy = static_cast<float>(frame.bounds.top  - panelRect.top);
                    draw.dc->SetTransform(
                        D2D1::Matrix3x2F::Scale(1.0f / kBlurDownscale, 1.0f / kBlurDownscale) *
                        D2D1::Matrix3x2F::Translation(
                            static_cast<float>(draw.offset.x) + dx,
                            static_cast<float>(draw.offset.y) + dy) );
                    draw.dc->DrawImage(blur.Get(), D2D1_INTERPOLATION_MODE_LINEAR);
                    draw.dc->SetTransform(toSurface);
                }
            }
        }

        const D2D1_RECT_F panelArea =
            D2D1::RectF(0, 0, static_cast<float>(width), static_cast<float>(height));

        ComPtr<ID2D1SolidColorBrush> tintBrush;
        draw.dc->CreateSolidColorBrush(theme.tint, tintBrush.Put());
        draw.dc->FillRectangle(panelArea, tintBrush.Get());

        draw.dc->PopLayer();

        // Hairline border on the shape itself, which is what gives the glass a
        // defined edge rather than fading into whatever is behind it.
        ComPtr<ID2D1SolidColorBrush> borderBrush;
        draw.dc->CreateSolidColorBrush(theme.border, borderBrush.Put());
        draw.dc->DrawGeometry(geometry.Get(), borderBrush.Get(), Scaled(1.0f));

        draw.dc->SetTransform(D2D1::Matrix3x2F::Identity());
    }

    backdropVisual.Brush(compositor.CreateSurfaceBrush(backdropSurface));
    backdropVisual.Size({ static_cast<float>(width), static_cast<float>(height) });

    MACTAB_DIAG("panel: backdrop baked %dx%d (capture %s)",
                width, height, capture::SourceName(frame.source));
}

// ---------------------------------------------------------------------------

void Panel::Impl::BakeLabel() {
    if (items.empty()) return;

    const int index = (std::max)(0, (std::min)(selected, static_cast<int>(items.size()) - 1));
    const std::wstring& text = items[static_cast<size_t>(index)].label;

    const int width  = panelRect.right - panelRect.left;
    const int height = static_cast<int>(std::ceil(Scaled(layout::kLabelHeight)));
    if (width <= 0 || height <= 0) return;

    labelSurface = graphics.CreateDrawingSurface(
        { static_cast<float>(width), static_cast<float>(height) },
        winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
        winrt::Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);

    SurfaceDraw draw(labelSurface);
    if (!draw.ok) return;

    draw.dc->Clear(D2D1::ColorF(0, 0, 0, 0));

    // Segoe UI Variable on Windows 11, Segoe UI before it. Shipping SF Pro
    // would be a licence violation, so the system UI font is the honest choice.
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

    ComPtr<ID2D1SolidColorBrush> brush;
    draw.dc->CreateSolidColorBrush(
        D2D1::ColorF(theme.label.R / 255.0f, theme.label.G / 255.0f,
                     theme.label.B / 255.0f, 1.0f),
        brush.Put());

    draw.dc->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(draw.offset.x),
                                                        static_cast<float>(draw.offset.y)));
    draw.dc->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format.Get(),
                       D2D1::RectF(0, 0, static_cast<float>(width), static_cast<float>(height)),
                       brush.Get());
    draw.dc->SetTransform(D2D1::Matrix3x2F::Identity());

    labelVisual.Brush(compositor.CreateSurfaceBrush(labelSurface));
    labelVisual.Size({ static_cast<float>(width), static_cast<float>(height) });
    labelVisual.Offset({ 0.0f,
                         static_cast<float>(panelRect.bottom - panelRect.top) -
                             Scaled(layout::kPanelPadding) - Scaled(layout::kLabelHeight) +
                             Scaled(4.0f),
                         0.0f });
}

// ---------------------------------------------------------------------------

void Panel::Impl::Layout(int count) {
    // Follow the foreground window's monitor, which matches Alt+Tab semantics
    // better than following the cursor.
    const HWND foreground = ::GetForegroundWindow();
    monitor = ::MonitorFromWindow(foreground ? foreground : hwnd, MONITOR_DEFAULTTOPRIMARY);

    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    ::GetMonitorInfoW(monitor, &monitorInfo);

    UINT dpiX = 96, dpiY = 96;
    ::GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
    dpiScale = static_cast<float>(dpiX) / 96.0f;

    const float workWidth = static_cast<float>(monitorInfo.rcWork.right - monitorInfo.rcWork.left);
    const float maxPanelWidth = workWidth - Scaled(80.0f);

    // macOS shrinks tiles to fit rather than wrapping to a second row, so the
    // panel is always a single strip.
    const layout::Metrics metrics = layout::Compute(
        count, maxPanelWidth, dpiScale,
        static_cast<float>(config::Current().tileSize));
    tilePx = metrics.tileSize;

    const float panelWidth  = metrics.panelWidth;
    const float panelHeight = metrics.panelHeight;

    const int width  = static_cast<int>(std::lround(panelWidth));
    const int height = static_cast<int>(std::lround(panelHeight));

    const int centreX = (monitorInfo.rcWork.left + monitorInfo.rcWork.right) / 2;
    const int centreY = (monitorInfo.rcWork.top + monitorInfo.rcWork.bottom) / 2;

    panelRect.left   = centreX - width / 2;
    panelRect.top    = centreY - height / 2;
    panelRect.right  = panelRect.left + width;
    panelRect.bottom = panelRect.top + height;

    // The window is inflated by the shadow margin so the shadow has room to
    // draw outside the panel.
    const int shadowMargin = static_cast<int>(std::ceil(Scaled(kShadowSigma) * 3.0f));
    shadowMarginPx = static_cast<float>(shadowMargin);

    ::SetWindowPos(hwnd, HWND_TOPMOST,
                   panelRect.left - shadowMargin, panelRect.top - shadowMargin,
                   width + shadowMargin * 2, height + shadowMargin * 2,
                   SWP_NOACTIVATE | SWP_NOREDRAW);

    // One offset for the whole panel; children position in panel-local space.
    content.Offset({ shadowMarginPx, shadowMarginPx, 0.0f });
    content.Size({ panelWidth, panelHeight });

    shadowVisual.Offset({ shadowMarginPx - Scaled(kShadowSigma) * 0.5f,
                          shadowMarginPx - Scaled(kShadowSigma) * 0.5f + Scaled(8.0f),
                          0.0f });
    shadowVisual.Size({ panelWidth + Scaled(kShadowSigma), panelHeight + Scaled(kShadowSigma) });

    // C8: the shadow is baked from dpiScale, which is only known once a monitor
    // has been chosen. Re-bake whenever it changes rather than keeping the 96
    // DPI version baked at startup.
    if (std::fabs(dpiScale - bakedShadowScale) > 0.01f) {
        BakeShadow();
        bakedShadowScale = dpiScale;
    }

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
    impl.BakeLabel();

    // C4: a relayout while the panel is already up (window expansion, an app
    // quitting) resizes panelRect, so the glass has to be re-baked at the new
    // size or it keeps the previous one. Reuse the frame already captured
    // rather than grabbing the desktop again with our own panel on it.
    if (impl.visible)
        impl.BakeBackdrop();
    else
        impl.StartCapture();
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
    impl.BakeBackdrop();

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
    impl.root.CenterPoint({ impl.shadowMarginPx + w * 0.5f,
                            impl.shadowMarginPx + h * 0.5f, 0.0f });

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
