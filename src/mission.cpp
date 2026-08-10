#include "winrt_pch.h"

#include <d2d1_1.h>
#include <d2d1effects_2.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <shellscalingapi.h>

#include <DispatcherQueue.h>
#include <windows.ui.composition.interop.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Composition.Desktop.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.h>

#include "mission.h"
#include "com.h"
#include "common.h"
#include "config.h"
#include "diag.h"
#include "geometry.h"
#include "mission_layout.h"
#include "thumbnail.h"
#include "wallpaper.h"

namespace WUC = winrt::Windows::UI::Composition;
namespace WUI = winrt::Windows::UI;
namespace WFN = winrt::Windows::Foundation::Numerics;
namespace WUCABI = ABI::Windows::UI::Composition;

namespace mactab {
namespace {

constexpr wchar_t kMissionClass[] = L"MacTabMissionWindow";

// Logical pixels at 96 DPI.
constexpr float kSpacesStripHeight = 132.0f;   // the whole band
constexpr float kSpaceChipHeight   = 92.0f;    // one desktop miniature
constexpr float kSpaceChipGap      = 18.0f;
constexpr float kSpaceChipRadius   = 10.0f;
constexpr float kSpaceLabelHeight  = 22.0f;   // the name under each miniature
constexpr float kOuterMargin       = 56.0f;
constexpr float kTitleHeight       = 30.0f;
constexpr float kTitleGap          = 10.0f;

// The wallpaper is blurred at a quarter of the screen's resolution and the
// visual stretches it back. At this radius the difference is not visible and it
// is a sixteenth of the pixels, which matters when the surface is the size of a
// 4K display.
constexpr float kBackdropDownscale = 0.25f;

// How long the snapshot tier may spend before the rest of the windows become
// cards instead. Only reached when the shared-visual path is unavailable.
constexpr double kSnapshotBudgetMs = 400.0;

struct Theme {
    D2D1_COLOR_F backdropTint;
    D2D1_COLOR_F chip;
    D2D1_COLOR_F chipCurrent;
    D2D1_COLOR_F chipBorder;
    D2D1_COLOR_F text;
    D2D1_COLOR_F selection;
};

Theme MakeTheme(bool light) {
    Theme theme{};
    if (light) {
        theme.backdropTint = { 0.86f, 0.87f, 0.90f, 0.55f };
        theme.chip         = { 1.00f, 1.00f, 1.00f, 0.35f };
        theme.chipCurrent  = { 1.00f, 1.00f, 1.00f, 0.70f };
        theme.chipBorder   = { 0.10f, 0.10f, 0.12f, 0.35f };
        theme.text         = { 0.08f, 0.08f, 0.10f, 1.00f };
        theme.selection    = { 0.10f, 0.10f, 0.12f, 0.80f };
    } else {
        theme.backdropTint = { 0.03f, 0.03f, 0.05f, 0.55f };
        theme.chip         = { 1.00f, 1.00f, 1.00f, 0.14f };
        theme.chipCurrent  = { 1.00f, 1.00f, 1.00f, 0.34f };
        theme.chipBorder   = { 1.00f, 1.00f, 1.00f, 0.55f };
        theme.text         = { 0.96f, 0.96f, 0.98f, 1.00f };
        theme.selection    = { 1.00f, 1.00f, 1.00f, 0.90f };
    }
    return theme;
}

bool SystemUsesLightTheme() {
    DWORD value = 0, size = sizeof(value);
    if (::RegGetValueW(HKEY_CURRENT_USER,
                       L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                       L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr,
                       &value, &size) != ERROR_SUCCESS)
        return false;
    return value != 0;
}

bool ResolveLightTheme() {
    const std::wstring& theme = config::Current().theme;
    if (theme == L"light") return true;
    if (theme == L"dark")  return false;
    return SystemUsesLightTheme();
}

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

} // namespace

// ---------------------------------------------------------------------------

struct Mission::Impl {
    HINSTANCE instance       = nullptr;
    HWND      hwnd           = nullptr;
    HWND      notifyWindow   = nullptr;
    HWND      restoreWindow  = nullptr;   // what had focus before we took it
    UINT      activateMessage = 0;
    UINT      dismissMessage  = 0;
    UINT      spaceMessage    = 0;

    bool ready   = false;
    bool visible = false;

    WUC::Compositor                   compositor{ nullptr };
    WUC::Desktop::DesktopWindowTarget target{ nullptr };
    WUC::CompositionGraphicsDevice    graphics{ nullptr };

    ComPtr<ID2D1Device>    d2dDevice;
    ComPtr<ID2D1Factory1>  d2dFactory;
    ComPtr<IDWriteFactory> dwriteFactory;

    // Whichever DirectComposition device interface the compositor will admit
    // to. Only used as an opaque pointer to hand to DWM.
    //
    // winrt::com_ptr rather than the project's own ComPtr, which is deliberately
    // minimal and has no way to adopt a pointer that is already referenced.
    // This file is MSVC-only anyway, so the WinRT one costs nothing here.
    winrt::com_ptr<IUnknown> dcompDevice;

    WUC::ContainerVisual root{ nullptr };
    WUC::SpriteVisual    backdropVisual{ nullptr };
    WUC::SpriteVisual    spacesVisual{ nullptr };
    WUC::ContainerVisual tileLayer{ nullptr };
    WUC::SpriteVisual    selectionVisual{ nullptr };
    WUC::SpriteVisual    titleVisual{ nullptr };

    WUC::CompositionDrawingSurface backdropSurface{ nullptr };
    WUC::CompositionDrawingSurface spacesSurface{ nullptr };
    WUC::CompositionDrawingSurface titleSurface{ nullptr };

    // One per window in the arrangement.
    struct Tile {
        WUC::ContainerVisual holder{ nullptr };
        WUC::SpriteVisual    sprite{ nullptr };   // snapshot or icon card
        WUC::CompositionDrawingSurface surface{ nullptr };
        HTHUMBNAIL           thumbnail = nullptr;
        RECT                 screenRect{};        // where it is on the overlay
        RECT                 sourceRect{};        // where the window really is
    };

    std::vector<Tile>         tiles;
    std::vector<MissionItem>  items;
    std::vector<MissionSpace> spaces;
    std::vector<mission::SpaceChip> chips;

    HMONITOR monitor  = nullptr;
    RECT     monitorRect{};
    float    dpiScale = 1.0f;
    Theme    theme    = MakeTheme(false);
    bool     themeIsLight = false;
    int      hovered  = -1;
    int      hoveredChip = -1;

