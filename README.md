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
   Win+Tab is Mission Control: every window spread out where you left it,
   with the desktops across the top.
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

**Material.** A second screenshot, over a photo of a building whose fins run
diagonally, which is what makes the material measurable at all: a row just inside
the panel and a row just outside it see the same structure shifted sideways by a
known slope. Align the two and fit the blur and the transfer that turn one into
the other. [`tools/measure`](tools/measure/measure.py) does it, and it runs on
MacTab's own screenshots too.

That gives a blur of sigma 14.6 on a panel 330 pixels tall, so 0.044 of the
panel's own height, and a transfer of `out = 0.709 * in + 0.068`, with mean luma
falling from 0.569 outside to 0.459 inside. Nearly three quarters of the
desktop's contrast survives. Earlier releases passed less than half of it through
a blur four to seven times too strong, which is why they read as frosted plastic
no matter what else got tuned.

Four things make that read as glass rather than as a blurred rectangle, in the
order they matter:

1. **The rim refracts.** A pane with thickness bends what is behind it, so
   content just outside the panel is pulled inward and squeezed against the
   edge. The surface profile is the convex squircle
   `h(x) = (1 - (1-x)^4) ^ (1/4)`, which is what Apple uses and is chosen for its
   join: it meets the flat interior with zero slope, so the bending fades out
   instead of stopping at a visible ring. Displacement is one Snell event at
   `n = 1.5`, giving a peak of 12.5px about 0.7px in from the edge, and about
   2.7:1 of compression across the bezel. Built as an 8-bit map on the CPU and
   handed to `D2D1DisplacementMap`.

   The bezel bends a much sharper copy of the desktop than the middle does,
   sigma 2 against sigma 8, faded into the interior over the bezel width. Blur
   and refraction are different effects, and a lens bending an image that has
   already been softened to nothing produces a smear rather than a bend. That is
   what separates a pane of glass from a frosted panel, and it is the thing this
   material was missing for four releases.

   0.4.0 tried a second tap and deleted it as redundant, because both taps went
   through the same quarter-resolution downsample: the "sharp" one was a sigma of
   0.5 followed by a 4x bilinear upscale, and the upscale alone softens more than
   that, so the two came out indistinguishable. It was not redundant, it was
   being thrown away one stage before it was used. The rim tap runs at full
   resolution now. `rimtap-on-4x.png` and `rimtap-off-4x.png` in the preview
   output are the comparison, and the build fails if the two agree.
2. **You can see through it**, and this is the number the project kept getting
   wrong. It is now measured rather than argued about, by
   [`tools/measure`](tools/measure/measure.py): the reference puts the panel as a
   horizontal band over a photo whose structure runs diagonally, so a row just
   inside the panel and a row just outside see the same content shifted sideways
   by a known slope. Align the two, then fit the blur and the transfer that turn
   one into the other.

   | | Apple, measured | 0.3 | 0.4.0 | now |
   |---|---|---|---|---|
   | blur sigma, as a fraction of panel height | 0.044 | 0.30 | 0.17 | 0.047 |
   | end gain | 0.71 | 0.42 | 0.53 | 0.71 |
   | end bias | 0.068 | 0.064 | 0.064 | 0.068 |

   That is the transfer. What a person actually sees is coarser than that, so
   the preview also measures it directly: a bar target of decreasing period sits
   under the panel and the amplitude of each period inside the glass is divided
   by the same bars outside it.

   | bar period | 96px | 64px | 48px | 32px | 16px |
   |---|---|---|---|---|---|
   | amplitude that survives | 74% | 70% | 59% | 30% | 19% |

   The build fails if the coarsest drops under half. Structure at the scale of a
   window on a building comes through at about three quarters, which is what
   "you can count the floors" means as a number.

   Three releases blurred at 34, 52 and 30 on the reasoning that the macOS
   backdrop is unrecognisable mush. It is not: in the reference you can count the
   floors of the building behind the panel. The preview now fails the build if
   the end gain drops under 0.65.
