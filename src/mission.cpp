#include "winrt_pch.h"

#include <d2d1_1.h>
#include <d2d1effects_2.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <shellscalingapi.h>

#include <map>
#include <thread>
#include <vector>

#include <DispatcherQueue.h>
#include <windows.ui.composition.interop.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Composition.Desktop.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.ViewManagement.h>
#include <winrt/Windows.UI.h>

#include "mission.h"
#include "com.h"
#include "desktops.h"
#include "hotkey.h"
#include "common.h"
#include "config.h"
#include "diag.h"
#include "geometry.h"
#include "glass.h"
#include "glass_draw.h"
#include "mission_layout.h"
#include "panel_layout.h"
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
constexpr float kBarHeight    = 168.0f;   // the full-width glass bar
constexpr float kChipHeight   = 90.0f;    // one desktop miniature inside it
constexpr float kChipGap      = 18.0f;
constexpr float kChipRadius   = 10.0f;
constexpr float kChipLabel    = 24.0f;    // the name under a miniature
constexpr float kBarInset     = 40.0f;    // how far the add button sits from the edge
constexpr float kOuterMargin  = 56.0f;
constexpr float kBadgeSize    = 46.0f;    // the app icon over a window
constexpr float kTitleHeight  = 22.0f;
constexpr float kTitleGap     = 1.0f;     // the name sits right under the icon

// How far the name's shadow is offset and how far it is spread.
//
// The name has no plate behind it any more, so this is the only thing keeping it
// legible, and it has to work over a photograph. Drawn as the same text several
// times in black at low alpha, offset around the glyphs, which is a cheap
// approximation of a blur and the one macOS uses on desktop icon labels.
constexpr float kTitleShadow  = 1.6f;
constexpr float kTileRadius   = 8.0f;
constexpr float kOutlineWidth = 3.0f;

// Drawn on every desktop rather than only on the one under the pointer. macOS
// reveals it on hover, but the bar here is baked as one surface and a hover
// state would mean re-baking every miniature, every icon and every name each
// time the pointer crossed one. At this size it is unobtrusive enough to leave
// out, and a control you can see is a control people find.
// The teardown timer, on the first overlay's own window.
constexpr UINT_PTR kCloseTimerId = 1;

// And the one that sharpens the previews once the flight has landed.
constexpr UINT_PTR kSharpenTimerId = 2;

// Posted by the sharpen worker when it has pixels to hand back. lParam owns a
// heap SharpBatch, and the window procedure takes that ownership before any of
// its own early returns.
constexpr UINT kMsgSharpReady = WM_APP + 1;

// Above this reduction, a live thumbnail is replaced by a still of the same
// window taken at a sensible resolution and filtered properly.
//
// The compositor has exactly one sampling knob, bilinear, and bilinear is only
// a correct reduction down to about 2:1, where each output pixel averages a
// 2x2 block. Past that it is skipping source pixels, and a 4K window in a 400
// pixel tile skips nine out of every ten. No amount of asking DWM for a
// different destination size changes that: a thumbnail is a live connection to
// the source's own surfaces, so the sampling happens once, at the end, with
// whatever the whole tree composes to.
//
// So the one good downscaler in this program, the box filter in image.cpp, is
// pointed at the problem instead.
constexpr float  kSharpRatio      = 1.8f;

// Rendered at twice the tile so the compositor's own step is exactly 2:1, which
// bilinear does correctly, and so a spread pile still has pixels to magnify.
constexpr float  kSharpOversample = 2.0f;

// How long the whole sharpening pass may take. It runs after the reveal, so it
// costs nothing anybody is waiting on, but a hung application can hold each
// window for 50 ms and thirty of those is not a pause anybody would forgive.
constexpr double kSharpBudgetMs   = 500.0;

// The little cross that closes a desktop, as a fraction of a miniature's height,
// and where its centre sits inside the miniature's top-left corner.
constexpr float kCloseSize   = 0.26f;
constexpr float kCloseInset  = 0.16f;

// How far outside the window the hover outline's texture reaches.
//
// Its own number, and small. Reusing the shadow's spread put the nine-grid's
// fixed corner region at 24 plus the corner radius, which on a window under
// about seventy pixels leaves no stretchable middle at all and the outline
// comes out crushed. This only has to cover the stroke.
constexpr float kOutlinePad   = 10.0f;

// The shadow under each window, as a nine-grid texture baked once.
//
// One texture stretched to every size rather than a Composition drop shadow per
// tile: a drop shadow needs a mask, and the thumbnail is a visual DWM owns which
// cannot be masked at all. The nine-grid also lives inside the tile, so it flies
// and scales with it, and a shadow travelling with its window is a large part of
// what makes the windows read as lifting off the desktop.
constexpr float kShadowSpread = 24.0f;
constexpr float kShadowSigma  = 9.0f;
constexpr float kShadowAlpha  = 0.45f;

// How much of the screen's resolution the backdrop is baked at, before the
// visual stretches it back.
//
// Derived from the blur rather than fixed, because the blur is what made the
// stretch invisible. macOS Mission Control does not blur the desktop at all: it
// dims it and lifts the windows off. With the blur gone, a 4x bilinear upscale
// of a photograph is plainly soft, and there is nothing left to hide it behind.
//
// The cost of that is real and worth stating: at native resolution the baked
// backdrop is a screen-sized surface per display, which on a 4K monitor is about
// 33 MB. MissionBlurSigma in settings.ini buys it back, at the price of a
// backdrop that no longer looks like macOS.
float BackdropScale() {
    const float sigma = config::Current().missionBlurSigma;
    if (sigma < 1.0f) return 1.0f;
    if (sigma < 4.0f) return 0.5f;
    return 0.25f;
}

// How long the snapshot tier may spend before the rest of the windows become
// cards instead. Only reached when the shared-visual path is unavailable.
constexpr double kSnapshotBudgetMs = 400.0;

struct Theme {
    D2D1_COLOR_F backdropTint;
    D2D1_COLOR_F chip;
    D2D1_COLOR_F chipBorder;
    D2D1_COLOR_F text;

    // The window name under an icon, which sits on the wallpaper rather than on
    // anything of ours. Near-white in both appearances, because what is behind it
    // is a photograph and not the theme: it is legible over a bright wallpaper
    // from its shadow rather than from its own colour, exactly as macOS does with
    // desktop icon labels.
    D2D1_COLOR_F tileName;
};

// The material the spaces bar is made of.
//
// Read from config rather than from the constants in glass.h, which is what
// makes "Reload glass from settings.ini" reach Mission Control at all: the same
// numbers the switcher's panel is tuned with, tuned once.
glass::Params BarMaterial(bool light) {
    return light ? config::Current().glassLight : config::Current().glassDark;
}

Theme MakeTheme(bool light) {
    Theme theme{};
    if (light) {
        theme.backdropTint = { 0.86f, 0.87f, 0.90f, 0.55f };
        theme.chip         = { 1.00f, 1.00f, 1.00f, 0.42f };
        theme.chipBorder   = { 0.10f, 0.10f, 0.12f, 0.45f };
        theme.text         = { 0.08f, 0.08f, 0.10f, 1.00f };
        theme.tileName     = { 1.00f, 1.00f, 1.00f, 1.00f };
    } else {
        theme.backdropTint = { 0.03f, 0.03f, 0.05f, 0.55f };
        theme.chip         = { 1.00f, 1.00f, 1.00f, 0.16f };
        theme.chipBorder   = { 1.00f, 1.00f, 1.00f, 0.62f };
        theme.text         = { 0.96f, 0.96f, 0.98f, 1.00f };
        theme.tileName     = { 1.00f, 1.00f, 1.00f, 1.00f };
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

// Composite a flat colour over an opaque bitmap, in place.
//
// The dim, applied on the CPU because the strip the spaces bar refracts has to
// arrive already dimmed: the material adapts to what is behind it, and what is
// behind it on screen is a dimmed desktop rather than the wallpaper file.
void Dim(Bitmap& image, const D2D1_COLOR_F& colour, float alpha) {
    if (image.Empty() || alpha <= 0.0f) return;
    alpha = (std::min)(1.0f, alpha);

    const float keep = 1.0f - alpha;
    const auto  mix  = [&](uint8_t channel, float over) {
        return static_cast<uint32_t>(channel * keep + over * 255.0f * alpha + 0.5f);
    };

    for (uint32_t& pixel : image.pixels) {
        pixel = MakePixel(static_cast<uint8_t>((std::min)(255u, mix(RedOf(pixel),   colour.r))),
                          static_cast<uint8_t>((std::min)(255u, mix(GreenOf(pixel), colour.g))),
                          static_cast<uint8_t>((std::min)(255u, mix(BlueOf(pixel),  colour.b))),
                          255);
    }
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
        if (ok)
            dc->SetTransform(D2D1::Matrix3x2F::Translation(
                static_cast<float>(offset.x), static_cast<float>(offset.y)));
    }
    ~SurfaceDraw() {
        if (ok && interop) interop->EndDraw();
    }
};

} // namespace

// ---------------------------------------------------------------------------

struct Mission::Impl {
    HINSTANCE instance        = nullptr;
    HWND      notifyWindow    = nullptr;
    HWND      restoreWindow   = nullptr;
    UINT      activateMessage = 0;
    UINT      dismissMessage  = 0;
    UINT      spaceMessage    = 0;

    bool ready   = false;
    bool visible = false;

    // On screen, but on its way out: the windows are flying back to where they
    // really are and nothing is listening any more.
    //
    // A separate flag rather than clearing `visible`, so the teardown can be
    // forced from anywhere if the timer that normally does it never fires. A
    // full-screen topmost window left up because a timer was missed is the worst
    // failure this file can have, and it is worth two lines to make it
    // impossible rather than unlikely.
    bool     closing    = false;
    UINT_PTR closeTimer = 0;
    bool     restoreOnHide = true;

    // Ignore focus loss until this tick.
    //
    // Adding or closing a desktop switches the view, and while the view is
    // somewhere else these overlays are cloaked, which Windows reports as losing
    // activation. The message loop is not running during the wait, so those
    // arrive in a batch AFTER the overlays have been brought back across, and
    // dismissing on them would close Mission Control the instant it was put
    // right. They are not a user leaving, so they are not listened to.
    DWORD ignoreFocusUntil = 0;

    void FinishHide();

    WUC::Compositor                compositor{ nullptr };
    WUC::CompositionGraphicsDevice graphics{ nullptr };

    ComPtr<ID2D1Device>      d2dDevice;
    ComPtr<ID2D1Factory1>    d2dFactory;
    ComPtr<IDWriteFactory>   dwriteFactory;
    winrt::com_ptr<IUnknown> dcompDevice;

    // Constructed once and kept. Activation is a WinRT call and the accent
    // colour can change while the process runs, so the object is cached and the
    // value read at initialisation, which is cheaper than subscribing to a
    // change event that fires on somebody else's thread.
    winrt::Windows::UI::ViewManagement::UISettings uiSettings{ nullptr };

    Theme        theme        = MakeTheme(false);
    bool         themeIsLight = false;
    D2D1_COLOR_F accent       = D2D1::ColorF(0.0f, 0.47f, 0.83f, 1.0f);

    // Baked once and reused for the life of the process. Both are stretched by
    // a nine-grid brush, so one small texture serves every window at every size.
    WUC::CompositionDrawingSurface shadowSurface{ nullptr };
    WUC::CompositionNineGridBrush  shadowBrush{ nullptr };
    float                          textureSpread = 0.0f;   // the shadow's

    struct Tile {
        WUC::ContainerVisual holder{ nullptr };
        WUC::SpriteVisual    shadow{ nullptr };
        WUC::SpriteVisual    content{ nullptr };   // snapshot or icon card
        WUC::Visual          live{ nullptr };      // the one DWM owns, if any
        WUC::SpriteVisual    chrome{ nullptr };    // badge and name, pile front only
        WUC::CompositionDrawingSurface contentSurface{ nullptr };
        WUC::CompositionDrawingSurface chromeSurface{ nullptr };
        HTHUMBNAIL thumbnail = nullptr;
        RECT       screenRect{};   // where it lands in the arrangement
        RECT       liveRect{};     // where it is NOW, which differs while expanded
        RECT       sourceRect{};   // where it flies IN from

        // Where the window really is, always.
        //
        // Not the same as sourceRect, which is only the window's own place on
        // the reveal: a desktop sliding past starts its tiles off the side of
        // the screen, and a rearrangement starts them wherever they happened to
        // be sitting. The collapse has to put every window back on itself
        // whichever of those last happened, so it reads this instead.
        RECT       homeRect{};
        float      baseW = 1.0f;   // the holder's own size, which Scale multiplies
        float      baseH = 1.0f;
        HWND       window = nullptr;   // the window it is showing
        int        item  = -1;     // index into Impl::items
        int        group = 0;
        int        depth = 0;      // 0 is the front of its pile
        bool       ownName = false;   // name this window, not its application
        float      pileX = 0.0f;   // the pile's box, for anchoring the chrome
        float      pileW = 0.0f;
        float      pileBottom = 0.0f;

        // How many window pixels each tile pixel has to stand for. Above about
        // two the compositor cannot reduce them honestly.
        float      reduction = 1.0f;
        bool       sharpened = false;
    };

    struct Screen {
        HMONITOR monitor = nullptr;
        RECT     rect{};
        float    dpiScale = 1.0f;
        HWND     hwnd = nullptr;

        WUC::Desktop::DesktopWindowTarget target{ nullptr };
        WUC::ContainerVisual root{ nullptr };
        WUC::SpriteVisual    backdrop{ nullptr };
        WUC::ContainerVisual tileLayer{ nullptr };
        WUC::ContainerVisual chromeLayer{ nullptr };
        WUC::SpriteVisual    outline{ nullptr };

        // The spaces bar, in two layers.
        //
        // The glass is baked once and kept: it depends on the wallpaper and the
        // monitor and on nothing else, and it is the expensive one, a full-width
        // displacement map and edge light on the CPU. Redrawing it every time an
        // arrow key walked to another desktop was several milliseconds per
        // display for a picture that had not changed.
        WUC::SpriteVisual    barGlass{ nullptr };
        WUC::SpriteVisual    bar{ nullptr };   // the miniatures on top of it

        WUC::CompositionDrawingSurface backdropSurface{ nullptr };
        WUC::CompositionDrawingSurface outlineSurface{ nullptr };
        WUC::CompositionDrawingSurface barGlassSurface{ nullptr };

        // What the outline surface was last drawn for, so moving between two
        // windows of the same size does not redraw it.
        float outlineW = 0.0f;
        float outlineH = 0.0f;

        // The wallpaper reduced to one miniature, at the size the chips are
        // drawn at. Depends on the picture and the monitor, like the backdrop,
        // and is dropped with it. See BakeBar for what it costs to work out.
        Bitmap chipPaper;
        int    chipPaperW = 0;
        int    chipPaperH = 0;
        WUC::CompositionDrawingSurface barSurface{ nullptr };

        // How far the glass runs past the screen on the left, right and top, so
        // its stroke and lit edge fall off the display instead of drawing a line
        // down both screen edges. Only the bottom edge of the bar is ever seen.
        float barOverhang = 0.0f;

        std::vector<Tile>               tiles;
        std::vector<mission::SpaceChip> chips;
        int hovered = -1;

        // The application whose pile is spread out, or -1. Spreading is how you
        // pick between several windows of one app without leaving.
        int expandedGroup = -1;

        float Scaled(float logical) const { return logical * dpiScale; }
        float Width()  const { return static_cast<float>(rect.right - rect.left); }
        float Height() const { return static_cast<float>(rect.bottom - rect.top); }
    };

    std::vector<Screen>       screens;
    std::vector<MissionItem>  items;
    std::vector<MissionSpace> spaces;

    // The desktop being looked at, which is not necessarily the one being used.
    int browsed = -1;

    // A window being dragged. Dragging is how a window is moved to another
    // display, and the one gesture where the tile leaves its arrangement.
    struct Drag {
        HWND  screen  = nullptr;   // the overlay the press happened on
        int   tile    = -1;
        POINT grab{};              // where the press was, in that overlay
        POINT origin{};            // the tile's offset when the press happened
        bool  moving  = false;     // past the threshold, so it is a drag
    };
    Drag drag;

    // The app icons, uploaded once each rather than once per window.
    std::map<std::wstring, ComPtr<ID2D1Bitmap1>> iconBitmaps;

    // --- Sharpening, which happens off this thread ---------------------------
    //
    // Everything that crosses is plain data. The worker never touches a Screen,
    // a Tile or anything belonging to the compositor: it is handed window
    // handles and sizes, and hands back pixels.

