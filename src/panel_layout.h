#pragma once

// Panel geometry, deliberately free of windows.h.
//
// These numbers are the difference between "looks like macOS" and "looks like a
// Windows app with a blurry background", and they were chosen without being able
// to see the result. Keeping the arithmetic here, rather than inline in
// panel.cpp, means tools/preview can render the real layout natively and the
// proportions can actually be looked at instead of guessed at.
#include <algorithm>
#include <cmath>

namespace mactab::layout {

// Where these numbers come from.
//
// I fitted the implicit superellipse |(Cx-x)/a|^n + |(Cy-y)/a|^n = 1 to a
// screenshot of the real macOS switcher, sweeping the shape origin, the corner
// extent a and the exponent n by least squares over boundary points recovered
// from the panel's specular rim. Two fits, both tight:
//
//   panel corner    a = 214, n = 2.24, residual 0.013 over 35 points
//                   on a panel measuring 1938 x 588
//   app icon corner a = 106, n = 2.46, residual 0.0014 over 35 points
//                   on an icon 348 wide
//
// Two things fall out of that. The panel corner is very nearly circular and
// only slightly squared, nothing like the n = 5 used for the icon tiles, and
// the panel radius follows Apple's concentric rule: the outer corner extent is
// the icon's corner extent plus the padding between them.
//
// Careful with the padding arithmetic. MakeIconTile draws the squircle at
// 824/1024 of the tile and centres it, so a 128px tile carries 12.5px of
// transparent margin per side and the visible shape is 103px. Every ratio taken
// off the screenshot is against the visible shape, so it has to be compared
// against 103, not 128.
//
//   visible padding  = kPanelPadding + 12.5 = 34.5   (0.335 of 103, macOS 0.346)
//   visible gap      = kTileGap      + 25.0 = 31.0   (0.301 of 103, macOS 0.286)
//   radius, two ways = 0.305 * 103 + 34.5   = 65.9
//                    = 214 * (103 / 350)    = 63.0
//
// 62 splits those and gives 62/172 = 0.36 against the panel height, which is
// what the screenshot measures (214/588 = 0.364).
//
// The padding is uniform on all four sides, deliberately. The reference panel is
// 588 tall around a 348 icon, so 120 above and 120 below: there is no taller
// bottom band. That means the app name cannot live inside the glass, and it is
// drawn below the panel instead, which is why kLabelHeight is not part of
// panelHeight any more.

// Logical pixels at 96 DPI.
inline constexpr float kTileSize     = 128.0f;
inline constexpr float kTileGap      = 6.0f;
inline constexpr float kPanelPadding = 22.0f;
inline constexpr float kPanelRadius  = 62.0f;   // NOT DWM's 8px; this is the point

// The app name, below the panel rather than inside it. kLabelGap is the space
// between the glass and the top of the text.
inline constexpr float kLabelHeight  = 28.0f;
inline constexpr float kLabelGap     = 10.0f;

// Superellipse exponent for the panel outline. Measured, see above. The icon
// tiles use 5, which is right for a shape whose corner extent is half its own
// width, and wrong here: at this radius n = 5 would hug the bounding box and
// read squarer than the reference, not rounder.
inline constexpr float kPanelCornerExponent = 2.24f;

// Floor for the shrink-to-fit pass. Below this the icons stop being
// recognisable, so the strip scrolls instead of shrinking any further.
//
// macOS never scrolls, and it never has to: its switcher lists applications and
// only applications, so the count is bounded by how many are running, and it has
// no floor to reach. One tile per window is ours, and the reference cannot say
// what should happen in a mode it does not have.
inline constexpr float kMinTileSize  = 40.0f;

// Highlight inset behind the selected tile, as a fraction of tile size.
inline constexpr float kSelectionInset = 0.06f;

struct Metrics {
    float tileSize    = kTileSize;
    float panelWidth  = 0.0f;
    float panelHeight = 0.0f;      // the glass only; the label sits below it
    float padding     = kPanelPadding;
    float gap         = kTileGap;
    float radius      = kPanelRadius;
    float labelHeight = kLabelHeight;
    float labelGap    = kLabelGap;

    // The whole strip, which is what panelWidth would be if the screen were
    // wide enough. Equal to panelWidth in every ordinary case; larger only when
    // the tiles have hit their floor and still do not fit, which is when the
    // strip scrolls under a panel that stays put.
    float contentWidth = 0.0f;

    // How many whole tiles the panel shows at once. Equal to the count asked
    // for unless the strip scrolls.
    int visibleCount = 0;

    // How far the strip can travel. Always a whole number of tile strides, so
    // a scrolled panel still begins and ends on a complete tile.
    float MaxScroll() const {
        const float slack = contentWidth - panelWidth;
        return slack > 0.0f ? slack : 0.0f;
    }

    // Left edge of tile `index`, relative to the panel's origin.
    float TileX(int index) const {
        return padding + static_cast<float>(index) * (tileSize + gap);
    }

    // Horizontal centre of tile `index`, which is where the label is anchored.
    float TileCentreX(int index) const { return TileX(index) + tileSize * 0.5f; }

