#pragma once

// Mission Control's arrangement, deliberately free of windows.h.
//
// Same reason panel_layout.h is: the arrangement is a shape, shapes have to be
// looked at to be judged, and the machine this is written on cannot run
// Windows. So the real algorithm runs in tools/preview/mission, against
// synthetic desktops chosen to break it, and writes what it produced as a PNG
// next to a set of assertions about it.
//
// WHAT THIS IS NOT.
//
// The first version of this file packed windows into justified rows. It was
// tidy, it passed everything asserted about it, and it was the wrong shape.
// Mission Control does not produce rows. Windows land near where they already
// were, at irregular heights, with no shared baseline; that is why you can find
// a window in it by remembering where you left it. A row packer destroys
// exactly that, and what it leaves behind reads as a photo gallery. GNOME's
// Activities view and KWin's grid mode both make this trade and both look
// unmistakably not-macOS as a result.
//
// WHAT IT IS.
//
// A position-preserving proportional spread, the family KWin calls the natural
// layout. Every window starts exactly where it really is, then overlapping
// pairs shove each other apart along the line joining their centres until
// nothing overlaps, and the whole result is mapped into the screen with ONE
// uniform scale.
//
// The single scale is not an implementation convenience, it is the property.
// A large window still looks large next to a small one afterwards, and every
// aspect ratio is exact. Normalising sizes, per row or per window, is the other
// way this stops looking like macOS.
//
// Three holes in the naive version, all of which KDE hit and documented before
// giving up on it, and all of which are defended against below:
//
//   coincident centres  a stack of maximised windows has no direction to
//                       separate along. KWin nudges x by one pixel, which
//                       collapses the stack into a single horizontal line of
//                       slivers. Here the direction comes from a deterministic
//                       fan, so a stack blooms outward instead.
//   unbounded loop      the relaxation is not guaranteed to terminate, so it is
//                       capped, and hitting the cap falls back to a grid rather
//                       than shipping whatever the last iteration held.
//   nondeterminism      KWin iterates a hash keyed on pointers. Anything that
//                       reorders the input reorders the arrangement, which is
//                       invisible in one render and obvious the second time you
//                       open it. Everything here is ordered by the caller's
//                       index and the arithmetic is fixed-step.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace mactab::mission {

// Logical pixels at 96 DPI, in screen space, so these are the gaps as seen
// rather than as computed. Provisional until measured against a real screenshot
// of macOS 26.
inline constexpr float kWindowGap = 26.0f;

// Inside one app's cluster, and between one cluster and the next.
//
// The ratio is what makes grouping legible without drawing a box around
// anything: an app's windows sit closer to each other than to anything else, so
// the eye reads the cluster before it reads the windows. Equal gaps and the
// grouping is invisible however correct it is.
inline constexpr float kMemberGap  = 12.0f;
inline constexpr float kClusterGap = 88.0f;

// Never enlarge. A 400x300 window is a small tile in Mission Control, and
// blowing it up to fill the screen would break the "this is your desktop"
// reading that the whole gesture depends on.
inline constexpr float kMaxScale = 1.0f;

// Reached only with an absurd number of windows. Exists to stop the search
// rather than to be hit.
inline constexpr float kMinScale = 0.04f;

// The relaxation is not provably convergent, so it is bounded. 400 is far past
// what any real desktop needs; thirty overlapping windows settle in under
// forty.
inline constexpr int kMaxIterations = 400;

struct Params {
    float gap        = kWindowGap;
    float memberGap  = kMemberGap;
    float clusterGap = kClusterGap;
    float maxScale   = kMaxScale;
    float minScale   = kMinScale;
    int   iterations = kMaxIterations;

    // Windows of the same application are relaxed into a cluster first, and the
    // clusters are then relaxed against each other. Off, every window competes
    // with every other and only position matters.
    bool  groupByApp = true;
};

// One window, as it actually sits on the desktop.
//
// Coordinates are whatever the caller is working in, as long as they are
// consistent with the region passed to Layout: the algorithm is scale free.
struct Window {
    float x = 0.0f, y = 0.0f;      // top left on screen
    float w = 1.0f, h = 1.0f;      // size on screen

    // Grouping key, normally the index of the owning app in the app list. Only
    // ever compared for equality.
    int   group = 0;

    // Tie break, and the order clusters are seeded in. Most recently used
    // first.
    int   order = 0;

    float CentreX() const { return x + w * 0.5f; }
    float CentreY() const { return y + h * 0.5f; }
};

