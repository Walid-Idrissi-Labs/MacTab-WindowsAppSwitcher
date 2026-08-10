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
// recognisable and a scrolling row would be better, but macOS never scrolls.
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

    // Everything the window has to contain: the glass, the gap, the label.
    float TotalHeight() const { return panelHeight + labelGap + labelHeight; }

    // Left edge of tile `index`, relative to the panel's origin.
    float TileX(int index) const {
        return padding + static_cast<float>(index) * (tileSize + gap);
    }

    // Horizontal centre of tile `index`, which is where the label is anchored.
    float TileCentreX(int index) const { return TileX(index) + tileSize * 0.5f; }
};

// Lay out `count` tiles into at most `availableWidth` physical pixels.
//
// macOS shrinks the tiles to fit rather than wrapping to a second row or
// scrolling, so the panel is always a single strip however many apps are open.
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
        m.panelWidth  = m.padding * 2;
        m.panelHeight = m.padding * 2;
        m.radius      = (std::min)(m.radius, m.panelHeight * 0.5f);
        return m;
    }

    auto widthFor = [&](float tile) {
        return m.padding * 2 + tile * count + m.gap * (count - 1);
    };

    if (widthFor(m.tileSize) > availableWidth) {
        const float usable = availableWidth - m.padding * 2 - m.gap * (count - 1);
        m.tileSize = (std::max)(kMinTileSize * dpiScale, usable / count);
    }

    m.panelWidth  = (std::min)(widthFor(m.tileSize), availableWidth);
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