    bool CreateMissionWindow();
    bool CreateDevices();
    bool CreateVisualTree();

    void Build();
    void BakeBackdrop();
    void BakeSpaces();
    void BakeTitle();
    void PositionSelection();
    void ReleaseTiles();

    int  HitTestTile(POINT client) const;
    int  HitTestChip(POINT client) const;
    void SetHovered(int index);
    int  Neighbour(int from, int dx, int dy) const;

    float Scaled(float logical) const { return logical * dpiScale; }
};

namespace {

template <typename F>
bool GuardMission(Mission::Impl& impl, const char* what, F&& fn) {
    if (!impl.ready) return false;
    try {
        fn();
        return true;
    } catch (const winrt::hresult_error& e) {
        MACTAB_FAIL("mission: %s threw 0x%08lX", what,
                    static_cast<unsigned long>(e.code()));
        return false;
    } catch (...) {
        MACTAB_FAIL("mission: %s threw a non-WinRT exception", what);
        return false;
    }
}

ComPtr<ID2D1Bitmap1> UploadBitmap(ID2D1DeviceContext* dc, Bitmap image) {
    if (!dc || image.Empty()) return {};

    PremultiplyInPlace(image);

    D2D1_BITMAP_PROPERTIES1 properties{};
    properties.pixelFormat = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                               D2D1_ALPHA_MODE_PREMULTIPLIED);

    ComPtr<ID2D1Bitmap1> bitmap;
    if (FAILED(dc->CreateBitmap(
            D2D1::SizeU(static_cast<UINT32>(image.width),
                        static_cast<UINT32>(image.height)),
            image.pixels.data(), static_cast<UINT32>(image.width) * 4,
            &properties, bitmap.Put())))
        return {};
    return bitmap;
}

// A rounded rectangle in the icons' shape language, which is what every piece
// of chrome in MacTab uses.
void FillSquircle(ID2D1DeviceContext* dc, ID2D1Factory* factory,
                  float x, float y, float w, float h, float radius,
                  const D2D1_COLOR_F& colour) {
    ComPtr<ID2D1PathGeometry> geometry =
        CreateSquircleGeometry(factory, w, h, radius, 5.0f);
    if (!geometry) return;

    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(dc->CreateSolidColorBrush(colour, brush.Put()))) return;

    const D2D1_MATRIX_3X2_F saved = D2D1::Matrix3x2F::Identity();
    dc->GetTransform(const_cast<D2D1_MATRIX_3X2_F*>(&saved));
    dc->SetTransform(D2D1::Matrix3x2F::Translation(x, y) * saved);
    dc->FillGeometry(geometry.Get(), brush.Get());
    dc->SetTransform(saved);
}

void FillSquircleWith(ID2D1DeviceContext* dc, ID2D1Factory* factory,
                      float x, float y, float w, float h, float radius,
                      ID2D1Brush* brush) {
    ComPtr<ID2D1PathGeometry> geometry =
        CreateSquircleGeometry(factory, w, h, radius, 5.0f);
    if (!geometry || !brush) return;

    D2D1_MATRIX_3X2_F saved{};
    dc->GetTransform(&saved);
    dc->SetTransform(D2D1::Matrix3x2F::Translation(x, y) * saved);
    dc->FillGeometry(geometry.Get(), brush);
    dc->SetTransform(saved);
}

void StrokeSquircle(ID2D1DeviceContext* dc, ID2D1Factory* factory,
                    float x, float y, float w, float h, float radius,
                    float thickness, const D2D1_COLOR_F& colour) {
    ComPtr<ID2D1PathGeometry> geometry =
        CreateSquircleGeometry(factory, w, h, radius, 5.0f);
    if (!geometry) return;

    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(dc->CreateSolidColorBrush(colour, brush.Put()))) return;

    D2D1_MATRIX_3X2_F saved{};
    dc->GetTransform(&saved);
    dc->SetTransform(D2D1::Matrix3x2F::Translation(x, y) * saved);
    dc->DrawGeometry(geometry.Get(), brush.Get(), thickness);
    dc->SetTransform(saved);
}

} // namespace

// ---------------------------------------------------------------------------

Mission::Mission() : m_impl(std::make_unique<Impl>()) {}
Mission::~Mission() { Shutdown(); }

namespace {

LRESULT CALLBACK MissionWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* impl = reinterpret_cast<Mission::Impl*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!impl) return ::DefWindowProcW(hwnd, msg, wParam, lParam);

    switch (msg) {
    case WM_MOUSEMOVE: {
        const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        impl->hoveredChip = impl->HitTestChip(point);
        impl->SetHovered(impl->HitTestTile(point));
        return 0;
    }

    case WM_LBUTTONUP: {
        const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };

        const int chip = impl->HitTestChip(point);
        if (chip >= 0 && impl->notifyWindow) {
            const mission::SpaceChip& c = impl->chips[static_cast<size_t>(chip)];
            ::PostMessageW(impl->notifyWindow, impl->spaceMessage,
                           c.add ? Mission::kSpaceAdd
                                 : static_cast<WPARAM>(c.index), 0);
            return 0;
        }

        const int tile = impl->HitTestTile(point);
        if (impl->notifyWindow) {
            // A click on empty space dismisses, which is what macOS does and
            // what makes the gesture feel like a place rather than a dialog.
            if (tile >= 0)
                ::PostMessageW(impl->notifyWindow, impl->activateMessage,
                               static_cast<WPARAM>(tile), 0);
            else
                ::PostMessageW(impl->notifyWindow, impl->dismissMessage, 0, 0);
        }
        return 0;
    }

    case WM_KEYDOWN: {
        if (!impl->notifyWindow) return 0;
        switch (wParam) {
        case VK_ESCAPE:
            ::PostMessageW(impl->notifyWindow, impl->dismissMessage, 0, 0);
            return 0;
        case VK_RETURN:
        case VK_SPACE:
            if (impl->hovered >= 0)
                ::PostMessageW(impl->notifyWindow, impl->activateMessage,
                               static_cast<WPARAM>(impl->hovered), 0);
            return 0;
        case VK_LEFT:  impl->SetHovered(impl->Neighbour(impl->hovered, -1,  0)); return 0;
        case VK_RIGHT: impl->SetHovered(impl->Neighbour(impl->hovered,  1,  0)); return 0;
        case VK_UP:    impl->SetHovered(impl->Neighbour(impl->hovered,  0, -1)); return 0;
        case VK_DOWN:  impl->SetHovered(impl->Neighbour(impl->hovered,  0,  1)); return 0;
        case VK_TAB:
            impl->SetHovered(impl->tiles.empty()
                                 ? -1
                                 : (impl->hovered + 1) %
                                       static_cast<int>(impl->tiles.size()));
            return 0;
        default:
            return 0;
        }
    }

    // Clicking away, or anything else taking foreground, closes it. Mission
    // Control is a place you are in, and you cannot be in it and somewhere else.
    case WM_KILLFOCUS:
        if (impl->visible && impl->notifyWindow)
            ::PostMessageW(impl->notifyWindow, impl->dismissMessage, 0, 0);
        return 0;

    default:
        break;
    }

    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

