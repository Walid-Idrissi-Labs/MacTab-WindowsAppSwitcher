# MacTab

[![build](https://github.com/Walid-Idrissi-Labs/MacTab-WindowsAppSwitcher/actions/workflows/build.yml/badge.svg)](https://github.com/Walid-Idrissi-Labs/MacTab-WindowsAppSwitcher/actions/workflows/build.yml)

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
   radius (62px, not DWM's 8px), a material whose numbers are fitted to a
   screenshot of the real thing, squircle icons synthesised from whatever
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

### About "Windows protected your PC"

That dialog, and the "Unknown publisher" line in it, come from **Authenticode
and nothing else**. It is worth being precise about this because several things
look like they should help and none of them do:

- `AppPublisher` in the installer shows in Add/Remove Programs and in the wizard.
  SmartScreen does not read it.
- `VersionInfoCompany` shows in the file's Properties, Details tab. SmartScreen
  does not read that either.
- A self-signed certificate changes nothing. Windows does not trust it, so the
  publisher still reads as unknown.

Only a certificate from a CA Windows trusts puts a name on that dialog. The
realistic options, cheapest first:

| Route | Cost | Effect |
|---|---|---|
| [SignPath Foundation](https://signpath.org/) | free for qualifying OSS | Real certificate, publisher named. Application and review required. |
| [Certum open source](https://shop.certum.eu/open-source-code-signing.html) | around €30/year | Real OV certificate for open-source authors, publisher named. |
| [Azure Trusted Signing](https://learn.microsoft.com/en-us/azure/trusted-signing/) | around $10/month | Short-lived certificates issued per signing, integrates with CI. |
| EV certificate | $300 to $600/year | Instant SmartScreen reputation, so no warning at all. |

With an OV certificate (the first three) the file is signed and named, but
SmartScreen can still warn until the signature accumulates reputation across
enough downloads. Only EV skips that wait.

The release workflow is already wired for this. Add `CERT_PFX_BASE64` and
`CERT_PASSWORD` as repository secrets and both the executable and the installer
get signed and timestamped, with no other change: the signing step is skipped
entirely when the secret is absent, which is why the current builds are
unsigned rather than broken.

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

Four things make that read as glass rather than as a blurred rectangle, in the
order they matter:

1. **The operating point moves.** One affine transfer cannot serve both a white
   wallpaper and a black one: the panel either washes out or turns into a slab.
   Apple avoids this with a stack of lighten and darken blends that have soft
   knees at both ends, which is not something one colour matrix can do. What is
   available here and not to Apple is that the backdrop is a *frozen frame*, so
   its mean luma is known before a pixel is drawn. `Adapt()` bends the bias per
   gesture to land the panel inside a per-theme band. The gain never moves: how
   much of the desktop's contrast survives is a property of the material, where
   that window sits is a property of what is behind it today.
2. **Saturation, well past unity.** Measured on the reference, relative
   saturation goes 0.737 outside the panel to 0.642 inside, a ratio of 0.87. The
   light material's 2.80 lands on 0.877. `CLSID_D2D1Saturation` is no use for
   this at all: its property is documented over `[0, 1]`, so it can only ever
   desaturate, and asking for more clamps to identity without complaining.
3. **An inner glow at the top edge.** The reference's interior luma is 161 just
   inside the top rim, decaying to 148.8 about 45px in, with a quarter of that
   along the bottom. That falloff is what reads as a lit curved surface.
4. **The rim is additive and nearly symmetric.** Measured lift over the adjacent
   interior is +33 luma at the top, +23 at the bottom, +22 at the sides, so 1.4:1
   rather than the 4:1 that looks plausible, and the colour delta at the peak is
   near neutral. It adds rather than covering.

The coefficients live in [`src/glass.h`](src/glass.h), free of `windows.h`, so
the preview harness below applies the identical matrix and prints the numbers
back. There is no drop shadow, and the panel's padding is uniform on all four
sides: the reference is 588 tall around a 348 icon, 120 above and 120 below,
with no bottom band. The app name therefore cannot live inside the glass, and
sits on a small capsule of the same material below it.

**Checking our own output against Apple's.** The measuring script that produced
the table above runs equally well on MacTab's own render. Fitting the preview's
panel corner recovers a boundary that tracks the ideal 62 / 2.24 curve to within
a pixel or two, giving a corner extent of 0.360 of the panel height against the
reference's 0.364, and an extent ratio of 1.97 between panel and icon against the
reference's 2.02. That is the only honest definition of "1:1" available to a
project that cannot run its own output: the script that measured Apple returns
the same numbers on ours.

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

*Uninstall MacTab* is in the same menu, below Settings. It runs the installer's
own uninstaller, and is greyed out when MacTab is running as a standalone
executable rather than an installed copy.

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
themes over three wallpapers, a blue gradient plus solid black and solid white.
It uses the same `panel_layout.h` and `glass.h` the app does, not copies, so the
geometry and the colour matrix are the real ones. Only the pixel loops differ:
Direct2D on Windows, a box blur and a per-pixel matrix here.

It also prints, and asserts on, the numbers that decide whether the material is
right:

```
theme     wallpaper backdrop    panel   target  sat in sat out    label
dark      gradient     0.399    0.269  0.15-0.38   0.398   0.395     6.5:1
dark      black        0.045    0.154  0.15-0.38   0.000   0.038    12.6:1
dark      white        0.947    0.384  0.15-0.38   0.000   0.010     5.2:1
light     gradient     0.399    0.553  0.55-0.85   0.398   0.349     7.0:1
light     black        0.045    0.552  0.55-0.85   0.000   0.007     5.8:1
light     white        0.947    0.766  0.55-0.85   0.000   0.003    10.7:1
```

The panel has to land inside its band and the app name has to clear 4.5:1
against its capsule, on every wallpaper, or the build fails. Those two are the
difference between tuning a material and guessing at one.

That boundary is the point. The numbers are the judgement calls and they are
shared; the plumbing is what CI and a real machine are for.

This is the only part of the project that gets looked at before it ships, and it
has already earned its keep; it caught icons being scaled by canvas instead of
content, generated tiles landing at the same brightness as the glyph on them,
and a square selection highlight sitting next to squircle icons.

`src/panel.cpp` is the one file neither tool reaches, because C++/WinRT ships
only with the Windows SDK. Every review round so far has found that defects
concentrate there, which is not a coincidence. Two things narrow that hole:

- `tools/d2d-check.cpp` mirrors the Direct2D and DirectWrite calls panel.cpp
  makes, with no WinRT in it, so mingw type-checks that half. Nothing enforces
  that the mirror stays in step, and it says so at the top of the file.
- The `build` workflow compiles the whole thing on `windows-latest` on every
  push. That runner is the MSVC this project does not otherwise have. In 0.1.0
  it only ran on tags, and the first tag failed twice on real compiler errors
  that three review rounds had missed, so now it runs per push.

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
4. **The switcher list.** Tray, then *Log current switcher list*, and compare
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