    struct SharpJob {
        size_t screen = 0;
        size_t tile   = 0;
        HWND   window = nullptr;
        int    bakeW  = 0;
        int    bakeH  = 0;
    };

    struct SharpResult {
        size_t screen = 0;
        size_t tile   = 0;
        HWND   window = nullptr;
        int    bakeW  = 0;
        int    bakeH  = 0;
        Bitmap picture;   // empty when the window would not be printed
    };

    struct SharpBatch {
        uint32_t                 epoch = 0;
        std::vector<SharpResult> results;
    };

    // Bumped whenever the tiles are torn down, so a batch that was taken
    // against an arrangement which no longer exists is recognised and dropped
    // rather than applied to whatever now sits at those indices.
    uint32_t tileEpoch = 0;

    // One pass at a time. Without this, browsing the desktops while a pass is
    // out would start a second one over the same windows.
    bool sharpInFlight = false;

    bool CreateDevices();
    bool BakeTextures();
    bool BuildScreens();
    Screen* ScreenFor(HWND hwnd);
    bool    IsOwnWindow(HWND hwnd) const;

    void BakeBackdrop(Screen& screen);
    void BakeBarGlass(Screen& screen);
    void RestBar(Screen& screen);
    void BakeBar(Screen& screen);
    float BarHeight(const Screen& screen) const;
    void BakeChrome(Screen& screen, Tile& tile);
    void SharpenTiles();
    void ApplySharpened(const SharpBatch& batch);
    void ScheduleSharpen();
    void BuildTiles(Screen& screen, const std::vector<int>& members, int slide);
    void BuildForDesktop(int desktop, int slide);
    void ExpandPile(Screen& screen, int group);
    void CollapsePile(Screen& screen);
    void ReleaseTiles(Screen& screen);
    void PositionOutline(Screen& screen);
    void SetHovered(Screen& screen, int index);
    int  HitTestTile(const Screen& screen, POINT client) const;
    int  PileSize(const Screen& screen, int group) const;
    int  HitTestChip(const Screen& screen, POINT client) const;
    int  HitTestClose(const Screen& screen, POINT client) const;
    static D2D1_POINT_2F CloseCentre(const mission::SpaceChip& chip);
    int  Neighbour(const Screen& screen, int from, int dx, int dy) const;

    ComPtr<ID2D1Bitmap1> IconFor(ID2D1DeviceContext* dc, const MissionItem& item);

    void BeginDrag(Screen& screen, POINT client);
    void UpdateDrag(Screen& screen, POINT client);
    bool FinishDrag(Screen& screen, POINT client);   // true if it was a drag
    void Rearrange(int desktop);
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

void FillSquircle(ID2D1DeviceContext* dc, ID2D1Factory* factory,
                  float x, float y, float w, float h, float radius,
                  const D2D1_COLOR_F& colour) {
    ComPtr<ID2D1PathGeometry> geometry =
        CreateSquircleGeometry(factory, w, h, radius, 5.0f);
    ComPtr<ID2D1SolidColorBrush> brush;
    if (!geometry || FAILED(dc->CreateSolidColorBrush(colour, brush.Put()))) return;

    D2D1_MATRIX_3X2_F saved{};
    dc->GetTransform(&saved);
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
    ComPtr<ID2D1SolidColorBrush> brush;
    if (!geometry || FAILED(dc->CreateSolidColorBrush(colour, brush.Put()))) return;

    D2D1_MATRIX_3X2_F saved{};
    dc->GetTransform(&saved);
    dc->SetTransform(D2D1::Matrix3x2F::Translation(x, y) * saved);
    dc->DrawGeometry(geometry.Get(), brush.Get(), thickness);
    dc->SetTransform(saved);
}

ComPtr<IDWriteTextFormat> MakeFormat(IDWriteFactory* dwrite, float size,
                                     DWRITE_FONT_WEIGHT weight) {
    ComPtr<IDWriteTextFormat> format;

    // Segoe UI Variable is the Windows 11 UI face and does not exist on 10, so
    // the older name is a fallback rather than an error.
    if (FAILED(dwrite->CreateTextFormat(L"Segoe UI Variable Display", nullptr, weight,
                                        DWRITE_FONT_STYLE_NORMAL,
                                        DWRITE_FONT_STRETCH_NORMAL, size, L"",
                                        format.Put())))
        dwrite->CreateTextFormat(L"Segoe UI", nullptr, weight,
                                 DWRITE_FONT_STYLE_NORMAL,
                                 DWRITE_FONT_STRETCH_NORMAL, size, L"",
                                 format.Put());

    if (format) {
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    return format;
}

} // namespace

// ---------------------------------------------------------------------------

Mission::Mission() : m_impl(std::make_unique<Impl>()) {}
Mission::~Mission() { Shutdown(); }

namespace {

LRESULT CALLBACK MissionWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* impl = reinterpret_cast<Mission::Impl*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    // Ahead of every early return below, because lParam carries ownership of a
    // heap batch and it has to be freed even when there is no longer anything
    // to apply it to.
    if (msg == kMsgSharpReady) {
        const std::unique_ptr<Mission::Impl::SharpBatch> batch(
            reinterpret_cast<Mission::Impl::SharpBatch*>(lParam));
        if (impl && batch) impl->ApplySharpened(*batch);
        return 0;
    }

    if (!impl) return ::DefWindowProcW(hwnd, msg, wParam, lParam);

    Mission::Impl::Screen* screen = impl->ScreenFor(hwnd);
    if (!screen) return ::DefWindowProcW(hwnd, msg, wParam, lParam);

    // While the windows are settling back onto the desktop the overlay is still
    // on screen and still on top, so it still receives clicks and keys. Nothing
    // it could do with them is what the user meant: they have already left, and
    // a click landing on a tile mid-flight would switch to whichever window
    // happened to be under the pointer.
    //
    // The timer is the exception, since that is what takes it off the screen.
    if (impl->closing && msg != WM_TIMER && msg != WM_DESTROY)
        return ::DefWindowProcW(hwnd, msg, wParam, lParam);

    switch (msg) {
    // wParam on dismissMessage says whether the user MEANT to leave. Escape and
    // a click on the background are decisions and take you to the desktop you
    // were looking at; losing focus is not, and must not move anybody.
    case WM_LBUTTONDOWN:
        impl->BeginDrag(*screen, POINT{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
        return 0;

    case WM_MOUSEMOVE: {
        const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (impl->drag.tile >= 0) {
            impl->UpdateDrag(*screen, point);
            return 0;
        }
        impl->SetHovered(*screen, impl->HitTestTile(*screen, point));
        return 0;
    }

    case WM_LBUTTONUP: {
        const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (!impl->notifyWindow) return 0;

        // A drag consumes the click. Releasing after moving a window across is
        // not a request to switch to it.
        if (impl->FinishDrag(*screen, point)) return 0;

        const int closing = impl->HitTestClose(*screen, point);
        if (closing >= 0) {
            ::PostMessageW(impl->notifyWindow, impl->spaceMessage,
                           Mission::kSpaceCloseBase + static_cast<WPARAM>(closing), 0);
            return 0;
        }

        const int chip = impl->HitTestChip(*screen, point);
        if (chip >= 0) {
            const mission::SpaceChip& c = screen->chips[static_cast<size_t>(chip)];
            ::PostMessageW(impl->notifyWindow, impl->spaceMessage,
                           c.add ? Mission::kSpaceAdd : static_cast<WPARAM>(c.index), 0);
            return 0;
        }

        const int tile = impl->HitTestTile(*screen, point);

        // A click on empty space closes a spread pile if there is one, and
        // dismisses otherwise, which is what macOS does and what makes the
        // gesture feel like a place rather than a dialog.
        if (tile < 0) {
            if (screen->expandedGroup >= 0) impl->CollapsePile(*screen);
            else ::PostMessageW(impl->notifyWindow, impl->dismissMessage, 1, 0);
            return 0;
        }

        // Clicking the app's icon spreads its pile instead of activating,
        // because the icon is the pile's handle. Anywhere else on the window
        // goes to that window.
        const Mission::Impl::Tile& hit = screen->tiles[static_cast<size_t>(tile)];
        if (screen->expandedGroup < 0 && hit.depth == 0 && impl->PileSize(*screen, hit.group) > 1) {
            const float badge = screen->Scaled(kBadgeSize);
            const float cx    = static_cast<float>(hit.liveRect.left + hit.liveRect.right) * 0.5f;
            const float cy    = static_cast<float>(hit.liveRect.bottom) - badge * 0.16f;
            const float dx    = point.x - cx;
            const float dy    = point.y - cy;
            if (dx * dx + dy * dy <= (badge * 0.5f) * (badge * 0.5f)) {
                impl->ExpandPile(*screen, hit.group);
                return 0;
            }
        }

        ::PostMessageW(impl->notifyWindow, impl->activateMessage,
                       static_cast<WPARAM>(hit.item), 0);
        return 0;
    }

    // The collapse has finished playing; take the windows off the screen.
    case WM_TIMER:
        if (wParam == kCloseTimerId) impl->FinishHide();
        if (wParam == kSharpenTimerId) {
            ::KillTimer(hwnd, kSharpenTimerId);
            impl->SharpenTiles();
        }
        return 0;

    // Capture can be taken away, by a system dialog or an alt-tab out. Leaving
    // the drag armed would make the next click somewhere else finish a move.
    case WM_CAPTURECHANGED:
        if (impl->drag.tile >= 0) {
            impl->drag = Mission::Impl::Drag{};
            impl->Rearrange(impl->browsed);
        }
        return 0;

    case WM_KEYDOWN: {
        if (!impl->notifyWindow) return 0;
        const int  hovered  = screen->hovered;
        const bool expanded = screen->expandedGroup >= 0;

        // Ctrl and the arrows walk the desktops, which is the binding macOS uses
        // and the one Ctrl+Win+Left and Ctrl+Win+Right arrive here as. Bare
        // arrows move between windows.
        //
        // Those two used to be the same key. An earlier round made bare arrows
        // walk the desktops, which left no way to move between windows at all
        // and meant the meaning of an arrow depended on how many desktops
        // happened to exist. They are separate now, and the only overlap left is
        // a screen with nothing on it, where moving between windows has nothing
        // to move between.
        const bool ctrl = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool walkSpaces =
            impl->spaces.size() > 1 && !expanded && (ctrl || screen->tiles.empty());

        const auto step = [&](int delta) {
            const int last = static_cast<int>(impl->spaces.size()) - 1;
            const int next = (std::max)(0, (std::min)(last, impl->browsed + delta));
            ::PostMessageW(impl->notifyWindow, impl->spaceMessage,
                           static_cast<WPARAM>(next), 0);
        };

        switch (wParam) {
        case VK_ESCAPE:
            // Out of the spread first, out of Mission Control second. Escape
            // that skipped a level would throw away the thing the user was
            // half way through doing.
            if (expanded) impl->CollapsePile(*screen);
            else          ::PostMessageW(impl->notifyWindow, impl->dismissMessage, 1, 0);
            return 0;

        case VK_RETURN:
        case VK_SPACE:
            if (hovered >= 0)
                ::PostMessageW(impl->notifyWindow, impl->activateMessage,
                               static_cast<WPARAM>(screen->tiles[static_cast<size_t>(hovered)].item), 0);
            return 0;

        case VK_LEFT:
            if (walkSpaces) step(-1);
            else impl->SetHovered(*screen, impl->Neighbour(*screen, hovered, -1, 0));
            return 0;

        case VK_RIGHT:
            if (walkSpaces) step(1);
            else impl->SetHovered(*screen, impl->Neighbour(*screen, hovered, 1, 0));
            return 0;

        case VK_UP:
            if (expanded) impl->CollapsePile(*screen);
            else          impl->SetHovered(*screen, impl->Neighbour(*screen, hovered, 0, -1));
            return 0;

        case VK_DOWN:
            // Down on a pile spreads it, which is how you pick between several
            // windows of one application.
            if (!expanded && hovered >= 0)
                impl->ExpandPile(*screen, screen->tiles[static_cast<size_t>(hovered)].group);
            else if (!expanded)
                impl->SetHovered(*screen, impl->Neighbour(*screen, hovered, 0, 1));
            return 0;

        case VK_TAB:
            if (!screen->tiles.empty()) {
                const int count = static_cast<int>(screen->tiles.size());
                const int delta = ((::GetKeyState(VK_SHIFT) & 0x8000) != 0) ? -1 : 1;
                impl->SetHovered(*screen, ((hovered + delta) % count + count) % count);
            }
            return 0;

        default:
            return 0;
        }
    }

    // Scrolling up on a pile spreads it, and down puts it back. The gesture the
    // reference uses is a two-finger swipe, which arrives here as a wheel.
    case WM_MOUSEWHEEL: {
        POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ::ScreenToClient(hwnd, &point);

        const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        if (delta > 0) {
            const int tile = impl->HitTestTile(*screen, point);
            if (tile >= 0 && screen->expandedGroup < 0)
                impl->ExpandPile(*screen, screen->tiles[static_cast<size_t>(tile)].group);
        } else if (delta < 0) {
            impl->CollapsePile(*screen);
        }
        return 0;
    }

    // Losing focus to something that is not one of our own overlays.
    //
    // The sibling check is what makes more than one display work at all. With
    // an overlay per screen, clicking the second one IS a focus change, and
    // dismissing on any focus change would close everything the moment the
    // pointer moved to another monitor. WM_KILLFOCUS carries the window gaining
    // focus, and for a same-thread transfer that handle is reliable.
    case WM_KILLFOCUS:
        if (impl->visible && impl->notifyWindow &&
            ::GetTickCount() >= impl->ignoreFocusUntil &&
            !impl->IsOwnWindow(reinterpret_cast<HWND>(wParam)))
            ::PostMessageW(impl->notifyWindow, impl->dismissMessage, 0, 0);
        return 0;

    // Belt and braces for the same thing from the other direction: this fires
    // only when activation crosses out of the process, which is exactly the
    // condition, and it catches the paths where the focus handle comes back
    // null. Dismissal is idempotent, so both firing is harmless.
    case WM_ACTIVATEAPP:
        if (wParam == FALSE && impl->visible && impl->notifyWindow &&
            ::GetTickCount() >= impl->ignoreFocusUntil)
            ::PostMessageW(impl->notifyWindow, impl->dismissMessage, 0, 0);
        return 0;

    default:
        break;
    }

    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

Mission::Impl::Screen* Mission::Impl::ScreenFor(HWND hwnd) {
    for (Screen& screen : screens)
        if (screen.hwnd == hwnd) return &screen;
    return nullptr;
}

bool Mission::Impl::IsOwnWindow(HWND hwnd) const {
    if (!hwnd) return false;
    for (const Screen& screen : screens)
        if (screen.hwnd == hwnd) return true;
    return false;
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
    // wants in order to hand back a thumbnail visual belonging to our tree.
    // Which of these a given build admits to is not documented and any of them
    // will do, so this tries newest first and takes what it gets.
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

// The shadow and the selection outline, baked once at the largest scale in use.
//
// Nine-grid textures: the middle stretches and the corners do not, so one small
// bitmap serves a 400 pixel window and a 1400 pixel one at the same cost.
// Drawing these per tile, per invocation, is the shape of the code this replaced
// and a large part of why the gesture felt heavy.
bool Mission::Impl::BakeTextures() {
    float dpi = 1.0f;
    for (const Screen& screen : screens) dpi = (std::max)(dpi, screen.dpiScale);

    const float spread = kShadowSpread * dpi;
    const float radius = kTileRadius   * dpi;
    const float side   = spread * 2 + radius * 2 + 4.0f;

    textureSpread = spread;

    shadowSurface = graphics.CreateDrawingSurface(
        { side, side },
        winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
        winrt::Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);

    {
        SurfaceDraw draw(shadowSurface);
        if (!draw.ok) return false;

        ID2D1DeviceContext* dc = draw.dc.Get();
        dc->Clear(D2D1::ColorF(0, 0, 0, 0));

        // The shape goes into a bitmap first, because a blur is an effect and
        // effects take an image rather than a geometry.
        ComPtr<ID2D1BitmapRenderTarget> scratch;
        if (FAILED(dc->CreateCompatibleRenderTarget(D2D1::SizeF(side, side), scratch.Put())))
            return false;

        scratch->BeginDraw();
        scratch->Clear(D2D1::ColorF(0, 0, 0, 0));
        {
            ComPtr<ID2D1SolidColorBrush> black;
            if (SUCCEEDED(scratch->CreateSolidColorBrush(
                    D2D1::ColorF(0.0f, 0.0f, 0.0f, kShadowAlpha), black.Put()))) {
                const D2D1_ROUNDED_RECT shape{
                    D2D1::RectF(spread, spread, side - spread, side - spread),
                    radius, radius };
                scratch->FillRoundedRectangle(shape, black.Get());
            }
        }
        scratch->EndDraw();

        ComPtr<ID2D1Bitmap> shape;
        if (FAILED(scratch->GetBitmap(shape.Put()))) return false;

        ComPtr<ID2D1Effect> blur;
        if (SUCCEEDED(dc->CreateEffect(CLSID_D2D1GaussianBlur, blur.Put()))) {
            blur->SetInput(0, shape.Get());
            blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, kShadowSigma * dpi);
            dc->DrawImage(blur.Get(), D2D1_INTERPOLATION_MODE_LINEAR);
        } else {
            dc->DrawBitmap(shape.Get());
        }
    }

    shadowBrush = compositor.CreateNineGridBrush();
    shadowBrush.Source(compositor.CreateSurfaceBrush(shadowSurface));
    shadowBrush.SetInsets(spread + radius);

    return true;
}

bool Mission::Impl::BuildScreens() {
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = MissionWndProc;
    wc.hInstance     = instance;
    wc.lpszClassName = kMissionClass;
    wc.hCursor       = ::LoadCursorW(nullptr, IDC_ARROW);
    ::RegisterClassExW(&wc);

    std::vector<Screen> built;

    ::EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR monitor, HDC, LPRECT, LPARAM param) -> BOOL {
            MONITORINFO info{};
            info.cbSize = sizeof(info);
            if (!::GetMonitorInfoW(monitor, &info)) return TRUE;

            Screen screen;
            screen.monitor = monitor;
            screen.rect    = info.rcMonitor;

            // Per display, not per process. Everything on this overlay scales
            // by this, and one shared value is the bug that makes a second
            // monitor at a different scale look wrong.
            UINT dpiX = 96, dpiY = 96;
            if (SUCCEEDED(::GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)))
                screen.dpiScale = static_cast<float>(dpiX) / 96.0f;

            reinterpret_cast<std::vector<Screen>*>(param)->push_back(std::move(screen));
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&built));

