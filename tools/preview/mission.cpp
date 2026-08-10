// Mission Control layout preview.
//
// src/mission_layout.h is free of windows.h for the same reason panel_layout.h
// is: the arrangement is a shape, shapes have to be looked at, and the machine
// this is written on cannot run Windows. So the real algorithm runs here,
// against synthetic desktops chosen to break it, and writes what it produced as
// a PNG next to a set of assertions about it.
//
//   ./tools/preview/build.sh
//
// The desktops are not decoration. Each one is a failure this layout can have:
//
//   typical    six windows, mixed sizes, the ordinary case
//   pair       two windows, where nothing should be shrunk much
//   single     one maximised window, the degenerate case
//   crowded    thirty windows, where the relaxation has to converge
//   lopsided   one window bigger than the screen next to twenty small ones
//   stacked    five windows of one app plus three others, for the grouping
//   columns    a deliberate left/right split, which is the ordering test
//
// Plus a stack of eight identical maximised windows, built inline, because
// coincident centres are the one input a spread has no answer for unless it was
// written to have one.
//
// The number that matters most is the agreement: every pair of windows votes on
// whether the side it was on is the side it ended up on. That is what makes
// this Mission Control rather than a photo gallery, and it is the property a
// tidier algorithm quietly trades away.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "canvas.h"
#include "image.h"
#include "mission_layout.h"

using namespace mactab;
using namespace mactab::preview;

namespace {

// The screen these synthetic desktops live on, and the area Mission Control
// gets to arrange them in. The strip across the top is where the spaces go.
constexpr int kScreenW    = 2560;
constexpr int kScreenH    = 1440;
constexpr int kSpacesBarH = 200;
constexpr int kMargin     = 64;

int g_failures = 0;

void Check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "CHECK FAILED: %s\n", what);
        ++g_failures;
    }
}

// --- synthetic desktops -----------------------------------------------------

struct Desktop {
    const char*                 name;
    std::vector<mission::Window> windows;
};

// Colours per app group, so the grouping is visible at a glance rather than
// only assertable.
uint32_t GroupColour(int group) {
    static const uint32_t kColours[] = {
        MakePixel( 62, 116, 214, 255),   // blue
        MakePixel(214,  86,  72, 255),   // red
        MakePixel( 76, 168, 106, 255),   // green
        MakePixel(196, 148,  56, 255),   // amber
        MakePixel(140,  92, 196, 255),   // violet
        MakePixel( 68, 168, 188, 255),   // teal
    };
    return kColours[static_cast<size_t>(group) % (sizeof(kColours) / sizeof(kColours[0]))];
}

mission::Window W(float x, float y, float w, float h, int group, int order) {
    mission::Window win;
    win.x = x; win.y = y; win.w = w; win.h = h;
    win.group = group; win.order = order;
    return win;
}