bool Mission::Impl::CreateMissionWindow() {
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = MissionWndProc;
    wc.hInstance     = instance;
    wc.lpszClassName = kMissionClass;
    wc.hCursor       = ::LoadCursorW(nullptr, IDC_ARROW);
    ::RegisterClassExW(&wc);

    // Activatable, unlike the switcher's panel.
    //
    // The panel must never take foreground, because it exists to hand
    // foreground to something else and the activation path depends on the old
    // window still being in front. Mission Control is the opposite: it is a
    // place the user is in, it owns the keyboard while it is up, and losing
    // focus is exactly the signal to close.
    hwnd = ::CreateWindowExW(
        WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        kMissionClass, L"MacTab Mission Control", WS_POPUP,
        0, 0, 1, 1, nullptr, nullptr, instance, nullptr);

    if (!hwnd) {
        MACTAB_FAIL("mission: CreateWindowEx failed (err %lu)", ::GetLastError());
        return false;
    }

    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    return true;
}

bool Mission::Impl::CreateDevices() {
    auto interop = compositor.as<WUCABI::ICompositorInterop>();

    winrt::com_ptr<WUCABI::ICompositionGraphicsDevice> abiGraphics;
    if (FAILED(interop->CreateGraphicsDevice(d2dDevice.Get(), abiGraphics.put()))) {
        MACTAB_FAIL("mission: CreateGraphicsDevice failed");
        return false;
    }
    graphics = abiGraphics.as<WUC::CompositionGraphicsDevice>();

    // The DirectComposition device behind the compositor, which is what DWM
    // wants in order to hand back a thumbnail visual that belongs to our tree.
    //
    // Tried newest first. Which of these a given build's compositor admits to
    // is not documented anywhere, and the only thing that matters is getting a
    // pointer DWM accepts, so any of them will do.
    if (auto device3 = compositor.try_as<IDCompositionDevice3>())
        dcompDevice = device3.as<IUnknown>();
    else if (auto desktop = compositor.try_as<IDCompositionDesktopDevice>())
        dcompDevice = desktop.as<IUnknown>();
    else if (auto device2 = compositor.try_as<IDCompositionDevice2>())
        dcompDevice = device2.as<IUnknown>();

    if (!dcompDevice)
        MACTAB_WARN("mission: no DirectComposition device behind the compositor; "
                    "falling back to snapshots");

    return true;
}

bool Mission::Impl::CreateVisualTree() {
    auto interop =
        compositor.as<ABI::Windows::UI::Composition::Desktop::ICompositorDesktopInterop>();
    if (FAILED(interop->CreateDesktopWindowTarget(
            hwnd, false,
            reinterpret_cast<ABI::Windows::UI::Composition::Desktop::IDesktopWindowTarget**>(
                winrt::put_abi(target))))) {
        MACTAB_FAIL("mission: CreateDesktopWindowTarget failed");
        return false;
    }

    root = compositor.CreateContainerVisual();
    root.RelativeSizeAdjustment({ 1.0f, 1.0f });
    target.Root(root);

    backdropVisual  = compositor.CreateSpriteVisual();
    tileLayer       = compositor.CreateContainerVisual();
    selectionVisual = compositor.CreateSpriteVisual();
    titleVisual     = compositor.CreateSpriteVisual();
    spacesVisual    = compositor.CreateSpriteVisual();

    auto children = root.Children();
    children.InsertAtTop(backdropVisual);
    children.InsertAtTop(selectionVisual);
    children.InsertAtTop(tileLayer);
    children.InsertAtTop(titleVisual);
    children.InsertAtTop(spacesVisual);

    root.Opacity(0.0f);
    return true;
}

bool Mission::Initialize(HINSTANCE instance, HWND notifyWindow,
                         UINT activateMessage, UINT dismissMessage,
                         UINT spaceMessage) {
    Impl& impl = *m_impl;
    impl.instance        = instance;
    impl.notifyWindow    = notifyWindow;
    impl.activateMessage = activateMessage;
    impl.dismissMessage  = dismissMessage;
    impl.spaceMessage    = spaceMessage;

    try {
        // A compositor of its own, on the thread the panel already put into an
        // apartment and gave a dispatcher queue.
        //
        // Not shared with the panel, which would be the obvious economy, and
        // the reason is the thumbnails: DWM is handed the DirectComposition
        // device sitting behind a compositor and returns a visual that belongs
        // to that compositor's tree. Device and tree have to be the same one,
        // and the panel does not expose either. Two compositors on one thread
        // is supported and costs a few hundred kilobytes.
        impl.compositor = WUC::Compositor();
    } catch (const winrt::hresult_error& e) {
        MACTAB_FAIL("mission: Compositor construction failed (0x%08lX)",
                    static_cast<unsigned long>(e.code()));
        return false;
    }

    D2D1_FACTORY_OPTIONS options{};
    if (FAILED(::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                   __uuidof(ID2D1Factory1), &options,
                                   impl.d2dFactory.PutVoid()))) {
        MACTAB_FAIL("mission: D2D1CreateFactory failed");
        return false;
    }

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    ComPtr<ID3D11Device> d3dDevice;
    HRESULT hr = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                     nullptr, 0, D3D11_SDK_VERSION,
                                     d3dDevice.Put(), nullptr, nullptr);
    if (FAILED(hr))
        hr = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
                                 nullptr, 0, D3D11_SDK_VERSION,
                                 d3dDevice.Put(), nullptr, nullptr);
    if (FAILED(hr)) {
        MACTAB_FAIL("mission: no D3D11 device (0x%08lX)", static_cast<unsigned long>(hr));
        return false;
    }

    ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(d3dDevice->QueryInterface(IID_PPV_ARGS(dxgiDevice.Put()))) ||
        FAILED(impl.d2dFactory->CreateDevice(dxgiDevice.Get(), impl.d2dDevice.Put()))) {
        MACTAB_FAIL("mission: could not create the D2D device");
        return false;
    }

    if (FAILED(::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                     reinterpret_cast<IUnknown**>(impl.dwriteFactory.Put())))) {
        MACTAB_FAIL("mission: DWriteCreateFactory failed");
        return false;
    }

    if (!impl.CreateMissionWindow()) return false;
    if (!impl.CreateDevices())       return false;
    if (!impl.CreateVisualTree())    return false;

    // Probe the thumbnail path now rather than on the reveal path, and log
    // which tier won so a screenshot arrives with the answer attached.
    //
    // The source is the host window rather than the overlay itself. Whether DWM
    // will compose a window's thumbnail into that same window is not documented
    // and is exactly the kind of thing that would answer "no" for a reason that
    // has nothing to do with whether the export works.
    thumbnail::Probe(impl.hwnd, notifyWindow);

    impl.ready = true;
    MACTAB_DIAG("mission: initialised, thumbnails via %s",
                thumbnail::TierName(thumbnail::Current()));
    return true;
}