3. **The operating point moves, and it moves with the desktop.** One affine
   transfer cannot serve both a white wallpaper and a black one: the panel either
   washes out or turns into a slab. Apple does not use one either, it flips
   treatment on backdrop luminance. What is available here and not to Apple is
   that the backdrop is a *frozen frame*, so its mean luma is known before a
   pixel is drawn. `Adapt()` bends the bias per gesture. The gain never moves:
   how much of the desktop's contrast survives is a property of the material,
   where that window sits is a property of what is behind it today.

   Up to 0.4.1 that landing point was a hard clamp into a per-theme band, and
   that was the single biggest reason the panel read as a card. Past either end
   of the band it was a constant plus texture, so a bright desktop and a very
   bright desktop gave exactly the same panel. A constant that ignores what is
   behind it is a UI colour, not a material. The band is a soft knee now: about a
   third of any excursion past it survives, so brighter reads brighter and darker
   reads darker. The light theme keeps its ceiling nearly hard, because the one
   thing it must not do is go white.

   Each piece of glass adapts on its own backdrop rather than the panel's. The
   app name's capsule sits somewhere else on the desktop and can be over
   completely different content; giving it the panel's operating point put it at
   1.7:1 over a half-white wallpaper, which the new test surfaces caught.
4. **Saturation, well past unity, and a lit edge that follows the surface.**
   Relative saturation goes 0.737 outside the panel to 0.642 inside on the
   reference, a ratio of 0.87, and the light material lands on 0.863.
   `CLSID_D2D1Saturation` is no use for this at all: its property is documented
   over `[0, 1]`, so it can only ever desaturate, and asking for more clamps to
   identity without complaining. The rim's measured lift over the adjacent
   interior is +33 luma at the top, +23 at the bottom and +22 at the sides, which
   is left-right symmetric, so the light is straight overhead. Modelled as
   ambient plus an upward-facing lobe plus a thin filament, all evaluated from
   the surface normal, so the corners sweep between the top and side values on
   their own. It adds rather than covering.

   All three are then scaled by the luma of the backdrop behind that piece of
   rim, sampled from the sharp capture at 16px and interpolated. A highlight is a
   reflection, and a rim that is the same brightness over a black wallpaper and a
   white one is a painted-on border rather than a lit surface. Over a wallpaper
   that is black on one side and white on the other, the two ends of the same
   edge come out a factor of five apart, which the build asserts.

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

## Where the icons come from

An app icon is the one thing on the panel that MacTab does not draw, so all the
quality is in picking the right source. The obvious route, asking the shell for
a 256px image, is the worst of the four for reasons that are not obvious at all.

**An executable's icon is read out of the file.** `RT_GROUP_ICON` is the same
icon group Explorer and the taskbar draw from, and taking it directly gives the
frames as they were authored, the largest one picked deliberately, and the true
native size. Since Vista a 256px frame is a whole PNG rather than a bitmap, so it
goes through WIC with its alpha intact.

The shell will not enlarge an icon past the largest frame an app actually ships.
Ask it for 256 from an app whose best frame is 48 and you get a 48px mark adrift
in a 256 canvas, with nothing in the result to tell you that is what happened. It
also hands the icon over as a GDI bitmap, which regularly arrives 32-bit with
every alpha byte zero, because the icon's transparency lived in an AND mask or
the handler drew through plain GDI. Read literally that is an invisible icon, so
it has to be forced opaque, which turns the padding around a small mark into
black. The shell stays as the fallback, because it is the only thing that covers
apps whose icon is not in the exe at all: a custom icon handler, or a
`DefaultIcon` pointing somewhere else.

**A packaged app's icon is read out of its package.** Binding
`shell:AppsFolder\<AUMID>` returns the logo already composited onto the
background colour from the app's manifest. Windows does not draw it that way
anywhere the user sees it; the taskbar and the built-in switcher use the unplated
asset. So MacTab resolves the AUMID to a package family, reads `Square44x44Logo`
and `BackgroundColor` out of `AppxManifest.xml`, and picks the best variant on
disk. Resolution decides, since not being able to get a big enough icon is the
whole reason for going there; between two of the same size the unplated design
wins. Every package in the family is searched, not just the main one, because
Store apps are split into a main package plus resource packages and the larger
assets often live in one of the latter. `BackgroundColor` becomes the colour of
the tile behind the mark, which is what Windows plates the same app with.

**Whatever arrives, anything painted behind the mark comes off.** Both of the
bad sources above hand over an icon with its background baked in, and both look
like the same defect: a small mark adrift in a big coloured square. A flood fill
runs inward from the border and takes out whatever flat colour it finds there. A
flood rather than a colour key, because a key would also punch out any part of
the artwork that matched, which for black padding means every dark pixel in the
icon. Padding reaches the border; the dark half of a logo does not, and the
flood will not cross transparency to get to it.

It runs twice, because two layers of background is a real combination rather
than a hypothetical: the shell pads an icon that already has a plate baked into
it out to a fixed canvas with transparency. The transparency is what the border
sees, so the first pass finds nothing to do and the plate only becomes reachable
once the padding has been cropped off.

