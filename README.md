# MacTab

A macOS-style application switcher for Windows 10 and 11.

Windows' Alt+Tab is a per-window grid. macOS' Cmd-Tab is a per-*application* row
of large squircle icons on a floating glass panel, ordered most-recently-used.
MacTab is the second thing, on Windows.

**Status: early. Not usable yet.** See [Milestones](#milestones).

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
to arbitrary antialiased geometry — which is what a macOS-radius glass panel
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

It does **not** cover the Composition rendering layer — the C++/WinRT headers
ship only with the Windows SDK, so those files are explicitly listed as skipped
rather than silently passing. `src/pch.h` deliberately excludes `winrt/base.h`
for this reason; files that need WinRT include `src/winrt_pch.h`.

A clean run means "this will probably compile", not "this works".

## Milestones

- [x] **M0** Skeleton — build system, tray, diagnostics logging
- [ ] **M1** Input capture — low-level keyboard hook, Alt state machine
- [ ] **M2** Window model — enumeration, app grouping, MRU
- [ ] **M3** Panel — Composition setup, tiered backdrop, squircle mask
- [ ] **M4** Icons — extraction, squircle processing, caching
- [ ] **M5** Visual fidelity — labels, highlight, animations, DPI
- [ ] **M6** Actions — Q/W/H, mouse, per-app window cycling
- [ ] **M7** Config — settings, icon theme packs, autostart
- [ ] **M8** Performance pass
- [ ] **M9** Installer and uninstaller

Ordered by risk rather than by user-visible value: the two pieces most likely to
not work at all (the keyboard hook and the backdrop) come first, so they fail
early rather than after everything is built on top of them.

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
- **Windows 10 pre-1803 is unsupported** — that is the Composition floor.

## Licence

Not yet chosen.