    if (built.empty()) {
        MACTAB_FAIL("mission: no displays enumerated");
        return false;
    }

    for (Screen& screen : built) {
        // Activatable, unlike the switcher's panel. The panel must never take
        // foreground because it exists to hand foreground to something else.
        // This is a place the user is in: it owns the keyboard while it is up,
        // and the process losing activation is the signal to close.
        screen.hwnd = ::CreateWindowExW(
            WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
            kMissionClass, L"MacTab Mission Control", WS_POPUP,
            screen.rect.left, screen.rect.top,
            screen.rect.right - screen.rect.left,
            screen.rect.bottom - screen.rect.top,
            nullptr, nullptr, instance, nullptr);

        if (!screen.hwnd) {
            MACTAB_FAIL("mission: CreateWindowEx failed (err %lu)", ::GetLastError());
            return false;
        }
        ::SetWindowLongPtrW(screen.hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

        auto interop =
            compositor.as<ABI::Windows::UI::Composition::Desktop::ICompositorDesktopInterop>();
        if (FAILED(interop->CreateDesktopWindowTarget(
                screen.hwnd, false,
                reinterpret_cast<ABI::Windows::UI::Composition::Desktop::IDesktopWindowTarget**>(
                    winrt::put_abi(screen.target))))) {
            MACTAB_FAIL("mission: CreateDesktopWindowTarget failed");
            return false;
        }

        screen.root = compositor.CreateContainerVisual();
        screen.root.RelativeSizeAdjustment({ 1.0f, 1.0f });
        screen.target.Root(screen.root);

        screen.backdrop    = compositor.CreateSpriteVisual();
        screen.outline     = compositor.CreateSpriteVisual();
        screen.tileLayer   = compositor.CreateContainerVisual();
        screen.chromeLayer = compositor.CreateContainerVisual();
        screen.barGlass    = compositor.CreateSpriteVisual();
        screen.bar         = compositor.CreateSpriteVisual();

        // The outline goes ABOVE the windows, not below them. Under them it was
        // covered by the hovered window's own shadow, which reaches further out
        // than the outline does, so most of it was never visible.
        auto children = screen.root.Children();
        children.InsertAtTop(screen.backdrop);
        children.InsertAtTop(screen.tileLayer);
        children.InsertAtTop(screen.outline);
        children.InsertAtTop(screen.chromeLayer);
        children.InsertAtTop(screen.barGlass);
        children.InsertAtTop(screen.bar);

        screen.outline.Opacity(0.0f);
        screen.root.Opacity(0.0f);
    }

    screens = std::move(built);
    MACTAB_DIAG("mission: %zu display(s)", screens.size());
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
        // device sitting behind a compositor and returns a visual belonging to
        // that compositor's tree. Device and tree have to be the same one, and
        // the panel exposes neither.
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

    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    ComPtr<ID3D11Device> d3dDevice;
    HRESULT hr = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                     nullptr, 0, D3D11_SDK_VERSION,
                                     d3dDevice.Put(), nullptr, nullptr);
    if (FAILED(hr)) {
        MACTAB_WARN("mission: hardware D3D device failed (0x%08lX), trying WARP",
                    static_cast<unsigned long>(hr));
        hr = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
                                 nullptr, 0, D3D11_SDK_VERSION,
                                 d3dDevice.Put(), nullptr, nullptr);
    }
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

    impl.themeIsLight = ResolveLightTheme();
    impl.theme        = MakeTheme(impl.themeIsLight);

    // The accent colour, from the API the shell itself uses.
    //
    // Deliberately NOT DwmGetColorizationColor, which answers with the blended
    // glass colour rather than the accent, and deliberately not the registry,
    // whose format nobody promises. This one also ignores the "show accent
    // colour on title bars" setting, which is correct: that setting controls
    // where DWM paints the accent, not what the accent is, and Windows uses the
    // accent for selection either way.
    try {
        impl.uiSettings = winrt::Windows::UI::ViewManagement::UISettings();
        const WUI::Color colour = impl.uiSettings.GetColorValue(
            winrt::Windows::UI::ViewManagement::UIColorType::Accent);
        if (colour.A != 0)
            impl.accent = D2D1::ColorF(colour.R / 255.0f, colour.G / 255.0f,
                                       colour.B / 255.0f, 1.0f);
    } catch (const winrt::hresult_error&) {
        MACTAB_WARN("mission: UISettings unavailable; using the default accent");
    }

    if (!impl.CreateDevices()) return false;
    if (!impl.BuildScreens())  return false;

    // Probed here rather than on the reveal path, using the host window as the
    // source: whether DWM will compose a window's thumbnail into that same
    // window is undocumented, and a refusal for that reason would say nothing
    // about whether the export works.
    thumbnail::Probe(impl.screens.front().hwnd, notifyWindow);

    impl.ready = true;

    if (!impl.BakeTextures())
        MACTAB_WARN("mission: shadow and outline textures unavailable");

    MACTAB_DIAG("mission: initialised, thumbnails via %s, accent %.2f %.2f %.2f",
                thumbnail::TierName(thumbnail::Current()),
                impl.accent.r, impl.accent.g, impl.accent.b);
    return true;
}

void Mission::Prewarm() {
    Impl& impl = *m_impl;
    if (!impl.ready) return;

    // The wallpapers, on a thread of their own, because they are the one part
    // that touches the disk. Nothing here goes near the compositor or D2D, both
    // of which have thread affinity.
    const float scale = BackdropScale();

    struct Job { HMONITOR monitor; int width, height; int screenW, screenH, band; };
    std::vector<Job> jobs;
    for (const Impl::Screen& screen : impl.screens) {
        Job job;
        job.monitor = screen.monitor;
        job.width   = (std::max)(1, static_cast<int>(screen.Width()  * scale));
        job.height  = (std::max)(1, static_cast<int>(screen.Height() * scale));
        job.screenW = (std::max)(1, static_cast<int>(screen.Width()));
        job.screenH = (std::max)(1, static_cast<int>(screen.Height()));
        // Exactly the band BakeBarGlass will ask for. A different number here
        // is not a smaller cache hit, it is a second decode of the same 4K
        // picture, which is precisely what this thread exists to avoid.
        job.band    = (std::min)(job.screenH,
                                 static_cast<int>(screen.Scaled(kBarHeight) +
                                                  glass::MarginPx(screen.dpiScale)));
        jobs.push_back(job);
    }

    std::thread([jobs] {
        for (const Job& job : jobs) {
            wallpaper::ForMonitor(job.monitor, job.width, job.height);

            // And the sharp strip the spaces bar is cut from, which is a
            // different decode from the backdrop's whenever the backdrop is
            // downscaled.
            wallpaper::Region(job.monitor, job.screenW, job.screenH,
                              RECT{ 0, 0, job.screenW, job.band });
        }
    }).detach();
}

void Mission::InvalidateBackdrop() {
    Impl& impl = *m_impl;
    wallpaper::Invalidate();
    for (Impl::Screen& screen : impl.screens) {
        screen.backdropSurface = nullptr;
        screen.barGlassSurface = nullptr;
        screen.chipPaper       = {};
        screen.chipPaperW      = 0;
        screen.chipPaperH      = 0;
    }
    MACTAB_DIAG("mission: backdrops invalidated");
}

void Mission::DisplaysChanged() {
    Impl& impl = *m_impl;
    if (!impl.ready) return;

    // Not the collapse: the windows these tiles came from are on displays that
    // may not exist any more, so there is nowhere to put them back.
    HideNow();
    wallpaper::Invalidate();

    for (Impl::Screen& screen : impl.screens) {
        impl.ReleaseTiles(screen);
        screen.root            = nullptr;
        screen.backdrop        = nullptr;
        screen.tileLayer       = nullptr;
        screen.chromeLayer     = nullptr;
        screen.outline         = nullptr;
        screen.barGlass        = nullptr;
        screen.bar             = nullptr;
        screen.backdropSurface = nullptr;
        screen.outlineSurface  = nullptr;
        screen.barGlassSurface = nullptr;
        screen.barSurface      = nullptr;
        screen.target          = nullptr;
        if (screen.hwnd) ::DestroyWindow(screen.hwnd);
    }
    impl.screens.clear();

    if (!GuardMission(impl, "DisplaysChanged", [&] {
            if (!impl.BuildScreens()) {
                MACTAB_FAIL("mission: could not rebuild the overlays");
                impl.ready = false;
                return;
            }
            impl.BakeTextures();
        })) {
        impl.ready = false;
        return;
    }

    Prewarm();
    MACTAB_DIAG("mission: rebuilt for %zu display(s)", impl.screens.size());
}

void Mission::Shutdown() {
    Impl& impl = *m_impl;
    if (!impl.ready && impl.screens.empty()) return;

    impl.FinishHide();
    impl.ready = false;

    for (Impl::Screen& screen : impl.screens) {
        impl.ReleaseTiles(screen);
        screen.root            = nullptr;
        screen.backdrop        = nullptr;
        screen.tileLayer       = nullptr;
        screen.chromeLayer     = nullptr;
        screen.outline         = nullptr;
        screen.barGlass        = nullptr;
        screen.bar             = nullptr;
        screen.backdropSurface = nullptr;
        screen.outlineSurface  = nullptr;
        screen.barGlassSurface = nullptr;
        screen.barSurface      = nullptr;
        screen.target          = nullptr;
        if (screen.hwnd) ::DestroyWindow(screen.hwnd);
    }
    impl.screens.clear();

    impl.iconBitmaps.clear();
    impl.shadowBrush    = nullptr;
    impl.shadowSurface  = nullptr;
    impl.graphics       = nullptr;
    impl.compositor     = nullptr;
    impl.dcompDevice    = nullptr;
    impl.uiSettings     = nullptr;

    MACTAB_DIAG("mission: shut down");
}

bool Mission::Ready() const   { return m_impl->ready; }
bool Mission::Visible() const { return m_impl->visible && !m_impl->closing; }

HWND Mission::ItemWindow(int index) const {
    if (index < 0 || index >= static_cast<int>(m_impl->items.size())) return nullptr;
    return m_impl->items[static_cast<size_t>(index)].hwnd;
}

void Mission::Impl::ReleaseTiles(Screen& screen) {
    // Any sharpening pass that is out was taken against the arrangement being
    // torn down here, and its indices mean nothing once this returns.
    ++tileEpoch;

    if (screen.tileLayer)   screen.tileLayer.Children().RemoveAll();
    if (screen.chromeLayer) screen.chromeLayer.Children().RemoveAll();

    for (Tile& tile : screen.tiles)
        thumbnail::ReleaseSharedVisual(tile.thumbnail);

    screen.tiles.clear();
    screen.hovered = -1;
}

// ---------------------------------------------------------------------------

void Mission::Impl::BakeBackdrop(Screen& screen) {
    // Kept for the life of the process. It depends on the wallpaper and the
    // monitor, neither of which changes while the machine is idle, and baking
    // it per invocation was the single largest thing on the reveal path.
    if (screen.backdropSurface) {
        screen.backdrop.Size({ screen.Width(), screen.Height() });
        return;
    }

    const float scale  = BackdropScale();
    const int   smallW = (std::max)(1, static_cast<int>(screen.Width()  * scale));
    const int   smallH = (std::max)(1, static_cast<int>(screen.Height() * scale));

    screen.backdropSurface = graphics.CreateDrawingSurface(
        { static_cast<float>(smallW), static_cast<float>(smallH) },
        winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
        winrt::Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);

    Bitmap paper = wallpaper::ForMonitor(screen.monitor, smallW, smallH);

    SurfaceDraw draw(screen.backdropSurface);
    if (!draw.ok) return;

    ID2D1DeviceContext* dc = draw.dc.Get();
    dc->Clear(D2D1::ColorF(0, 0, 0, 0));

    if (paper.Empty()) {
        const uint32_t solid = wallpaper::SolidColour();
        dc->Clear(D2D1::ColorF(RedOf(solid)   / 255.0f,
                               GreenOf(solid) / 255.0f,
                               BlueOf(solid)  / 255.0f, 1.0f));
    } else if (ComPtr<ID2D1Bitmap1> source = UploadBitmap(dc, std::move(paper))) {
        ComPtr<ID2D1Effect> blur;
        if (SUCCEEDED(dc->CreateEffect(CLSID_D2D1GaussianBlur, blur.Put()))) {
            blur->SetInput(0, source.Get());
            blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION,
                           screen.Scaled(config::Current().missionBlurSigma) * scale);
            blur->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD);
            dc->DrawImage(blur.Get(), D2D1_INTERPOLATION_MODE_LINEAR);
        } else {
            dc->DrawBitmap(source.Get());
        }
    }

    // The dim. Mission Control pushes the desktop back so the windows read as
    // floating above it; without it a busy wallpaper competes with the
    // arrangement and wins.
    D2D1_COLOR_F dim = theme.backdropTint;
    dim.a = config::Current().missionDim;

    ComPtr<ID2D1SolidColorBrush> brush;
    if (SUCCEEDED(dc->CreateSolidColorBrush(dim, brush.Put())))
        dc->FillRectangle(D2D1::RectF(0, 0, static_cast<float>(smallW),
                                      static_cast<float>(smallH)), brush.Get());

    auto surfaceBrush = compositor.CreateSurfaceBrush(screen.backdropSurface);
    surfaceBrush.Stretch(WUC::CompositionStretch::Fill);
    screen.backdrop.Brush(surfaceBrush);
    screen.backdrop.Size({ screen.Width(), screen.Height() });
    screen.backdrop.Offset({ 0.0f, 0.0f, 0.0f });
}

float Mission::Impl::BarHeight(const Screen& screen) const {
    return spaces.empty() ? 0.0f : screen.Scaled(kBarHeight);
}

