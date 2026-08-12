#pragma once

#include "pch.h"
#include "glass.h"
#include "image.h"

// Settings, icon theme packs and autostart.
//
// Plain INI at %LOCALAPPDATA%\MacTab\settings.ini rather than JSON: it is
// hand-editable without a parser dependency, and GetPrivateProfileString is in
// the OS. The file is written with defaults on first run so it doubles as its
// own documentation.

namespace mactab::config {

// Which display the panel opens on.
enum class PanelDisplay {
    ActiveWindow,   // the monitor holding the foreground window (default)
    Mouse,          // the monitor under the cursor
    Primary,        // always the main display
};

struct Settings {
    // Hold time before the panel appears. Below this a tap commits invisibly,
    // which is the macOS quick-switch behaviour.
    UINT revealDelayMs = 180;

    // Trigger on Left Alt only and ignore anything with Ctrl held. Required on
    // French/Arabic layouts where AltGr is Ctrl+RightAlt and is used constantly
    // for @ # { } [ ].
    bool leftAltOnly = true;

    // Logical tile size at 96 DPI.
    int tileSize = 128;

    // "auto" follows the system app theme; "dark" and "light" pin it.
    std::wstring theme = L"auto";

    // How the desktop behind the panel is grabbed: "auto", "duplication",
    // "bitblt" or "plain".
    //
    // Here for the same reason missionThumbnails is, and it is the most
    // important of these switches rather than the least. Without a usable grab
    // the panel draws a nearly opaque coat and there is no glass at all, and
    // which path works is a property of a machine's GPU, its compositor and its
    // display that cannot be established from here. "auto" tries each in turn
    // and takes the first that comes back with something in it.
    std::wstring captureSource = L"auto";

    // Group windows into applications (macOS behaviour). When false, every
    // window gets its own tile.
    bool groupByApp = true;

    // Where the panel opens. Default matches what v0.1 shipped with.
    PanelDisplay panelDisplay = PanelDisplay::ActiveWindow;

    // Bend the backdrop at the panel's rim, which is what makes it read as a
    // pane of glass rather than a blurred rectangle.
    //
    // Here as a switch because it is the one part of the material that goes
    // through a Direct2D path nothing off Windows can exercise: the generated
    // map is checked hard by tools/preview, but whether D2D1DisplacementMap
    // lines its two inputs up the way the documentation says is only knowable on
    // real hardware. If it comes out doubled or smeared on some driver, turning
    // this off in settings.ini gets a working panel back without a new build.
    bool glassRefraction = true;

    // The rim's second, sharper tap. Off means the bezel bends the same soft
    // backdrop the interior uses, which is what 0.4.1 shipped.
    //
    // Here for the same reason glassRefraction is: it is a Direct2D path nobody
    // off Windows can execute, and if CLSID_D2D1AlphaMask lines its inputs up
    // differently on some driver this gets a working panel back without waiting
    // for a build.
    bool glassRimTap = true;

    // The material, as the ini has it. Shipped defaults unless a key overrides
    // one. See glass_tune.h for the names and why they exist at all.
    //
    // Plain values rather than pointers: Params is trivially copyable, and the
    // panel takes a copy per gesture anyway.
    glass::Params glassDark  = glass::kDark;
    glass::Params glassLight = glass::kLight;

    // --- Mission Control ----------------------------------------------------

    // Mission Control at all.
    //
    // Off by default. Alt+Tab is the product and it replaces a key most people
    // press a hundred times a day; this takes over a second one, and taking over
    // two keys uninvited is a different proposition from taking over one. The
    // tray menu turns it on without a restart.
    bool missionEnabled = false;

    // Which chord opens it: "wintab", "winup", "both" or "none".
    //
    // Win+Tab by default. Win+Up is Aero Snap's maximise, so choosing it costs
    // the user a shortcut that does something else; Win+Tab is Task View, which
    // is the same feature by another name. See hotkey::Gesture.
    std::wstring missionGesture = L"wintab";

    // Space between two windows, how far each window of an app is offset
    // from the one in front of it in its pile, and how much room is left
    // between one pile and the next. Logical pixels at 96 DPI.
    float missionGap        = 26.0f;
    float missionFan        = 30.0f;
    float missionClusterGap = 88.0f;

    // Relax each app's windows into a cluster before arranging, and put the
    // app's icon and name under the cluster.
    //
    // It costs real estate: pulling a scattered app's windows together is a
    // promise to move them a long way, which shows up as a smaller scale in
    // tools/preview/mission. Worth it, because an arrangement you cannot read
    // by application is just a pile of rectangles.
    bool missionGroupByApp = true;

    // The wallpaper behind the arrangement: how far it is blurred and how far
    // it is pushed back. Dim is the alpha of the tint over it, 0 to 1.
    //
    // Sigma is 0 because macOS does not blur the desktop in Mission Control; it
    // dims it and lifts the windows off. It also decides what resolution the
    // backdrop is baked at, since a blur is what makes a stretched copy
    // invisible: see BackdropScale in mission.cpp, and the note in the shipped
    // settings.ini about what that costs in memory.
    float missionBlurSigma = 0.0f;
    float missionDim       = 0.45f;

    // How long the windows take to fly from where they are to where they land.
    UINT missionRevealMs = 260;

    // Replace a live preview with a properly filtered still once the windows
    // have landed, for the ones the compositor cannot reduce honestly.
    //
    // On by default, because the complaint this answers is that big windows come
    // out unreadable and the compositor has exactly one sampling mode. The cost
    // is that those previews stop moving; 0 keeps them live and soft.
    bool missionSharpPreviews = true;

