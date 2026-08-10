#pragma once

// Mission Control's arrangement, deliberately free of windows.h.
//
// Same reason panel_layout.h is: this is a shape that has to be looked at to be
// judged, and it cannot be looked at on the machine it is written on. Keeping
// the arithmetic here means tools/preview can run the real algorithm natively,
// draw the result to a PNG, and assert on the numbers, which is the only
// verification available before a Windows machine sees it.
//
// WHAT MISSION CONTROL ACTUALLY DOES, and what it does not do.
//
// It does not tile. It does not normalise every window to a common size, and it
// does not stretch anything to fill a row. A large window still looks large next
// to a small one after the arrangement, which means there is ONE scale factor
// shared by every window, not one per row and not one per window. That single
// fact rules out the justified-gallery layout that this otherwise resembles, and
// it is the difference between an arrangement that reads as "my desktop, pulled
// apart" and one that reads as a photo grid.
//
// So the algorithm is: pick the largest scale at which everything still fits,
// then pack rows greedily at that scale.
//
//   1. Order the windows roughly as they sit on screen, reading order.
//   2. For a trial scale, walk the order filling rows left to right, starting a
//      new row when the next window would not fit the width.
//   3. That gives a total height. If it overflows, the scale was too big.
//   4. Binary search the scale for the largest one that fits, capped at 1.0
//      because nothing is ever shown larger than it really is.
//
// The ordering step matters more than it looks. Packing in an arbitrary order
// still produces a tidy arrangement, but windows land nowhere near where they
// were, and the whole point of the gesture is that you already know where you
// left the thing you are looking for. Sorting by y and then x is the obvious
// approach and it is wrong: two windows side by side almost never share an exact
// y, so a plain y sort interleaves left and right columns. They have to be
// banded into rows first, and the number of bands worth using is the number of
// rows the packing produces, which is not known until after packing. Hence two
// passes: pack once to learn the row count, band by that count, pack again.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace mactab::mission {

// Logical pixels at 96 DPI. Provisional until measured against a real
// screenshot of macOS 26; they are here rather than inline so that the
// measurement, when it happens, changes one place.
inline constexpr float kTileGapX = 28.0f;
inline constexpr float kTileGapY = 44.0f;

// Never enlarge. A 400x300 window is a small tile in Mission Control, and
// blowing it up to fill space would break the "this is your desktop" reading.
inline constexpr float kMaxScale = 1.0f;

// Below this there is nothing left to recognise. Reached only with a very large
// number of windows, and at that point macOS is unreadable too, so this exists
// to stop the search rather than to be hit.
inline constexpr float kMinScale = 0.04f;

struct Params {
    float gapX     = kTileGapX;
    float gapY     = kTileGapY;
    float maxScale = kMaxScale;
    float minScale = kMinScale;

    // Windows of the same application are kept adjacent in the order, which is
    // what macOS 26 does by default ("Group windows by application"). Off, the
    // order is purely positional.
    bool  groupByApp = true;
};

// One window, as it actually sits on the desktop.
//
// Coordinates are whatever the caller is working in, as long as they are
// consistent with the region passed to Layout: the algorithm is scale free. The
// panel uses physical pixels relative to the monitor.
struct Window {
    float x = 0.0f, y = 0.0f;      // top left on screen
    float w = 1.0f, h = 1.0f;      // size on screen

    // Grouping key, normally the index of the owning app in the app list. Only
    // compared for equality, never ordered on directly.
    int   group = 0;

    // Tie break, and the order groups appear in. Most recently used first, so
    // the app you were just in leads.
    int   order = 0;

    float CentreX() const { return x + w * 0.5f; }
    float CentreY() const { return y + h * 0.5f; }
    float Aspect()  const { return h > 0.0f ? w / h : 1.0f; }
};

struct Placement {
    float x = 0.0f, y = 0.0f;      // top left within the region
    float w = 0.0f, h = 0.0f;
    int   row    = 0;
    int   source = 0;              // index into the input vector
};

struct Result {
    std::vector<Placement> tiles;  // in input order, not layout order
    float scale    = 1.0f;
    int   rows     = 0;
    float contentW = 0.0f;         // bounding box of the arrangement
    float contentH = 0.0f;
};