void Mission::Shutdown() {
    Impl& impl = *m_impl;
    if (!impl.ready && !impl.hwnd) return;

    impl.ready = false;
    impl.ReleaseTiles();

    impl.root            = nullptr;
    impl.backdropVisual  = nullptr;
    impl.spacesVisual    = nullptr;
    impl.tileLayer       = nullptr;
    impl.selectionVisual = nullptr;
    impl.titleVisual     = nullptr;
    impl.backdropSurface = nullptr;
    impl.spacesSurface   = nullptr;
    impl.titleSurface    = nullptr;
    impl.target          = nullptr;
    impl.graphics        = nullptr;
    impl.compositor      = nullptr;
    impl.dcompDevice = nullptr;

    if (impl.hwnd) {
        ::DestroyWindow(impl.hwnd);
        impl.hwnd = nullptr;
    }
    MACTAB_DIAG("mission: shut down");
}

bool Mission::Ready() const   { return m_impl->ready; }

HWND Mission::ItemWindow(int index) const {
    if (index < 0 || index >= static_cast<int>(m_impl->items.size())) return nullptr;
    return m_impl->items[static_cast<size_t>(index)].hwnd;
}

bool Mission::Visible() const { return m_impl->visible; }
HWND Mission::Hwnd() const    { return m_impl->hwnd; }

void Mission::Impl::ReleaseTiles() {
    if (tileLayer)
        tileLayer.Children().RemoveAll();

    for (Tile& tile : tiles)
        thumbnail::ReleaseSharedVisual(tile.thumbnail);

    tiles.clear();
}

// ---------------------------------------------------------------------------

void Mission::Impl::BakeBackdrop() {
    const int width  = monitorRect.right - monitorRect.left;
    const int height = monitorRect.bottom - monitorRect.top;
    if (width <= 0 || height <= 0) return;

    const int smallW = (std::max)(1, static_cast<int>(width  * kBackdropDownscale));
    const int smallH = (std::max)(1, static_cast<int>(height * kBackdropDownscale));

    backdropSurface = graphics.CreateDrawingSurface(
        { static_cast<float>(smallW), static_cast<float>(smallH) },
        winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
        winrt::Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);

    Bitmap paper = wallpaper::ForMonitor(monitor, smallW, smallH);

    SurfaceDraw draw(backdropSurface);
    if (!draw.ok) return;

    ID2D1DeviceContext* dc = draw.dc.Get();
    dc->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(draw.offset.x),
                                                   static_cast<float>(draw.offset.y)));
    dc->Clear(D2D1::ColorF(0, 0, 0, 0));

    if (paper.Empty()) {
        // No picture, or it could not be read. The desktop colour is what the
        // user would be looking at anyway.
        const uint32_t solid = wallpaper::SolidColour();
        dc->Clear(D2D1::ColorF(RedOf(solid)   / 255.0f,
                               GreenOf(solid) / 255.0f,
                               BlueOf(solid)  / 255.0f, 1.0f));
    } else if (ComPtr<ID2D1Bitmap1> source = UploadBitmap(dc, std::move(paper))) {
        ComPtr<ID2D1Effect> blur;
        if (SUCCEEDED(dc->CreateEffect(CLSID_D2D1GaussianBlur, blur.Put()))) {
            blur->SetInput(0, source.Get());
            blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION,
                           Scaled(config::Current().missionBlurSigma) * kBackdropDownscale);
            blur->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE,
                           D2D1_BORDER_MODE_HARD);
            dc->DrawImage(blur.Get(), D2D1_INTERPOLATION_MODE_LINEAR);
        } else {
            dc->DrawBitmap(source.Get());
        }
    }

    // The dim. Mission Control pushes the desktop back so the windows read as
    // floating above it; without this the arrangement competes with the
    // wallpaper for attention and a busy picture wins.
    D2D1_COLOR_F dim = theme.backdropTint;
    dim.a = config::Current().missionDim;

    ComPtr<ID2D1SolidColorBrush> brush;
    if (SUCCEEDED(dc->CreateSolidColorBrush(dim, brush.Put())))
        dc->FillRectangle(D2D1::RectF(0, 0, static_cast<float>(smallW),
                                      static_cast<float>(smallH)),
                          brush.Get());

    auto surfaceBrush = compositor.CreateSurfaceBrush(backdropSurface);
    surfaceBrush.Stretch(WUC::CompositionStretch::Fill);
    backdropVisual.Brush(surfaceBrush);
    backdropVisual.Size({ static_cast<float>(width), static_cast<float>(height) });
    backdropVisual.Offset({ 0.0f, 0.0f, 0.0f });
}