    // Where the strip has to sit for tile `index` to be wholly inside the
    // panel, starting from wherever it sits now. Returns `current` unchanged
    // when the tile is already visible, so tabbing through the middle of a long
    // list does not drag the strip about.
    //
    // This is the invariant the whole scrolling design rests on: the selected
    // tile is always fully on screen, so there is no way to commit to an
    // application nobody can see. It lives here rather than in panel.cpp so
    // that tools/preview can assert it, which matters because panel.cpp cannot
    // be compiled anywhere except CI.
    //
    // Both bounds land on whole tile strides, and so does MaxScroll, so a
    // scrolled panel always begins and ends on a complete tile.
    float ScrollFor(int index, float current) const {
        const float limit = MaxScroll();
        if (limit <= 0.0f || index < 0) return 0.0f;

        const float stride = tileSize + gap;
        if (stride <= 0.0f) return 0.0f;

        // Snapped before anything else, so the answer rests on a tile boundary
        // whatever it was handed. In practice `current` is always a previous
        // answer and is already on one, but making that a property of the
        // function rather than of its callers is what stops the panel ever
        // coming to a stop showing two half tiles.
        float scroll = std::round(current / stride) * stride;

        const float lead  = static_cast<float>(index) * stride;
        const float trail = lead + stride - static_cast<float>(visibleCount) * stride;

        if (scroll > lead)  scroll = lead;    // the tile is off to the left
        if (scroll < trail) scroll = trail;   // the tile is off to the right

        if (scroll < 0.0f)  scroll = 0.0f;
        if (scroll > limit) scroll = limit;
        return scroll;
    }
};

// Lay out `count` tiles into at most `availableWidth` physical pixels.
//
// Always a single row: shrink the tiles to fit, and once they cannot shrink any
// further without becoming unrecognisable, hold as many whole tiles as the
// screen allows and let the strip scroll under a panel that does not move. No
// second row and no scrollbar, both of which the reference goes without.
inline Metrics Compute(int count, float availableWidth, float dpiScale,
                       float baseTileSize = kTileSize) {
    Metrics m;
    m.padding     = kPanelPadding * dpiScale;
    m.gap         = kTileGap      * dpiScale;
    m.radius      = kPanelRadius  * dpiScale;
    m.labelHeight = kLabelHeight  * dpiScale;
    m.labelGap    = kLabelGap     * dpiScale;
    m.tileSize    = baseTileSize  * dpiScale;

    if (count <= 0) {
        m.panelWidth   = m.padding * 2;
        m.contentWidth = m.panelWidth;
        m.panelHeight  = m.padding * 2;
        m.radius      = (std::min)(m.radius, m.panelHeight * 0.5f);
        return m;
    }

    // A caller that could not read its monitor hands us a negative width, and
    // everything downstream of that is nonsense: a negative panel width reaches
    // SetWindowPos, and the radius clamp below turns it into a negative corner.
    // One tile is the smallest thing worth drawing, so that is the floor.
    availableWidth = (std::max)(availableWidth,
                                kMinTileSize * dpiScale + m.padding * 2);

    auto widthFor = [&](float tile, int n) {
        return m.padding * 2 + tile * n + m.gap * (n - 1);
    };

    if (widthFor(m.tileSize, count) > availableWidth) {
        const float usable = availableWidth - m.padding * 2 - m.gap * (count - 1);
        m.tileSize = (std::max)(kMinTileSize * dpiScale, usable / count);
    }

    m.contentWidth = widthFor(m.tileSize, count);
    m.visibleCount = count;
    m.panelWidth   = m.contentWidth;

    // Past the floor they no longer all fit, however small they are made. The
    // panel then takes as many WHOLE tiles as the screen allows and the strip
    // scrolls underneath it.
    //
    // Whole tiles, not as many pixels as there is room for. A panel stretched
    // to the last pixel ends part way through an icon, and a sliced tile at the
    // glass edge reads as something drawn wrong rather than as a list that
    // continues. It also keeps the scroll a whole number of tile strides, so
    // the strip can never come to rest showing two half tiles.
    //
    // Before this, the panel was clamped to the screen while the tiles carried
    // on being placed at their own offsets, so everything past the edge was
    // simply drawn outside the window and thrown away by the compositor: no
    // tile, no selection highlight, and nothing to say why.
    if (m.contentWidth > availableWidth) {
        const float stride = m.tileSize + m.gap;

        // n tiles carry n-1 gaps, so the gap comes back before dividing.
        const float room = availableWidth - m.padding * 2 + m.gap;

        m.visibleCount = (std::max)(1, static_cast<int>(room / stride));
        m.panelWidth   = widthFor(m.tileSize, m.visibleCount);
    }

    m.panelHeight = m.padding * 2 + m.tileSize;   // uniform padding, no chin

    // A corner extent past half the shorter side is meaningless, and at
    // kMinTileSize the panel is short enough for 62 to reach it. Clamp once,
    // here, so the backdrop and the shadow behind it cannot end up with
    // different corners: they are drawn from separate surfaces and a mismatch
    // shows as a grey fringe outside the glass.
    m.radius = (std::min)(m.radius, (std::min)(m.panelWidth, m.panelHeight) * 0.5f);
    return m;
}

} // namespace mactab::layout
