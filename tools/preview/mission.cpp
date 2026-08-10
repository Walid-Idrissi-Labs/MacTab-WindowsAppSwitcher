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
//   crowded    thirty windows, where the scale search does the work
//   lopsided   one window bigger than the screen next to twenty small ones
//   stacked    five windows of one app plus three others, for the grouping
//   columns    a deliberate left/right split, which is the ordering test
//
// The last one is the one that matters most. Any packer produces a tidy grid;
// what makes this Mission Control rather than a photo gallery is that a window
// on the left of the screen is still on the left afterwards, and that is the
// property a plain y-then-x sort quietly destroys.

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

    char caption[160];
    std::snprintf(caption, sizeof(caption), "%s  %d WINDOWS  SCALE 0.%02d  %d ROWS%s",
                  desktop.name, static_cast<int>(desktop.windows.size()),
                  static_cast<int>(result.scale * 100.0f + 0.5f) % 100,
                  result.rows, params.groupByApp ? "  GROUPED" : "");
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

    // Grouped mode has to keep an app's windows together. Walk the arrangement
    // in layout order and confirm each group is one contiguous run.
    if (params.groupByApp) {
        std::vector<size_t> order(r.tiles.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            if (r.tiles[a].row != r.tiles[b].row) return r.tiles[a].row < r.tiles[b].row;
            return r.tiles[a].x < r.tiles[b].x;
        });

        std::vector<int> seen;
        int  previous = -1;
        bool contiguous = true;
        for (size_t i : order) {
            const int g = desktop.windows[i].group;
            if (g == previous) continue;
            if (std::find(seen.begin(), seen.end(), g) != seen.end()) contiguous = false;
            seen.push_back(g);
            previous = g;
        }
        Check(contiguous, (tag + ": an app's windows stay together").c_str());
    }
}

} // namespace

int main(int argc, char** argv) {
    const std::string outDir = (argc > 1) ? argv[1] : ".";

    const float regionW = static_cast<float>(kScreenW - kMargin * 2);
    const float regionH = static_cast<float>(kScreenH - kSpacesBarH - kMargin * 2);

    std::printf("\nmission control layout, region %.0fx%.0f\n\n", regionW, regionH);
    std::printf("%-10s %7s %7s %6s %8s\n", "desktop", "windows", "scale", "rows", "fill");

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

            if (grouped) {
                std::printf("%-10s %7zu  %6.3f %6d %7.1f%%\n",
                            desktop.name, desktop.windows.size(), r.scale, r.rows,
                            100.0 * covered / (regionW * regionH));
            }

            const std::string path = outDir + "/mission-" + desktop.name +
                                     (grouped ? "-grouped" : "-positional") + ".png";
            if (!WritePng(path, Render(desktop, r, params)))
                std::fprintf(stderr, "failed to write %s\n", path.c_str());
        }
    }

    // Ordering: on the two-column desktop, every left-hand window must still be
    // left of every right-hand window it shares a row with. This is the whole
    // reason the banding pass exists.
    {
        // Bind the vector, not the element. MakeDesktops().back() is a
        // reference into a temporary that dies at the end of the statement,
        // and the checks below then run on freed memory and pass vacuously.
        const std::vector<Desktop> all = MakeDesktops();
        const Desktop& columns = all.back();
        mission::Params params;
        params.groupByApp = false;
        const mission::Result r = mission::Layout(columns.windows, regionW, regionH, params);

        bool preserved = true;
        for (size_t i = 0; i < r.tiles.size(); ++i) {
            for (size_t j = 0; j < r.tiles.size(); ++j) {
                if (r.tiles[i].row != r.tiles[j].row) continue;
                const bool wasLeft = columns.windows[i].CentreX() < columns.windows[j].CentreX();
                const bool isLeft  = r.tiles[i].x < r.tiles[j].x;
                if (i != j && wasLeft != isLeft) preserved = false;
            }
        }
        Check(preserved, "columns: left stays left within a row");
        std::printf("\ncolumns: %d rows, side order %s\n",
                    r.rows, preserved ? "preserved" : "BROKEN");
    }

    // More windows must not produce a larger scale. Not a tautology: the
    // packing is a step function and the search could settle on the wrong side
    // of a step.
    {
        std::vector<mission::Window> growing;
        float previousScale = 2.0f;
        bool  monotone      = true;
        for (int n = 1; n <= 24; ++n) {
            growing.push_back(W(static_cast<float>((n % 5) * 400),
                                static_cast<float>((n / 5) * 300),
                                640.0f, 420.0f, n % 3, n));
            const float s = mission::Layout(growing, regionW, regionH).scale;
            if (s > previousScale + 1e-4f) monotone = false;
            previousScale = s;
        }
        Check(monotone, "adding a window never makes the tiles bigger");
    }

    // An empty desktop must not crash or return junk.
    {
        const mission::Result r = mission::Layout({}, regionW, regionH);
        Check(r.tiles.empty() && r.rows == 0, "an empty desktop lays out to nothing");
    }

    std::printf("\nwrote PNGs to %s/\n", outDir.c_str());

    if (g_failures > 0) {
        std::fprintf(stderr, "\n%d layout check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("layout self-checks: all passed\n");
    return 0;
}