void Mission::Impl::BakeSpaces() {
    const float width  = static_cast<float>(monitorRect.right - monitorRect.left);
    const float strip  = Scaled(kSpacesStripHeight);

    chips.clear();
    if (spaces.empty()) {
        spacesVisual.Size({ 0.0f, 0.0f });
        return;
    }

    const float height = static_cast<float>(monitorRect.bottom - monitorRect.top);
    // Centre the miniatures in the band ABOVE the labels, not in the whole
    // band, so the names have somewhere to sit.
    chips = mission::LayoutSpaces(static_cast<int>(spaces.size()), width,
                                  strip - Scaled(kSpaceLabelHeight),
                                  Scaled(kSpaceChipHeight),
                                  (height > 0.0f) ? width / height : 1.6f,
                                  Scaled(kSpaceChipGap));

    spacesSurface = graphics.CreateDrawingSurface(
        { width, strip },
        winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
        winrt::Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);

    SurfaceDraw draw(spacesSurface);
    if (!draw.ok) return;

    ID2D1DeviceContext* dc = draw.dc.Get();
    dc->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(draw.offset.x),
                                                   static_cast<float>(draw.offset.y)));
    dc->Clear(D2D1::ColorF(0, 0, 0, 0));

    ComPtr<IDWriteTextFormat> format;
    dwriteFactory->CreateTextFormat(L"Segoe UI Variable Display", nullptr,
                                    DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                    DWRITE_FONT_STYLE_NORMAL,
                                    DWRITE_FONT_STRETCH_NORMAL,
                                    Scaled(13.0f), L"", format.Put());
    if (!format)
        dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr,
                                        DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                        DWRITE_FONT_STYLE_NORMAL,
                                        DWRITE_FONT_STRETCH_NORMAL,
                                        Scaled(13.0f), L"", format.Put());
    if (format) {
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    ComPtr<ID2D1SolidColorBrush> textBrush;
    dc->CreateSolidColorBrush(theme.text, textBrush.Put());

    const float radius = Scaled(kSpaceChipRadius);

    // Every desktop shows the wallpaper, which is what an empty one looks like
    // and is a great deal closer to the real thing than a grey rectangle.
    //
    // Not a picture of what is actually on each desktop. Windows on another
    // desktop are shell-cloaked, and DWM does not compose a cloaked window
    // through any path available here, so there is nothing to show but the
    // desktop itself. Saying so in the release notes beats pretending.
    // ID2D1BitmapBrush1 and the ...PROPERTIES1 struct, not the plain ones.
    // ID2D1DeviceContext declares its own overloads of CreateBitmapBrush, which
    // hide every one the render target had, so the older form does not resolve.
    ComPtr<ID2D1BitmapBrush1> paperBrush;
    if (!chips.empty()) {
        const int chipW = (std::max)(1, static_cast<int>(chips[0].w));
        const int chipH = (std::max)(1, static_cast<int>(chips[0].h));
        Bitmap paper = wallpaper::ForMonitor(monitor, chipW, chipH);
        if (ComPtr<ID2D1Bitmap1> bitmap = UploadBitmap(dc, std::move(paper))) {
            D2D1_BITMAP_BRUSH_PROPERTIES1 props{};
            props.extendModeX = D2D1_EXTEND_MODE_CLAMP;
            props.extendModeY = D2D1_EXTEND_MODE_CLAMP;
            props.interpolationMode = D2D1_INTERPOLATION_MODE_LINEAR;
            // Four arguments, with no brush properties. The three-argument
            // convenience form is an inline helper that only some SDK
            // versions carry, and the explicit one resolves on all of them.
            dc->CreateBitmapBrush(bitmap.Get(), &props, nullptr, paperBrush.Put());
        }
    }

    for (const mission::SpaceChip& chip : chips) {
        const bool current = !chip.add &&
                             chip.index >= 0 &&
                             chip.index < static_cast<int>(spaces.size()) &&
                             spaces[static_cast<size_t>(chip.index)].current;

        if (!chip.add && paperBrush) {
            paperBrush->SetTransform(D2D1::Matrix3x2F::Translation(chip.x, chip.y));
            FillSquircleWith(dc, d2dFactory.Get(), chip.x, chip.y, chip.w, chip.h,
                             radius, paperBrush.Get());

            // The desktop you are not on is pushed back, the same way the
            // wallpaper behind the arrangement is.
            if (!current)
                FillSquircle(dc, d2dFactory.Get(), chip.x, chip.y, chip.w, chip.h,
                             radius,
                             themeIsLight ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.45f)
                                          : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.45f));
        } else {
            FillSquircle(dc, d2dFactory.Get(), chip.x, chip.y, chip.w, chip.h, radius,
                         current ? theme.chipCurrent : theme.chip);
        }

        StrokeSquircle(dc, d2dFactory.Get(), chip.x, chip.y, chip.w, chip.h,
                       radius, Scaled(current ? 2.5f : 1.0f),
                       current ? theme.chipBorder
                               : D2D1::ColorF(theme.chipBorder.r, theme.chipBorder.g,
                                              theme.chipBorder.b,
                                              theme.chipBorder.a * 0.35f));

        if (!format || !textBrush) continue;

        if (chip.add) {
            dc->DrawTextW(L"+", 1, format.Get(),
                          D2D1::RectF(chip.x, chip.y, chip.x + chip.w, chip.y + chip.h),
                          textBrush.Get());
        } else if (chip.index >= 0 && chip.index < static_cast<int>(spaces.size())) {
            // Under the miniature, not on it. A name printed over a photograph
            // is unreadable on some fraction of all wallpapers, and there is no
            // colour that fixes that.
            const std::wstring& name = spaces[static_cast<size_t>(chip.index)].name;
            dc->DrawTextW(name.c_str(), static_cast<UINT32>(name.size()),
                          format.Get(),
                          D2D1::RectF(chip.x, chip.y + chip.h,
                                      chip.x + chip.w, strip),
                          textBrush.Get());
        }
    }

    spacesVisual.Brush(compositor.CreateSurfaceBrush(spacesSurface));
    spacesVisual.Size({ width, strip });
    spacesVisual.Offset({ 0.0f, 0.0f, 0.0f });
}