// The spaces bar, as a piece of the switcher's glass.
//
// Same material, same numbers, same code: glass_draw.cpp. That is the whole
// point of it. Up to 0.7.2 this was a translucent fill over an already-blurred
// wallpaper, which is a fair description of frosted plastic and no description
// at all of Liquid Glass. It could not refract, it did not react to what was
// behind it, and it shared nothing with the panel it was supposed to match.
//
// Three things are particular to this piece rather than to the material.
//
// It runs PAST the screen on the left, right and top. The glass has a dark outer
// stroke and a lit edge all the way round, which is right for a floating panel
// and wrong for a bar clipped to the top of a display: it would draw a hairline
// down both screen edges. Pushed out by the overhang, only the bottom edge is
// ever on screen, which is exactly the profile macOS shows.
//
// Its backdrop is the wallpaper, at full resolution, not the overlay's own baked
// one. The rim tap exists to bend a SHARP image; handing it the quarter-scale
// backdrop would make the second tap indistinguishable from the first, which is
// a bug this material has already had once.
//
// And the strip is dimmed before it is handed over, by the same amount the
// backdrop is. The bar sits on a dimmed desktop, so that is the scene it has to
// refract and adapt to. Feeding it the raw picture would have the material pick
// an operating point for somewhere brighter than the screen, and a rim reflecting
// light that is not there is a painted-on border again.
void Mission::Impl::BakeBarGlass(Screen& screen) {
    const float height = BarHeight(screen);
    if (height <= 0.0f) {
        screen.barGlass.Size({ 0.0f, 0.0f });
        return;
    }

    screen.barOverhang = std::ceil(glass::MarginPx(screen.dpiScale));

    const float over  = screen.barOverhang;
    const float width = screen.Width() + over * 2.0f;
    const float tall  = height + over;

    if (screen.barGlassSurface) {
        screen.barGlass.Size({ width, tall });
        screen.barGlass.Offset({ -over, -over, 0.0f });
        return;
    }

    screen.barGlassSurface = graphics.CreateDrawingSurface(
        { width, tall },
        winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
        winrt::Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);

    SurfaceDraw draw(screen.barGlassSurface);
    if (!draw.ok) return;

    ID2D1DeviceContext* dc = draw.dc.Get();
    dc->Clear(D2D1::ColorF(0, 0, 0, 0));

    // The sharp strip behind it, plus a blur margin below. The left, right and
    // top margins are off the screen, so wallpaper::Region clips them away and
    // the material's own edge clamp covers what is missing, exactly as it does
    // for a panel that reaches a screen edge.
    const int screenW = (std::max)(1, static_cast<int>(screen.Width()));
    const int screenH = (std::max)(1, static_cast<int>(screen.Height()));
    const int band    = (std::min)(screenH,
                                   static_cast<int>(height + glass::MarginPx(screen.dpiScale)));

    capture::Frame backdrop;
    backdrop.pixels = wallpaper::Region(screen.monitor, screenW, screenH,
                                        RECT{ 0, 0, screenW, band });

    if (backdrop.pixels.Empty()) {
        // No picture, so the desktop colour is what is behind the bar. Filled in
        // rather than left empty: an empty frame sends the material down its
        // degraded path, which lays a nearly opaque base coat and would make the
        // bar the one part of the overlay you cannot see through.
        backdrop.pixels = Bitmap::Create(screenW, band, wallpaper::SolidColour());
    }

    Dim(backdrop.pixels, theme.backdropTint, config::Current().missionDim);

    backdrop.bounds = RECT{ screen.rect.left, screen.rect.top,
                            screen.rect.left + screenW, screen.rect.top + band };

    glass::Piece piece;
    piece.dc             = dc;
    piece.factory        = d2dFactory.Get();
    // Glass off means the bar is the same plain plate the switcher becomes: it
    // is one material and one switch, not two that can disagree.
    piece.frame          = config::Current().glassEnabled ? &backdrop : nullptr;
    piece.base           = BarMaterial(themeIsLight);
    piece.dpiScale       = screen.dpiScale;
    piece.cornerExponent = layout::kPanelCornerExponent;

    const RECT area{ screen.rect.left  - static_cast<LONG>(over),
                     screen.rect.top   - static_cast<LONG>(over),
                     screen.rect.right + static_cast<LONG>(over),
                     screen.rect.top   + static_cast<LONG>(height) };

    glass::Draw(piece, draw.offset, area, 0.0f);

    screen.barGlass.Brush(compositor.CreateSurfaceBrush(screen.barGlassSurface));
    screen.barGlass.Size({ width, tall });
    screen.barGlass.Offset({ -over, -over, 0.0f });
}

// Put the strip back where it rests.
//
// The collapse leaves it above the top of the screen, and a Composition offset
// animation does not reset itself. Without this the second invocation would open
// with no strip at all.
void Mission::Impl::RestBar(Screen& screen) {
    if (screen.barGlass) {
        screen.barGlass.StopAnimation(L"Offset");
        screen.barGlass.Offset({ -screen.barOverhang, -screen.barOverhang, 0.0f });
    }
    if (screen.bar) {
        screen.bar.StopAnimation(L"Offset");
        screen.bar.Offset({ 0.0f, 0.0f, 0.0f });
    }
}

void Mission::Impl::BakeBar(Screen& screen) {
    screen.chips.clear();

    BakeBarGlass(screen);

    if (spaces.empty()) {
        screen.bar.Size({ 0.0f, 0.0f });
        return;
    }

    const float width  = screen.Width();
    const float height = screen.Scaled(kBarHeight);

    // The miniatures are centred in the band above the labels, so the names
    // have somewhere to sit.
    screen.chips = mission::LayoutSpaces(
        static_cast<int>(spaces.size()), width, height - screen.Scaled(kChipLabel),
        screen.Scaled(kChipHeight),
        (screen.Height() > 0.0f) ? screen.Width() / screen.Height() : 1.6f,
        screen.Scaled(kChipGap));

    // The add button is round, smaller than a desktop, and all the way to the
    // right rather than trailing the centred run.
    if (!screen.chips.empty()) {
        mission::SpaceChip& add = screen.chips.back();
        const float side = screen.Scaled(kChipHeight) * 0.42f;
        add.w = side;
        add.h = side;
        add.x = width - screen.Scaled(kBarInset) - side;
        add.y = (height - screen.Scaled(kChipLabel) - side) * 0.5f;
    }

    screen.barSurface = graphics.CreateDrawingSurface(
        { width, height },
        winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
        winrt::Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);

    SurfaceDraw draw(screen.barSurface);
    if (!draw.ok) return;

    ID2D1DeviceContext* dc = draw.dc.Get();
    dc->Clear(D2D1::ColorF(0, 0, 0, 0));

    // The miniatures come from the wallpaper already decoded for the backdrop,
    // resized here rather than read again. Asking the cache for a second size
    // means a second decode of what can be a 4K photograph, and it happened on
    // every single invocation.
    //
    // The reduction itself is then kept, because it was not cheap either. With
    // the blur off, which is how this ships, the backdrop is decoded at the
    // screen's own size, so this was box-filtering eight megapixels down to
    // about a hundred and sixty by ninety, and doing it again on every reveal,
    // every arrow press and every rearrangement. The result depends on the
    // picture and the monitor and nothing else, exactly like the backdrop it
    // comes from, and it is dropped alongside it.
    ComPtr<ID2D1BitmapBrush1> paperBrush;
    if (!screen.chips.empty()) {
        const float scale   = BackdropScale();
        const int backdropW = (std::max)(1, static_cast<int>(screen.Width()  * scale));
        const int backdropH = (std::max)(1, static_cast<int>(screen.Height() * scale));
        const int chipW = (std::max)(1, static_cast<int>(screen.chips[0].w));
        const int chipH = (std::max)(1, static_cast<int>(screen.chips[0].h));

        if (screen.chipPaper.Empty() ||
            screen.chipPaperW != chipW || screen.chipPaperH != chipH) {
            Bitmap paper = wallpaper::ForMonitor(screen.monitor, backdropW, backdropH);
            if (!paper.Empty()) {
                screen.chipPaper  = Resize(paper, chipW, chipH);
                screen.chipPaperW = chipW;
                screen.chipPaperH = chipH;
            }
        }

        if (!screen.chipPaper.Empty()) {
            Bitmap chip = screen.chipPaper;   // the upload takes ownership
            if (ComPtr<ID2D1Bitmap1> bitmap = UploadBitmap(dc, std::move(chip))) {
                D2D1_BITMAP_BRUSH_PROPERTIES1 props{};
                props.extendModeX       = D2D1_EXTEND_MODE_CLAMP;
                props.extendModeY       = D2D1_EXTEND_MODE_CLAMP;
                props.interpolationMode = D2D1_INTERPOLATION_MODE_LINEAR;
                dc->CreateBitmapBrush(bitmap.Get(), &props, nullptr, paperBrush.Put());
            }
        }
    }

    ComPtr<IDWriteTextFormat> format =
        MakeFormat(dwriteFactory.Get(), screen.Scaled(12.0f), DWRITE_FONT_WEIGHT_SEMI_BOLD);
    ComPtr<ID2D1SolidColorBrush> textBrush;
    dc->CreateSolidColorBrush(theme.text, textBrush.Put());

    const float radius = screen.Scaled(kChipRadius);

    for (const mission::SpaceChip& chip : screen.chips) {
        if (chip.add) {
            const float cx = chip.x + chip.w * 0.5f;
            const float cy = chip.y + chip.h * 0.5f;
            const float r  = chip.w * 0.5f;

            ComPtr<ID2D1SolidColorBrush> disc;
            if (SUCCEEDED(dc->CreateSolidColorBrush(theme.chip, disc.Put())))
                dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), disc.Get());

            if (textBrush) {
                const float arm    = r * 0.42f;
                const float stroke = screen.Scaled(2.0f);
                dc->DrawLine(D2D1::Point2F(cx - arm, cy), D2D1::Point2F(cx + arm, cy),
                             textBrush.Get(), stroke);
                dc->DrawLine(D2D1::Point2F(cx, cy - arm), D2D1::Point2F(cx, cy + arm),
                             textBrush.Get(), stroke);
                dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r),
                                textBrush.Get(), screen.Scaled(1.0f));
            }
            continue;
        }

        // Highlighted by what is being LOOKED AT, not by what the machine is
        // running. Every display shows the same strip, and marking the running
        // desktop meant that walking to another one from one screen left every
        // other screen pointing at a desktop whose windows were no longer being
        // shown anywhere.
        const bool current = (chip.index == browsed);

        // Every desktop shows the wallpaper, which is what an empty one looks
        // like, and then its own windows on top of it.
        if (paperBrush) {
            paperBrush->SetTransform(D2D1::Matrix3x2F::Translation(chip.x, chip.y));
            FillSquircleWith(dc, d2dFactory.Get(), chip.x, chip.y, chip.w, chip.h,
                             radius, paperBrush.Get());
        } else {
            FillSquircle(dc, d2dFactory.Get(), chip.x, chip.y, chip.w, chip.h, radius,
                         theme.chip);
        }

        // The windows on that desktop, at the position and size they really
        // have, scaled into the miniature.
        //
        // Not a picture of them: a window on another desktop is shell-cloaked
        // and DWM will not compose a cloaked window through any path a normal
        // process has, so there are no pixels to be had. Their shapes and their
        // icons are what there is, and a miniature that shows where things are
        // is worth a great deal more than an empty rectangle.
        {
            const float sx = chip.w / (std::max)(1.0f, screen.Width());
            const float sy = chip.h / (std::max)(1.0f, screen.Height());

            for (const MissionItem& item : items) {
                if (item.desktop >= 0 && item.desktop != chip.index) continue;

                const POINT centre{ (item.bounds.left + item.bounds.right) / 2,
                                    (item.bounds.top + item.bounds.bottom) / 2 };
                if (::MonitorFromPoint(centre, MONITOR_DEFAULTTONEAREST) != screen.monitor)
                    continue;

                const float x = chip.x + (item.bounds.left - screen.rect.left) * sx;
                const float y = chip.y + (item.bounds.top  - screen.rect.top)  * sy;
                const float w = (item.bounds.right - item.bounds.left) * sx;
                const float h = (item.bounds.bottom - item.bounds.top) * sy;
                if (w < 2.0f || h < 2.0f) continue;

                FillSquircle(dc, d2dFactory.Get(), x, y, w, h, screen.Scaled(2.0f),
                             themeIsLight ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.86f)
                                          : D2D1::ColorF(0.12f, 0.12f, 0.15f, 0.88f));
                StrokeSquircle(dc, d2dFactory.Get(), x, y, w, h, screen.Scaled(2.0f),
                               1.0f,
                               themeIsLight ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.30f)
                                            : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.30f));

                if (ComPtr<ID2D1Bitmap1> icon = IconFor(dc, item)) {
                    const float side = (std::min)(w, h) * 0.55f;
                    if (side >= 5.0f)
                        dc->DrawBitmap(icon.Get(),
                                       D2D1::RectF(x + (w - side) * 0.5f, y + (h - side) * 0.5f,
                                                   x + (w + side) * 0.5f, y + (h + side) * 0.5f));
                }
            }
        }

        if (!current)
            FillSquircle(dc, d2dFactory.Get(), chip.x, chip.y, chip.w, chip.h, radius,
                         themeIsLight ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.42f)
                                      : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.42f));

        StrokeSquircle(dc, d2dFactory.Get(), chip.x, chip.y, chip.w, chip.h, radius,
                       screen.Scaled(current ? 2.5f : 1.0f),
                       current ? accent
                               : D2D1::ColorF(theme.chipBorder.r, theme.chipBorder.g,
                                              theme.chipBorder.b, theme.chipBorder.a * 0.3f));

        // The cross that closes it. Never on the last desktop: the shell ignores
        // the request, and a control that does nothing is worse than no control.
        if (spaces.size() > 1) {
            const D2D1_POINT_2F centre = CloseCentre(chip);
            const float r = chip.h * kCloseSize * 0.5f;

            ComPtr<ID2D1SolidColorBrush> disc;
            if (SUCCEEDED(dc->CreateSolidColorBrush(
                    D2D1::ColorF(0.06f, 0.06f, 0.08f, 0.72f), disc.Put())))
                dc->FillEllipse(D2D1::Ellipse(centre, r, r), disc.Get());

            ComPtr<ID2D1SolidColorBrush> mark;
            if (SUCCEEDED(dc->CreateSolidColorBrush(
                    D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.92f), mark.Put()))) {
                const float arm    = r * 0.44f;
                const float stroke = (std::max)(1.0f, screen.Scaled(1.4f));
                dc->DrawLine(D2D1::Point2F(centre.x - arm, centre.y - arm),
                             D2D1::Point2F(centre.x + arm, centre.y + arm),
                             mark.Get(), stroke);
                dc->DrawLine(D2D1::Point2F(centre.x + arm, centre.y - arm),
                             D2D1::Point2F(centre.x - arm, centre.y + arm),
                             mark.Get(), stroke);
            }
        }

        // The name goes under the miniature, not on it: a name printed over a
        // photograph is unreadable on some fraction of all wallpapers, and no
        // colour fixes that.
        if (format && textBrush && chip.index >= 0 &&
            chip.index < static_cast<int>(spaces.size())) {
            const std::wstring& name = spaces[static_cast<size_t>(chip.index)].name;
            const D2D1_RECT_F where = D2D1::RectF(
                chip.x, chip.y + chip.h, chip.x + chip.w,
                chip.y + chip.h + screen.Scaled(kChipLabel));

            // A shadow in the opposite direction to the text, one pixel down.
            //
            // The names used to sit on a flat fill of known brightness. They now
            // sit on glass, which by design takes most of its brightness from
            // whatever the wallpaper happens to be under it, so there is no
            // colour that is readable on all of them. This costs one extra
            // DrawText per desktop and removes the question.
            ComPtr<ID2D1SolidColorBrush> halo;
            if (SUCCEEDED(dc->CreateSolidColorBrush(
                    themeIsLight ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.55f)
                                 : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.55f),
                    halo.Put()))) {
                D2D1_RECT_F under = where;
                under.top    += screen.Scaled(1.0f);
                under.bottom += screen.Scaled(1.0f);
                dc->DrawTextW(name.c_str(), static_cast<UINT32>(name.size()),
                              format.Get(), under, halo.Get());
            }

            dc->DrawTextW(name.c_str(), static_cast<UINT32>(name.size()), format.Get(),
                          where, textBrush.Get());
        }
    }

    screen.bar.Brush(compositor.CreateSurfaceBrush(screen.barSurface));
    screen.bar.Size({ width, height });
    screen.bar.Offset({ 0.0f, 0.0f, 0.0f });
}

ComPtr<ID2D1Bitmap1> Mission::Impl::IconFor(ID2D1DeviceContext* dc,
                                            const MissionItem& item) {
    const auto existing = iconBitmaps.find(item.appKey);
    if (existing != iconBitmaps.end()) return existing->second;
    if (item.icon.Empty()) return {};

    // Uploaded once per application rather than once per window. A D2D bitmap
    // belongs to the device, not to the context it was made on, so the same one
    // draws into every surface.
    ComPtr<ID2D1Bitmap1> bitmap = UploadBitmap(dc, item.icon);
    if (bitmap) iconBitmaps.emplace(item.appKey, bitmap);
    return bitmap;
}

