#pragma once

// Panel geometry, deliberately free of windows.h.
//
// These numbers are the difference between "looks like macOS" and "looks like a
// Windows app with a blurry background", and they were chosen without being able
// to see the result. Keeping the arithmetic here — rather than inline in
// panel.cpp — means tools/preview can render the real layout natively and the
// proportions can actually be looked at instead of guessed at.
#include <algorithm>
#include <cmath>

namespace mactab::layout {

// Logical pixels at 96 DPI.
inline constexpr float kTileSize     = 128.0f;
inline constexpr float kTileGap      = 8.0f;
inline constexpr float kPanelPadding = 20.0f;
inline constexpr float kPanelRadius  = 24.0f;   // NOT DWM's 8px; this is the point
inline constexpr float kLabelHeight  = 28.0f;

// Floor for the shrink-to-fit pass. Below this the icons stop being
// recognisable and a scrolling row would be better, but macOS never scrolls.
inline constexpr float kMinTileSize  = 40.0f;

// Highlight inset behind the selected tile, as a fraction of tile size.
inline constexpr float kSelectionInset = 0.06f;

struct Metrics {
    float tileSize    = kTileSize;
    float panelWidth  = 0.0f;
    float panelHeight = 0.0f;
    float padding     = kPanelPadding;
    float gap         = kTileGap;
    float radius      = kPanelRadius;
    float labelHeight = kLabelHeight;

    // Left edge of tile `index`, relative to the panel's origin.
    float TileX(int index) const {
        return padding + static_cast<float>(index) * (tileSize + gap);
    }
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
    m.tileSize    = baseTileSize  * dpiScale;

    if (count <= 0) {
        m.panelWidth  = m.padding * 2;
        m.panelHeight = m.padding * 2 + m.labelHeight;
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
    m.panelHeight = m.padding * 2 + m.tileSize + m.labelHeight;
    return m;
}

} // namespace mactab::layout