void Mission::Impl::BakeTitle() {
    if (hovered < 0 || hovered >= static_cast<int>(items.size()) ||
        hovered >= static_cast<int>(tiles.size())) {
        titleVisual.Size({ 0.0f, 0.0f });
        return;
    }

    const MissionItem& item = items[static_cast<size_t>(hovered)];
    const std::wstring text = item.title.empty() ? item.appName : item.title;

    const float height = Scaled(kTitleHeight);
    const float width  = (std::min)(Scaled(560.0f),
                                    static_cast<float>(monitorRect.right - monitorRect.left));

    titleSurface = graphics.CreateDrawingSurface(
        { width, height },
        winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
        winrt::Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);

    SurfaceDraw draw(titleSurface);
    if (!draw.ok) return;

    ID2D1DeviceContext* dc = draw.dc.Get();
    dc->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(draw.offset.x),
                                                   static_cast<float>(draw.offset.y)));
    dc->Clear(D2D1::ColorF(0, 0, 0, 0));

    ComPtr<IDWriteTextFormat> format;
    dwriteFactory->CreateTextFormat(L"Segoe UI Variable Display", nullptr,
                                    DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                    DWRITE_FONT_STYLE_NORMAL,
                                    DWRITE_FONT_STRETCH_NORMAL,
                                    Scaled(14.0f), L"", format.Put());
    if (!format) return;
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    ComPtr<IDWriteInlineObject> ellipsis;
    if (SUCCEEDED(dwriteFactory->CreateEllipsisTrimmingSign(format.Get(), ellipsis.Put()))) {
        DWRITE_TRIMMING trimming{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
        format->SetTrimming(&trimming, ellipsis.Get());
    }

    // A dark capsule behind the text. The title sits over the wallpaper, which
    // can be any colour at all, so a bare string is unreadable on roughly half
    // of all desktops.
    ComPtr<ID2D1SolidColorBrush> plate;
    if (SUCCEEDED(dc->CreateSolidColorBrush(
            themeIsLight ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.82f)
                         : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.55f), plate.Put()))) {
        const D2D1_ROUNDED_RECT capsule{
            D2D1::RectF(0.0f, 0.0f, width, height), height * 0.5f, height * 0.5f };
        dc->FillRoundedRectangle(capsule, plate.Get());
    }

    ComPtr<ID2D1SolidColorBrush> brush;
    if (SUCCEEDED(dc->CreateSolidColorBrush(theme.text, brush.Put())))
        dc->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format.Get(),
                      D2D1::RectF(Scaled(14.0f), 0.0f, width - Scaled(14.0f), height),
                      brush.Get());

    titleVisual.Brush(compositor.CreateSurfaceBrush(titleSurface));
    titleVisual.Size({ width, height });

    const RECT& rect = tiles[static_cast<size_t>(hovered)].screenRect;
    const float centre = static_cast<float>(rect.left + rect.right) * 0.5f;
    titleVisual.Offset({ centre - width * 0.5f,
                         static_cast<float>(rect.bottom) + Scaled(kTitleGap), 0.0f });
}

void Mission::Impl::PositionSelection() {
    if (hovered < 0 || hovered >= static_cast<int>(tiles.size())) {
        selectionVisual.Opacity(0.0f);
        return;
    }

    const RECT& rect = tiles[static_cast<size_t>(hovered)].screenRect;
    const float inset = Scaled(6.0f);

    // Springing from wherever it was left is right between two tiles and wrong
    // for the first one, where "wherever it was left" is the top left corner of
    // the screen and the highlight would fly in from nothing.
    const bool wasHidden = selectionVisual.Opacity() < 0.5f;
    selectionVisual.Opacity(1.0f);
    selectionVisual.Size({ static_cast<float>(rect.right - rect.left) + inset * 2,
                           static_cast<float>(rect.bottom - rect.top) + inset * 2 });

    const WFN::float3 destination{ static_cast<float>(rect.left) - inset,
                                   static_cast<float>(rect.top) - inset, 0.0f };

    if (wasHidden) {
        selectionVisual.Offset(destination);
        return;
    }

    auto spring = compositor.CreateSpringVector3Animation();
    spring.DampingRatio(0.85f);
    spring.Period(std::chrono::milliseconds(45));
    spring.FinalValue(destination);
    selectionVisual.StartAnimation(L"Offset", spring);
}

void Mission::Impl::SetHovered(int index) {
    if (index == hovered) return;
    hovered = index;

    GuardMission(*this, "SetHovered", [&] {
        PositionSelection();
        BakeTitle();
    });
}

int Mission::Impl::HitTestTile(POINT client) const {
    for (size_t i = 0; i < tiles.size(); ++i) {
        const RECT& r = tiles[i].screenRect;
        if (client.x >= r.left && client.x < r.right &&
            client.y >= r.top  && client.y < r.bottom)
            return static_cast<int>(i);
    }
    return -1;
}

int Mission::Impl::HitTestChip(POINT client) const {
    for (size_t i = 0; i < chips.size(); ++i) {
        const mission::SpaceChip& c = chips[i];
        if (client.x >= c.x && client.x < c.x + c.w &&
            client.y >= c.y && client.y < c.y + c.h)
            return static_cast<int>(i);
    }
    return -1;
}

// The nearest tile in a direction, by centre distance.
//
// Not an index step. The arrangement has no rows, so "the next one to the
// right" is a geometric question, and stepping through the list would jump
// across the screen in a way that looks random.
int Mission::Impl::Neighbour(int from, int dx, int dy) const {
    if (tiles.empty()) return -1;
    if (from < 0 || from >= static_cast<int>(tiles.size())) return 0;

    const RECT& origin = tiles[static_cast<size_t>(from)].screenRect;
    const float ox = static_cast<float>(origin.left + origin.right) * 0.5f;
    const float oy = static_cast<float>(origin.top + origin.bottom) * 0.5f;

    int   best     = -1;
    float bestCost = 0.0f;

    for (size_t i = 0; i < tiles.size(); ++i) {
        if (static_cast<int>(i) == from) continue;

        const RECT& r = tiles[i].screenRect;
        const float cx = static_cast<float>(r.left + r.right) * 0.5f;
        const float cy = static_cast<float>(r.top + r.bottom) * 0.5f;

        const float along  = (cx - ox) * dx + (cy - oy) * dy;
        const float across = (cx - ox) * dy + (cy - oy) * dx;
        if (along <= 1.0f) continue;   // not in the requested direction

        // Distance along the direction, plus a penalty for drifting off it, so
        // a tile straight ahead beats a nearer one far to the side.
        const float cost = along + std::fabs(across) * 2.0f;
        if (best < 0 || cost < bestCost) {
            best     = static_cast<int>(i);
            bestCost = cost;
        }
    }

    return (best >= 0) ? best : from;
}

// ---------------------------------------------------------------------------