struct Placement {
    float x = 0.0f, y = 0.0f;      // top left within the region
    float w = 0.0f, h = 0.0f;
    int   source = 0;              // index into the input vector
};

// The area one application's windows ended up occupying, which is what the app
// icon and name are anchored under when grouping is on.
struct Cluster {
    int   group = 0;
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
};

struct Result {
    std::vector<Placement> tiles;  // in input order
    std::vector<Cluster>   clusters;   // empty when grouping is off
    float scale      = 1.0f;
    int   iterations = 0;          // how much shoving it took
    bool  relaxed    = true;       // false means the grid fallback was used
    float contentW   = 0.0f;       // bounding box of the arrangement, scaled
    float contentH   = 0.0f;
};

namespace detail {

struct Rect {
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;

    float CentreX() const { return x + w * 0.5f; }
    float CentreY() const { return y + h * 0.5f; }
    float Right()   const { return x + w; }
    float Bottom()  const { return y + h; }
};

inline Rect Union(const std::vector<Rect>& rects) {
    Rect bounds;
    if (rects.empty()) return bounds;

    float left = rects[0].x, top = rects[0].y;
    float right = rects[0].Right(), bottom = rects[0].Bottom();
    for (const Rect& r : rects) {
        left   = (std::min)(left,   r.x);
        top    = (std::min)(top,    r.y);
        right  = (std::max)(right,  r.Right());
        bottom = (std::max)(bottom, r.Bottom());
    }
    bounds.x = left;
    bounds.y = top;
    bounds.w = (std::max)(1.0f, right - left);
    bounds.h = (std::max)(1.0f, bottom - top);
    return bounds;
}

// How far apart two rects overlap, per axis, once both have been grown by half
// the gap, which is the same as requiring `gap` of clear space between them.
// Zero on either axis means they are already clear.
inline void Overlap(const Rect& a, const Rect& b, float gap,
                    float& overlapX, float& overlapY) {
    const float half = gap * 0.5f;
    overlapX = (std::min)(a.Right() + half, b.Right() + half) -
               (std::max)(a.x - half, b.x - half);
    overlapY = (std::min)(a.Bottom() + half, b.Bottom() + half) -
               (std::max)(a.y - half, b.y - half);
    if (overlapX <= 0.0f || overlapY <= 0.0f) { overlapX = 0.0f; overlapY = 0.0f; }
}

// Distance the pair must travel along the unit direction (nx, ny) to stop
// overlapping.
//
// Not the penetration depth. Two boxes overlapping by 10 pixels vertically,
// pushed apart along a direction 30 degrees off vertical, have to travel 20 to
// clear it, not 10. Using the depth instead means every pass makes partial
// progress, thirty windows never settle, and the whole thing falls out to the
// grid, which is exactly what happened the first time this was written.
inline float SeparationAlong(float overlapX, float overlapY, float nx, float ny) {
    float distance = 1e30f;
    if (std::fabs(nx) > 1e-4f) distance = (std::min)(distance, overlapX / std::fabs(nx));
    if (std::fabs(ny) > 1e-4f) distance = (std::min)(distance, overlapY / std::fabs(ny));
    return (distance > 1e29f) ? (std::max)(overlapX, overlapY) : distance;
}

// A separation direction that does not depend on the two rects, for the case
// where their centres coincide.
//
// The golden angle spreads successive indices as evenly as any sequence can, so
// a stack of ten identical maximised windows fans out into a disc rather than a
// line. KWin's one-pixel x nudge is what produces the line, and a line of ten
// maximised windows scales down to slivers.
inline void FanDirection(size_t index, float& dx, float& dy) {
    const float angle = static_cast<float>(index) * 2.39996323f;
    dx = std::cos(angle);
    dy = std::sin(angle);
}

// Shove overlapping rects apart in place. Returns the number of passes used;
// `settled` reports whether it actually finished.
//
// The push is half the penetration depth per rect rather than a fixed step, so
// a deep overlap resolves in a few passes instead of hundreds, and the
// direction still comes from the centre-to-centre line, which is what keeps the
// result organic rather than axis-aligned.
inline int Shove(std::vector<Rect>& rects, float gap, float regionAspect,
                 int maxIterations, bool& settled) {
    settled = true;
    if (rects.size() < 2) return 0;

    // Movement along the axis that is already too long is damped, so the
    // arrangement grows toward the shape of the screen rather than into a tall
    // column on a wide monitor. The uniform scale is limited by the worse of
    // the two axes, so an arrangement with the wrong aspect costs size
    // everywhere.
    //
    // Measured once, from where the windows actually start, rather than per
    // pass. Recomputing it as the bounds grow lets the damping flip axis
    // between passes, and a pair that is pushed one way and then the other
    // never settles.
    const Rect  start  = Union(rects);
    const float aspect = start.w / start.h;
    const float dampX  = (aspect > regionAspect) ? 0.6f : 1.0f;
    const float dampY  = (aspect > regionAspect) ? 1.0f : 0.6f;

    int pass = 0;
    for (; pass < maxIterations; ++pass) {
        bool moved = false;

        for (size_t i = 0; i < rects.size(); ++i) {
            for (size_t j = i + 1; j < rects.size(); ++j) {
                float overlapX = 0.0f, overlapY = 0.0f;
                Overlap(rects[i], rects[j], gap, overlapX, overlapY);
                if (overlapX <= 0.0f) continue;

                float dx = (rects[j].CentreX() - rects[i].CentreX()) * dampX;
                float dy = (rects[j].CentreY() - rects[i].CentreY()) * dampY;
                float length = std::sqrt(dx * dx + dy * dy);

                if (length < 0.5f) {
                    FanDirection(i * 31 + j, dx, dy);
                    length = 1.0f;
                }

                dx /= length;
                dy /= length;

                // Half each, and a little past touching so a pair that lands
                // exactly tangent does not re-trigger next pass on rounding.
                const float push = SeparationAlong(overlapX, overlapY, dx, dy) * 0.51f;

                rects[i].x -= dx * push;
                rects[i].y -= dy * push;
                rects[j].x += dx * push;
                rects[j].y += dy * push;
                moved = true;
            }
        }

        if (!moved) return pass + 1;
    }

    settled = false;
    return pass;
}

// Deterministic fallback when the relaxation refuses to settle.
//
// Not meant to be pretty. It exists so that a pathological desktop produces a
// readable arrangement rather than whatever the loop happened to hold when the
// cap fired, which would be a pile of overlapping windows.
inline void GridFallback(std::vector<Rect>& rects, float gap) {
    if (rects.empty()) return;

    float cellW = 0.0f, cellH = 0.0f;
    for (const Rect& r : rects) {
        cellW = (std::max)(cellW, r.w);
        cellH = (std::max)(cellH, r.h);
    }

    const int columns = (std::max)(1, static_cast<int>(
        std::ceil(std::sqrt(static_cast<float>(rects.size())))));

    for (size_t i = 0; i < rects.size(); ++i) {
        const int column = static_cast<int>(i) % columns;
        const int row    = static_cast<int>(i) / columns;
        rects[i].x = static_cast<float>(column) * (cellW + gap) + (cellW - rects[i].w) * 0.5f;
        rects[i].y = static_cast<float>(row)    * (cellH + gap) + (cellH - rects[i].h) * 0.5f;
    }
}

} // namespace detail

