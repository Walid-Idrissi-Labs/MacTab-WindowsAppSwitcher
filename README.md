# MacTab

A macOS-style application switcher for Windows 10 and 11.

Windows' Alt+Tab is a per-window grid. macOS' Cmd-Tab is a per-*application* row
of large squircle icons on a floating glass panel, ordered most-recently-used.
MacTab is the second thing, on Windows.

**Status: feature-complete, unverified on hardware.** Every milestone is
implemented, but the project is developed on macOS and has never been compiled
or run on Windows. Treat the first build as a bring-up exercise; see
[What to check first](#what-to-check-first).

## Goals

Two, and they pull against each other:

1. **Look genuinely like macOS.** Real backdrop blur, a macOS-sized corner
   radius (~24px, not DWM's 8px), squircle icons synthesised from whatever
   Windows hands us, MRU app grouping, `Q` to quit an app from the switcher.
2. **Cost nothing.** No polling, no timers at rest, no runtime dependencies.

Target budget:

| | |
|---|---|
| Idle CPU | 0.0% |
| Idle working set | < 20 MB |
| Binary | < 2 MB, no redistributables |
| Warm Alt+Tab → first pixel | < 16 ms |
| CPU during animation | ~0% (runs on the compositor thread) |

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

The image, squircle and panel-layout code is deliberately kept free of
`windows.h`, which means it can be compiled *and executed* natively:

```
./tools/preview/build.sh
```

runs a set of regression checks over that layer, then renders the icon pipeline
and a mock of the real panel to PNGs in `build-preview/out/`. It uses the same
`panel_layout.h` the app does, not a copy, so what it draws is the actual
geometry.

This is the only part of the project that gets looked at before it ships, and it
has already earned its keep; it caught icons being scaled by canvas instead of
content, generated tiles landing at the same brightness as the glyph on them,
and a square selection highlight sitting next to squircle icons.

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

None of this has run on Windows. Build it, launch with `--diag`, and work down
this list; the log names the code path taken at each decision point.

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
6. **Does the panel appear within a frame?** `gesture: reveal ... shown in N ms`.
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