// The app icon and the name, in one small surface under each pile.
//
// One surface per pile, not per window and not one the size of the screen. A
// screen-sized premultiplied surface on a 4K display is thirty-three megabytes
// for what amounts to a few hundred kilobytes of ink, and a surface per window
// is a separate BeginDraw and flush for each one.
void Mission::Impl::BakeChrome(Screen& screen, Tile& tile) {
    if (tile.item < 0 || tile.item >= static_cast<int>(items.size())) return;
    if (!tile.chrome) return;

    const MissionItem& item = items[static_cast<size_t>(tile.item)];

    // Grouped, the label names the application, because the pile is the
    // application. Ungrouped, every window is its own pile of one and the label
    // names the window.
    const bool grouped = config::Current().missionGroupByApp && !tile.ownName;
    const std::wstring text =
        (grouped && !item.appName.empty()) ? item.appName
                                           : (item.title.empty() ? item.appName : item.title);

    const float badge  = screen.Scaled(kBadgeSize);
    const float titleH = screen.Scaled(kTitleHeight);

    // Wide enough for a readable name under a narrow window, and never so wide
    // that two neighbours' names run into each other.
    const float width  = (std::max)(tile.pileW, screen.Scaled(190.0f));

    // Room under the name for its own shadow. Without it the lowest ring offset
    // falls outside the surface and is thrown away, which is the same mistake
    // that once clipped the top half off every application icon.
    const float height = badge + screen.Scaled(kTitleGap) + titleH +
                         screen.Scaled(kTitleShadow) * 2.0f;

    tile.chromeSurface = graphics.CreateDrawingSurface(
        { width, height },
        winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
        winrt::Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);

    SurfaceDraw draw(tile.chromeSurface);
    if (!draw.ok) return;

    ID2D1DeviceContext* dc = draw.dc.Get();
    dc->Clear(D2D1::ColorF(0, 0, 0, 0));

    const float centre = width * 0.5f;

    // The icon sits ON the window, over its lower edge, rather than under the
    // pile. That is what makes it read as this window's application instead of
    // a caption floating below a group of rectangles.
    //
    // The whole badge is inside this surface. The first version drew it from
    // minus half a badge, so the top half fell outside the surface and was
    // silently clipped away, and only the bottom half of every icon appeared.
    if (ComPtr<ID2D1Bitmap1> icon = IconFor(dc, item)) {
        dc->DrawBitmap(icon.Get(),
                       D2D1::RectF(centre - badge * 0.5f, 0.0f,
                                   centre + badge * 0.5f, badge));
    }

    ComPtr<IDWriteTextFormat> format =
        MakeFormat(dwriteFactory.Get(), screen.Scaled(12.5f), DWRITE_FONT_WEIGHT_SEMI_BOLD);
    if (!format) return;

    ComPtr<IDWriteInlineObject> ellipsis;
    if (SUCCEEDED(dwriteFactory->CreateEllipsisTrimmingSign(format.Get(), ellipsis.Put()))) {
        DWRITE_TRIMMING trimming{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
        format->SetTrimming(&trimming, ellipsis.Get());
    }

    const float top = badge + screen.Scaled(kTitleGap);
    const D2D1_RECT_F where = D2D1::RectF(screen.Scaled(6.0f), top,
                                          width - screen.Scaled(6.0f), top + titleH);

    // No plate behind the name.
    //
    // It had one, because the name sits over the wallpaper and bare text is
    // unreadable on some fraction of all desktops. A capsule solves that and
    // costs the thing the label is for: macOS puts a name under an icon, not a
    // pill under an icon, and the pill was the first thing anyone noticed.
    //
    // A shadow does the same job without drawing anything of its own. Eight
    // offsets of black at low alpha around the glyphs, which is a cheap
    // approximation of a blur, plus a heavier one straight down for weight. Over
    // a white wallpaper the shadow is what separates the letters; over a dark one
    // it costs nothing, because black on dark is invisible.
    const float spread = screen.Scaled(kTitleShadow);

    ComPtr<ID2D1SolidColorBrush> shadow;
    if (SUCCEEDED(dc->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.42f),
                                            shadow.Put()))) {
        static const float kRing[8][2] = {
            { -1.0f, -1.0f }, { 0.0f, -1.0f }, { 1.0f, -1.0f },
            { -1.0f,  0.0f },                  { 1.0f,  0.0f },
            { -1.0f,  1.0f }, { 0.0f,  1.0f }, { 1.0f,  1.0f },
        };

        for (const auto& offset : kRing) {
            D2D1_RECT_F at = where;
            at.left   += offset[0] * spread;
            at.right  += offset[0] * spread;
            at.top    += offset[1] * spread;
            at.bottom += offset[1] * spread;
            dc->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format.Get(),
                          at, shadow.Get());
        }
    }

    ComPtr<ID2D1SolidColorBrush> brush;
    if (SUCCEEDED(dc->CreateSolidColorBrush(theme.tileName, brush.Put())))
        dc->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format.Get(),
                      where, brush.Get());

    tile.chrome.Brush(compositor.CreateSurfaceBrush(tile.chromeSurface));
    tile.chrome.Size({ width, height });

    // Anchored on the FRONT window, not on the pile's bounding box: the pile
    // fans down and to the right, so its box bottom is under the last window in
    // the fan and the icon would sit under a window nobody is looking at. Two
    // thirds of the badge overlaps the window it belongs to.
    const float anchorX = static_cast<float>(tile.screenRect.left + tile.screenRect.right) * 0.5f;
    tile.chrome.Offset({ anchorX - width * 0.5f,
                         static_cast<float>(tile.screenRect.bottom) - badge * 0.66f, 0.0f });
}

void Mission::Impl::BuildTiles(Screen& screen, const std::vector<int>& members,
                               int slide) {
    ReleaseTiles(screen);
    if (members.empty()) return;

    const float margin  = screen.Scaled(kOuterMargin);
    const float barH    = spaces.empty() ? 0.0f : screen.Scaled(kBarHeight);
    // What the badge and the name need below the lowest window. The badge
    // mostly overlaps the window, so only its tail counts.
    const float chromeH = screen.Scaled(kBadgeSize) * 0.34f + screen.Scaled(kTitleGap) +
                          screen.Scaled(kTitleHeight);

    const float regionX = margin;
    const float regionY = barH + margin;

    // Floored, not just computed. On a short display the bar, the margins and
    // the chrome band can add up to more than the screen, and Layout answers a
    // non-positive region with an empty result.
    const float regionW = (std::max)(screen.Scaled(160.0f), screen.Width() - margin * 2);
    const float regionH = (std::max)(screen.Scaled(120.0f),
                                     screen.Height() - barH - margin * 2 - chromeH);

    std::vector<mission::Window> windows;
    windows.reserve(members.size());
    for (int index : members) {
        const MissionItem& item = items[static_cast<size_t>(index)];
        mission::Window w;
        w.x     = static_cast<float>(item.bounds.left - screen.rect.left);
        w.y     = static_cast<float>(item.bounds.top  - screen.rect.top);
        w.w     = static_cast<float>((std::max)(1l, item.bounds.right - item.bounds.left));
        w.h     = static_cast<float>((std::max)(1l, item.bounds.bottom - item.bounds.top));
        w.group = item.group;
        w.order = item.order;
        windows.push_back(w);
    }

    mission::Params params;
    params.gap        = screen.Scaled(config::Current().missionGap);
    params.fan        = screen.Scaled(config::Current().missionFan);
    params.clusterGap = screen.Scaled(config::Current().missionClusterGap);
    params.groupByApp = config::Current().missionGroupByApp;

    const double started = NowMs();
    const mission::Result result = mission::Layout(windows, regionW, regionH, params);

    MACTAB_DIAG("mission: %zu window(s) arranged in %.2f ms, scale %.3f, %d pass(es)%s",
                members.size(), NowMs() - started, result.scale, result.iterations,
                result.relaxed ? "" : " (grid fallback)");

    if (result.tiles.size() != members.size()) {
        MACTAB_FAIL("mission: arrangement returned %zu placement(s) for %zu window(s)",
                    result.tiles.size(), members.size());
        return;
    }

    // Back to front, so the most recent window of a pile is not buried under
    // the ones behind it.
    std::vector<size_t> order(members.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        if (result.tiles[a].depth != result.tiles[b].depth)
            return result.tiles[a].depth > result.tiles[b].depth;
        return a < b;
    });

    screen.tiles.resize(members.size());

    const double snapshotsStarted = NowMs();
    int snapshots = 0, skipped = 0;

    for (size_t slot : order) {
        const mission::Placement& place = result.tiles[slot];
        Tile& tile = screen.tiles[slot];
        tile.item  = members[slot];
        tile.depth = place.depth;

        const MissionItem& item = items[static_cast<size_t>(tile.item)];

        tile.screenRect = RECT{
            static_cast<LONG>(regionX + place.x),
            static_cast<LONG>(regionY + place.y),
            static_cast<LONG>(regionX + place.x + place.w),
            static_cast<LONG>(regionY + place.y + place.h),
        };
        tile.liveRect = tile.screenRect;
        tile.group    = item.group;
        tile.baseW    = place.w;
        tile.baseH    = place.h;

        tile.window   = item.hwnd;
        tile.homeRect = RECT{ item.bounds.left   - screen.rect.left,
                              item.bounds.top    - screen.rect.top,
                              item.bounds.right  - screen.rect.left,
                              item.bounds.bottom - screen.rect.top };

        // Where it flies in from. Zero means the window's own place on screen,
        // which is the reveal; a direction means off the side, which is a
        // desktop sliding past.
        tile.sourceRect = (slide == 0)
            ? tile.homeRect
            : RECT{ tile.screenRect.left   + slide * static_cast<LONG>(screen.Width()),
                    tile.screenRect.top,
                    tile.screenRect.right  + slide * static_cast<LONG>(screen.Width()),
                    tile.screenRect.bottom };

        // The pile's box, so the badge and the name sit under all of it rather
        // than under the front window alone.
        tile.pileX      = regionX + place.x;
        tile.pileW      = place.w;
        tile.pileBottom = regionY + place.y + place.h;
        for (const mission::Cluster& cluster : result.clusters) {
            if (cluster.group != item.group) continue;
            tile.pileX      = regionX + cluster.x;
            tile.pileW      = cluster.w;
            tile.pileBottom = regionY + cluster.y + cluster.h;
            break;
        }

        tile.holder = compositor.CreateContainerVisual();
        tile.holder.Size({ place.w, place.h });
        tile.holder.Offset({ regionX + place.x, regionY + place.y, 0.0f });
        screen.tileLayer.Children().InsertAtTop(tile.holder);

        // The shadow rides with the window, so it flies and scales with it.
        if (shadowBrush) {
            tile.shadow = compositor.CreateSpriteVisual();
            tile.shadow.Brush(shadowBrush);
            tile.shadow.Size({ place.w + textureSpread * 2, place.h + textureSpread * 2 });
            tile.shadow.Offset({ -textureSpread, -textureSpread, 0.0f });
            tile.holder.Children().InsertAtTop(tile.shadow);
        }

        bool haveThumbnail = false;

        if (dcompDevice) {
            // What the thumbnail's pixels cover, and what part of that is the
            // window you can see. They differ by the invisible resize border,
            // around seven pixels a side at 100%, and the arrangement is built
            // from the visible frame.
            RECT sourceWindow{}, sourceFrame{};
            const bool haveGeometry =
                thumbnail::SourceGeometry(item.hwnd, sourceWindow, sourceFrame);

            // Deliberately GetWindowRect rather than DWM's own idea of the
            // thumbnail's size, which the two can disagree about on a cloaked
            // UWP window. Everything below scales and offsets in this same
            // space, and a size from one space with an offset from another is
            // how the window ends up not filling its tile.
            const SIZE render{
                haveGeometry ? sourceWindow.right - sourceWindow.left
                             : tile.sourceRect.right - tile.sourceRect.left,
                haveGeometry ? sourceWindow.bottom - sourceWindow.top
                             : tile.sourceRect.bottom - tile.sourceRect.top };

            void* raw = nullptr;
            if (thumbnail::CreateSharedVisual(dcompDevice.get(), screen.hwnd, item.hwnd,
                                              render, &raw, &tile.thumbnail) && raw) {
                winrt::com_ptr<IUnknown> unknown;
                unknown.attach(reinterpret_cast<IUnknown*>(raw));

                if (auto visual = unknown.try_as<WUC::Visual>()) {
                    // Fit the VISIBLE frame to the tile, not the whole
                    // thumbnail. Scaling by the thumbnail's own size leaves the
                    // window smaller than its tile and pushed up and left
                    // inside it, which is what made the hover outline look like
                    // it was drawn around the wrong rectangle.
                    const float frameW = haveGeometry
                        ? static_cast<float>(sourceFrame.right - sourceFrame.left)
                        : static_cast<float>(render.cx);
                    const float frameH = haveGeometry
                        ? static_cast<float>(sourceFrame.bottom - sourceFrame.top)
                        : static_cast<float>(render.cy);

                    const float scaleX = place.w / (std::max)(1.0f, frameW);
                    const float scaleY = place.h / (std::max)(1.0f, frameH);

                    visual.Scale({ scaleX, scaleY, 1.0f });

                    // Slide the invisible border back off the top and the left,
                    // so what lands on the tile's origin is the frame's origin.
                    if (haveGeometry)
                        visual.Offset({ (sourceWindow.left - sourceFrame.left) * scaleX,
                                        (sourceWindow.top  - sourceFrame.top)  * scaleY,
                                        0.0f });

                    tile.holder.Children().InsertAtTop(visual);
                    tile.live      = visual;
                    tile.reduction = (place.w > 0.0f) ? frameW / place.w : 1.0f;
                    haveThumbnail  = true;
                }
            }
        }

        if (!haveThumbnail) {
            // Snapshots run on the thread that owes a frame: a fifty
            // millisecond ping plus a full-size readback each. So the tier gets
            // a budget and everything past it becomes a card, because a late
            // window is worse than a plain one.
            Bitmap content;
            if (thumbnail::Current() != thumbnail::Tier::IconOnly &&
                NowMs() - snapshotsStarted < kSnapshotBudgetMs) {
                content = thumbnail::Snapshot(item.hwnd, static_cast<int>(place.w),
                                              static_cast<int>(place.h));
                ++snapshots;
            } else if (thumbnail::Current() != thumbnail::Tier::IconOnly) {
                ++skipped;
            }

            tile.contentSurface = graphics.CreateDrawingSurface(
                { place.w, place.h },
                winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                winrt::Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);

            SurfaceDraw draw(tile.contentSurface);
            if (draw.ok) {
                ID2D1DeviceContext* dc = draw.dc.Get();
                dc->Clear(D2D1::ColorF(0, 0, 0, 0));

                if (!content.Empty()) {
                    if (ComPtr<ID2D1Bitmap1> bitmap = UploadBitmap(dc, std::move(content))) {
                        // Cubic rather than the linear DrawBitmap defaults to.
                        //
                        // The snapshot has already been reduced to fit the tile
                        // by a box filter, which is the right way to do the big
                        // reduction, but aspect ratio is preserved so it lands a
                        // pixel or two short in one axis and this last step is a
                        // small stretch. Linear on a small stretch is visibly
                        // soft, and this is drawn once per window per gesture,
                        // so the better filter costs nothing worth counting.
                        dc->DrawBitmap(bitmap.Get(),
                                       D2D1::RectF(0.0f, 0.0f, place.w, place.h),
                                       1.0f,
                                       D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC,
                                       nullptr);
                    }
                } else {
                    // A window-shaped plate with the app's icon on it, which is
                    // a design rather than a hole.
                    FillSquircle(dc, d2dFactory.Get(), 0.0f, 0.0f, place.w, place.h,
                                 screen.Scaled(kTileRadius),
                                 themeIsLight ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.80f)
                                              : D2D1::ColorF(0.10f, 0.10f, 0.13f, 0.85f));

                    if (ComPtr<ID2D1Bitmap1> icon = IconFor(dc, item)) {
                        const float side = (std::min)(place.w, place.h) * 0.40f;
                        dc->DrawBitmap(icon.Get(),
                                       D2D1::RectF((place.w - side) * 0.5f,
                                                   (place.h - side) * 0.5f,
                                                   (place.w + side) * 0.5f,
                                                   (place.h + side) * 0.5f));
                    }
                }
            }

            tile.content = compositor.CreateSpriteVisual();
            tile.content.Size({ place.w, place.h });
            tile.content.Brush(compositor.CreateSurfaceBrush(tile.contentSurface));
            tile.holder.Children().InsertAtTop(tile.content);
        }

        // One badge and one name per pile, under the front window. The chrome
        // does NOT ride with the tiles: a name and an icon hurtling across the
        // screen and shrinking as they go is unreadable, and it is not what the
        // reference does. It fades in as the flight settles instead.
        if (tile.depth == 0) {
            tile.chrome = compositor.CreateSpriteVisual();
            screen.chromeLayer.Children().InsertAtTop(tile.chrome);
            BakeChrome(screen, tile);
        }
    }

    if (skipped > 0)
        MACTAB_WARN("mission: %d snapshot(s) in %.0f ms, %d window(s) fell back to cards",
                    snapshots, NowMs() - snapshotsStarted, skipped);
}