void Mission::Impl::Build() {
    ReleaseTiles();

    const float width  = static_cast<float>(monitorRect.right - monitorRect.left);
    const float height = static_cast<float>(monitorRect.bottom - monitorRect.top);

    const float margin  = Scaled(kOuterMargin);
    const float stripH  = spaces.empty() ? 0.0f : Scaled(kSpacesStripHeight);
    const float regionX = margin;
    const float regionY = stripH + margin;
    // Floored, not just computed. On a short display the strip, the margins and
    // the title band can add up to more than the screen, and Layout answers a
    // non-positive region with an empty result, which the loop below would then
    // index straight past the end of.
    const float regionW = (std::max)(Scaled(160.0f), width - margin * 2);
    const float regionH = (std::max)(Scaled(120.0f),
                                     height - stripH - margin * 2 -
                                     Scaled(kTitleHeight + kTitleGap));

    std::vector<mission::Window> windows;
    windows.reserve(items.size());
    for (const MissionItem& item : items) {
        mission::Window w;
        w.x     = static_cast<float>(item.bounds.left - monitorRect.left);
        w.y     = static_cast<float>(item.bounds.top  - monitorRect.top);
        w.w     = static_cast<float>((std::max)(1l, item.bounds.right - item.bounds.left));
        w.h     = static_cast<float>((std::max)(1l, item.bounds.bottom - item.bounds.top));
        w.group = item.group;
        w.order = item.order;
        windows.push_back(w);
    }

    mission::Params params;
    params.gap        = Scaled(config::Current().missionGap);
    params.clusterGap = Scaled(config::Current().missionClusterGap);
    params.groupByApp = config::Current().missionGroupByApp;

    const double started = NowMs();
    const mission::Result result = mission::Layout(windows, regionW, regionH, params);

    MACTAB_DIAG("mission: %zu window(s) arranged in %.2f ms, scale %.3f, "
                "%d pass(es)%s, agreement %.2f",
                items.size(), NowMs() - started, result.scale, result.iterations,
                result.relaxed ? "" : " (grid fallback)",
                mission::SpatialAgreement(windows, result));

    // Belt and braces against the case above: if the arrangement ever comes
    // back short, draw what it did produce rather than reading past it.
    if (result.tiles.size() != items.size()) {
        MACTAB_FAIL("mission: arrangement returned %zu placement(s) for %zu window(s)",
                    result.tiles.size(), items.size());
        return;
    }

    tiles.resize(items.size());

    const double snapshotsStarted = NowMs();
    int snapshots = 0, skipped = 0;

    for (size_t i = 0; i < items.size(); ++i) {
        const mission::Placement& place = result.tiles[i];
        Tile& tile = tiles[i];

        tile.screenRect = RECT{
            static_cast<LONG>(regionX + place.x),
            static_cast<LONG>(regionY + place.y),
            static_cast<LONG>(regionX + place.x + place.w),
            static_cast<LONG>(regionY + place.y + place.h),
        };
        tile.sourceRect = RECT{
            items[i].bounds.left   - monitorRect.left,
            items[i].bounds.top    - monitorRect.top,
            items[i].bounds.right  - monitorRect.left,
            items[i].bounds.bottom - monitorRect.top,
        };

        tile.holder = compositor.CreateContainerVisual();
        tile.holder.Size({ place.w, place.h });
        tile.holder.Offset({ regionX + place.x, regionY + place.y, 0.0f });
        tileLayer.Children().InsertAtTop(tile.holder);

        bool haveThumbnail = false;

        // Tier 1: a visual DWM owns, living in our tree. Live, and animatable
        // by the compositor at no CPU cost, which is what the reveal needs.
        if (dcompDevice) {
            void* raw = nullptr;
            if (thumbnail::CreateSharedVisual(dcompDevice.get(), hwnd, items[i].hwnd,
                                              &raw, &tile.thumbnail) && raw) {
                winrt::com_ptr<IUnknown> unknown;
                unknown.attach(reinterpret_cast<IUnknown*>(raw));

                if (auto visual = unknown.try_as<WUC::Visual>()) {
                    SIZE source{};
                    if (!thumbnail::SourceSize(items[i].hwnd, source) ||
                        source.cx <= 0 || source.cy <= 0) {
                        source.cx = tile.sourceRect.right - tile.sourceRect.left;
                        source.cy = tile.sourceRect.bottom - tile.sourceRect.top;
                    }

                    // The visual draws at the source window's own size, so it
                    // is scaled to the slot rather than resized.
                    visual.Scale({ place.w / (std::max)(1.0f, static_cast<float>(source.cx)),
                                   place.h / (std::max)(1.0f, static_cast<float>(source.cy)),
                                   1.0f });
                    tile.holder.Children().InsertAtTop(visual);
                    haveThumbnail = true;
                }
            }
        }

        // Tier 2 and 3 share a sprite: either the snapshot or the icon card
        // goes into the same surface, so the rest of the code does not care
        // which it got.
        if (!haveThumbnail) {
            // Snapshots are taken on this thread, which is the thread that
            // owes a frame, and each one is a fifty millisecond ping plus a
            // full-size readback of somebody else's window. Thirty of those is
            // seconds, so the tier gets a budget and everything past it gets a
            // card. A late window is worse than a plain one.
            Bitmap content;
            if (thumbnail::Current() != thumbnail::Tier::IconOnly &&
                NowMs() - snapshotsStarted < kSnapshotBudgetMs) {
                content = thumbnail::Snapshot(items[i].hwnd,
                                              static_cast<int>(place.w),
                                              static_cast<int>(place.h));
                ++snapshots;
            } else if (thumbnail::Current() != thumbnail::Tier::IconOnly) {
                ++skipped;
            }

            tile.surface = graphics.CreateDrawingSurface(
                { place.w, place.h },
                winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                winrt::Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);

            SurfaceDraw draw(tile.surface);
            if (draw.ok) {
                ID2D1DeviceContext* dc = draw.dc.Get();
                dc->SetTransform(D2D1::Matrix3x2F::Translation(
                    static_cast<float>(draw.offset.x), static_cast<float>(draw.offset.y)));
                dc->Clear(D2D1::ColorF(0, 0, 0, 0));

                if (!content.Empty()) {
                    if (ComPtr<ID2D1Bitmap1> bitmap = UploadBitmap(dc, std::move(content)))
                        dc->DrawBitmap(bitmap.Get(),
                                       D2D1::RectF(0.0f, 0.0f, place.w, place.h));
                } else {
                    // The card. A window-shaped plate with the app's icon in
                    // the middle of it, which is a design rather than a hole.
                    FillSquircle(dc, d2dFactory.Get(), 0.0f, 0.0f, place.w, place.h,
                                 Scaled(10.0f),
                                 themeIsLight ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.80f)
                                              : D2D1::ColorF(0.10f, 0.10f, 0.13f, 0.85f));

                    if (!items[i].icon.Empty()) {
                        const float side = (std::min)(place.w, place.h) * 0.42f;
                        if (ComPtr<ID2D1Bitmap1> icon =
                                UploadBitmap(dc, items[i].icon)) {
                            dc->DrawBitmap(icon.Get(),
                                           D2D1::RectF((place.w - side) * 0.5f,
                                                       (place.h - side) * 0.5f,
                                                       (place.w + side) * 0.5f,
                                                       (place.h + side) * 0.5f));
                        }
                    }
                }
            }

            tile.sprite = compositor.CreateSpriteVisual();
            tile.sprite.Size({ place.w, place.h });
            tile.sprite.Brush(compositor.CreateSurfaceBrush(tile.surface));
            tile.holder.Children().InsertAtTop(tile.sprite);
        }
    }

    if (skipped > 0)
        MACTAB_WARN("mission: %d snapshot(s) taken in %.0f ms, %d window(s) fell "
                    "back to cards", snapshots, NowMs() - snapshotsStarted, skipped);

    // The selection outline, sized per hover.
    selectionVisual.Brush(compositor.CreateColorBrush(
        WUI::ColorHelper::FromArgb(
            static_cast<uint8_t>(theme.selection.a * 255.0f),
            static_cast<uint8_t>(theme.selection.r * 255.0f),
            static_cast<uint8_t>(theme.selection.g * 255.0f),
            static_cast<uint8_t>(theme.selection.b * 255.0f))));
    selectionVisual.Opacity(0.0f);
}