What is *not* stripped is a plate the app meant to have. An icon that is a black
square with a white mark on it keeps its black, because an icon adrift in
padding is a mark on its own and takes the artwork path, where there is no tile
to colour in the first place.

**Then the mark is measured, not the canvas it came in.** How full the mark's own
bounding box is decides whether it stands on its own or gets a tile generated
behind it: a circle fills 79% of its box, a rounded square nearly all of it, a
letterform about a third. Measuring the canvas instead means a small but
genuinely full-bleed icon reads as a sparse glyph and gets a synthesised plate
behind artwork that already had its own background.

Enlargement is Catmull-Rom rather than bilinear, and stops at 2.5x. Past that a
mark is drawn smaller instead. A small sharp icon reads as an icon; a large soft
one reads as a mistake.

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
| `Win+Tab` | Mission Control: every window, spread out. Off until you switch it on |

A quick tap and release switches to the previous application without the panel
ever appearing, which is what macOS does. The panel only shows if Alt is held
past `RevealDelayMs`.

### Mission Control

Off by default. Turn it on in the tray menu under *Settings*, or set
`MissionEnabled=1`. Alt+Tab is the product, and taking over a second key
uninvited is a different proposition from taking over one.

`Win+Tab` then spreads every window out so none of them overlap, on every
display at once, and puts the virtual desktops in a glass bar across the top.
It is a toggle, not a hold. Mission Control is a place you are in.

`MissionGesture=winup` moves it to `Win+Up` instead, or `both` for either.
Win+Tab is the default deliberately: it is Task View, which is this feature under
another name, where Win+Up is Aero Snap's maximise and taking that costs you a
key that already does something else.

| In Mission Control | |
|---|---|
| Click a window, or `Enter` | Go to it. If it is on another desktop, that takes you there |
| `←` `→` `↑` `↓` | Move between windows, geometrically rather than through a list |
| `Tab`, `Shift+Tab` | Next and previous window |
| Click an app's icon, scroll up, or `↓` on a pile | Spread that app's pile so its windows can be told apart |
| Click the background, scroll down, `↑` or `Esc` | Put the pile back, or leave |
| Click a desktop, or `Ctrl+←` `Ctrl+→` | Look at that desktop's windows |
| `Ctrl+Win+←` `Ctrl+Win+→` | The same thing, so the shell's own chord keeps its meaning |
| Click the `+` | Add a desktop, without leaving |
| Click the `⨯` on a desktop | Close it, without leaving |
| Drag a window to another display | Move it there, keeping its size and its proportional place |
| Drag a window onto a desktop | Move it there, if Windows allows it (see below) |
| `Esc` | Leave |

**Looking at a desktop is not the same as being on it, until you leave.** Clicking
one shows its windows sliding in; the machine stays where it is, because the
overlay belongs to the desktop it was built on and switching underneath it would
strand a full-screen window somewhere nobody is looking. Leaving on purpose,
with `Esc`, the toggle, or a click on the background, is what makes it real and
takes you to the desktop you were looking at. Losing focus, a display appearing,
a session lock or Alt+Tab taking over do not: those are not decisions, and none
of them should move your desktop.

`Ctrl+Win+←` and `Ctrl+Win+→` never reach the shell while it is open, for the
same reason, and are handed to the strip instead.

**Adding and closing a desktop happen without leaving.** Both go through the same
keyboard shortcuts you would press, both move the view, and these overlays keep
whichever desktop they were assigned to, so each one is: make the change, wait for
the shell to admit it happened, bring the overlays across to wherever the view
ended up, rebuild in place.

Closing a desktop other than the one you are on needs the view to go there first,
because the public API can only close the current one. It goes there, confirms it
arrived against the shell's own record, and only then sends the close. If that
confirmation never comes, nothing is closed at all. Closing the wrong desktop is
not something you can undo.

It comes back afterwards, too. The shell leaves you on a neighbour of the desktop
it removed, and deleting a desktop is not a request to go anywhere.

**It stays honest while it is open.** A window that closes or minimises disappears
within a frame and everything else relaxes into the space it left. A window that
opens, and a desktop created or removed by anything else, show up within half a
second, because there is no event for "a window has become eligible for the
switcher" and the ones that come closest fire for every menu and splash screen in
the session, so arrivals are noticed by looking rather than by listening. Both
watchers are installed when it opens and removed when it closes; the idle budget
for this process is still zero.

**The arrangement.** One scale for every window, so a large window still looks
large next to a small one and every aspect ratio is exact. Windows start where
they really are and shove each other apart until nothing overlaps, which is
what lets you find something by remembering where you left it. There is a
number for that: every pair of windows votes on whether the side it was on is
the side it ended up on. A typical desktop scores 0.80, thirty windows 0.87.