    // Which thumbnail path to use: "auto", "shared", "snapshot" or "icon".
    //
    // Here for the same reason glassRimTap is. The shared-visual path goes
    // through an undocumented DWM export that nothing off Windows can execute,
    // and if it misbehaves on some driver this gets a working Mission Control
    // back without waiting for a build.
    std::wstring missionThumbnails = L"auto";
};

const Settings& Current();

// Reads settings.ini, writing it with defaults if absent.
void Load();

// Re-read ONLY the glass keys, from the tray.
//
// Deliberately not Load(), for the reason documented on SetPanelDisplay below:
// Load() reassigns the themes directory string, which the icon worker reads on
// its own thread. This touches the two Params and glass::g_tuning, and nothing
// off the UI thread reads either, so there is no race to guard.
//
// The point of it is the loop. Changing a number in the material used to mean a
// push, a CI build, an install and a restart, on a project whose author cannot
// see the result at all. Now it means saving the file and picking a menu item.
void ReloadGlass();

// Re-read the WHOLE file, not just the material.
//
// Same rule as ReloadGlass: everything it writes is read on the UI thread, and
// it deliberately does not touch the themes directory string that the icon
// worker reads on its own thread.
//
// This is what a save of settings.ini now runs. Until 0.8.5 only the glass was
// re-read, so every other key in the file appeared to do nothing until MacTab
// was restarted, which is indistinguishable from the key being broken. The
// caller applies what the running process holds separately: see ApplySettings in
// main.cpp.
void Reload();

// Put settings.ini back exactly as it ships, keeping what was there as
// settings.ini.bak, and re-read everything from it.
//
// The escape hatch for the file this release opens up. Every number in the
// material is now editable by hand, which is the point, and a material that can
// be tuned into an unreadable panel by hand needs a way back that does not
// involve knowing which of forty numbers did it.
//
// Refuses rather than proceeds if the backup cannot be written: the file being
// replaced is the only copy of whatever tuning is being abandoned.
//
// UI thread only, and deliberately not Load(): it re-reads the settings and the
// material without reassigning the themes directory string the icon worker reads
// on its own thread. The caller owns re-applying anything the running process
// took from the old file, which is what ApplySettings in main.cpp does.
bool ResetSettings();

// Change the display setting and persist it.
//
// Writes the single key with WritePrivateProfileString rather than rewriting
// the file: the profile APIs edit in place and leave every other key, and the
// whole annotated comment block, exactly as they were. That also makes the
// "user hand-edited the ini while MacTab was running" case a non-issue, because
// nothing the user typed is ever read back and rewritten.
//
// Deliberately does NOT re-run Load(). Load() reassigns the themes directory
// string, which the icon worker reads on its own thread, and reassigning a
// std::wstring under a concurrent reader is a real race. This touches one
// trivially-copyable field and nothing else.
bool SetPanelDisplay(PanelDisplay display);

// Same mechanism for the appearance. `value` is "auto", "light" or "dark".
//
// UI thread only, which is where the tray menu runs and where the panel reads
// it back. Nothing else touches it, so the wstring assignment has no reader on
// another thread the way the themes directory does.
bool SetTheme(const wchar_t* value);

// Turn Mission Control on or off and persist it. Same in-place single-key
// write as the two above, and for the same reasons.
bool SetMissionEnabled(bool enabled);

// --- Uninstall --------------------------------------------------------------
//
// Inno Setup writes an UninstallString under
//   Software\Microsoft\Windows\CurrentVersion\Uninstall\{AppId}_is1
// in HKCU for a per-user install and HKLM for a per-machine one. The AppId is
// frozen in installer/MacTab.iss and must not change, or an upgrade forks the
// lineage and leaves two entries in Add/Remove Programs.
//
// Empty when MacTab is running as a standalone exe rather than an installed
// copy, which is a supported way to use it, so the tray item is disabled rather
// than hidden in that case.
std::wstring UninstallCommand();

// Launch the uninstaller and return true if it started.
//
// The uninstaller posts WM_CLOSE to our host window before it removes anything,
// so this process exits on its own; there is deliberately no PostQuitMessage
// here. Quitting first would leave nothing running if the user then cancels the
// uninstall wizard.
bool RunUninstaller();

// --- Autostart --------------------------------------------------------------
//
// The Run key is the SINGLE source of truth: there is deliberately no mirrored
// boolean in settings.ini. The installer writes it, this reads and toggles it,
// and Task Manager's Startup tab shows the same thing, so the three can never
// disagree.

bool AutostartEnabled();
bool SetAutostart(bool enabled);

// --- Icon theme packs -------------------------------------------------------
//
// %LOCALAPPDATA%\MacTab\themes\<name>.png replaces the generated tile for an
// app. `name` is the executable stem (chrome, code, explorer) or the AUMID with
// path-illegal characters replaced by underscores. Some apps will never look
// right no matter how good the synthesis is; this is the escape hatch.

const std::wstring& ThemesDir();

// --- Settings file -----------------------------------------------------------

// Full path to settings.ini, for the tray item that opens it directly. Empty
// until Load() has run.
const std::wstring& SettingsPath();

// How many glass values the last read actually took out of settings.ini.
//
// Reported by the tray's reload. Somebody whose edit appeared to do nothing
// needs to know whether the file was read and ignored or never read at all, and
// on this project there is nobody standing next to them to ask.
int GlassOverrides();

// Returns an empty bitmap when there is no override for this app.
Bitmap LoadThemeOverride(const std::wstring& exePath, const std::wstring& aumid);

} // namespace mactab::config