// The hover outline, drawn for the window it is actually going round.
//
// It used to be one small texture baked at startup and stretched to every window
// by a nine-grid brush. That is the cheap way and it was never right. A nine-grid
// keeps its corners at a fixed size in SOURCE pixels, so the corner radius was
// whatever the largest display's scale said at startup, on every display; the
// stroke width was baked in the same way; and on any window narrower than twice
// the corner there is no stretchable middle left at all, so the outline came out
// crushed. None of that can be fixed while one texture has to serve every size.
//
// So it is drawn per hover, at that window's exact size, on that window's own
// display at that display's scale, with the same corner the tiles have. One
// small surface and one stroked geometry, and only when the size actually
// changes: moving along a row of windows that are all the same size redraws
// nothing.
void Mission::Impl::PositionOutline(Screen& screen) {
    if (screen.hovered < 0 || screen.hovered >= static_cast<int>(screen.tiles.size())) {
        screen.outline.Opacity(0.0f);
        return;
    }

    const RECT& rect = screen.tiles[static_cast<size_t>(screen.hovered)].liveRect;

    const float stroke = screen.Scaled(kOutlineWidth);
    const float pad    = std::ceil(stroke);
    const float w      = static_cast<float>(rect.right - rect.left);
    const float h      = static_cast<float>(rect.bottom - rect.top);
    if (w <= 1.0f || h <= 1.0f) {
        screen.outline.Opacity(0.0f);
        return;
    }

    const float surfaceW = w + pad * 2.0f;
    const float surfaceH = h + pad * 2.0f;

    if (!screen.outlineSurface ||
        screen.outlineW != surfaceW || screen.outlineH != surfaceH) {
        screen.outlineSurface = graphics.CreateDrawingSurface(
            { surfaceW, surfaceH },
            winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
            winrt::Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);
        screen.outlineW = surfaceW;
        screen.outlineH = surfaceH;

        SurfaceDraw draw(screen.outlineSurface);
        if (!draw.ok) {
            screen.outline.Opacity(0.0f);
            return;
        }

        ID2D1DeviceContext* dc = draw.dc.Get();
        dc->Clear(D2D1::ColorF(0, 0, 0, 0));

        // Centred on the window's own edge, so half the stroke sits outside it
        // and half over its border. Sitting entirely outside leaves a hairline
        // of desktop between the line and the window at fractional scales.
        StrokeSquircle(dc, d2dFactory.Get(), pad, pad, w, h,
                       screen.Scaled(kTileRadius), stroke, accent);

        screen.outline.Brush(compositor.CreateSurfaceBrush(screen.outlineSurface));
    }

    const bool wasHidden = screen.outline.Opacity() < 0.5f;

    screen.outline.Opacity(1.0f);
    screen.outline.Size({ surfaceW, surfaceH });

    const WFN::float3 destination{ static_cast<float>(rect.left) - pad,
                                   static_cast<float>(rect.top)  - pad, 0.0f };

    // Springing from wherever it was left is right between two windows and
    // wrong for the first one, where "wherever it was left" is the corner.
    if (wasHidden) {
        screen.outline.StopAnimation(L"Offset");
        screen.outline.Offset(destination);
        return;
    }

    auto spring = compositor.CreateSpringVector3Animation();
    spring.DampingRatio(0.85f);
    spring.Period(std::chrono::milliseconds(45));
    spring.FinalValue(destination);
    screen.outline.StartAnimation(L"Offset", spring);
}

void Mission::Impl::SetHovered(Screen& screen, int index) {
    if (index == screen.hovered) return;
    screen.hovered = index;
    GuardMission(*this, "SetHovered", [&] { PositionOutline(screen); });
}

int Mission::Impl::HitTestTile(const Screen& screen, POINT client) const {
    // Front to back, so the window on top of a pile takes the click.
    int best = -1;
    for (size_t i = 0; i < screen.tiles.size(); ++i) {
        const RECT& r = screen.tiles[i].liveRect;
        if (client.x < r.left || client.x >= r.right ||
            client.y < r.top  || client.y >= r.bottom)
            continue;
        if (best < 0 || screen.tiles[i].depth < screen.tiles[static_cast<size_t>(best)].depth)
            best = static_cast<int>(i);
    }
    return best;
}

int Mission::Impl::PileSize(const Screen& screen, int group) const {
    int n = 0;
    for (const Tile& tile : screen.tiles)
        if (tile.group == group) ++n;
    return n;
}

// Where the close cross sits on a desktop miniature, in bar coordinates.
D2D1_POINT_2F Mission::Impl::CloseCentre(const mission::SpaceChip& chip) {
    return D2D1::Point2F(chip.x + chip.h * kCloseInset,
                         chip.y + chip.h * kCloseInset);
}

// Which desktop's close cross the point is on, or -1.
//
// Checked before the miniature itself, because the cross is inside it: without
// this the click would simply be a click on the desktop and the cross would do
// nothing at all.
int Mission::Impl::HitTestClose(const Screen& screen, POINT client) const {
    if (spaces.size() < 2) return -1;

    for (size_t i = 0; i < screen.chips.size(); ++i) {
        const mission::SpaceChip& c = screen.chips[i];
        if (c.add) continue;

        const D2D1_POINT_2F centre = CloseCentre(c);
        const float radius = c.h * kCloseSize * 0.5f;
        const float dx = client.x - centre.x;
        const float dy = client.y - centre.y;

        // A little larger than it is drawn. It is a small target and a miss by
        // two pixels reads as the control not working rather than as a miss.
        const float reach = radius + c.h * 0.06f;
        if (dx * dx + dy * dy <= reach * reach) return c.index;
    }
    return -1;
}

int Mission::Impl::HitTestChip(const Screen& screen, POINT client) const {
    for (size_t i = 0; i < screen.chips.size(); ++i) {
        const mission::SpaceChip& c = screen.chips[i];
        if (client.x >= c.x && client.x < c.x + c.w &&
            client.y >= c.y && client.y < c.y + c.h)
            return static_cast<int>(i);
    }
    return -1;
}

// The nearest window in a direction, by centre distance.
//
// Not an index step. The arrangement has no rows, so "the next one to the
// right" is a geometric question, and walking the list would jump across the
// screen in a way that looks random.
int Mission::Impl::Neighbour(const Screen& screen, int from, int dx, int dy) const {
    if (screen.tiles.empty()) return -1;

    // The first arrow press starts on the most recently used window rather than
    // on whichever tile happened to be built first. Nothing is highlighted when
    // the overlay opens, which is what macOS does, so this is where a keyboard
    // user comes in and it should be somewhere they recognise.
    if (from < 0 || from >= static_cast<int>(screen.tiles.size())) {
        int best = 0;
        for (size_t i = 1; i < screen.tiles.size(); ++i)
            if (screen.tiles[i].item >= 0 &&
                screen.tiles[i].item < screen.tiles[static_cast<size_t>(best)].item)
                best = static_cast<int>(i);
        return best;
    }

    const RECT& origin = screen.tiles[static_cast<size_t>(from)].liveRect;
    const float ox = static_cast<float>(origin.left + origin.right) * 0.5f;
    const float oy = static_cast<float>(origin.top + origin.bottom) * 0.5f;

    int   best     = -1;
    float bestCost = 0.0f;

    for (size_t i = 0; i < screen.tiles.size(); ++i) {
        if (static_cast<int>(i) == from) continue;

        const RECT& r = screen.tiles[i].liveRect;
        const float cx = static_cast<float>(r.left + r.right) * 0.5f;
        const float cy = static_cast<float>(r.top + r.bottom) * 0.5f;

        const float along  = (cx - ox) * dx + (cy - oy) * dy;
        const float across = (cx - ox) * dy + (cy - oy) * dx;
        if (along <= 1.0f) continue;

        // Distance along the direction plus a penalty for drifting off it, so a
        // window straight ahead beats a nearer one far to the side.
        const float cost = along + std::fabs(across) * 2.0f;
        if (best < 0 || cost < bestCost) {
            best     = static_cast<int>(i);
            bestCost = cost;
        }
    }

    return (best >= 0) ? best : from;
}

// ---------------------------------------------------------------------------

// Rebuild every display's arrangement for one desktop.
//
// `slide` is 0 for the reveal, where each window flies from where it really is,
// and plus or minus one when a desktop is sliding past, where the outgoing
// arrangement leaves one way and the incoming arrives from the other.
// Replace the worst-reduced live previews with a properly filtered still.
//
// Run after the flight has landed, so none of it is on the path anybody is
// waiting on, and only for tiles where the compositor is being asked to do a
// reduction it cannot do honestly. Below that ratio bilinear is a correct box
// average and a still would be no better and would stop moving.
//
// The still is baked at twice the tile so the compositor's own remaining step is
// exactly 2:1, and so a pile that spreads out still has pixels to magnify.
//
// What this costs is that those previews stop being live. A video keeps playing
// until the sharpening lands and then holds its last frame. That is the trade,
// and it is the right way round: Mission Control is a picture of where things
// are, and the thing people actually complain about is not being able to tell
// two documents apart.
void Mission::Impl::SharpenTiles() {
    if (!visible || closing) return;
    if (!config::Current().missionSharpPreviews) return;
    if (sharpInFlight) return;

    // Which windows want one. Nothing is captured here except handles and
    // sizes, because the loop that reads them runs on another thread.
    std::vector<SharpJob> jobs;

    for (size_t s = 0; s < screens.size(); ++s) {
        Screen& screen = screens[s];
        for (size_t t = 0; t < screen.tiles.size(); ++t) {
            const Tile& tile = screen.tiles[t];
            if (tile.sharpened || !tile.live) continue;
            if (tile.reduction <= kSharpRatio) continue;
            if (tile.item < 0 || tile.item >= static_cast<int>(items.size())) continue;

            const float w = static_cast<float>(tile.screenRect.right - tile.screenRect.left);
            const float h = static_cast<float>(tile.screenRect.bottom - tile.screenRect.top);
            if (w < 8.0f || h < 8.0f) continue;

            // Never above the window's own resolution: there is nothing there to
            // find, and asking for it would only cost memory.
            const float over = (std::min)(kSharpOversample, tile.reduction);

            SharpJob job;
            job.screen = s;
            job.tile   = t;
            job.window = items[static_cast<size_t>(tile.item)].hwnd;
            job.bakeW  = (std::max)(1, static_cast<int>(w * over));
            job.bakeH  = (std::max)(1, static_cast<int>(h * over));
            jobs.push_back(job);
        }
    }

    if (jobs.empty() || screens.empty()) return;

    sharpInFlight = true;

    // PrintWindow drives the target application's own paint path, so a window
    // that is busy holds the caller for the fifty millisecond guard, and thirty
    // of those is a stall the compositor cannot hide. This used to run right
    // here, on the thread that answers the mouse and the keyboard, which is why
    // Mission Control went deaf for up to half a second shortly after opening.
    //
    // If the post fails the overlay is already gone, and the batch is dropped
    // by the only thing still holding it.
    const HWND     target = screens.front().hwnd;
    const uint32_t epoch  = tileEpoch;

    std::thread([jobs, target, epoch] {
        auto* batch  = new SharpBatch();
        batch->epoch = epoch;

        const double started = NowMs();
        for (const SharpJob& job : jobs) {
            if (NowMs() - started >= kSharpBudgetMs) break;

            SharpResult result;
            result.screen  = job.screen;
            result.tile    = job.tile;
            result.window  = job.window;
            result.bakeW   = job.bakeW;
            result.bakeH   = job.bakeH;
            result.picture = thumbnail::Snapshot(job.window, job.bakeW, job.bakeH);
            batch->results.push_back(std::move(result));
        }

        if (!::PostMessageW(target, kMsgSharpReady, 0,
                            reinterpret_cast<LPARAM>(batch)))
            delete batch;
    }).detach();
}

// The pixels, back on the thread that owns the visual tree.
void Mission::Impl::ApplySharpened(const SharpBatch& batch) {
    sharpInFlight = false;

    if (!visible || closing) return;
    if (batch.epoch != tileEpoch) {
        MACTAB_DIAG("mission: %zu sharpened preview(s) arrived for an arrangement "
                    "that is gone", batch.results.size());
        return;
    }

    // Guarded because this is now reached from the window procedure directly,
    // and everything below it that talks to the compositor throws on failure.
    GuardMission(*this, "ApplySharpened", [&] {

    const double started = NowMs();
    int done = 0, skipped = 0;

    for (const SharpResult& result : batch.results) {
        if (result.screen >= screens.size()) continue;
        Screen& screen = screens[result.screen];
        if (result.tile >= screen.tiles.size()) continue;

        Tile& tile = screen.tiles[result.tile];
        if (tile.sharpened || !tile.live) continue;
        if (tile.item < 0 || tile.item >= static_cast<int>(items.size())) continue;
        if (items[static_cast<size_t>(tile.item)].hwnd != result.window) continue;

        if (result.picture.Empty()) {
            // A window that will not be printed keeps its live thumbnail,
            // which is exactly what it had before and better than nothing.
            ++skipped;
            tile.sharpened = true;
            continue;
        }

        const float w = static_cast<float>(tile.screenRect.right - tile.screenRect.left);
        const float h = static_cast<float>(tile.screenRect.bottom - tile.screenRect.top);

        const int bakeW = result.bakeW;
        const int bakeH = result.bakeH;
        Bitmap picture  = result.picture;

        auto surface = graphics.CreateDrawingSurface(
            { static_cast<float>(bakeW), static_cast<float>(bakeH) },
            winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
            winrt::Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);

        {
            SurfaceDraw draw(surface);
            if (!draw.ok) continue;

            ID2D1DeviceContext* dc = draw.dc.Get();
            dc->Clear(D2D1::ColorF(0, 0, 0, 0));

            if (ComPtr<ID2D1Bitmap1> bitmap = UploadBitmap(dc, std::move(picture)))
                dc->DrawBitmap(bitmap.Get(),
                               D2D1::RectF(0.0f, 0.0f, static_cast<float>(bakeW),
                                           static_cast<float>(bakeH)),
                               1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC,
                               nullptr);
        }

        auto brush = compositor.CreateSurfaceBrush(surface);
        brush.Stretch(WUC::CompositionStretch::Fill);

        auto still = compositor.CreateSpriteVisual();
        still.Brush(brush);
        still.Size({ w, h });

        tile.contentSurface = surface;
        tile.content        = still;
        tile.holder.Children().InsertAtTop(still);

        // Swapped rather than cross-faded. It is the same picture, only
        // honest, so there is nothing to fade between.
        //
        // Opacity rather than IsVisible on purpose. Both would do, and Opacity
        // is in the very first version of this API, where IsVisible is one of
        // the ones this project has to check the build number for before it can
        // use. Not worth finding out the hard way on somebody else's Windows 10.
        tile.live.Opacity(0.0f);
        tile.sharpened = true;
        ++done;
    }

    if (done || skipped)
        MACTAB_DIAG("mission: sharpened %d preview(s), skipped %d, in %.1f ms",
                    done, skipped, NowMs() - started);
    });
}

void Mission::Impl::ScheduleSharpen() {
    if (screens.empty()) return;
    ::SetTimer(screens.front().hwnd, kSharpenTimerId,
               config::Current().missionRevealMs + 40, nullptr);
}

void Mission::Impl::BuildForDesktop(int desktop, int slide) {
    browsed = desktop;

    // Which windows belong to which display. A window straddling two monitors
    // belongs to whichever holds its centre. A window with no desktop of its
    // own is pinned and appears on every one.
    std::vector<std::vector<int>> members(screens.size());
    for (size_t i = 0; i < items.size(); ++i) {
        if (items[i].desktop >= 0 && items[i].desktop != desktop) continue;

        const RECT& b = items[i].bounds;
        const POINT centre{ (b.left + b.right) / 2, (b.top + b.bottom) / 2 };
        const HMONITOR monitor = ::MonitorFromPoint(centre, MONITOR_DEFAULTTONEAREST);

        for (size_t s = 0; s < screens.size(); ++s)
            if (screens[s].monitor == monitor) {
                members[s].push_back(static_cast<int>(i));
                break;
            }
    }

    for (size_t s = 0; s < screens.size(); ++s) {
        Screen& screen = screens[s];
        screen.expandedGroup = -1;
        BakeBackdrop(screen);
        BakeBar(screen);
        BuildTiles(screen, members[s], slide);
    }

    // Every surface cut from the wallpaper now exists, and each of those is
    // kept for the life of the process, so the decoded pixels behind them are
    // finished with. They are the largest thing this program ever holds: with
    // the blur off the decode is at the display's native size, which is
    // thirty-three megabytes on a 4K monitor, per monitor, and they were
    // sitting there from the prewarm at startup onward against an idle budget
    // of twenty. Anything that invalidates a surface decodes again.
    wallpaper::Trim();
}