std::vector<Desktop> MakeDesktops() {
    std::vector<Desktop> all;

    all.push_back({ "typical", {
        W(  120,  140, 1200,  820, 0, 0),
        W( 1400,  100,  900,  600, 1, 1),
        W( 1500,  760,  980,  560, 2, 2),
        W(  200, 1000,  700,  380, 3, 3),
        W(  980,  520,  620,  760, 4, 4),
        W( 1900,  300,  520,  400, 1, 5),
    }});

    all.push_back({ "pair", {
        W(  200,  200, 1000,  700, 0, 0),
        W( 1400,  500,  900,  650, 1, 1),
    }});

    // One maximised window. It cannot be shown at full size because the region
    // is smaller than the screen, so the scale has to come out below 1 without
    // anything special-casing a single tile.
    all.push_back({ "single", {
        W(    0,    0, 2560, 1400, 0, 0),
    }});

    {
        Desktop d{ "crowded", {} };
        for (int i = 0; i < 30; ++i) {
            const float x = static_cast<float>(60 + (i % 6) * 400);
            const float y = static_cast<float>(80 + (i / 6) * 260);
            const float w = 300.0f + static_cast<float>((i * 37) % 5) * 60.0f;
            const float h = 220.0f + static_cast<float>((i * 53) % 4) * 50.0f;
            d.windows.push_back(W(x, y, w, h, i % 6, i));
        }
        all.push_back(d);
    }

    {
        // A window larger than the screen itself, which happens with some
        // borderless full-screen games and with a window dragged off the edge.
        Desktop d{ "lopsided", { W(-200, -100, 2900, 1600, 0, 0) } };
        for (int i = 0; i < 20; ++i) {
            const float x = static_cast<float>(100 + (i % 5) * 300);
            const float y = static_cast<float>(200 + (i / 5) * 240);
            d.windows.push_back(W(x, y, 260.0f, 180.0f, 1 + i % 4, i + 1));
        }
        all.push_back(d);
    }

    all.push_back({ "stacked", {
        W(  100,  120,  760,  540, 0, 0),
        W(  400,  300,  760,  540, 0, 1),
        W(  700,  480,  760,  540, 0, 2),
        W( 1000,  660,  760,  540, 0, 3),
        W( 1300,  840,  760,  540, 0, 4),
        W( 1700,  120,  700,  500, 1, 5),
        W( 1800,  700,  620,  460, 2, 6),
        W(  200,  980,  560,  360, 3, 7),
    }});

    // Two clean columns, three rows. Nothing here should end up crossing sides.
    {
        Desktop d{ "columns", {} };
        for (int row = 0; row < 3; ++row) {
            // Deliberately staggered vertically: real windows never line up,
            // and a banding step that assumes they do would pass this test
            // without doing anything.
            d.windows.push_back(W(  120.0f, 80.0f + row * 440.0f + 17.0f,
                                    900.0f, 380.0f, 0, row * 2));
            d.windows.push_back(W( 1480.0f, 80.0f + row * 440.0f,
                                    900.0f, 380.0f, 1, row * 2 + 1));
        }
        all.push_back(d);
    }

    return all;
}

// --- rendering --------------------------------------------------------------

void StrokeRect(Bitmap& b, int x, int y, int w, int h, int thickness, uint32_t colour) {
    FillRect(b, x, y, w, thickness, colour);
    FillRect(b, x, y + h - thickness, w, thickness, colour);
    FillRect(b, x, y, thickness, h, colour);
    FillRect(b, x + w - thickness, y, thickness, h, colour);
}

// One tile, drawn to look enough like a window that the arrangement can be
// read: a title bar in the app's colour, a body, and the window's index.
void DrawTile(Bitmap& b, const mission::Placement& p, int index, int group) {
    const int x = static_cast<int>(p.x + 0.5f);
    const int y = static_cast<int>(p.y + 0.5f);
    const int w = (std::max)(2, static_cast<int>(p.w + 0.5f));
    const int h = (std::max)(2, static_cast<int>(p.h + 0.5f));

    const int barH = (std::min)(h / 4, (std::max)(4, h / 8));

    FillRect(b, x, y, w, h, MakePixel(244, 244, 246, 255));
    FillRect(b, x, y, w, barH, GroupColour(group));
    StrokeRect(b, x, y, w, h, 2, MakePixel(18, 18, 22, 255));

    char label[8];
    std::snprintf(label, sizeof(label), "%d", index);

    const int scale = (std::max)(1, (std::min)(w, h) / 28);
    const int tw    = WordWidth(label, scale);
    DrawWord(b, label, x + (w - tw) / 2, y + (h - 7 * scale) / 2, scale,
             MakePixel(40, 40, 46, 255));
}