**Applications are piles.** One app's windows are fanned, each offset behind the
one in front, so the most recent is almost entirely visible and the others peek
out from under it. That is what macOS does with grouping on, and it is worth a
lot of room: thirty windows in six apps come out at a scale of 0.77 as piles
against 0.27 when each window has to have its own space. The app's icon and name
sit under the pile. `MissionGroupByApp=0` arranges purely by position instead.

Each window carries a soft shadow that travels with it, and hovering one outlines
it in your Windows accent colour. The outline is drawn for the window it is going
round, at that window's size on that window's display, rather than being one
texture stretched to fit: a stretched one keeps its corners at a fixed size in
source pixels, so it carried the wrong corner and the wrong stroke width on every
display but one, and came out crushed on anything narrower than twice its corner.

The application's name sits under its icon with a shadow and nothing behind it.
It had a capsule, because a name over a wallpaper is unreadable on some fraction
of all desktops, and a capsule costs the thing the label is for: macOS puts a name
under an icon, not a pill under an icon.

**What you see in each tile** depends on what the machine supports, and the log
names the tier that won:

| Tier | What it is | What you get |
|---|---|---|
| `shared visual` | `dwmapi` ordinal 147, undocumented | Live contents, and the windows fly out from where they really are |
| `snapshot` | `PrintWindow`, fully documented | One still picture per window, taken when you press the keys |
| `icon` | Nothing at all | The app's icon on a window-shaped card |

The first is the only one that can be animated by the compositor, which is why
it is first: that flight is most of what makes the gesture read as Mission
Control rather than as a dialog. If it misbehaves on your driver, set
`MissionThumbnails=snapshot` and everything else stays as it is.

**The previews are sharpened once the windows have landed.** The compositor has
exactly one sampling mode, bilinear, and bilinear is only a correct reduction down
to about 2:1, where each output pixel averages a 2x2 block. Past that it skips
source pixels, and a 4K window in a 400-pixel tile skips nine out of every ten.
Asking DWM for a different destination size does not help: a thumbnail is a live
connection to the source's own surfaces, so the sampling happens once, at the end,
with whatever the whole visual tree composes to.

There is one more thing that has to be said out loud, because it is documented and
it is easy to miss: if no visual in a composition tree sets an interpolation mode,
the default for the entire tree is **nearest neighbour**. That is not a soft
downscale, it is decimation. Every thumbnail is now explicitly asked to sample
bilinearly, which reaches the visuals DWM builds inside its own because the mode
inherits down the subtree.

Above roughly a 2:1 reduction the live preview is then replaced with a still of
the same window, taken at twice the size it is shown at and reduced by the box
filter in `image.cpp`, which is the one honest downscaler here. Those previews
stop being live: a video plays until the sharpening lands and then holds its last
frame. `MissionSharpPreviews=0` keeps them live and soft.

**The bar is the same glass the switcher is made of.** Not a similar one: the same
code, in `src/glass_draw.cpp`, with the same measured numbers, so tuning the
material tunes both. Three things about this piece are its own. It runs past the
screen on the left, right and top, so its dark stroke and its lit edge fall off
the display and only the bottom edge is ever seen, which is the profile macOS
shows. It is cut from the wallpaper at full resolution rather than from the
overlay's own backdrop, because the rim bends a sharp image and a quarter-scale
copy would make that second bend indistinguishable from the first. And the strip
is dimmed before the material sees it, by the same amount the backdrop is, so the
glass adapts to the scene you are actually looking at instead of to a brighter one
that only exists in a file.

**The background is the wallpaper, not the screen.** Mission Control lifts the
windows off and leaves the desktop, so capturing the screen would put every
window into the blurred backdrop as well as into the arrangement, and each one
would appear twice. It also means there is nothing to capture on the reveal
path.

It is not blurred by default, because macOS does not blur it either: it dims it
and lifts the windows off. That has a cost worth knowing about. The blur is what
used to make it safe to bake the backdrop at a quarter of the screen's resolution
and stretch it back; with nothing to hide the stretch behind, it is baked at full
size, which on a 4K display is about 33 MB per screen. `MissionBlurSigma` buys
that back: above 1 it halves, above 4 it quarters.

**The windows go back down when you leave.** The reveal lifts them off the desktop
and the dismissal lowers them onto it, so activating a window has its tile land on
the real window at the moment the real window comes forward. The one path that
skips it is committing a desktop switch, because the shell animates that itself
and an overlay still on screen while the desktop slides underneath looks like a
fault.

