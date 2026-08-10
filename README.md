# MacTab

A macOS-style application switcher for Windows 10 and 11.

Windows' Alt+Tab is a per-window grid. macOS' Cmd-Tab is a per-*application* row
of large squircle icons on a floating glass panel, ordered most-recently-used.
MacTab is the second thing, on Windows.

**Status: beta.** 0.1.0 works on Windows 11. 0.2.0 reworks the panel's shape and
material and adds a tray settings menu. Grab a build from
[Releases](https://github.com/Walid-Idrissi-Labs/MacTab-WindowsAppSwitcher/releases),
or see [Building](#building).

The project is developed on macOS, so nothing here is compiled or run by its
author. See [Development on macOS](#development-on-macos) for how that is
managed and what it means for how much to trust a given part of it.

## Goals

Two, and they pull against each other:

1. **Look genuinely like macOS.** Real backdrop blur, a macOS-sized corner
   radius (62px, not DWM's 8px), squircle icons synthesised from whatever
   Windows hands us, MRU app grouping, `Q` to quit an app from the switcher.
2. **Cost nothing.** No polling, no timers at rest, no runtime dependencies.

Target budget:

| | |
|---|---|
| Idle CPU | 0.0% |
| Idle working set | < 20 MB |
| Binary | < 2 MB, no redistributables |
| Warm Alt+Tab to first pixel | < 16 ms |
| CPU during animation | ~0% (runs on the compositor thread) |

## Install

Download `MacTab-Setup-x.y.z.exe` from
[Releases](https://github.com/Walid-Idrissi-Labs/MacTab-WindowsAppSwitcher/releases)
and run it. It installs per-user under `%LOCALAPPDATA%` and never asks for
administrator rights.

The build is not code-signed, so SmartScreen will warn about it: *More info*
then *Run anyway*. A global keyboard hook is also a heuristic antivirus trigger.
The hook is the whole mechanism, `RegisterHotKey` cannot bind `Alt+Tab` because
Windows reserves it, and the code is in
[`src/hotkey.cpp`](src/hotkey.cpp) if you want to read what it does before
running it. Please do not disable your antivirus for this.

## Why C++/WinRT and Windows.UI.Composition

The corner radius decided it. `DWMWA_SYSTEMBACKDROP_TYPE` gives real Desktop
Acrylic but locks corners to DWM's ~8px. `SetWindowCompositionAttribute` blurs
the whole window rect, and clipping that needs `SetWindowRgn`, which is aliased.
The Composition Visual Layer is the only API that will clip a blurred backdrop
to arbitrary antialiased geometry, which is what a macOS-radius glass panel
actually requires.

It is also the cheapest option: `windowsapp.lib` is part of Windows (10 1803+),
so there is no Windows App SDK and nothing to redistribute; C++/WinRT is
header-only and compiles down to direct COM vtable calls; and Composition
animations run on DWM's thread rather than ours.

## The look, measured rather than guessed

"Looks like macOS" is not a thing you can get right by taste when you cannot see
the result. So the shape and the material are fitted to a screenshot of the real
switcher and the numbers are in the source next to the code that uses them.

**Corners.** Least-squares fit of the implicit superellipse
`|(Cx-x)/a|^n + |(Cy-y)/a|^n = 1` to boundary points recovered from the panel's
specular rim, sweeping origin, corner extent `a` and exponent `n`:

| | extent `a` | exponent `n` | residual | measured on |
|---|---|---|---|---|
| panel corner | 214 | 2.24 | 0.013 over 35 points | a 1938 x 588 panel |
| app icon corner | 106 | 2.46 | 0.0014 over 35 points | a 348 wide icon |

Two things fall out of that. The panel corner is very nearly circular, nothing
like the `n = 5` that suits an icon tile whose corner extent is half its own
width, so `CreateSquircleGeometry` takes the exponent as a parameter and the
panel and its shadow use 2.24. And the panel radius follows Apple's concentric
rule, outer extent equals inner extent plus the padding between them, which puts
it at 62 logical pixels rather than the 24 it started at.

**Material.** Same screenshot, different measurement: sample the wallpaper just
outside the rim and the glass just inside it along the top and bottom edges,
then fit per channel. Red is the only channel whose input spans a useful range
there, and it gives `out = 0.439 * in + 0.221`, with mean luma rising from 0.467
outside to 0.550 inside. So the material compresses the backdrop's contrast to a
little under half and *lifts* it, rather than veiling it.

That is one Direct2D colour matrix, not an alpha blend, because an alpha blend
cannot do either half of it. Saturation is `1 / (1 - tintAlpha)`, which restores
exactly the relative saturation that mixing with the tint took away and nothing
more; without it the glass reads as frosted plastic rather than glass.
`CLSID_D2D1Saturation` is no use for this at all, its property is documented over
`[0, 1]` so it can only ever desaturate. The coefficients live in
[`src/glass.h`](src/glass.h), free of `windows.h`, so the preview harness below
applies the identical matrix and writes a PNG.

## Building

Requires **Visual Studio 2022** (or Build Tools 2022) with the *Desktop
development with C++* workload, and a **Windows SDK ≥ 10.0.17763.0**.

```
build.bat            release
build.bat debug      debug
build.bat clean      wipe build/
```

Output lands in `build\bin\MacTab.exe`. The script finds MSVC via `vswhere` and
enters the environment itself, so it works from a plain `cmd` prompt. It uses
Ninja when available and falls back to the Visual Studio generator.

CMake needs to find the C++/WinRT projection headers, which live in the SDK at
`Include/<version>/cppwinrt` and are not on the default include path.
`CMakeLists.txt` reads the vcvars environment first, then scans the standard kit
locations newest-first.

## Running

Launch `MacTab.exe`. It sits in the notification area; right-click for the menu.
Only one instance runs per logon session.

```
MacTab.exe --diag
```

writes `%LOCALAPPDATA%\MacTab\diag.log`. The log records which code paths were
chosen (which backdrop tier won, why a window was filtered out of the list) and
timings for the hot paths. It is opened with full sharing, so it can be tailed
while the app runs.

### Keys

| Key | Does |
|---|---|
| `Alt+Tab` | Next application, most-recently-used order |
| `Alt+Shift+Tab` | Previous application |
| `←` `→` | Same as Tab and Shift+Tab |
| `Alt+` `` ` `` | Cycle windows within the highlighted application |
| `↓` | Expand the highlighted application's windows |
| `Q` | Quit the application. `WM_CLOSE` to each of its windows, never a kill |
| `W` | Close only its frontmost window |
| `H` | Minimise all its windows. Windows has no "hide application" |
| `Esc` | Cancel without switching |
| Mouse | Hover selects, click activates |

A quick tap and release switches to the previous application without the panel
ever appearing, which is what macOS does. The panel only shows if Alt is held
past `RevealDelayMs`.

### Settings

Tray icon, then *Settings*. Panel display (active window's display, the display
with the mouse, or always the main display) and appearance (follow Windows,
light, dark). Both take effect on the next Alt+Tab.

Everything else is in `%LOCALAPPDATA%\MacTab\settings.ini`, which is written with
defaults and comments on first run:

| Key | Default | |
|---|---|---|
| `RevealDelayMs` | 180 | How long Alt must be held before the panel appears |
| `LeftAltOnly` | 1 | Ignore Right Alt and anything with Ctrl held. Keep this on if AltGr types `@ # { } [ ]` on your layout |
| `TileSize` | 128 | Icon size in logical pixels at 100% scaling |
| `Theme` | auto | `auto`, `light` or `dark` |
| `GroupByApp` | 1 | 0 gives one tile per window instead of one per application |
| `PanelDisplay` | active | `active`, `mouse` or `main` |

Drop a PNG into `%LOCALAPPDATA%\MacTab\themes\` named after an executable
(`chrome.png`, `code.png`) to override that app's generated tile. Some icons will
never look right no matter how good the synthesis is.

## Development on macOS

Most of this is written on a Mac, where it cannot be compiled or run. To keep
that from turning into a pile of trivial build breaks:

```
./tools/syntax-check.sh
```

parses and type-checks the Win32 sources against the mingw-w64 headers
(`brew install mingw-w64`). It catches syntax errors, unknown identifiers, type
mismatches, and wrong Win32 signatures.

It does **not** cover the Composition rendering layer: the C++/WinRT headers
ship only with the Windows SDK, so those files are explicitly listed as skipped
rather than silently passing. `src/pch.h` deliberately excludes `winrt/base.h`
for this reason; files that need WinRT include `src/winrt_pch.h`.

A clean run means "this will probably compile", not "this works".

The image, squircle, panel-layout and glass code is deliberately kept free of
`windows.h`, which means it can be compiled *and executed* natively:

```
./tools/preview/build.sh
```

runs a set of regression checks over that layer, then writes PNGs to
`build-preview/out/`: the icon pipeline case by case, and the whole panel in both
themes over a blurred wallpaper, in `panel-dark.png` and `panel-light.png`. It
uses the same `panel_layout.h` and `glass.h` the app does, not copies, so the
geometry and the colour matrix are the real ones. Only the pixel loops differ:
Direct2D on Windows, a box blur and a per-pixel matrix here.

That boundary is the point. The numbers are the judgement calls and they are
shared; the plumbing is what CI and a real machine are for.

This is the only part of the project that gets looked at before it ships, and it
has already earned its keep; it caught icons being scaled by canvas instead of
content, generated tiles landing at the same brightness as the glyph on them,
and a square selection highlight sitting next to squircle icons.

`src/panel.cpp` is the one file neither tool reaches, because C++/WinRT ships
only with the Windows SDK. Every review round so far has found that defects
concentrate there, which is not a coincidence.

## Milestones

- [x] **M0** Skeleton: build system, tray, diagnostics logging
- [x] **M1** Input capture: low-level keyboard hook, Alt state machine
- [x] **M2** Window model: enumeration, app grouping, MRU
- [x] **M3** Panel: Composition, captured-and-blurred backdrop, squircle
- [x] **M4** Icons: extraction, squircle processing, caching
- [x] **M5** Visual fidelity: labels, highlight, spring animation, DPI
- [x] **M6** Actions: Q/W/H, per-app window cycling
- [x] **M7** Config: settings, icon theme packs, autostart
- [x] **M8** Performance pass
- [x] **M9** Installer and uninstaller

Ordered by risk rather than by user-visible value: the two pieces most likely to
not work at all (the keyboard hook and the backdrop) come first, so they fail
early rather than after everything is built on top of them.

## What to check first

Launch with `--diag` and work down this list. The log names the code path taken
at each decision point, so a report of "the panel looks wrong" is much more
useful with the log attached than without it.

1. **Does Alt+Tab get intercepted at all?** Windows' own switcher must never
   appear. If it does, the hook was refused; the log says why.
2. **Quick tap vs. hold.** A fast Alt+Tab must switch with no panel and must
   *never* log `gesture: reveal`. Holding Alt must log it exactly once.
3. **AltGr.** On a French or Arabic layout, typing `@ # { } [ ]` must not open
   the switcher. This is the guard most likely to need tuning; loosen it with
   `LeftAltOnly` in `settings.ini`.
4. **The switcher list.** Tray → *Log current switcher list*, and compare
   against what Windows' Alt+Tab shows. Minimised windows, a UWP app such as
   Settings, and a second virtual desktop are the interesting cases.
5. **Which capture path won.** The log names it: `desktop-duplication` is the
   good one, `gdi-bitblt` is the fallback, `none` means a flat tint. Hybrid-GPU
   laptops and HDR displays are the likely failures.
6. **The glass over extremes.** A pure white wallpaper and a pure black one. The
   material compresses the backdrop's range specifically so neither blows
   through, and the app name has to stay readable over both.
7. **A long app name.** "Visual Studio Code" or similar, with that app first and
   last in the row. The label is anchored under its icon and clamped inside the
   panel, so at the ends it should slide inward rather than overhang.
8. **Does the panel appear within a frame?** `gesture: reveal ... shown in N ms`.
   This is the one performance claim that could not be verified from
   documentation, so it is measured rather than assumed.

## Performance

Everything below is a design property rather than a measurement, since nothing
has been profiled on real hardware yet. The `--diag` log carries timings for
each of these so the claims can be checked rather than trusted.

| Claim | How it is achieved |
|---|---|
| No timers at rest | The only timer is the reveal delay, armed on the first Tab and killed on commit. |
| No polling | MRU comes from one `EVENT_SYSTEM_FOREGROUND` hook. Dead windows are pruned lazily rather than by subscribing to `EVENT_OBJECT_DESTROY`, which fires for every menu and tooltip in the session. |
| One-frame reveal | The window, devices and visual tree are built once at startup and never destroyed. Showing is `SetWindowPos` plus property writes. |
| No CPU during animation | Fades, scale and the selection spring are Composition animations, which run on DWM's thread. |
| Bounded memory | Icon tiles are an LRU capped at 96 entries (~6 MB at 128px). |
| No blocking on the reveal path | Icon work is queued to a worker; missing tiles render as placeholders and fill in on arrival. |
| No redistributables | `windowsapp.lib` is part of Windows; the CRT is linked statically. |

## Known limitations

- **Elevated windows.** A process running `asInvoker` cannot see keyboard input
  destined for a higher-integrity window, so while an elevated app is foreground
  the built-in Alt+Tab takes over. The real fix is `uiAccess="true"`, which
  additionally requires an Authenticode-signed binary installed under
  `%ProgramFiles%`.
- **`H` is an approximation.** Windows has no "hide application", so `H` maps to
  minimising every window of the app.
- **Antivirus false positives.** A global low-level keyboard hook is a heuristic
  detection trigger. The binary is not packed or compressed, which helps;
  code-signing would help more.
- **The Start menu renders above the panel.** Since Windows 8, `HWND_TOPMOST`
  lives in a lower z-band than the Start menu and shell flyouts, and no amount
  of `SetWindowPos` crosses that boundary from a normal process. The legitimate
  fix is the same `uiAccess` that fixes elevated windows. Nothing renders above
  true exclusive-fullscreen apps either; borderless-windowed is fine.
- **Windows 10 pre-1803 is unsupported**; that is the Composition floor.
- **HDR displays** fall back to GDI capture. Desktop duplication hands back
  scRGB float rather than BGRA when HDR is on and does not convert, so the
  duplication path bails rather than showing wrong colours.

## Licence

Not yet chosen.