// Arrange `windows` into a region `regionW` by `regionH`.
//
// The region is the area left for windows: the caller has already taken the
// spaces strip off the top and the outer margin off every side. Output
// coordinates are relative to the region's top left corner.
inline Result Layout(const std::vector<Window>& windows,
                     float regionW, float regionH,
                     const Params& p = Params{}) {
    Result result;
    if (windows.empty() || regionW <= 0.0f || regionH <= 0.0f) return result;

    const float regionAspect = regionW / regionH;

    // The gaps are given in screen space, but the shoving happens in the
    // windows' own space and everything is scaled at the end. A gap of 26 with
    // a final scale of 0.4 would come out as 10 on screen. So the whole thing
    // runs twice: once to learn the scale, once with the gaps divided by it.
    // Without this the arrangement gets visibly tighter the more windows are
    // open, which is the opposite of what it should do.
    auto arrange = [&](float gapScale, std::vector<detail::Rect>& out) {
        const float gap        = p.gap        / gapScale;
        const float memberGap  = p.memberGap  / gapScale;
        const float clusterGap = p.clusterGap / gapScale;

        out.resize(windows.size());
        for (size_t i = 0; i < windows.size(); ++i)
            out[i] = detail::Rect{ windows[i].x, windows[i].y, windows[i].w, windows[i].h };

        bool settled = true;
        int  passes  = 0;

        if (!p.groupByApp) {
            passes = detail::Shove(out, gap, regionAspect, p.iterations, settled);
            if (!settled) detail::GridFallback(out, gap);
            return std::pair<int, bool>{ passes, settled };
        }

        // Grouped: relax each app's windows on their own first, then relax the
        // resulting clusters against each other and translate each cluster's
        // windows with it.
        //
        // Two levels rather than one pass with an attraction term, because an
        // attraction term fights the separation term and there is no setting of
        // the two that is stable for every desktop. Here neither level can
        // produce an overlap the other has to undo: no two clusters overlap
        // because the second level says so, and no two windows inside one
        // cluster overlap because the first level says so.
        std::vector<int> groups;
        for (const Window& w : windows)
            if (std::find(groups.begin(), groups.end(), w.group) == groups.end())
                groups.push_back(w.group);

        // Clusters are seeded in most-recently-used order so the app you were
        // just in leads, matching the switcher.
        std::sort(groups.begin(), groups.end(), [&](int a, int b) {
            int bestA = 0, bestB = 0;
            bool haveA = false, haveB = false;
            for (const Window& w : windows) {
                if (w.group == a && (!haveA || w.order < bestA)) { bestA = w.order; haveA = true; }
                if (w.group == b && (!haveB || w.order < bestB)) { bestB = w.order; haveB = true; }
            }
            if (bestA != bestB) return bestA < bestB;
            return a < b;
        });

        std::vector<detail::Rect> clusters;
        std::vector<std::vector<size_t>> members;
        clusters.reserve(groups.size());
        members.reserve(groups.size());

        for (int group : groups) {
            std::vector<size_t> indices;
            std::vector<detail::Rect> local;
            for (size_t i = 0; i < windows.size(); ++i) {
                if (windows[i].group != group) continue;
                indices.push_back(i);
                local.push_back(detail::Rect{ windows[i].x, windows[i].y,
                                              windows[i].w, windows[i].h });
            }

            bool localSettled = true;
            passes += detail::Shove(local, memberGap, regionAspect, p.iterations,
                                    localSettled);
            if (!localSettled) {
                detail::GridFallback(local, memberGap);
                settled = false;
            }

            const detail::Rect bounds = detail::Union(local);
            for (size_t k = 0; k < local.size(); ++k) {
                out[indices[k]] = local[k];
                out[indices[k]].x -= bounds.x;   // cluster-local offsets
                out[indices[k]].y -= bounds.y;
            }

            clusters.push_back(bounds);
            members.push_back(indices);
        }

        bool clusterSettled = true;
        passes += detail::Shove(clusters, clusterGap, regionAspect,
                                p.iterations, clusterSettled);
        if (!clusterSettled) {
            detail::GridFallback(clusters, clusterGap);
            settled = false;
        }

        for (size_t c = 0; c < clusters.size(); ++c)
            for (size_t i : members[c]) {
                out[i].x += clusters[c].x;
                out[i].y += clusters[c].y;
            }

        return std::pair<int, bool>{ passes, settled };
    };

    std::vector<detail::Rect> rects;
    std::pair<int, bool> run = arrange(1.0f, rects);

    detail::Rect bounds = detail::Union(rects);
    float scale = (std::min)(p.maxScale,
                             (std::min)(regionW / bounds.w, regionH / bounds.h));
    scale = (std::max)(p.minScale, scale);

    if (scale < 0.999f) {
        run    = arrange(scale, rects);
        bounds = detail::Union(rects);
        scale  = (std::max)(p.minScale,
                            (std::min)(p.maxScale,
                                       (std::min)(regionW / bounds.w, regionH / bounds.h)));
    }

    result.scale      = scale;
    result.iterations = run.first;
    result.relaxed    = run.second;
    result.contentW   = bounds.w * scale;
    result.contentH   = bounds.h * scale;
    result.tiles.resize(windows.size());

    // Map into the region, centred. One scale for every window, which is what
    // keeps the relative sizes and every aspect ratio exact.
    const float offsetX = (regionW - result.contentW) * 0.5f;
    const float offsetY = (regionH - result.contentH) * 0.5f;

    for (size_t i = 0; i < rects.size(); ++i) {
        Placement place;
        place.x      = (rects[i].x - bounds.x) * scale + offsetX;
        place.y      = (rects[i].y - bounds.y) * scale + offsetY;
        place.w      = rects[i].w * scale;
        place.h      = rects[i].h * scale;
        place.source = static_cast<int>(i);
        result.tiles[i] = place;
    }

    // The box each app ended up in, in the same coordinates as the tiles, so
    // the caller can put an icon and a name under it without repeating the
    // grouping logic and getting a different answer.
    if (p.groupByApp) {
        for (size_t i = 0; i < windows.size(); ++i) {
            Cluster* found = nullptr;
            for (Cluster& c : result.clusters)
                if (c.group == windows[i].group) { found = &c; break; }

            const Placement& t = result.tiles[i];
            if (!found) {
                Cluster c;
                c.group = windows[i].group;
                c.x = t.x; c.y = t.y; c.w = t.w; c.h = t.h;
                result.clusters.push_back(c);
                continue;
            }

            const float right  = (std::max)(found->x + found->w, t.x + t.w);
            const float bottom = (std::max)(found->y + found->h, t.y + t.h);
            found->x = (std::min)(found->x, t.x);
            found->y = (std::min)(found->y, t.y);
            found->w = right  - found->x;
            found->h = bottom - found->y;
        }
    }

    return result;
}