namespace detail {

// Where each row starts and ends in the ordered sequence, plus its extent at
// the trial scale.
struct Row {
    size_t begin  = 0;
    size_t end    = 0;             // exclusive
    float  width  = 0.0f;
    float  height = 0.0f;
};

// Greedy left-to-right fill at a fixed scale.
//
// Greedy rather than an optimal partition on purpose. An optimal packer would
// balance the rows, which looks tidier and is exactly wrong here: balancing
// moves windows out of the row they visually belong to in order to even up the
// line lengths, and the arrangement stops matching the desktop it came from. A
// ragged last row is what macOS produces too.
inline std::vector<Row> PackRows(const std::vector<const Window*>& ordered,
                                 float scale, float regionW, const Params& p) {
    std::vector<Row> rows;
    if (ordered.empty()) return rows;

    Row row;
    row.begin = 0;

    for (size_t i = 0; i < ordered.size(); ++i) {
        const float w = ordered[i]->w * scale;
        const float h = ordered[i]->h * scale;

        // Width this row would have if the window joined it.
        const bool  empty = (i == row.begin);
        const float grown = empty ? w : row.width + p.gapX + w;

        if (!empty && grown > regionW) {
            row.end = i;
            rows.push_back(row);

            row        = Row{};
            row.begin  = i;
            row.width  = w;
            row.height = h;
        } else {
            row.width  = grown;
            row.height = (std::max)(row.height, h);
        }
    }

    row.end = ordered.size();
    rows.push_back(row);
    return rows;
}

inline float TotalHeight(const std::vector<Row>& rows, const Params& p) {
    if (rows.empty()) return 0.0f;
    float total = p.gapY * static_cast<float>(rows.size() - 1);
    for (const Row& r : rows) total += r.height;
    return total;
}

inline float WidestRow(const std::vector<Row>& rows) {
    float widest = 0.0f;
    for (const Row& r : rows) widest = (std::max)(widest, r.width);
    return widest;
}

// A trial scale is acceptable when the packing fits the region in both axes.
//
// The width check is not redundant with the packer. The packer will place a
// window that is wider than the whole region on a row of its own rather than
// loop forever, so a single oversized window can only be caught here.
inline bool Fits(const std::vector<Row>& rows, const Params& p,
                 float regionW, float regionH) {
    return WidestRow(rows) <= regionW + 0.01f &&
           TotalHeight(rows, p) <= regionH + 0.01f;
}

} // namespace detail

// The order windows are packed in.
//
// `bands` is how many horizontal bands the screen is cut into before sorting,
// which should be the number of rows the arrangement will end up with. Pass 1
// for the provisional pass, where the row count is not known yet.
inline std::vector<const Window*> OrderWindows(const std::vector<Window>& windows,
                                               int bands, const Params& p) {
    std::vector<const Window*> ordered;
    ordered.reserve(windows.size());
    for (const Window& w : windows) ordered.push_back(&w);
    if (ordered.size() < 2) return ordered;

    // Band boundaries come from the spread of the window centres, not from the
    // screen. A desktop where everything sits in the top half should still be
    // cut into distinct rows rather than collapsing into band 0.
    float minY = ordered.front()->CentreY();
    float maxY = minY;
    for (const Window* w : ordered) {
        minY = (std::min)(minY, w->CentreY());
        maxY = (std::max)(maxY, w->CentreY());
    }

    const int   bandCount = (std::max)(1, bands);
    const float span      = (std::max)(1.0f, maxY - minY);
    const float bandSize  = span / static_cast<float>(bandCount);

    auto bandOf = [&](const Window* w) {
        const int b = static_cast<int>((w->CentreY() - minY) / bandSize);
        return (std::min)(bandCount - 1, (std::max)(0, b));
    };

    if (!p.groupByApp) {
        // Reading order: down the bands, left to right inside each.
        //
        // The trailing keys are not decoration. std::sort needs a strict weak
        // ordering and two windows can genuinely share a band and a centre, so
        // the comparator has to bottom out somewhere deterministic or the
        // arrangement changes between runs on identical input.
        std::sort(ordered.begin(), ordered.end(),
                  [&](const Window* a, const Window* b) {
                      const int ba = bandOf(a), bb = bandOf(b);
                      if (ba != bb) return ba < bb;
                      if (a->CentreX() != b->CentreX()) return a->CentreX() < b->CentreX();
                      if (a->CentreY() != b->CentreY()) return a->CentreY() < b->CentreY();
                      return a->order < b->order;
                  });
        return ordered;
    }

    // Grouped: applications appear in most-recently-used order, and an app's
    // windows stay together. Within an app, positional order again, so a
    // three-window app still reads left to right.
    //
    // The group's position is its best (lowest) order value rather than its
    // first window's, because the input is not required to be sorted.
    std::vector<std::pair<int, int>> best;   // group, best order
    for (const Window* w : ordered) {
        auto it = std::find_if(best.begin(), best.end(),
                               [&](const std::pair<int, int>& e) { return e.first == w->group; });
        if (it == best.end()) best.emplace_back(w->group, w->order);
        else it->second = (std::min)(it->second, w->order);
    }

    auto rankOf = [&](int group) {
        for (size_t i = 0; i < best.size(); ++i)
            if (best[i].first == group) return best[i].second;
        return 0;
    };

    std::sort(ordered.begin(), ordered.end(),
              [&](const Window* a, const Window* b) {
                  const int ra = rankOf(a->group), rb = rankOf(b->group);
                  if (ra != rb) return ra < rb;
                  if (a->group != b->group) return a->group < b->group;
                  const int ba = bandOf(a), bb = bandOf(b);
                  if (ba != bb) return ba < bb;
                  if (a->CentreX() != b->CentreX()) return a->CentreX() < b->CentreX();
                  if (a->CentreY() != b->CentreY()) return a->CentreY() < b->CentreY();
                  return a->order < b->order;
              });
    return ordered;
}