void Mission::Show(HMONITOR monitor, std::vector<MissionItem> items,
                   std::vector<MissionSpace> spaces) {
    Impl& impl = *m_impl;
    if (impl.visible) return;

    const bool ok = GuardMission(impl, "Show", [&] {
        MACTAB_DIAG_TIMER("mission: Show");

        impl.monitor = monitor;
        impl.items   = std::move(items);
        impl.spaces  = std::move(spaces);
        impl.hovered = -1;

        MONITORINFO info{};
        info.cbSize = sizeof(info);
        if (::GetMonitorInfoW(monitor, &info))
            impl.monitorRect = info.rcMonitor;

        UINT dpiX = 96, dpiY = 96;
        if (SUCCEEDED(::GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)))
            impl.dpiScale = static_cast<float>(dpiX) / 96.0f;

        impl.themeIsLight = ResolveLightTheme();
        impl.theme        = MakeTheme(impl.themeIsLight);

        ::SetWindowPos(impl.hwnd, HWND_TOPMOST,
                       impl.monitorRect.left, impl.monitorRect.top,
                       impl.monitorRect.right - impl.monitorRect.left,
                       impl.monitorRect.bottom - impl.monitorRect.top,
                       SWP_NOACTIVATE);

        impl.BakeBackdrop();
        impl.BakeSpaces();
        impl.Build();

        impl.restoreWindow = ::GetForegroundWindow();

        ::ShowWindow(impl.hwnd, SW_SHOW);
        ::SetForegroundWindow(impl.hwnd);
        ::SetFocus(impl.hwnd);

        // The reveal. Every window starts at the position and size it really
        // has on screen and travels to its slot, which is the whole illusion:
        // the desktop pulls itself apart rather than a dialog appearing.
        //
        // Offset and Scale only, both on the compositor thread, so this costs
        // this process nothing while it runs.
        const auto duration = std::chrono::milliseconds(
            config::Current().missionRevealMs);

        auto easing = impl.compositor.CreateCubicBezierEasingFunction(
            { 0.22f, 1.0f }, { 0.36f, 1.0f });

        for (Mission::Impl::Tile& tile : impl.tiles) {
            const float finalW = static_cast<float>(tile.screenRect.right - tile.screenRect.left);
            const float finalH = static_cast<float>(tile.screenRect.bottom - tile.screenRect.top);
            if (finalW <= 0.0f || finalH <= 0.0f) continue;

            const float startScaleX =
                static_cast<float>(tile.sourceRect.right - tile.sourceRect.left) / finalW;
            const float startScaleY =
                static_cast<float>(tile.sourceRect.bottom - tile.sourceRect.top) / finalH;

            auto offset = impl.compositor.CreateVector3KeyFrameAnimation();
            offset.InsertKeyFrame(0.0f, { static_cast<float>(tile.sourceRect.left),
                                          static_cast<float>(tile.sourceRect.top), 0.0f });
            offset.InsertKeyFrame(1.0f, { static_cast<float>(tile.screenRect.left),
                                          static_cast<float>(tile.screenRect.top), 0.0f },
                                  easing);
            offset.Duration(duration);
            tile.holder.StartAnimation(L"Offset", offset);

            auto scale = impl.compositor.CreateVector3KeyFrameAnimation();
            scale.InsertKeyFrame(0.0f, { startScaleX, startScaleY, 1.0f });
            scale.InsertKeyFrame(1.0f, { 1.0f, 1.0f, 1.0f }, easing);
            scale.Duration(duration);
            tile.holder.StartAnimation(L"Scale", scale);
        }

        auto fade = impl.compositor.CreateScalarKeyFrameAnimation();
        fade.InsertKeyFrame(0.0f, 0.0f);
        fade.InsertKeyFrame(1.0f, 1.0f, easing);
        fade.Duration(duration);
        impl.root.StartAnimation(L"Opacity", fade);

        impl.visible = true;
    });

    if (!ok) {
        MACTAB_FAIL("mission: Show failed; hiding again");
        Hide();
    }
}

void Mission::Hide(bool restoreFocus) {
    Impl& impl = *m_impl;
    if (!impl.visible && !impl.hwnd) return;

    impl.visible = false;

    GuardMission(impl, "Hide", [&] {
        impl.root.Opacity(0.0f);
    });

    ::ShowWindow(impl.hwnd, SW_HIDE);

    // Release the thumbnails immediately rather than at the next invocation.
    // Each one is a registration DWM holds on our behalf, and the budget for
    // this process while nothing is happening is zero.
    GuardMission(impl, "ReleaseTiles", [&] { impl.ReleaseTiles(); });

    impl.items.clear();
    impl.spaces.clear();
    impl.chips.clear();
    impl.hovered = -1;

    if (restoreFocus && impl.restoreWindow && ::IsWindow(impl.restoreWindow))
        ::SetForegroundWindow(impl.restoreWindow);

    impl.restoreWindow = nullptr;
}

} // namespace mactab