namespace {

// Fly every window from where it came in from to where it lands, and bring the
// names and icons up behind them.
//
// Offset and Scale only, both on the compositor thread, so this costs the
// process nothing while it runs.
void StartReveal(WUC::Compositor compositor, Mission::Impl::Screen& screen,
                 std::chrono::milliseconds duration, bool fadeRoot) {
    auto easing = compositor.CreateCubicBezierEasingFunction({ 0.22f, 1.0f },
                                                            { 0.36f, 1.0f });

    for (Mission::Impl::Tile& tile : screen.tiles) {
        const float finalW = static_cast<float>(tile.screenRect.right - tile.screenRect.left);
        const float finalH = static_cast<float>(tile.screenRect.bottom - tile.screenRect.top);
        if (finalW <= 0.0f || finalH <= 0.0f) continue;

        auto offset = compositor.CreateVector3KeyFrameAnimation();
        offset.InsertKeyFrame(0.0f, { static_cast<float>(tile.sourceRect.left),
                                      static_cast<float>(tile.sourceRect.top), 0.0f });
        offset.InsertKeyFrame(1.0f, { static_cast<float>(tile.screenRect.left),
                                      static_cast<float>(tile.screenRect.top), 0.0f },
                              easing);
        offset.Duration(duration);
        tile.holder.StartAnimation(L"Offset", offset);

        auto scale = compositor.CreateVector3KeyFrameAnimation();
        scale.InsertKeyFrame(0.0f,
            { static_cast<float>(tile.sourceRect.right - tile.sourceRect.left) / finalW,
              static_cast<float>(tile.sourceRect.bottom - tile.sourceRect.top) / finalH,
              1.0f });
        scale.InsertKeyFrame(1.0f, { 1.0f, 1.0f, 1.0f }, easing);
        scale.Duration(duration);
        tile.holder.StartAnimation(L"Scale", scale);
    }

    // The names and icons arrive once the windows have landed. A label
    // hurtling across the screen and shrinking as it goes is unreadable.
    auto chromeFade = compositor.CreateScalarKeyFrameAnimation();
    chromeFade.InsertKeyFrame(0.0f, 0.0f);
    chromeFade.InsertKeyFrame(0.62f, 0.0f);
    chromeFade.InsertKeyFrame(1.0f, 1.0f, easing);
    chromeFade.Duration(duration);
    screen.chromeLayer.StartAnimation(L"Opacity", chromeFade);

    if (!fadeRoot) return;

    // The strip comes down from the top edge rather than fading in place, which
    // is what macOS does with it and what makes it read as a thing arriving
    // rather than as part of the same wash as everything else.
    //
    // Only on the way in. Walking from one desktop to another rebuilds the
    // arrangement under a strip that has not moved, and re-running this would
    // have the desktops jump every time an arrow was pressed.
    const float barH = screen.Scaled(kBarHeight);
    if (barH > 0.0f) {
        auto slide = [&](WUC::SpriteVisual& visual, float restX, float restY) {
            if (!visual) return;
            auto drop = compositor.CreateVector3KeyFrameAnimation();
            drop.InsertKeyFrame(0.0f, { restX, restY - barH, 0.0f });
            drop.InsertKeyFrame(1.0f, { restX, restY, 0.0f }, easing);
            drop.Duration(duration);
            visual.StartAnimation(L"Offset", drop);
        };
        slide(screen.barGlass, -screen.barOverhang, -screen.barOverhang);
        slide(screen.bar, 0.0f, 0.0f);
    }

    auto fade = compositor.CreateScalarKeyFrameAnimation();
    fade.InsertKeyFrame(0.0f, 0.0f);
    fade.InsertKeyFrame(1.0f, 1.0f, easing);
    fade.Duration(duration);
    screen.root.StartAnimation(L"Opacity", fade);
}

} // namespace

// Spread one application's pile out so its windows can be told apart.
//
// The tiles are not rebuilt, they are moved: the same visuals travel from the
// pile to their spread positions, which is what makes it read as the pile
// opening rather than as one arrangement being swapped for another. Everything
// belonging to another application dims rather than disappearing, so the pile
// is still somewhere on a desktop you recognise.
void Mission::Impl::ExpandPile(Screen& screen, int group) {
    std::vector<size_t> pile;
    for (size_t i = 0; i < screen.tiles.size(); ++i)
        if (screen.tiles[i].group == group) pile.push_back(i);

    if (pile.size() < 2) return;

    const float margin  = screen.Scaled(kOuterMargin);
    const float barH    = spaces.empty() ? 0.0f : screen.Scaled(kBarHeight);
    const float chromeH = screen.Scaled(kBadgeSize) * 0.34f + screen.Scaled(kTitleGap) +
                          screen.Scaled(kTitleHeight);

    const float regionX = margin;
    const float regionY = barH + margin;
    const float regionW = (std::max)(screen.Scaled(160.0f), screen.Width() - margin * 2);
    const float regionH = (std::max)(screen.Scaled(120.0f),
                                     screen.Height() - barH - margin * 2 - chromeH);

    std::vector<mission::Window> windows;
    for (size_t i : pile) {
        const MissionItem& item = items[static_cast<size_t>(screen.tiles[i].item)];
        mission::Window w;
        w.x = static_cast<float>(item.bounds.left - screen.rect.left);
        w.y = static_cast<float>(item.bounds.top  - screen.rect.top);
        w.w = static_cast<float>((std::max)(1l, item.bounds.right - item.bounds.left));
        w.h = static_cast<float>((std::max)(1l, item.bounds.bottom - item.bounds.top));
        w.group = 0;
        w.order = item.order;
        windows.push_back(w);
    }

    mission::Params params;
    params.gap        = screen.Scaled(config::Current().missionGap);
    params.groupByApp = false;   // inside one app there is nothing left to group

    const mission::Result result = mission::Layout(windows, regionW, regionH, params);
    if (result.tiles.size() != pile.size()) return;

    const auto duration = std::chrono::milliseconds(config::Current().missionRevealMs);
    auto easing = compositor.CreateCubicBezierEasingFunction({ 0.22f, 1.0f }, { 0.36f, 1.0f });

    for (size_t k = 0; k < pile.size(); ++k) {
        Tile& tile = screen.tiles[pile[k]];
        const mission::Placement& place = result.tiles[k];

        tile.liveRect = RECT{
            static_cast<LONG>(regionX + place.x),
            static_cast<LONG>(regionY + place.y),
            static_cast<LONG>(regionX + place.x + place.w),
            static_cast<LONG>(regionY + place.y + place.h),
        };

        auto offset = compositor.CreateVector3KeyFrameAnimation();
        offset.InsertKeyFrame(1.0f, { regionX + place.x, regionY + place.y, 0.0f }, easing);
        offset.Duration(duration);
        tile.holder.StartAnimation(L"Offset", offset);

        auto scale = compositor.CreateVector3KeyFrameAnimation();
        scale.InsertKeyFrame(1.0f, { place.w / (std::max)(1.0f, tile.baseW),
                                     place.h / (std::max)(1.0f, tile.baseH), 1.0f }, easing);
        scale.Duration(duration);
        tile.holder.StartAnimation(L"Scale", scale);

        // Every window in the pile is its own thing now, so each gets its own
        // name rather than sharing the application's.
        if (!tile.chrome) {
            tile.chrome = compositor.CreateSpriteVisual();
            screen.chromeLayer.Children().InsertAtTop(tile.chrome);
        }
        tile.screenRect = tile.liveRect;
        tile.pileX      = static_cast<float>(tile.liveRect.left);
        tile.pileW      = place.w;
        tile.pileBottom = static_cast<float>(tile.liveRect.bottom);
        tile.ownName    = true;   // each of them is its own thing now
    }

    for (size_t i = 0; i < screen.tiles.size(); ++i) {
        if (screen.tiles[i].group == group) continue;

        auto dim = compositor.CreateScalarKeyFrameAnimation();
        dim.InsertKeyFrame(1.0f, 0.18f, easing);
        dim.Duration(duration);
        screen.tiles[i].holder.StartAnimation(L"Opacity", dim);
        if (screen.tiles[i].chrome)
            screen.tiles[i].chrome.StartAnimation(L"Opacity", dim);
    }

    screen.expandedGroup = group;
    screen.hovered = -1;

    // Re-baked after the geometry is settled, because the label text changes
    // from the application's name to each window's own.
    for (size_t i : pile) BakeChrome(screen, screen.tiles[i]);

    MACTAB_DIAG("mission: spread %zu window(s) of app %d", pile.size(), group);
}

void Mission::Impl::CollapsePile(Screen& screen) {
    if (screen.expandedGroup < 0) return;

    const int group = screen.expandedGroup;
    screen.expandedGroup = -1;

    // Where everything is right now, keyed by window, so the rebuilt tiles can
    // travel back from there rather than from the real desktop. Without this
    // the pile closing looks like the whole arrangement being revealed again,
    // with every window flying in from its actual position on screen.
    std::map<int, RECT> current;
    for (const Screen& other : screens)
        for (const Tile& tile : other.tiles)
            current.emplace(tile.item, tile.liveRect);

    // Nothing here knows the collapsed geometry any more, and reconstructing it
    // by hand would be a second copy of the arrangement.
    BuildForDesktop(browsed, 0);

    for (Screen& other : screens) {
        for (Tile& tile : other.tiles) {
            tile.holder.Opacity(1.0f);
            if (tile.chrome) tile.chrome.Opacity(1.0f);

            const auto was = current.find(tile.item);
            if (was != current.end()) tile.sourceRect = was->second;
        }
        StartReveal(compositor, other,
                    std::chrono::milliseconds(config::Current().missionRevealMs), false);
    }

    MACTAB_DIAG("mission: pile of app %d collapsed", group);
}

// --- dragging ---------------------------------------------------------------
//
// A window can be picked up and put on another display, or on another desktop
// in the strip. It is the one gesture where a tile leaves the arrangement, so
// the tile itself is moved and everything else is left alone until the drop.

void Mission::Impl::BeginDrag(Screen& screen, POINT client) {
    drag = Drag{};

    const int tile = HitTestTile(screen, client);
    if (tile < 0) return;

    drag.screen = screen.hwnd;
    drag.tile   = tile;
    drag.grab   = client;
    drag.origin = POINT{ screen.tiles[static_cast<size_t>(tile)].liveRect.left,
                         screen.tiles[static_cast<size_t>(tile)].liveRect.top };

    // Capture, so the pointer can leave this overlay and land on another
    // display's without the moves stopping.
    ::SetCapture(screen.hwnd);
}

void Mission::Impl::UpdateDrag(Screen& screen, POINT client) {
    if (drag.tile < 0 || drag.tile >= static_cast<int>(screen.tiles.size())) return;

    const int dx = client.x - drag.grab.x;
    const int dy = client.y - drag.grab.y;

    // A threshold, so a click with a shaky hand is still a click.
    if (!drag.moving) {
        const int slack = static_cast<int>(screen.Scaled(6.0f));
        if (dx * dx + dy * dy < slack * slack) return;
        drag.moving = true;
        screen.outline.Opacity(0.0f);
    }

    Tile& tile = screen.tiles[static_cast<size_t>(drag.tile)];
    tile.holder.Offset({ static_cast<float>(drag.origin.x + dx),
                         static_cast<float>(drag.origin.y + dy), 0.0f });
    if (tile.chrome) tile.chrome.Opacity(0.0f);
}

bool Mission::Impl::FinishDrag(Screen& screen, POINT client) {
    const Drag held = drag;
    drag = Drag{};

    if (held.screen) ::ReleaseCapture();
    if (!held.moving || held.tile < 0 ||
        held.tile >= static_cast<int>(screen.tiles.size()))
        return false;

    Tile& tile = screen.tiles[static_cast<size_t>(held.tile)];
    if (tile.item < 0 || tile.item >= static_cast<int>(items.size())) return true;

    MissionItem& item = items[static_cast<size_t>(tile.item)];

    // Where the drop landed, in virtual-screen coordinates. The pointer is
    // captured, so it can be well outside this overlay by now.
    const POINT dropped{ client.x + screen.rect.left, client.y + screen.rect.top };

    // Dropped on a desktop in the strip.
    for (const mission::SpaceChip& chip : screen.chips) {
        if (chip.add || chip.index < 0 || chip.index >= static_cast<int>(spaces.size()))
            continue;
        if (client.x < chip.x || client.x >= chip.x + chip.w ||
            client.y < chip.y || client.y >= chip.y + chip.h)
            continue;

        if (desktops::MoveWindowTo(item.hwnd, spaces[static_cast<size_t>(chip.index)].id)) {
            item.desktop = chip.index;
            MACTAB_DIAG("mission: moved %p to desktop %d",
                        static_cast<void*>(item.hwnd), chip.index);
        } else {
            // Not allowed for another process's window, and there is no public
            // way round it. The arrangement snapping back is the honest answer:
            // nothing happened, and nothing pretends it did.
            MACTAB_WARN("mission: Windows does not allow moving another "
                        "application's window between desktops");
        }
        Rearrange(browsed);
        return true;
    }

    // Dropped on another display.
    const HMONITOR target = ::MonitorFromPoint(dropped, MONITOR_DEFAULTTONEAREST);
    if (target && target != screen.monitor) {
        MONITORINFO from{}, to{};
        from.cbSize = sizeof(from);
        to.cbSize   = sizeof(to);

        if (::GetMonitorInfoW(screen.monitor, &from) && ::GetMonitorInfoW(target, &to)) {
            // Kept where it was on its old screen in proportion, rather than at
            // the same pixel offset: two displays are rarely the same size, and
            // a window three quarters of the way across a wide screen belongs
            // three quarters of the way across the narrow one.
            const float fw = static_cast<float>(from.rcWork.right - from.rcWork.left);
            const float fh = static_cast<float>(from.rcWork.bottom - from.rcWork.top);
            const float tw = static_cast<float>(to.rcWork.right - to.rcWork.left);
            const float th = static_cast<float>(to.rcWork.bottom - to.rcWork.top);

            const float u = (item.bounds.left - from.rcWork.left) / (std::max)(1.0f, fw);
            const float v = (item.bounds.top  - from.rcWork.top)  / (std::max)(1.0f, fh);

            LONG w = item.bounds.right - item.bounds.left;
            LONG h = item.bounds.bottom - item.bounds.top;
            w = (std::min)(w, static_cast<LONG>(tw));
            h = (std::min)(h, static_cast<LONG>(th));

            LONG x = to.rcWork.left + static_cast<LONG>(u * tw);
            LONG y = to.rcWork.top  + static_cast<LONG>(v * th);
            x = (std::min)(x, static_cast<LONG>(to.rcWork.right)  - w);
            y = (std::min)(y, static_cast<LONG>(to.rcWork.bottom) - h);
            x = (std::max)(x, static_cast<LONG>(to.rcWork.left));
            y = (std::max)(y, static_cast<LONG>(to.rcWork.top));

            // A maximised window has to be restored first or it snaps straight
            // back to the display it was maximised on.
            WINDOWPLACEMENT placement{};
            placement.length = sizeof(placement);
            const bool maximised = ::GetWindowPlacement(item.hwnd, &placement) &&
                                   placement.showCmd == SW_SHOWMAXIMIZED;
            if (maximised) ::ShowWindow(item.hwnd, SW_RESTORE);

            ::SetWindowPos(item.hwnd, nullptr, x, y, w, h,
                           SWP_NOACTIVATE | SWP_NOZORDER);
            if (maximised) ::ShowWindow(item.hwnd, SW_SHOWMAXIMIZED);

            RECT moved{};
            if (SUCCEEDED(::DwmGetWindowAttribute(item.hwnd, DWMWA_EXTENDED_FRAME_BOUNDS,
                                                  &moved, sizeof(moved))) ||
                ::GetWindowRect(item.hwnd, &moved))
                item.bounds = moved;

            MACTAB_DIAG("mission: moved %p to another display",
                        static_cast<void*>(item.hwnd));
        }
    }

    Rearrange(browsed);
    return true;
}