**It opens on every display**, each with its own arrangement of that screen's
windows and its own copy of the bar, which is what macOS does.

**Virtual desktops** are read from the registry and driven with the same
keyboard shortcuts you would press. Everything here is public API. The private
interface that most tools use is not: its layout was restructured in 24H2
without its identifier changing, so the type check passed, the call went to the
wrong function, and the tools using it crashed rather than failed. That is the
worst failure this project can have, because it would be found out weeks later
on a machine nobody here can see.

### Settings

Tray icon, then *Settings*. Panel display (active window's display, the display
with the mouse, or always the main display) and appearance (follow Windows,
light, dark), and *Glass backdrop*, which is **off by default** and is what
turns the material on: unchecked, the panel is a plain tinted plate. All three
take effect on the next Alt+Tab.

Saving `settings.ini` re-reads the whole file and applies it, and *Reload
settings.ini* on the main menu does the same on demand and reports how many
glass values it found set, which is the one thing you cannot see from outside
when an edit looks like it did nothing. Up to 0.9 both of those re-read only
the material, and only the material's optics ever reached the screen without a
restart; see the note under *Tuning the glass yourself*.

*Reset settings.ini* is under it, and puts the file back exactly as it ships.
It asks first, and it keeps what you had as `settings.ini.bak` rather than
throwing it away; if that copy cannot be written it does nothing at all, since
the file being replaced is the only record of whatever tuning is being
abandoned. Everything the running process took from the file is re-applied on
the spot, so there is nothing to restart.

*Uninstall MacTab* is in the same menu, below Settings. It runs the installer's
own uninstaller, and is greyed out when MacTab is running as a standalone
executable rather than an installed copy.

Everything else is in `%LOCALAPPDATA%\MacTab\settings.ini`, which is written with
defaults and comments on first run:

| Key | Default | |
|---|---|---|
| `SettingsVersion` | 3 | Written by MacTab so it knows which defaults a file predates. It never overwrites a value you have changed yourself |
| `RevealDelayMs` | 180 | How long Alt must be held before the panel appears |
| `LeftAltOnly` | 1 | Ignore Right Alt and anything with Ctrl held. Keep this on if AltGr types `@ # { } [ ]` on your layout |
| `TileSize` | 128 | Icon size in logical pixels at 100% scaling |
| `Theme` | auto | `auto`, `light` or `dark` |
| `Glass` | 0 | The material itself, **off by default**. The panel is a plain tinted plate: no grab of the desktop, no blur, no refraction. 1 turns the glass on, as does *Glass backdrop* in the tray menu under *Settings* |
| `CaptureSource` | auto | How the desktop behind the panel is grabbed: `auto`, `duplication`, `bitblt` or `plain`. **If the panel is a flat grey slab, this is the key to try**, in the order `plain`, `bitblt`, `duplication` |
| `GroupByApp` | 1 | 0 gives one tile per window instead of one per application |
| `PanelDisplay` | active | `active`, `mouse` or `main` |
| `GlassRefraction` | 1 | Bend the desktop at the panel's rim. Set to 0 if the edge looks doubled or smeared on your machine |
| `GlassRimTap` | 1 | Bend a sharper copy at the rim than in the middle, which is what makes the edge read as a lens. Set to 0 if the bezel looks doubled or banded |
| `MissionEnabled` | 0 | 1 turns Mission Control on. Also in the tray menu |
| `MissionGesture` | wintab | `wintab`, `winup`, `both` or `none`. `winup` costs you Aero Snap's maximise |
| `MissionGroupByApp` | 1 | 0 arranges purely by position and ignores which app owns what |
| `MissionGap` | 26 | Space kept between windows, logical pixels |
| `MissionFan` | 30 | How far each window of an app is offset from the one in front of it in its pile |
| `MissionClusterGap` | 88 | Space between one app's pile and the next |
| `MissionBlurSigma` | 0 | How soft the wallpaper behind the arrangement goes. Also decides what resolution it is baked at: raise it if Mission Control costs more memory than you want |
| `MissionDim` | 0.45 | How far back the wallpaper is pushed, 0 to 1 |
| `MissionRevealMs` | 260 | How long the windows take to fly to their places |
| `MissionSharpPreviews` | 1 | Replace a live preview with a properly filtered still once the windows have landed, for anything the compositor cannot reduce honestly. 0 keeps them live and soft |
| `MissionThumbnails` | auto | `auto`, `shared`, `snapshot` or `icon`. Drop to `snapshot` if the windows come out blank or misplaced |

### Tuning the glass yourself