Bitmap Render(const Desktop& desktop, const mission::Result& result,
              const mission::Params& params) {
    Bitmap out = Bitmap::Create(kScreenW, kScreenH, MakePixel(24, 26, 32, 255));

    // The spaces strip, as a placeholder so the region the tiles get is honest
    // about what is left over.
    FillRect(out, 0, 0, kScreenW, kSpacesBarH, MakePixel(38, 40, 48, 255));
    for (int i = 0; i < 3; ++i) {
        const int tw = 260, th = 146;
        const int tx = kScreenW / 2 - (tw + 24) * 3 / 2 + i * (tw + 24);
        FillRect(out, tx, (kSpacesBarH - th) / 2, tw, th,
                 MakePixel(58, 62, 74, 255));
        if (i == 1)
            StrokeRect(out, tx, (kSpacesBarH - th) / 2, tw, th, 3,
                       MakePixel(230, 230, 240, 255));
    }

    // The region outline, so overflow is visible and not only asserted.
    const int rx = kMargin;
    const int ry = kSpacesBarH + kMargin;
    const int rw = kScreenW - kMargin * 2;
    const int rh = kScreenH - kSpacesBarH - kMargin * 2;
    StrokeRect(out, rx, ry, rw, rh, 1, MakePixel(70, 74, 88, 255));

    for (size_t i = 0; i < result.tiles.size(); ++i) {
        mission::Placement p = result.tiles[i];
        p.x += rx;
        p.y += ry;
        DrawTile(out, p, static_cast<int>(i), desktop.windows[i].group);
    }

    char caption[200];
    std::snprintf(caption, sizeof(caption),
                  "%s  %d WINDOWS  SCALE 0.%02d  %d PASSES  AGREE 0.%02d%s%s",
                  desktop.name, static_cast<int>(desktop.windows.size()),
                  static_cast<int>(result.scale * 100.0f + 0.5f) % 100,
                  result.iterations,
                  static_cast<int>(mission::SpatialAgreement(desktop.windows, result)
                                   * 100.0f + 0.5f) % 100,
                  params.groupByApp ? "  GROUPED" : "",
                  result.relaxed ? "" : "  FELL BACK TO A GRID");
    for (char* c = caption; *c; ++c)
        if (*c >= 'a' && *c <= 'z') *c = static_cast<char>(*c - 32);
    DrawWord(out, caption, 40, 30, 4, MakePixel(210, 212, 222, 255));

    return out;
}

// --- assertions -------------------------------------------------------------

bool Overlaps(const mission::Placement& a, const mission::Placement& b, float slack) {
    return a.x < b.x + b.w - slack && b.x < a.x + a.w - slack &&
           a.y < b.y + b.h - slack && b.y < a.y + a.h - slack;
}

void CheckDesktop(const Desktop& desktop, const mission::Result& r,
                  float regionW, float regionH, const mission::Params& params) {
    const std::string tag = desktop.name;

    Check(r.tiles.size() == desktop.windows.size(),
          (tag + ": every window gets a placement").c_str());
    Check(r.scale > 0.0f && r.scale <= mission::kMaxScale + 1e-4f,
          (tag + ": scale is positive and never enlarges").c_str());

    for (size_t i = 0; i < r.tiles.size(); ++i) {
        const mission::Placement& p = r.tiles[i];
        const mission::Window&    w = desktop.windows[i];

        // One scale for every window. This is the property that separates
        // Mission Control from a justified photo grid, and it is a single
        // careless per-row normalisation away from being lost.
        const float sx = p.w / w.w;
        const float sy = p.h / w.h;
        if (std::fabs(sx - r.scale) > 1e-3f || std::fabs(sy - r.scale) > 1e-3f) {
            std::fprintf(stderr, "  %s tile %zu scaled %.4f x %.4f, expected %.4f\n",
                         tag.c_str(), i, sx, sy, r.scale);
            Check(false, (tag + ": one global scale, aspect preserved").c_str());
            break;
        }
    }

    for (size_t i = 0; i < r.tiles.size(); ++i) {
        const mission::Placement& p = r.tiles[i];
        if (p.x < -0.01f || p.y < -0.01f ||
            p.x + p.w > regionW + 0.01f || p.y + p.h > regionH + 0.01f) {
            std::fprintf(stderr, "  %s tile %zu at %.1f,%.1f %.1fx%.1f escapes %.0fx%.0f\n",
                         tag.c_str(), i, p.x, p.y, p.w, p.h, regionW, regionH);
            Check(false, (tag + ": every tile stays inside the region").c_str());
            break;
        }
    }

    for (size_t i = 0; i < r.tiles.size(); ++i) {
        for (size_t j = i + 1; j < r.tiles.size(); ++j) {
            if (Overlaps(r.tiles[i], r.tiles[j], 0.5f)) {
                std::fprintf(stderr, "  %s tiles %zu and %zu overlap\n",
                             tag.c_str(), i, j);
                Check(false, (tag + ": no two tiles overlap").c_str());
                return;
            }
        }
    }

    // Grouped mode has to keep an app's windows together, and the honest test
    // of that is not adjacency in some traversal order: it is that no app's
    // bounding box overlaps another's. That is the thing the two-level
    // relaxation promises, and it is what makes the grouping legible without
    // drawing a box around anything.
    if (params.groupByApp) {
        std::vector<int> groups;
        for (const mission::Window& w : desktop.windows)
            if (std::find(groups.begin(), groups.end(), w.group) == groups.end())
                groups.push_back(w.group);

        std::vector<mission::Placement> boxes;
        for (int g : groups) {
            float l = 0, t = 0, rr = 0, bb = 0;
            bool first = true;
            for (size_t i = 0; i < r.tiles.size(); ++i) {
                if (desktop.windows[i].group != g) continue;
                const mission::Placement& p = r.tiles[i];
                if (first) { l = p.x; t = p.y; rr = p.x + p.w; bb = p.y + p.h; first = false; }
                else {
                    l  = (std::min)(l,  p.x);
                    t  = (std::min)(t,  p.y);
                    rr = (std::max)(rr, p.x + p.w);
                    bb = (std::max)(bb, p.y + p.h);
                }
            }
            mission::Placement box;
            box.x = l; box.y = t; box.w = rr - l; box.h = bb - t;
            boxes.push_back(box);
        }

        bool clear = true;
        for (size_t i = 0; i < boxes.size() && clear; ++i)
            for (size_t j = i + 1; j < boxes.size() && clear; ++j)
                if (Overlaps(boxes[i], boxes[j], 1.0f)) clear = false;

        Check(clear, (tag + ": no app's cluster overlaps another's").c_str());
    }
}

} // namespace