// Lay everything out again and fly it from wherever it currently is.
//
// Keyed by window handle rather than by index into `items`, which matters as
// soon as anything can remove an item: a closed window shifts every index after
// it, and every window past the gap would then fly in from the position of its
// neighbour.
void Mission::Impl::Rearrange(int desktop) {
    // Keyed on the handle each tile carries, not on its index into `items`.
    //
    // The index is the obvious thing to reach for and it is wrong wherever this
    // is worth having: a removed window shifts every index after it, and a
    // rebuild replaces the list outright, so looking the old tile's index up in
    // the new list pairs each surviving window with its neighbour's position.
    // Every window past the gap would then fly in from next door.
    std::map<HWND, RECT> current;
    for (const Screen& other : screens)
        for (const Tile& tile : other.tiles)
            if (tile.window) current.emplace(tile.window, tile.liveRect);

    BuildForDesktop(desktop, 0);

    for (Screen& other : screens) {
        for (Tile& tile : other.tiles) {
            tile.holder.Opacity(1.0f);
            if (tile.chrome) tile.chrome.Opacity(1.0f);

            const auto was = current.find(tile.window);
            if (was != current.end()) tile.sourceRect = was->second;
        }
        StartReveal(compositor, other,
                    std::chrono::milliseconds(config::Current().missionRevealMs), false);
    }

    ScheduleSharpen();
}

void Mission::Show(std::vector<MissionItem> items, std::vector<MissionSpace> spaces,
                   int desktop) {
    Impl& impl = *m_impl;

    // A collapse still playing is finished at once rather than raced with.
    if (impl.closing) impl.FinishHide();

    if (impl.visible || impl.screens.empty()) return;

    const bool ok = GuardMission(impl, "Show", [&] {
        MACTAB_DIAG_TIMER("mission: Show");

        impl.items  = std::move(items);
        impl.spaces = std::move(spaces);

        // Re-made every time it opens, not only when the appearance flips, for
        // the reason in Panel::SetItems: the material inside it comes from
        // config, and reloading settings.ini changes the material while leaving
        // the appearance alone. The baked surfaces are dropped on a flip only,
        // because a reload already drops them through InvalidateBackdrop.
        const bool light = ResolveLightTheme();
        if (light != impl.themeIsLight) {
            impl.themeIsLight = light;
            for (Impl::Screen& screen : impl.screens) {
                screen.backdropSurface = nullptr;
                screen.barGlassSurface = nullptr;
            }
        }
        impl.theme = MakeTheme(light);

        // Move the overlays onto the desktop being shown.
        //
        // Not cosmetic, and not obvious. These windows are created once, at
        // initialisation, and a top-level window keeps whichever virtual desktop
        // it was created on for life. SetForegroundWindow on a window belonging
        // to another desktop makes Windows switch to that desktop to show it,
        // so once the user has left the desktop MacTab started on, opening
        // Mission Control would drag them back to it.
        //
        // The public API refuses this for other processes' windows and permits
        // it for our own, which is exactly the case here.
        if (desktop >= 0 && desktop < static_cast<int>(impl.spaces.size())) {
            const GUID& here = impl.spaces[static_cast<size_t>(desktop)].id;
            for (Impl::Screen& screen : impl.screens)
                desktops::MoveWindowTo(screen.hwnd, here);
        }

        impl.BuildForDesktop(desktop, 0);

        impl.restoreWindow = ::GetForegroundWindow();

        for (Impl::Screen& screen : impl.screens)
            ::SetWindowPos(screen.hwnd, HWND_TOPMOST,
                           screen.rect.left, screen.rect.top,
                           screen.rect.right - screen.rect.left,
                           screen.rect.bottom - screen.rect.top,
                           SWP_NOACTIVATE | SWP_SHOWWINDOW);

        // Focus goes to the display the user was already on. The others stay
        // clickable, and clicking one moves activation between two windows of
        // this process, which is exactly why dismissal checks whether the window
        // taking focus is one of ours.
        const HMONITOR active = impl.restoreWindow
            ? ::MonitorFromWindow(impl.restoreWindow, MONITOR_DEFAULTTOPRIMARY)
            : ::MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);

        HWND focus = impl.screens.front().hwnd;
        for (const Impl::Screen& screen : impl.screens)
            if (screen.monitor == active) focus = screen.hwnd;

        ::SetForegroundWindow(focus);
        ::SetFocus(focus);

        const auto duration = std::chrono::milliseconds(config::Current().missionRevealMs);
        for (Impl::Screen& screen : impl.screens)
            StartReveal(impl.compositor, screen, duration, true);
        impl.ScheduleSharpen();

        impl.visible = true;
    });

    if (!ok) {
        MACTAB_FAIL("mission: Show failed; hiding again");
        Hide();
    }
}

int Mission::BrowsedDesktop() const { return m_impl->browsed; }

// Look at another desktop from inside Mission Control.
//
// The desktop is NOT switched. Windows on another desktop are shell-cloaked but
// they are still enumerable and still have geometry, so the arrangement can be
// built for any of them, and activating one of those windows is what actually
// takes you there, which Windows does as part of the activation. Switching for
// real here would mean either losing the overlay, because it belongs to the
// desktop it was made on, or moving it, and either way the user would arrive
// somewhere instead of looking somewhere.
void Mission::BrowseDesktop(int index) {
    Impl& impl = *m_impl;
    if (!impl.visible || impl.closing) return;
    if (index < 0 || index >= static_cast<int>(impl.spaces.size())) return;
    if (index == impl.browsed) return;

    GuardMission(impl, "BrowseDesktop", [&] {
        const int direction = (index > impl.browsed) ? 1 : -1;
        const auto duration = std::chrono::milliseconds(config::Current().missionRevealMs);

        // The incoming arrangement arrives from the side you are travelling
        // towards, over a backdrop and a bar that never move. The outgoing one
        // is not slid out with it: the rebuild throws those visuals away, and
        // keeping them alive through the swap is a lot of machinery for a
        // quarter of a second nobody is looking at.
        for (Impl::Screen& screen : impl.screens)
            screen.outline.Opacity(0.0f);

        impl.BuildForDesktop(index, direction);

        for (Impl::Screen& screen : impl.screens) {
            screen.chromeLayer.Opacity(0.0f);
            StartReveal(impl.compositor, screen, duration, false);
        }

        impl.ScheduleSharpen();

        MACTAB_DIAG("mission: browsing desktop %d", index);
    });
}

void Mission::BeginDesktopChurn() {
    // Set BEFORE the shortcut is injected, not only after.
    //
    // The message loop does not run while we wait for the shell, so the
    // deactivations the switch produces queue up and are delivered in a batch
    // afterwards. Squelching only on the way back out works by that argument
    // alone, which is a thin thing to rely on for the difference between the
    // overlay staying up and it closing the moment a desktop is added.
    m_impl->ignoreFocusUntil = ::GetTickCount() + 3000;
}

void Mission::FollowDesktop(const GUID& desktop) {
    Impl& impl = *m_impl;
    if (!impl.visible) return;

    // Creating or closing a desktop moves the VIEW, and these windows belong to
    // whichever desktop they were last assigned to, so without this the overlay
    // is left cloaked on a desktop nobody is looking at while the machine sits
    // on the new one with nothing on screen.
    // Everything the switch made Windows say about our activation is now stale,
    // and it has been queued rather than delivered, because the message loop was
    // not running while we waited for the shell.
    impl.ignoreFocusUntil = ::GetTickCount() + 800;

    for (Impl::Screen& screen : impl.screens) {
        desktops::MoveWindowTo(screen.hwnd, desktop);
        ::SetWindowPos(screen.hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                       SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

    // Allowed because this process produced the last input event, which is one
    // of the conditions under which foreground may be taken. That was true of
    // the chord that changed the desktop, but the shell has taken foreground
    // since, so it is made true again rather than assumed. Without it, coming
    // back is a taskbar flash and the overlay is on screen with no keyboard.
    hotkey::QualifyForeground();

    if (!impl.screens.empty()) {
        ::SetForegroundWindow(impl.screens.front().hwnd);
        ::SetFocus(impl.screens.front().hwnd);
    }
}

void Mission::Rebuild(std::vector<MissionItem> items, std::vector<MissionSpace> spaces,
                      int desktop) {
    Impl& impl = *m_impl;
    if (!impl.visible || impl.closing) return;

    GuardMission(impl, "Rebuild", [&] {
        impl.items  = std::move(items);
        impl.spaces = std::move(spaces);

        // The uploads are keyed by application and the applications may have
        // changed underneath us. Cheap to redo; wrong to keep.
        impl.iconBitmaps.clear();

        // A dragged tile no longer refers to anything.
        impl.drag = Impl::Drag{};

        const int last   = static_cast<int>(impl.spaces.size()) - 1;
        const int target = (last < 0) ? 0 : (std::max)(0, (std::min)(last, desktop));

        impl.Rearrange(target);
    });
}

bool Mission::ForgetWindow(HWND hwnd) {
    Impl& impl = *m_impl;
    if (!impl.visible || impl.closing || !hwnd) return false;

    const auto gone = std::find_if(impl.items.begin(), impl.items.end(),
                                   [hwnd](const MissionItem& item) {
                                       return item.hwnd == hwnd;
                                   });
    if (gone == impl.items.end()) return false;

    // If it was the one under the pointer, the drag has nothing left to hold.
    if (impl.drag.tile >= 0) {
        ::ReleaseCapture();
        impl.drag = Impl::Drag{};
    }

    impl.items.erase(gone);

    // Rebuilt rather than deleted from the tree. The arrangement is a
    // position-preserving relaxation, so taking one window out legitimately
    // moves every other one, and the windows gliding into the space left behind
    // is both the correct result and the one that reads as something happening.
    GuardMission(impl, "ForgetWindow", [&] { impl.Rearrange(impl.browsed); });
    return true;
}

std::vector<HWND> Mission::Windows() const {
    std::vector<HWND> handles;
    handles.reserve(m_impl->items.size());
    for (const MissionItem& item : m_impl->items) handles.push_back(item.hwnd);
    return handles;
}

// Everything that actually takes the overlay off the screen.
//
// Split out of Hide because the windows fly back to where they came from first,
// and that takes as long as the reveal did. Safe to call twice.
void Mission::Impl::FinishHide() {
    // No early return on "not visible".
    //
    // The path that needs this most is the one where Show threw after the
    // windows were already on screen but before `visible` was set, which is
    // exactly when a device is lost. Bailing there left full-screen topmost
    // windows up with nothing able to take them down again, since Visible() was
    // false and every caller checks it. Doing the work twice costs nothing.
    if (!screens.empty()) {
        if (closeTimer) ::KillTimer(screens.front().hwnd, closeTimer);
        ::KillTimer(screens.front().hwnd, kSharpenTimerId);
    }
    closeTimer = 0;

    visible = false;
    closing = false;
    ignoreFocusUntil = 0;

    for (Screen& screen : screens) {
        GuardMission(*this, "Hide", [&] {
            screen.root.Opacity(0.0f);
            screen.outline.Opacity(0.0f);
            RestBar(screen);
        });
        ::ShowWindow(screen.hwnd, SW_HIDE);

        // Released immediately rather than at the next invocation. Each
        // thumbnail is a registration DWM holds on our behalf, and the budget
        // for this process while nothing is happening is zero.
        GuardMission(*this, "ReleaseTiles", [&] { ReleaseTiles(screen); });
    }

    items.clear();
    spaces.clear();

    // The uploaded icons go with them. They exist so an app's five windows share
    // one upload within a single invocation, which is where the saving is;
    // keeping them across a session that opens and closes hundreds of
    // applications would grow without a bound.
    iconBitmaps.clear();

    if (restoreOnHide && restoreWindow && ::IsWindow(restoreWindow))
        ::SetForegroundWindow(restoreWindow);

    restoreWindow = nullptr;
}

// Put every window back where it really is, then leave.
//
// The reveal lifts the windows off the desktop; the dismissal has to lower them
// onto it again, or the whole gesture is half an animation. `immediate` is for
// the paths that cannot wait: a desktop switch is about to run its own animation
// underneath, and an overlay still on screen while the desktop slides under it
// looks like a fault.
void Mission::Hide(bool restoreFocus, bool immediate) {
    Impl& impl = *m_impl;
    if (!impl.visible && impl.screens.empty()) return;

    impl.restoreOnHide = restoreFocus;

    const auto duration = std::chrono::milliseconds(config::Current().missionRevealMs);

    if (!immediate && impl.visible && !impl.closing && !impl.screens.empty()) {
        impl.closing = true;

        const bool started = GuardMission(impl, "Collapse", [&] {
            auto easing = impl.compositor.CreateCubicBezierEasingFunction(
                { 0.4f, 0.0f }, { 0.2f, 1.0f });

            const auto lift = [&](WUC::SpriteVisual& visual, float restX, float restY,
                                  float barH) {
                if (!visual || barH <= 0.0f) return;
                auto up = impl.compositor.CreateVector3KeyFrameAnimation();
                up.InsertKeyFrame(1.0f, { restX, restY - barH, 0.0f }, easing);
                up.Duration(duration);
                visual.StartAnimation(L"Offset", up);
            };

            for (Impl::Screen& screen : impl.screens) {
                const float barH = screen.Scaled(kBarHeight);
                lift(screen.barGlass, -screen.barOverhang, -screen.barOverhang, barH);
                lift(screen.bar, 0.0f, 0.0f, barH);

                screen.outline.Opacity(0.0f);
                screen.chromeLayer.StartAnimation(
                    L"Opacity", [&] {
                        auto fade = impl.compositor.CreateScalarKeyFrameAnimation();
                        fade.InsertKeyFrame(0.0f, 1.0f);
                        fade.InsertKeyFrame(0.35f, 0.0f, easing);
                        fade.Duration(duration);
                        return fade;
                    }());

                for (Impl::Tile& tile : screen.tiles) {
                    const float w = static_cast<float>(tile.liveRect.right - tile.liveRect.left);
                    const float h = static_cast<float>(tile.liveRect.bottom - tile.liveRect.top);
                    if (w <= 0.0f || h <= 0.0f) continue;

                    auto offset = impl.compositor.CreateVector3KeyFrameAnimation();
                    offset.InsertKeyFrame(1.0f,
                        { static_cast<float>(tile.homeRect.left),
                          static_cast<float>(tile.homeRect.top), 0.0f }, easing);
                    offset.Duration(duration);
                    tile.holder.StartAnimation(L"Offset", offset);

                    const float baseW = (std::max)(1.0f, tile.baseW);
                    const float baseH = (std::max)(1.0f, tile.baseH);

                    auto scale = impl.compositor.CreateVector3KeyFrameAnimation();
                    scale.InsertKeyFrame(1.0f,
                        { (tile.homeRect.right - tile.homeRect.left) / baseW,
                          (tile.homeRect.bottom - tile.homeRect.top) / baseH,
                          1.0f }, easing);
                    scale.Duration(duration);
                    tile.holder.StartAnimation(L"Scale", scale);
                }

                auto fade = impl.compositor.CreateScalarKeyFrameAnimation();
                fade.InsertKeyFrame(0.55f, 1.0f);
                fade.InsertKeyFrame(1.0f, 0.0f, easing);
                fade.Duration(duration);
                screen.root.StartAnimation(L"Opacity", fade);
            }
        });

        // The timer is what eventually hides the windows, so a failure to set
        // one is not something to shrug at: fall straight through to the
        // immediate path rather than leaving a full-screen overlay up.
        if (started) {
            impl.closeTimer = ::SetTimer(impl.screens.front().hwnd, kCloseTimerId,
                                         config::Current().missionRevealMs + 30, nullptr);
            if (impl.closeTimer) return;
            MACTAB_WARN("mission: no timer for the collapse; hiding at once");
        }

        impl.closing = false;
    }

    impl.FinishHide();
}

void Mission::HideNow() { m_impl->FinishHide(); }

void Mission::UpdateIcon(const std::wstring& appKey, const Bitmap& icon) {
    Impl& impl = *m_impl;
    if (!impl.visible || icon.Empty()) return;

    GuardMission(impl, "UpdateIcon", [&] {
        bool touched = false;
        for (MissionItem& item : impl.items) {
            if (item.appKey != appKey) continue;
            item.icon = icon;
            touched = true;
        }
        if (!touched) return;

        // The upload is keyed by app, so dropping the stale one is what makes
        // the next draw pick up the new artwork.
        impl.iconBitmaps.erase(appKey);

        for (Impl::Screen& screen : impl.screens)
            for (Impl::Tile& tile : screen.tiles) {
                if (tile.depth != 0 || tile.item < 0 ||
                    tile.item >= static_cast<int>(impl.items.size()))
                    continue;
                if (impl.items[static_cast<size_t>(tile.item)].appKey != appKey) continue;
                impl.BakeChrome(screen, tile);
            }
    });
}

} // namespace mactab