Every number in the material is a key in the same file, and every one of them
is written into it, commented out, holding the value this build ships with:

```ini
; How soft the backdrop goes. The single most decisive number in the
; material: it decides whether you can see the desktop through the panel
; or only that something is behind it. Range 0 to 60.
;GlassBlurSigma=8
```

To change one, copy the line, take the semicolon off the copy and edit that.
The original stays above it, so what you are trying always sits next to the
default it replaced, which is the thing you actually want when the fourth
number you have changed makes it worse.

*Open settings.ini* in the tray menu opens the file in whatever the shell has
associated with `.ini` (Notepad on a machine with no override). Saving it
re-reads the file on its own; *Reload settings.ini* does the same from the menu
and reports what it found. *Reset settings.ini* is the way back.

**The glass is off until you turn it on.** `Glass=1`, or *Glass backdrop* in the
tray menu, and everything below applies from the next Alt+Tab. Left off, the
panel is a plain tinted plate and the desktop is never grabbed, so the gesture
costs nothing. That plate is not a second way of drawing the panel: it is the
path the material already falls back to when there is no picture of the desktop
to be had, so it cannot drift from the version people actually use.

**If the glass is on and the panel is still a flat grey slab, do not tune
anything yet.** It means the
panel has no picture of the desktop behind it, so it is drawing a coat that is
96% opaque and there is nothing there for any of these numbers to act on. MacTab
now says so with a balloon the first time it happens in a session, and
`CaptureSource` in `settings.ini` is the way out: try `plain`, then `bitblt`,
then `duplication`, saving after each.

That state has two causes, both fixed in 0.9 and 0.9.1 as far as they can be
fixed from a machine that cannot run the program. A grab that arrived after the
panel was revealed used to be thrown away, leaving the whole gesture flat; it is
now taken whenever it lands and the backdrop is re-baked. And nothing ever
looked at the captured pixels, so a blit that succeeded and returned black,
which is a known state under DWM, was treated as a genuinely black desktop and
blurred into a grey slab. Grabs are now assessed for whether there is a picture
in them, and three paths are tried in order until one comes back with something.

Desktop duplication deserves a note of its own, because it explains the shape of
the complaint. It hands over a frame when the desktop has *changed* and never
otherwise, so on a still screen it cannot deliver at all, while a window
animating behind the panel makes it return at once. Glass while something moves
and grey the rest of the time is what that looks like from the outside.

Until 0.9 the reloading did not work either, and it is worth being precise about
why, because the file was documented as tunable for four releases while most of
it was not.
The panel copies the material into its own theme, and that copy was only
refreshed when Windows switched between light and dark. Reloading changed the
numbers in the settings and nothing else, so the six shared `Glass*` optics,
which are read at draw time, took effect, and all thirty-six `GlassDark*` and
`GlassLight*` values did nothing at all until MacTab was restarted. Both the
save and the menu item also re-read only the material, so every key outside it
needed a restart too. The theme is now rebuilt on every gesture and the whole
file is re-read on every save.

Shared by both appearances, in logical pixels at 100% scaling:

| Key | Default | |
|---|---|---|
| `GlassBlurSigma` | 8 | How soft the backdrop goes. The most decisive number in the material |
| `GlassRimBlurSigma` | 2 | The bezel's own, sharper blur |
| `GlassBezelWidth` | 14 | How far in from the edge the surface curves |
| `GlassDepth` | 24 | How thick the pane is. Drives how hard the rim bends |
| `GlassMaxDisplacement` | 16 | Ceiling on that bending |
| `GlassRimSpan` | 13 | How far in the lit edge reaches |

Per appearance, prefixed `GlassDark` or `GlassLight`: `Saturation`, `Gain`,
`Bias`, `TintR` `TintG` `TintB` `TintA`, `RimAmbient`, `RimLobe`, `SpecLine`,
`RimEnvFloor`, `RimEnvGain`, `RimOuterDark`, `TargetMin`, `TargetMax`,
`KneeBelow`, `KneeAbove`, `FallbackAlpha`. So `GlassDarkTintA=0.03` thins the
dark tint further than the 0.06 it already ships at. Values are clamped to
ranges the rest of the material was designed inside, and out-of-range or
unparseable ones are logged rather than ignored silently. A comma typed where
the decimal point goes is read as a decimal point rather than as the end of the
number, which is what it would otherwise be on a French keyboard.