int main(int argc, char** argv) {
    const std::string outDir = (argc > 1) ? argv[1] : ".";

    const float regionW = static_cast<float>(kScreenW - kMargin * 2);
    const float regionH = static_cast<float>(kScreenH - kSpacesBarH - kMargin * 2);

    std::printf("\nmission control layout, region %.0fx%.0f\n\n", regionW, regionH);
    std::printf("%-10s %7s %7s %7s %7s %7s\n",
                "desktop", "windows", "scale", "passes", "agree", "fill");

    const std::vector<Desktop> desktops = MakeDesktops();

    for (const Desktop& desktop : desktops) {
        for (int grouped = 0; grouped < 2; ++grouped) {
            mission::Params params;
            params.groupByApp = (grouped != 0);

            const mission::Result r =
                mission::Layout(desktop.windows, regionW, regionH, params);

            CheckDesktop(desktop, r, regionW, regionH, params);

            // Determinism. The comparators sort pointers into a vector, and a
            // comparator that is not a strict weak ordering can shuffle
            // identical input between runs, which would be invisible in a
            // single render.
            const mission::Result again =
                mission::Layout(desktop.windows, regionW, regionH, params);
            bool same = again.tiles.size() == r.tiles.size();
            for (size_t i = 0; same && i < r.tiles.size(); ++i)
                same = again.tiles[i].x == r.tiles[i].x &&
                       again.tiles[i].y == r.tiles[i].y;
            Check(same, (std::string(desktop.name) + ": layout is deterministic").c_str());

            double covered = 0.0;
            for (const mission::Placement& p : r.tiles) covered += p.w * p.h;

            if (!grouped) {
                std::printf("%-10s %7zu  %6.3f %7d  %6.2f %6.1f%%%s\n",
                            desktop.name, desktop.windows.size(), r.scale,
                            r.iterations,
                            mission::SpatialAgreement(desktop.windows, r),
                            100.0 * covered / (regionW * regionH),
                            r.relaxed ? "" : "  GRID FALLBACK");
            }

            // The arrangement exists to be recognisable, and that is a number:
            // every pair of windows votes on whether the side it was on is the
            // side it ended up on. Anything below this is not a spread any
            // more, it is a reshuffle.
            const float agreement = mission::SpatialAgreement(desktop.windows, r);
            if (grouped)
                std::printf("%-10s grouped: agreement %.2f, scale %.3f, %d passes%s\n",
                            desktop.name, agreement, r.scale, r.iterations,
                            r.relaxed ? "" : "  GRID FALLBACK");
            // Grouping is allowed to cost spatial order, because pulling an
            // app's windows together is a promise to move them. The floor is
            // only there to catch a genuine scramble: chance is 0.
            Check(agreement > (grouped ? 0.25f : 0.60f),
                  (std::string(desktop.name) +
                   (grouped ? ": grouping keeps most of the spatial order"
                            : ": the arrangement keeps the desktop's spatial order")).c_str());

            const std::string path = outDir + "/mission-" + desktop.name +
                                     (grouped ? "-grouped" : "-positional") + ".png";
            if (!WritePng(path, Render(desktop, r, params)))
                std::fprintf(stderr, "failed to write %s\n", path.c_str());
        }
    }

    // The two-column desktop is the ordering test. Nothing on the left may end
    // up right of something that was on its right, and with a spread rather
    // than a packer that has to hold for every pair, not just within a row.
    {
        // Bind the vector, not the element. MakeDesktops().back() is a
        // reference into a temporary that dies at the end of the statement,
        // and the checks below then run on freed memory and pass vacuously.
        const std::vector<Desktop> all = MakeDesktops();
        const Desktop& columns = all.back();

        mission::Params params;
        params.groupByApp = false;
        const mission::Result r = mission::Layout(columns.windows, regionW, regionH, params);

        Check(mission::SpatialAgreement(columns.windows, r) > 0.999f,
              "columns: every pair keeps the side it was on");
        std::printf("\ncolumns: agreement %.3f\n",
                    mission::SpatialAgreement(columns.windows, r));
    }

    // A stack of identical maximised windows has no direction to separate
    // along. KWin nudges x by one pixel here, which collapses the stack into a
    // horizontal line and scales every window down to a sliver. It has to bloom
    // in two dimensions instead, so assert the arrangement is not a line.
    {
        std::vector<mission::Window> stack;
        for (int i = 0; i < 8; ++i) stack.push_back(W(0, 0, 2560, 1400, 0, i));

        const mission::Result r = mission::Layout(stack, regionW, regionH);

        float left = r.tiles[0].x, top = r.tiles[0].y;
        float right = r.tiles[0].x + r.tiles[0].w, bottom = r.tiles[0].y + r.tiles[0].h;
        for (const mission::Placement& t : r.tiles) {
            left   = (std::min)(left,   t.x);
            top    = (std::min)(top,    t.y);
            right  = (std::max)(right,  t.x + t.w);
            bottom = (std::max)(bottom, t.y + t.h);
        }
        const float aspect = (right - left) / (std::max)(1.0f, bottom - top);
        Check(aspect > 0.4f && aspect < 6.0f,
              "stack: identical windows bloom in two dimensions, not a line");
        std::printf("stack of 8 identical maximised windows: "
                    "scale %.3f, bounding aspect %.2f\n", r.scale, aspect);
    }

    // More windows must not produce a larger scale. Not a tautology: the
    // relaxation grows the bounding box, and a badly damped push could grow it
    // less for a bigger set.
    {
        std::vector<mission::Window> growing;
        float previousScale = 2.0f;
        float worstRise     = 0.0f;
        float firstScale    = 0.0f;
        for (int n = 1; n <= 24; ++n) {
            growing.push_back(W(static_cast<float>((n % 5) * 400),
                                static_cast<float>((n / 5) * 300),
                                640.0f, 420.0f, n % 3, n));
            const float s = mission::Layout(growing, regionW, regionH).scale;
            if (n == 1) firstScale = s;
            worstRise = (std::max)(worstRise, s - previousScale);
            previousScale = s;
        }
        std::printf("growing 1 to 24 windows: scale %.3f down to %.3f, "
                    "worst rise %.3f\n", firstScale, previousScale, worstRise);
        Check(previousScale < firstScale, "more windows means smaller tiles overall");
    }

    // An empty desktop must not crash or return junk.
    {
        const mission::Result r = mission::Layout({}, regionW, regionH);
        Check(r.tiles.empty() && r.iterations == 0,
              "an empty desktop lays out to nothing");
    }

    std::printf("\nwrote PNGs to %s/\n", outDir.c_str());

    if (g_failures > 0) {
        std::fprintf(stderr, "\n%d layout check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("layout self-checks: all passed\n");
    return 0;
}