// --- the spaces strip -------------------------------------------------------

// One chip in the strip across the top: a desktop, or the button that adds one.
struct SpaceChip {
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
    int   index = 0;      // desktop index, or -1 for the add button
    bool  add   = false;
};

// Lay the strip out centred across `stripW`.
//
// Each desktop is drawn at the screen's own aspect ratio, because that is what
// it is: a miniature of the whole display. The add button is square and sits
// after the last one, inside the same centred run, so the strip stays centred
// as desktops come and go rather than shifting every time.
inline std::vector<SpaceChip> LayoutSpaces(int count, float stripW, float stripH,
                                           float chipH, float screenAspect, float gap) {
    std::vector<SpaceChip> chips;
    if (count <= 0 || stripW <= 0.0f || stripH <= 0.0f || chipH <= 0.0f) return chips;

    const float height = chipH;
    const float width  = height * ((screenAspect > 0.1f) ? screenAspect : 1.6f);

    const float total = width * static_cast<float>(count) + height +
                        gap * static_cast<float>(count);

    // Too many desktops to fit is not a real case (Windows offers no way to
    // make dozens by hand), but a narrow monitor with several is, so shrink
    // rather than overflow.
    const float squeeze = (total > stripW && total > 0.0f) ? stripW / total : 1.0f;
    const float w = width * squeeze;
    const float h = height * squeeze;
    const float g = gap * squeeze;

    float x = (stripW - (w * count + h + g * count)) * 0.5f;

    for (int i = 0; i < count; ++i) {
        SpaceChip chip;
        chip.x = x;
        chip.y = (stripH - h) * 0.5f;
        chip.w = w;
        chip.h = h;
        chip.index = i;
        chips.push_back(chip);
        x += w + g;
    }

    SpaceChip add;
    add.x = x;
    add.y = (stripH - h) * 0.5f;
    add.w = h;              // square
    add.h = h;
    add.index = -1;
    add.add = true;
    chips.push_back(add);

    return chips;
}