That table is not maintained by hand any more, and neither is the file: both
come from the one table of names, ranges and defaults in
[`src/glass_tune.h`](src/glass_tune.h). `GlassDepth` is the reason it works
that way. It has been documented here and in the shipped settings file since
the keys existed, and until 0.8.4 the code looked for `GlassGlassDepth`, so the
one number in the material anybody was likely to try changing by hand was the
one key that did nothing.

This exists because of the constraint at the top of this file: MacTab is written
on a Mac and cannot be run there, so the only person who can see the glass is
whoever is running it. Four releases went round the loop of change a number,
push, wait for CI, install, look, report, and that loop being twenty minutes long
did more damage than any single number in it. Now it is seconds.

The same names work in the preview harness, minus the `Glass` prefix:

```
./build-preview/preview out/ --set dark.gain=0.74 --set blursigma=5
```

so a value that looked right on Windows can be replayed here, measured against
the reference, and shipped as a default if it holds up. One table of names in
[`src/glass_tune.h`](src/glass_tune.h) drives both sides, so they cannot drift.
`--diag` logs the material every run, which means a screenshot arrives with the
numbers that produced it.

The preview also generates the settings file's glass section and reads every
line of it back through the same parser the running program uses, checking that
each one reproduces the shipped value exactly. A comment claiming a default the
binary does not hold is worse than no comment, because somebody would tune
against it, and this is a program whose author cannot run it.

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

builds and runs two harnesses, then writes PNGs to `build-preview/out/`: the
icon pipeline case by case, the whole panel in both themes over eleven surfaces,
and the Mission Control arrangement over seven synthetic desktops. It uses the same `panel_layout.h` and `glass.h` the
app does, not copies, so the geometry and the colour matrix are the real ones.
Only the pixel loops differ: Direct2D on Windows, a box blur and a per-pixel
matrix here.

The surfaces are chosen to break the material rather than to flatter it:

| Surface | What it is there to catch |
|---|---|
| gradient | the everyday case, and where the saturation ratio is judged |
| black, white | the two ends of the range the adaptation exists for |
| bars | diagonal high-contrast structure, the only honest test of the lens |
| red | whether the environment's hue reaches the panel at all |
| split | black one side, white the other: whether the rim reflects or is painted on |
| solids | five saturated bands under one panel, including a hard colour boundary |
| shapes | large circles and rectangles, the brief's own visibility test |
| text | letterforms through the bezel, both polarities |
| detail | a bar target of decreasing period, measured rather than looked at |
| ramp | strong colour gradients |
| photo | sky, sun and a building whose windows should stay countable |

The Mission Control desktops are picked the same way, each one a failure the
arrangement can have: thirty windows, a window larger than the screen next to
twenty small ones, five windows of one app, a stack of eight identical maximised
windows with no direction to separate along, and a clean two-column desktop
whose only job is to catch the ordering. It asserts that nothing overlaps,
nothing escapes the region, every window shares one scale, an app's cluster
never crosses another's, the same input gives byte-identical output twice, and
the spatial agreement stays above 0.60:

```
desktop    windows   scale  passes   agree    fill
typical          6   0.700      13    0.80   54.6%
crowded         30   0.579      59    0.87   45.8%
lopsided        21   0.534      49    0.92   58.9%
columns          6   0.871       1    1.00   57.5%
```

It prints, and asserts on, the numbers that decide whether the material is right:

```
theme     wallpaper backdrop    panel   target  sat in sat out    label
dark      gradient     0.419    0.365  0.16-0.44   0.324   0.388     5.6:1
dark      black        0.010    0.130  0.16-0.44   0.000   0.019    14.4:1
dark      white        0.975    0.552  0.16-0.44   0.000   0.003     2.7:1
                      label needs its shadow: 2.7:1 bare, 5.0:1 shadowed
dark      split        0.498    0.420  0.16-0.44   0.125   0.064    14.2:1
                      rim reflects: 0.016 dark end, 0.079 bright end, 5.0x
```

The build fails if the panel does not land where `Adapt` predicts, if a brighter
desktop does not give a brighter panel, if the rim stops reacting to what is
behind it, if a red desktop leaves the panel grey, if the coarsest bars lose more
than half their contrast, if the rim's sharper tap changes nothing, or if the app
name drops under 4.5:1 with the shadow it gets. Those are the difference between
tuning a material and guessing at one.

That boundary is the point. The numbers are the judgement calls and they are
shared; the plumbing is what CI and a real machine are for.