// Arrange `windows` into a region `regionW` by `regionH`.
//
// The region is the area left for tiles: the caller has already taken the
// spaces strip off the top and the outer margin off every side. Output
// coordinates are relative to the region's top left corner.
inline Result Layout(const std::vector<Window>& windows,
                     float regionW, float regionH,
                     const Params& p = Params{}) {
    Result result;
    if (windows.empty() || regionW <= 0.0f || regionH <= 0.0f) return result;

    auto search = [&](const std::vector<const Window*>& ordered) {
        // Largest scale that fits.
        //
        // The packing is a step function of the scale, so this is monotone in
        // the ordinary case but not provably so at every step boundary. The
        // walk-down afterwards is what makes an overflow impossible rather than
        // unlikely, and it costs nothing when the search was already right.
        float lo = p.minScale, hi = p.maxScale;
        for (int i = 0; i < 40; ++i) {
            const float mid = (lo + hi) * 0.5f;
            if (detail::Fits(detail::PackRows(ordered, mid, regionW, p), p, regionW, regionH))
                lo = mid;
            else
                hi = mid;
        }

        float scale = lo;
        for (int i = 0; i < 64; ++i) {
            if (detail::Fits(detail::PackRows(ordered, scale, regionW, p), p, regionW, regionH))
                break;
            scale *= 0.97f;
        }
        return (std::max)(p.minScale, scale);
    };

    // Pass one exists only to learn how many rows this set wants, so pass two
    // can band the screen by that count. Ordering by a wrong band count is what
    // scrambles left and right, and the count is not knowable before packing.
    std::vector<const Window*> ordered = OrderWindows(windows, 1, p);
    float scale = search(ordered);
    const int provisionalRows =
        static_cast<int>(detail::PackRows(ordered, scale, regionW, p).size());

    if (provisionalRows > 1) {
        ordered = OrderWindows(windows, provisionalRows, p);
        scale   = search(ordered);
    }

    const std::vector<detail::Row> rows = detail::PackRows(ordered, scale, regionW, p);

    result.scale    = scale;
    result.rows     = static_cast<int>(rows.size());
    result.contentW = detail::WidestRow(rows);
    result.contentH = detail::TotalHeight(rows, p);
    result.tiles.resize(windows.size());

    // Centre the block vertically, and every row within the block horizontally.
    float y = (regionH - result.contentH) * 0.5f;

    for (size_t r = 0; r < rows.size(); ++r) {
        const detail::Row& row = rows[r];
        float x = (regionW - row.width) * 0.5f;

        for (size_t i = row.begin; i < row.end; ++i) {
            const Window* w = ordered[i];
            const size_t  source = static_cast<size_t>(w - windows.data());

            Placement place;
            place.w      = w->w * scale;
            place.h      = w->h * scale;
            place.x      = x;
            // Windows in a row are centred on the row rather than sitting on a
            // shared baseline. A short window between two tall ones floats in
            // the middle of the gap, which is what the reference does.
            place.y      = y + (row.height - place.h) * 0.5f;
            place.row    = static_cast<int>(r);
            place.source = static_cast<int>(source);

            result.tiles[source] = place;
            x += place.w + p.gapX;
        }

        y += row.height + p.gapY;
    }

    return result;
}

} // namespace mactab::mission
