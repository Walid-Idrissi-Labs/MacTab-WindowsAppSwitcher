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
};

const Settings& Current();

// Reads settings.ini, writing it with defaults if absent.
void Load();

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