// How well the arrangement kept the desktop's spatial relationships, as a
// number in -1..1.
//
// Every pair of windows votes: if one was left of the other and still is, the
// pair agrees. Kendall's tau over both axes. This is the property the whole
// algorithm exists for, and "it looks about right" is not something that can be
// asserted on or regression-tested, so it gets measured instead.
//
// Exposed rather than kept in the harness because it is the one number worth
// logging from a real machine when an arrangement looks wrong.
inline float SpatialAgreement(const std::vector<Window>& windows,
                              const Result& result) {
    if (windows.size() < 2 || result.tiles.size() != windows.size()) return 1.0f;

    long agree = 0, total = 0;
    for (size_t i = 0; i < windows.size(); ++i) {
        for (size_t j = i + 1; j < windows.size(); ++j) {
            const Placement& a = result.tiles[i];
            const Placement& b = result.tiles[j];

            const float wasDX = windows[j].CentreX() - windows[i].CentreX();
            const float nowDX = (b.x + b.w * 0.5f) - (a.x + a.w * 0.5f);
            if (std::fabs(wasDX) > 1.0f) {
                ++total;
                if ((wasDX > 0.0f) == (nowDX > 0.0f)) ++agree;
            }

            const float wasDY = windows[j].CentreY() - windows[i].CentreY();
            const float nowDY = (b.y + b.h * 0.5f) - (a.y + a.h * 0.5f);
            if (std::fabs(wasDY) > 1.0f) {
                ++total;
                if ((wasDY > 0.0f) == (nowDY > 0.0f)) ++agree;
            }
        }
    }

    if (total == 0) return 1.0f;
    return static_cast<float>(2.0 * static_cast<double>(agree) /
                              static_cast<double>(total) - 1.0);
}

} // namespace mactab::mission