This is the only part of the project that gets looked at before it ships, and it
has already earned its keep; it caught icons being scaled by canvas instead of
content, generated tiles landing at the same brightness as the glyph on them,
and a square selection highlight sitting next to squircle icons. Two of its
cases are not shapes at all but reproductions of how a real icon arrives
damaged: a mark flattened onto black padding, and a logo composited onto its
manifest's background colour. Both used to come out as a dot in the middle of a
coloured square, which is exactly what they look like on Windows, and now both
are visibly fixed in a PNG before anything is tagged.

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
- [x] **M10** Mission Control: the spread, the desktops strip, `Win+Tab`

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
   laptops and HDR displays are the likely failures. `capture none` on the
   `panel: backdrop baked` line is the one to watch for: it means the panel is
   showing its fallback coat, which is 96% opaque, so the material is not being
   applied at all and no amount of tuning will show through it.
6. **The glass over extremes.** A pure white wallpaper and a pure black one. The
   material compresses the backdrop's range specifically so neither blows
   through, and the app name has to stay readable over both.
7. **A long app name.** "Visual Studio Code" or similar, with that app first and
   last in the row. The label is anchored under its icon and clamped inside the
   panel, so at the ends it should slide inward rather than overhang.
8. **Which thumbnail tier Mission Control got.** The log names it at startup.
   `shared visual` is the good one and the only one that animates; `snapshot`
   means the undocumented path was refused, which is worth knowing about.
9. **Win+Tab.** The Start menu must not open when you let go of Win, the
   arrangement must have no two windows overlapping, and the desktops strip must
   show the right number with the right one highlighted. *Log virtual desktops*
   in the tray menu prints what was read out of the registry, which is the one
   thing about desktops that cannot be reasoned about from here.
10. **The desktops, from inside.** With three of them: click the third, press
   `Esc`, and you should be on the third. Click the `+` and Mission Control
   should stay up with a fourth in the strip. Click the `⨯` on a desktop you are
   not on and it should go, with nothing else moving. If any of those does
   nothing, the log says whether the shell ever admitted the change happened.
11. **Open it, leave a desktop, open it again.** The second one must not take you
   back to the first desktop. These overlays are made once, a window keeps its
   desktop for life, and asking Windows to show one is asking it to go there.
12. **Does the panel appear within a frame?** `gesture: reveal ... shown in N ms`.
   This is the one performance claim that could not be verified from
   documentation, so it is measured rather than assumed.

## Performance

Everything below is a design property rather than a measurement, since nothing
has been profiled on real hardware yet. The `--diag` log carries timings for
each of these so the claims can be checked rather than trusted.

| Claim | How it is achieved |
|---|---|
| No timers at rest | The reveal delay, armed on the first Tab and killed on commit. Mission Control adds two while it is on screen, for the collapse and for watching the desktops, and both are killed when it closes. |
| No polling | MRU comes from one `EVENT_SYSTEM_FOREGROUND` hook. `EVENT_OBJECT_DESTROY` fires for every menu and tooltip in the session, so it is hooked only while Mission Control is open, and filtered to the handful of windows it is showing. |
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
- **Dragging a window to another desktop only works for a few windows.** The
  public API moves a window between desktops only when the calling process owns
  it, which is almost never what you want to drag. The drop is attempted and the
  window snaps back when Windows refuses, rather than pretending it worked. The
  interface that could do it properly is the one described above.

  Closing a desktop other than the one you are on is a different case and does
  work: the view goes there first, which is allowed, and the close is only sent
  once the shell's own record says it arrived. What that costs is a moment of the
  desktop actually switching underneath the overlay before it comes back.
- **The desktop miniatures show where the windows are, not what is in them.**
  A window on another desktop is shell-cloaked and DWM will not compose a
  cloaked window through any path available to a normal process, so each one is
  drawn as its own shape at its own position with its app icon on it.
- **Windows 10 window thumbnails have square corners**, because that is the
  shape Windows 10 draws them. A rounded composition clip needs build 17763,
  above this project's floor, and the visual DWM hands back for a thumbnail
  cannot be masked in any case.
- **Minimised windows are not in Mission Control**, which is also true on macOS.
- **Mission Control opens on every display**, as macOS does, rather than only on
  the one holding the focused window. Each display gets its own arrangement of
  its own windows, its own DPI and its own copy of the strip; what it does not
  get is a separate set of desktops per display, because Windows does not have
  those.
- **The backdrop is held in memory while Mission Control is enabled**, at the
  screen's own resolution, which on a 4K display is about 33 MB per screen. That
  is the price of not blurring it. `MissionBlurSigma` above 1 halves it and above
  4 quarters it.
- **HDR displays** fall back to GDI capture. Desktop duplication hands back
  scRGB float rather than BGRA when HDR is on and does not convert, so the
  duplication path bails rather than showing wrong colours.

## Licence

Not yet chosen.
