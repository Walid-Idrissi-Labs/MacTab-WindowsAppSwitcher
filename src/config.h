#pragma once

#include "pch.h"
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

    // Group windows into applications (macOS behaviour). When false, every
    // window gets its own tile.
    bool groupByApp = true;

    // Where the panel opens. Default matches what v0.1 shipped with.
    PanelDisplay panelDisplay = PanelDisplay::ActiveWindow;
};

const Settings& Current();

// Reads settings.ini, writing it with defaults if absent.
void Load();

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

// Returns an empty bitmap when there is no override for this app.
Bitmap LoadThemeOverride(const std::wstring& exePath, const std::wstring& aumid);

} // namespace mactab::config
