#include "pch.h"

#include "config.h"
#include "glass_tune.h"
#include "com.h"
#include "common.h"
#include "diag.h"
#include "icon_source.h"

namespace mactab::config {
namespace {

constexpr wchar_t kSection[]  = L"MacTab";
constexpr wchar_t kRunKey[]   = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"MacTab";

Settings     g_settings;
std::wstring g_settingsPath;
std::wstring g_themesDir;

// How many glass values the last read took out of the file. See ReadGlass.
int g_glassOverrides = 0;

// Everything above the glass section, which is generated. See DefaultIni().
const wchar_t* kDefaultIniHead =
    L"; MacTab settings. Delete this file to restore defaults, or use Reset\r\n"
    L"; settings.ini in the tray menu, which keeps a copy of the old one.\r\n"
    L"[MacTab]\r\n"
    L"; Milliseconds Alt must be held before the panel appears. A quicker tap\r\n"
    L"; switches to the previous app without showing anything, like macOS.\r\n"
    L"RevealDelayMs=180\r\n"
    L"\r\n"
    L"; Trigger on Left Alt only, and never while Ctrl is held. Keep this on if\r\n"
    L"; you use a layout where AltGr (Ctrl+RightAlt) types @ # { } [ ].\r\n"
    L"LeftAltOnly=1\r\n"
    L"\r\n"
    L"; Icon tile size in logical pixels at 100% scaling.\r\n"
    L"TileSize=128\r\n"
    L"\r\n"
    L"; auto | dark | light\r\n"
    L"Theme=auto\r\n"
    L"\r\n"
    L"; How the desktop behind the panel is grabbed, which is what the glass is\r\n"
    L"; made of. If the panel is a flat grey slab with nothing showing through\r\n"
    L"; it, no grab is getting back, and this is the key to try.\r\n"
    L";   auto         try each of the three below and use the first that\r\n"
    L";                comes back with a picture in it\r\n"
    L";   duplication  the compositor's own copy. Cannot work on a still\r\n"
    L";                desktop: it has nothing to hand over until the screen\r\n"
    L";                changes, which is why the glass can appear only while\r\n"
    L";                something behind the panel is moving\r\n"
    L";   bitblt       a screen copy including layered windows\r\n"
    L";   plain        the same copy without the flag that forces a compositor\r\n"
    L";                sync, which is the one most likely to come back black\r\n"
    L"CaptureSource=auto\r\n"
    L"\r\n"
    L"; Keep the desktop behind the panel live while the panel is up, instead of\r\n"
    L"; freezing it as it was when you pressed Alt+Tab. Needs Windows 10 2004 or\r\n"
    L"; later, because it works by taking MacTab's own window out of what the\r\n"
    L"; screen grab can see; below that the backdrop stays frozen.\r\n"
    L";\r\n"
    L"; That has one consequence worth knowing: while this is on, the panel does\r\n"
    L"; not appear in screen recordings or in a shared screen, because it is\r\n"
    L"; hidden from exactly the same machinery. Set this to 0 if you need to\r\n"
    L"; record or demonstrate it.\r\n"
    L"LiveBackdrop=1\r\n"
    L"\r\n"
    L"; Ceiling on how often it refreshes, in frames per second. 0 follows the\r\n"
    L"; display. Lower it if you would rather the panel did less work while it\r\n"
    L"; is on screen.\r\n"
    L"LiveBackdropHz=0\r\n"
    L"\r\n"
    L"; 1 = one tile per application (macOS). 0 = one tile per window.\r\n"
    L"GroupByApp=1\r\n"
    L"\r\n"
    L"; Which display the panel opens on.\r\n"
    L";   active = the display holding the window you are currently in\r\n"
    L";   mouse  = the display the mouse pointer is on\r\n"
    L";   main   = always the main display\r\n"
    L"; Also settable from the tray menu.\r\n"
    L"PanelDisplay=active\r\n"
    L"\r\n"
    L"; Bend the desktop at the panel's rim, the way a real pane of glass would.\r\n"
    L"; Set to 0 if the edge of the panel looks doubled or smeared on your\r\n"
    L"; machine; everything else about the glass stays as it is.\r\n"
    L"GlassRefraction=1\r\n"
    L"\r\n"
    L"; The rim bends a much sharper copy of the desktop than the middle\r\n"
    L"; does, which is what makes the edge read as a lens rather than as\r\n"
    L"; frost. Set to 0 if the bezel looks doubled or banded.\r\n"
    L"GlassRimTap=1\r\n"
    L"\r\n"
    L"; Written by MacTab so it knows which defaults this file predates. Leave it\r\n"
    L"; alone; it never overwrites a value you have changed yourself.\r\n"
    L"SettingsVersion=";   // the number itself is appended from kSettingsVersion

const wchar_t* kDefaultIniBody =
    L"\r\n"
    L"\r\n"
    L"; --- Mission Control -------------------------------------------------\r\n"
    L";\r\n"
    L"; Mission Control spreads every window out and puts the desktops along the\r\n"
    L"; top. Off by default; also in the tray menu under Settings.\r\n"
    L"MissionEnabled=0\r\n"
    L"\r\n"
    L"; Which chord opens it.\r\n"
    L";   wintab   Win+Tab, replacing Task View, which is the same feature\r\n"
    L";   winup    Win+Up, which COSTS YOU Aero Snap's maximise\r\n"
    L";   both     either one\r\n"
    L";   none     neither; open it from the tray instead\r\n"
    L"MissionGesture=wintab\r\n"
    L"\r\n"
    L"; 1 = pull each app's windows into a cluster with its icon and name under\r\n"
    L"; it. Costs room, because gathering a scattered app's windows means moving\r\n"
    L"; them a long way and everything ends up smaller.\r\n"
    L"MissionGroupByApp=1\r\n"
    L"\r\n"
    L"; Space between two windows, how far each window of an app is offset from\r\n"
    L"; the one in front of it in its pile, and the room left between one pile\r\n"
    L"; and the next.\r\n"
    L"MissionGap=26\r\n"
    L"MissionFan=30\r\n"
    L"MissionClusterGap=88\r\n"
    L"\r\n"
    L"; The wallpaper behind the windows: how soft it goes, and how far back it\r\n"
    L"; is pushed. Dim is 0 for none and 1 for opaque.\r\n"
    L";\r\n"
    L"; Sigma is 0 because macOS does not blur the desktop here, it dims it and\r\n"
    L"; lifts the windows off. Anything above 0 also lets the backdrop be baked at\r\n"
    L"; a fraction of the screen's resolution, since the blur hides the stretch:\r\n"
    L"; below 1 it is baked at full size, below 4 at half, above that at a\r\n"
    L"; quarter. On a 4K screen that is the difference between about 33 MB per\r\n"
    L"; display and about 2 MB, so if Mission Control costs more memory than you\r\n"
    L"; want it to, this is the number to raise.\r\n"
    L"MissionBlurSigma=0\r\n"
    L"MissionDim=0.45\r\n"
    L"\r\n"
    L"; How long the windows take to fly out to their places, in milliseconds.\r\n"
    L"MissionRevealMs=260\r\n"
    L"\r\n"
    L"; Once the windows have landed, replace the live preview of anything the\r\n"
    L"; compositor cannot reduce honestly with a still of the same window, taken\r\n"
    L"; at twice the size it is shown at and filtered properly. Those previews\r\n"
    L"; stop moving; 0 keeps them live and soft.\r\n"
    L"MissionSharpPreviews=1\r\n"
    L"\r\n"
    L"; How window contents are shown.\r\n"
    L";   auto     use the best one this machine supports\r\n"
    L";   shared   live thumbnails, which is the only one that can animate\r\n"
    L";   snapshot one still picture per window, taken when you press the keys\r\n"
    L";   icon     no window contents at all, just the app icon on a card\r\n"
    L"; Drop to snapshot if the windows come out blank, misplaced or flickering.\r\n"
    L"MissionThumbnails=auto\r\n"
    L"\r\n";

// Throw away whatever the profile APIs are holding for this file.
//
// GetPrivateProfileString does not go to disk every time: the system keeps the
// most recently used ini mapping cached, and a file edited by another process
// after we have read it can go on being served from that cache. That is the
// exact shape of this program's tuning loop, where Notepad writes the file and
// MacTab re-reads it seconds later inside the same session, so a stale read here
// would look precisely like a key that does nothing.
//
// Passing three nulls is the documented way to flush it. It writes no key and
// creates no file; on a path that does not exist it fails and there is nothing
// to flush anyway.
void FlushIniCache() {
    if (g_settingsPath.empty()) return;
    ::WritePrivateProfileStringW(nullptr, nullptr, nullptr, g_settingsPath.c_str());
}

std::wstring ReadString(const wchar_t* key, const wchar_t* fallback) {
    wchar_t buffer[128] = L"";
    ::GetPrivateProfileStringW(kSection, key, fallback, buffer, ARRAYSIZE(buffer),
                               g_settingsPath.c_str());
    return buffer;
}

// The same, for a key whose value is one of a fixed set of words.
//
// Lowercased and trimmed, because every reader of these compares against a
// lowercase literal: Theme=Dark and PanelDisplay=Main silently fell through to
// the default, which from the outside is a setting that does nothing. Key names
// are already case-insensitive in the profile APIs, so a file where the key
// tolerates any casing and the value does not is a trap of our own making.
std::wstring ReadKeyword(const wchar_t* key, const wchar_t* fallback) {
    std::wstring value = ReadString(key, fallback);

    if (const size_t comment = value.find_first_of(L";#"); comment != std::wstring::npos)
        value.erase(comment);

    while (!value.empty() && (value.back() == L' ' || value.back() == L'\t'))
        value.pop_back();

    for (wchar_t& c : value) {
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c + 32);
    }
    return value;
}

// Settings whose SHIPPED default has changed, in a file that was written before
// it changed.
//
// The file is only written once, on first run, so an upgrade leaves every value
// in it exactly as it was. That is right for anything the user chose and wrong
// for a default they never touched: 0.7 shipped Mission Control with the
// wallpaper blurred at 18 and dimmed by 0.55, and 0.8 stopped blurring it,
// because macOS does not. Left alone, the change would be invisible on every
// machine that already had the file.
//
// Stamped rather than sniffed, so a value somebody has deliberately set is only
// ever rewritten once and never again.
//
// 3 adds the generated glass section. A file at 2 has none of it, so the
// migration appends it: the keys were always readable, but nobody could be
// expected to know their names from a paragraph of prose that did not list half
// of them.
constexpr int kSettingsVersion = 3;

// The whole file as it ships.
//
// Assembled rather than stored, because two of its parts must not be typed out
// by hand: the version stamp, which has to be the number this build migrates to,
// and the glass section, which is generated from the tables in glass_tune.h so
// that a default written in the file is the default the binary holds.
std::wstring DefaultIni() {
    std::wstring text = kDefaultIniHead;
    text += std::to_wstring(kSettingsVersion);
    text += kDefaultIniBody;
    text += glass::IniBlock();
    return text;
}

// Write `text` over the settings file as UTF-16LE with a BOM, which is what the
// profile APIs write and the only encoding they read back as Unicode.
//
// `createNew` is first-run: fail rather than truncate if something is already
// there, so a file somebody has tuned cannot be lost to a race with a second
// instance starting.
bool WriteIni(const std::wstring& text, bool createNew) {
    const UniqueHandle file(::CreateFileW(g_settingsPath.c_str(), GENERIC_WRITE, 0,
                                          nullptr, createNew ? CREATE_NEW : CREATE_ALWAYS,
                                          FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file || file.get() == INVALID_HANDLE_VALUE) {
        if (!createNew)
            MACTAB_WARN("config: could not open settings.ini for writing (err %lu)",
                        ::GetLastError());
        return false;
    }

    const wchar_t bom = 0xFEFF;
    DWORD written = 0;
    if (!::WriteFile(file.get(), &bom, sizeof(bom), &written, nullptr))
        return false;

    return ::WriteFile(file.get(), text.data(),
                       static_cast<DWORD>(text.size() * sizeof(wchar_t)),
                       &written, nullptr) != FALSE;
}

int ReadInt(const wchar_t* key, int fallback) {
    return static_cast<int>(::GetPrivateProfileIntW(kSection, key, fallback,
                                                    g_settingsPath.c_str()));
}

const wchar_t* PanelDisplayKeyword(PanelDisplay display) {
    switch (display) {
        case PanelDisplay::Mouse:   return L"mouse";
        case PanelDisplay::Primary: return L"main";
        default:                    return L"active";
    }
}

// The field names in glass_tune.h are ASCII, so widening them is a copy. Local
// rather than in common.h because nothing else needs it.
std::wstring Widen(const char* ascii) {
    std::wstring out;
    for (const char* c = ascii; *c; ++c) out.push_back(static_cast<wchar_t>(*c));
    return out;
}

// A float from the ini, or the fallback if the key is absent or unparseable.
//
// GetPrivateProfileInt cannot do this, so the value comes back as a string and
// goes through wcstod. A key present but garbage keeps the fallback and says so,
// because a value silently ignored is the worst outcome for someone tuning by
// hand who cannot see why nothing changed.
bool ReadFloat(const wchar_t* key, float& out) {
    wchar_t buffer[64] = L"";
    ::GetPrivateProfileStringW(kSection, key, L"", buffer, ARRAYSIZE(buffer),
                               g_settingsPath.c_str());
    if (buffer[0] == L'\0') return false;

    // A comment after the value. The profile APIs only treat a semicolon as a
    // comment at the start of a line, so "0.03 ; was 0.06" arrives here whole,
    // and a file that ships every default as a commented line invites exactly
    // that note being left behind on the copy. Cut it off rather than refuse the
    // line: the number in front of it is unambiguous.
    if (wchar_t* comment = ::wcspbrk(buffer, L";#"))
        *comment = L'\0';

    // A comma where the decimal point should be, which is what most of Europe
    // types without thinking about it. wcstod works in the C locale here, so it
    // would stop at the comma, hand back the whole-number part and report a
    // successful parse: 0,45 would arrive as 0. Swapped rather than rejected,
    // because the number the user meant is unambiguous.
    for (wchar_t& c : buffer) {
        if (c == L',') c = L'.';
    }

    wchar_t* end = nullptr;
    const double v = ::wcstod(buffer, &end);
    if (end == buffer) {
        MACTAB_WARN("config: %s is not a number, ignoring", ToUtf8(key).c_str());
        return false;
    }

    // Anything after the number that is not blank means the value was not what
    // it looked like: 0.5px, 8;comment, 1.2.3. Reported for the same reason the
    // unparseable case is, since a value quietly reinterpreted is a worse
    // outcome for somebody tuning by hand than one that says it was wrong.
    for (const wchar_t* rest = end; *rest; ++rest) {
        if (*rest != L' ' && *rest != L'\t') {
            MACTAB_WARN("config: %s has trailing text after the number, ignoring",
                        ToUtf8(key).c_str());
            return false;
        }
    }

    out = static_cast<float>(v);
    return true;
}

// Read the whole material out of the ini, over the shipped defaults.
//
// Names come from glass_tune.h and nowhere else. A second list here would drift
// from the one tools/preview uses, and the entire value of these keys is that a
// number that looked right on Windows can be replayed and measured on the Mac
// under the same name.
void ReadGlass() {
    g_settings.glassDark  = glass::kDark;
    g_settings.glassLight = glass::kLight;
    glass::g_tuning = glass::Tuning{};

    int overrides = 0;

    for (const glass::OpticsField& f : glass::kOpticsFields) {
        const std::wstring key = L"Glass" + Widen(f.name);
        float v = 0.0f;
        if (ReadFloat(key.c_str(), v) && glass::SetOptic(glass::g_tuning, f.name, v))
            ++overrides;
    }

    struct ThemeKeys { const wchar_t* prefix; glass::Params* params; };
    const ThemeKeys themes[] = {
        { L"GlassDark",  &g_settings.glassDark  },
        { L"GlassLight", &g_settings.glassLight },
    };

    auto apply = [&](const ThemeKeys& t, const char* name) {
        const std::wstring key = t.prefix + Widen(name);
        float v = 0.0f;
        if (ReadFloat(key.c_str(), v) && glass::SetField(*t.params, name, v))
            ++overrides;
    };

    for (const ThemeKeys& t : themes) {
        for (const glass::Field& f : glass::kFields) apply(t, f.name);
        for (const glass::TintField& f : glass::kTintFields) apply(t, f.name);
    }

    // Kept, not just logged. The tray's reload reports it, because the question
    // somebody whose edit appeared to do nothing actually has is whether the
    // file was read at all, and a count of the values taken out of it answers
    // that in one number. Zero after an edit means the line is still commented
    // out or misspelt, which is the likeliest thing to have gone wrong and the
    // hardest to see when the file is full of commented lines by design.
    g_glassOverrides = overrides;

    if (overrides > 0) {
        MACTAB_DIAG("config: %d glass override%s from settings.ini", overrides,
                    overrides == 1 ? "" : "s");
    }

    // Always logged, overridden or not, so a screenshot arrives with the numbers
    // that produced it rather than with the numbers the release shipped.
    MACTAB_DIAG("glass: sigma %.1f bezel %.1f depth %.1f maxDisp %.1f rimSpan %.1f",
                glass::g_tuning.blurSigma, glass::g_tuning.bezelWidth,
                glass::g_tuning.glassDepth, glass::g_tuning.maxDisplacement,
                glass::g_tuning.rimSpan);
    for (const ThemeKeys& t : themes) {
        const glass::Params& p = *t.params;
        MACTAB_DIAG("glass: %s sat %.2f gain %.3f bias %.3f tint %.2f/%.2f/%.2f a %.2f "
                    "rim %.3f/%.3f/%.3f env %.2f+%.2f band %.2f-%.2f knee %.2f/%.2f",
                    ToUtf8(t.prefix).c_str(), p.saturation, p.gain, p.bias,
                    p.tint[0], p.tint[1], p.tint[2], p.tint[3],
                    p.rimAmbient, p.rimLobe, p.specLine,
                    p.rimEnvFloor, p.rimEnvGain,
                    p.targetMin, p.targetMax, p.kneeBelow, p.kneeAbove);
    }
}

PanelDisplay ParsePanelDisplay(const std::wstring& text) {
    if (text == L"mouse") return PanelDisplay::Mouse;
    if (text == L"main" || text == L"primary") return PanelDisplay::Primary;
    return PanelDisplay::ActiveWindow;
}

// Executable stem, or a filesystem-safe form of the AUMID.
std::wstring ThemeName(const std::wstring& exePath, const std::wstring& aumid) {
    if (!aumid.empty()) {
        std::wstring safe = aumid;
        for (wchar_t& c : safe) {
            if (c == L'\\' || c == L'/' || c == L':' || c == L'!' ||
                c == L'*'  || c == L'?' || c == L'"' || c == L'<' ||
                c == L'>'  || c == L'|')
                c = L'_';
        }
        return safe;
    }

    const size_t slash = exePath.find_last_of(L"\\/");
    std::wstring name = (slash == std::wstring::npos) ? exePath : exePath.substr(slash + 1);
    const size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0)
        name.erase(dot);
    return name;
}

// The settings file as text, or false if it is not the UTF-16LE the profile APIs
// write. The BOM is not included in `out`.
//
// The encoding check is the whole point of this: the file belongs to the user
// now, and an editor that re-saved it as UTF-8 or ANSI would turn an appended
// wide string into a run of interleaved nulls in the middle of their settings.
bool ReadIniUtf16(std::wstring& out) {
    const UniqueHandle file(::CreateFileW(g_settingsPath.c_str(), GENERIC_READ,
                                          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file || file.get() == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(file.get(), &size)) return false;

    // Two bytes of BOM and nothing else is an empty file, and anything past a
    // megabyte is not a settings file this program wrote.
    if (size.QuadPart < 4 || size.QuadPart > 1024 * 1024) return false;

    std::vector<wchar_t> buffer(static_cast<size_t>(size.QuadPart) / sizeof(wchar_t));
    DWORD read = 0;
    if (!::ReadFile(file.get(), buffer.data(),
                    static_cast<DWORD>(buffer.size() * sizeof(wchar_t)), &read, nullptr))
        return false;

    const size_t chars = read / sizeof(wchar_t);
    if (chars < 2 || buffer[0] != 0xFEFF) return false;

    out.assign(buffer.begin() + 1, buffer.begin() + static_cast<ptrdiff_t>(chars));
    return true;
}

// Give a file written before the glass section existed one.
//
// Comments only. Not a single value in the file changes, and a key the user has
// set is not touched even when the block below documents a different default for
// it, which is exactly the rule the version stamp exists to keep.
//
// Returns false only when the section is still missing afterwards, which is what
// decides whether the version is stamped: a file that was locked by an editor or
// re-saved as UTF-8 is a case to try again on the next run, not one to record as
// done. Already having the section counts as success.
bool AppendGlassSection() {
    std::wstring existing;
    if (!ReadIniUtf16(existing)) {
        MACTAB_WARN("config: settings.ini is not UTF-16, leaving it alone. "
                    "Reset settings.ini from the tray menu to get the glass section");
        return false;
    }

    // Belt and braces against a file that has the section but lost its stamp.
    if (existing.find(glass::kIniMarker) != std::wstring::npos) return true;

    std::wstring block;
    if (!existing.empty() && existing.back() != L'\n') block += L"\r\n";
    block += L"\r\n";
    block += glass::IniBlock();

    const UniqueHandle file(::CreateFileW(g_settingsPath.c_str(), FILE_APPEND_DATA,
                                          FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                          FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file || file.get() == INVALID_HANDLE_VALUE) {
        MACTAB_WARN("config: could not append the glass section (err %lu)",
                    ::GetLastError());
        return false;
    }

    DWORD written = 0;
    if (!::WriteFile(file.get(), block.data(),
                     static_cast<DWORD>(block.size() * sizeof(wchar_t)), &written, nullptr)) {
        MACTAB_WARN("config: writing the glass section failed (err %lu)", ::GetLastError());
        return false;
    }

    MACTAB_DIAG("config: glass section added to settings.ini");
    return true;
}

void Migrate() {
    const int version = ReadInt(L"SettingsVersion", 1);
    if (version >= kSettingsVersion) return;

    if (version < 2) {
        // Only the two whose meaning changed, and only when they still hold the
        // value 0.7 wrote. Anything else is a choice somebody made.
        wchar_t buffer[64] = L"";
        ::GetPrivateProfileStringW(kSection, L"MissionBlurSigma", L"", buffer,
                                   ARRAYSIZE(buffer), g_settingsPath.c_str());
        if (::lstrcmpW(buffer, L"18") == 0)
            ::WritePrivateProfileStringW(kSection, L"MissionBlurSigma", L"0",
                                         g_settingsPath.c_str());

        ::GetPrivateProfileStringW(kSection, L"MissionDim", L"", buffer,
                                   ARRAYSIZE(buffer), g_settingsPath.c_str());
        if (::lstrcmpW(buffer, L"0.55") == 0)
            ::WritePrivateProfileStringW(kSection, L"MissionDim", L"0.45",
                                         g_settingsPath.c_str());
    }

    if (version < 3)
        AppendGlassSection();

    // Stamped last. An append that failed because an editor had the file open is
    // then retried on the next run instead of being remembered as done, and the
    // marker check above makes that retry harmless.
    ::WritePrivateProfileStringW(kSection, L"SettingsVersion",
                                 std::to_wstring(kSettingsVersion).c_str(),
                                 g_settingsPath.c_str());
    MACTAB_DIAG("config: settings brought from version %d up to %d",
                version, kSettingsVersion);
}

// Everything in the file, into g_settings and the material.
//
// Split out of Load() because Reset() needs it and must NOT re-run the rest:
// Load() assigns the themes directory string, which the icon worker reads on its
// own thread, and reassigning a std::wstring under a concurrent reader is a real
// race. Nothing here touches anything off the UI thread.
void ReadSettings() {
    g_settings.revealDelayMs = static_cast<UINT>(
        (std::max)(0, (std::min)(2000, ReadInt(L"RevealDelayMs", 180))));
    g_settings.leftAltOnly = ReadInt(L"LeftAltOnly", 1) != 0;
    g_settings.tileSize    = (std::max)(48, (std::min)(256, ReadInt(L"TileSize", 128)));
    g_settings.theme       = ReadKeyword(L"Theme", L"auto");
    g_settings.captureSource = ReadKeyword(L"CaptureSource", L"auto");
    g_settings.liveBackdrop   = ReadInt(L"LiveBackdrop", 1) != 0;
    g_settings.liveBackdropHz =
        (std::max)(0, (std::min)(240, ReadInt(L"LiveBackdropHz", 0)));
    g_settings.groupByApp  = ReadInt(L"GroupByApp", 1) != 0;
    g_settings.panelDisplay = ParsePanelDisplay(ReadKeyword(L"PanelDisplay", L"active"));
    g_settings.glassRefraction = ReadInt(L"GlassRefraction", 1) != 0;
    g_settings.glassRimTap     = ReadInt(L"GlassRimTap", 1) != 0;

    g_settings.missionEnabled    = ReadInt(L"MissionEnabled", 0) != 0;
    g_settings.missionGesture    = ReadKeyword(L"MissionGesture", L"wintab");
    g_settings.missionGroupByApp = ReadInt(L"MissionGroupByApp", 1) != 0;
    g_settings.missionSharpPreviews = ReadInt(L"MissionSharpPreviews", 1) != 0;
    g_settings.missionRevealMs   = static_cast<UINT>(
        (std::max)(0, (std::min)(2000, ReadInt(L"MissionRevealMs", 260))));
    g_settings.missionThumbnails = ReadKeyword(L"MissionThumbnails", L"auto");

    g_settings.missionGap        = 26.0f;
    g_settings.missionFan        = 30.0f;
    g_settings.missionClusterGap = 88.0f;
    g_settings.missionBlurSigma  = 0.0f;
    g_settings.missionDim        = 0.45f;

    ReadFloat(L"MissionGap",        g_settings.missionGap);
    ReadFloat(L"MissionFan",        g_settings.missionFan);
    ReadFloat(L"MissionClusterGap", g_settings.missionClusterGap);
    ReadFloat(L"MissionBlurSigma",  g_settings.missionBlurSigma);
    ReadFloat(L"MissionDim",        g_settings.missionDim);

    // Held to ranges the rest of the layout was designed inside, the same way
    // the material's keys are. A gap of 4000 typed by hand would put every
    // window off screen and look like a crash rather than a mistake.
    g_settings.missionGap        = (std::max)(0.0f,  (std::min)(400.0f, g_settings.missionGap));
    g_settings.missionFan        = (std::max)(0.0f,  (std::min)(400.0f, g_settings.missionFan));
    g_settings.missionClusterGap = (std::max)(0.0f,  (std::min)(600.0f, g_settings.missionClusterGap));
    g_settings.missionBlurSigma  = (std::max)(0.0f,  (std::min)(120.0f, g_settings.missionBlurSigma));
    g_settings.missionDim        = (std::max)(0.0f,  (std::min)(1.0f,   g_settings.missionDim));

    ReadGlass();

    MACTAB_DIAG("config: revealDelay %u ms, leftAltOnly %d, tile %d, theme %s, "
                "groupByApp %d, panelDisplay %s, glassRefraction %d",
                g_settings.revealDelayMs, g_settings.leftAltOnly ? 1 : 0,
                g_settings.tileSize, ToUtf8(g_settings.theme).c_str(),
                g_settings.groupByApp ? 1 : 0,
                ToUtf8(PanelDisplayKeyword(g_settings.panelDisplay)).c_str(),
                g_settings.glassRefraction ? 1 : 0);
    MACTAB_DIAG("config: glassRimTap %d", g_settings.glassRimTap ? 1 : 0);
    MACTAB_DIAG("config: mission enabled %d, groupByApp %d, gap %.0f/%.0f, "
                "blur %.0f, dim %.2f, reveal %u ms, thumbnails %s",
                g_settings.missionEnabled ? 1 : 0,
                g_settings.missionGroupByApp ? 1 : 0,
                g_settings.missionGap, g_settings.missionClusterGap,
                g_settings.missionBlurSigma, g_settings.missionDim,
                g_settings.missionRevealMs,
                ToUtf8(g_settings.missionThumbnails).c_str());
}

} // namespace

const Settings& Current() { return g_settings; }

void ReloadGlass() {
    if (g_settingsPath.empty()) return;
    FlushIniCache();
    ReadGlass();
}

void Reload() {
    if (g_settingsPath.empty()) return;
    FlushIniCache();
    ReadSettings();
}

void Load() {
    const std::wstring& dir = AppDataDir();
    if (dir.empty()) {
        MACTAB_WARN("config: no app data directory; using built-in defaults");
        return;
    }

    g_settingsPath = dir + L"\\settings.ini";
    g_themesDir    = dir + L"\\themes";

    // Write the annotated default file on first run so the settings are
    // discoverable without documentation.
    if (::GetFileAttributesW(g_settingsPath.c_str()) == INVALID_FILE_ATTRIBUTES)
        WriteIni(DefaultIni(), true);

    ::SHCreateDirectoryExW(nullptr, g_themesDir.c_str(), nullptr);

    Migrate();

    // Both the first-run write and the migration's append go through raw file
    // writes rather than the profile APIs, so anything those APIs cached while
    // Migrate was reading the old file is stale by now.
    FlushIniCache();
    ReadSettings();
}

bool ResetSettings() {
    if (g_settingsPath.empty()) {
        MACTAB_WARN("config: no settings file to reset");
        return false;
    }

    // The old file is kept, and its survival is a precondition rather than a
    // nicety. This exists for somebody who has tuned the material into a state
    // they cannot get out of, and overwriting the only copy of that tuning
    // because a backup silently failed would be the same accident again, larger.
    // A file that is not there at all is the one acceptable failure: there is
    // nothing to lose and the reset is what writes it.
    const std::wstring backup = g_settingsPath + L".bak";
    if (::GetFileAttributesW(g_settingsPath.c_str()) != INVALID_FILE_ATTRIBUTES &&
        !::CopyFileW(g_settingsPath.c_str(), backup.c_str(), FALSE)) {
        MACTAB_FAIL("config: could not write settings.ini.bak (err %lu), not resetting",
                    ::GetLastError());
        return false;
    }

    if (!WriteIni(DefaultIni(), false)) {
        MACTAB_FAIL("config: could not rewrite settings.ini");
        return false;
    }

    // Deliberately not Load(): the themes directory string is read by the icon
    // worker on its own thread, and it has not changed anyway.
    FlushIniCache();
    ReadSettings();
    MACTAB_DIAG("config: settings.ini reset to defaults, old file kept as .bak");
    return true;
}

bool SetPanelDisplay(PanelDisplay display) {
    // Apply first, persist second. Failing to write is a reason to lose the
    // setting on the next launch, not a reason to ignore what the user just
    // clicked, and that has to hold for the no-settings-file case too.
    g_settings.panelDisplay = display;

    if (g_settingsPath.empty()) {
        MACTAB_WARN("config: no settings file, PanelDisplay will not survive a restart");
        return false;
    }

    const BOOL ok = ::WritePrivateProfileStringW(kSection, L"PanelDisplay",
                                                 PanelDisplayKeyword(display),
                                                 g_settingsPath.c_str());

    MACTAB_DIAG("config: panelDisplay -> %s (%s)",
                ToUtf8(PanelDisplayKeyword(display)).c_str(), ok ? "saved" : "not saved");
    return ok != FALSE;
}

bool SetTheme(const wchar_t* value) {
    if (!value) return false;

    g_settings.theme = value;   // same order, same reason, see SetPanelDisplay

    if (g_settingsPath.empty()) {
        MACTAB_WARN("config: no settings file, Theme will not survive a restart");
        return false;
    }

    const BOOL ok = ::WritePrivateProfileStringW(kSection, L"Theme", value,
                                                 g_settingsPath.c_str());

    MACTAB_DIAG("config: theme -> %s (%s)", ToUtf8(value).c_str(),
                ok ? "saved" : "not saved");
    return ok != FALSE;
}

bool SetMissionEnabled(bool enabled) {
    if (g_settingsPath.empty()) return false;

    const bool ok = ::WritePrivateProfileStringW(kSection, L"MissionEnabled",
                                                 enabled ? L"1" : L"0",
                                                 g_settingsPath.c_str()) != FALSE;
    g_settings.missionEnabled = enabled;
    MACTAB_DIAG("config: MissionEnabled -> %d (persisted %d)",
                enabled ? 1 : 0, ok ? 1 : 0);
    return ok;
}

// Must match AppId in installer/MacTab.iss. Inno appends _is1.
constexpr wchar_t kUninstallKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\"
    L"{E2C26C45-D5E7-4EFD-A956-4168F7C3E0D6}_is1";

std::wstring UninstallCommand() {
    // HKCU first: per-user is the default install. A per-machine install (the
    // uiAccess route, if a certificate ever appears) writes HKLM instead.
    for (const HKEY root : { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE }) {
        wchar_t value[MAX_PATH * 2] = L"";
        DWORD size = sizeof(value);
        if (::RegGetValueW(root, kUninstallKey, L"UninstallString",
                           RRF_RT_REG_SZ, nullptr, value, &size) == ERROR_SUCCESS &&
            value[0] != L'\0') {
            return value;
        }
    }
    return {};
}

bool RunUninstaller() {
    std::wstring command = UninstallCommand();
    if (command.empty()) {
        MACTAB_WARN("config: no uninstall entry, this is not an installed copy");
        return false;
    }

    // The value is a quoted path, sometimes with arguments. ShellExecute wants
    // them separate, so split on the closing quote of the executable.
    std::wstring file = command;
    std::wstring args;
    if (!file.empty() && file.front() == L'"') {
        const size_t end = file.find(L'"', 1);
        if (end != std::wstring::npos) {
            args = file.substr(end + 1);
            file = file.substr(1, end - 1);
        }
    }

    while (!args.empty() && args.front() == L' ')
        args.erase(args.begin());

    // Inno's own convention for "show the wizard normally". Not /SILENT: the
    // user asked to uninstall from a tray menu, so they get the confirmation.
    const HINSTANCE result = ::ShellExecuteW(nullptr, L"open", file.c_str(),
                                             args.empty() ? nullptr : args.c_str(),
                                             nullptr, SW_SHOWNORMAL);

    // ShellExecute returns a value <= 32 as an error code, not a handle.
    const bool ok = reinterpret_cast<INT_PTR>(result) > 32;
    MACTAB_DIAG("config: uninstaller %s (%s)", ToUtf8(file).c_str(),
                ok ? "started" : "failed to start");
    return ok;
}

bool AutostartEnabled() {
    wchar_t value[MAX_PATH * 2] = L"";
    DWORD size = sizeof(value);
    return ::RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue,
                          RRF_RT_REG_SZ, nullptr, value, &size) == ERROR_SUCCESS;
}

bool SetAutostart(bool enabled) {
    HKEY key = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return false;

    bool ok = false;
    if (enabled) {
        wchar_t exePath[MAX_PATH * 2] = L"";
        if (::GetModuleFileNameW(nullptr, exePath, ARRAYSIZE(exePath))) {
            // Quoted: the path very often contains spaces, and an unquoted Run
            // value is parsed at the first one.
            std::wstring command = L"\"";
            command += exePath;
            command += L"\"";

            ok = ::RegSetValueExW(key, kRunValue, 0, REG_SZ,
                                  reinterpret_cast<const BYTE*>(command.c_str()),
                                  static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)))
                 == ERROR_SUCCESS;
        }
    } else {
        const LSTATUS status = ::RegDeleteValueW(key, kRunValue);
        ok = (status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND);
    }

    ::RegCloseKey(key);
    MACTAB_DIAG("config: autostart %s -> %s", enabled ? "enable" : "disable",
                ok ? "ok" : "failed");
    return ok;
}

const std::wstring& ThemesDir() { return g_themesDir; }

const std::wstring& SettingsPath() { return g_settingsPath; }

int GlassOverrides() { return g_glassOverrides; }

Bitmap LoadThemeOverride(const std::wstring& exePath, const std::wstring& aumid) {
    if (g_themesDir.empty()) return {};

    const std::wstring name = ThemeName(exePath, aumid);
    if (name.empty()) return {};

    for (const wchar_t* extension : { L".png", L".PNG" }) {
        const std::wstring path = g_themesDir + L"\\" + name + extension;
        if (::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
            continue;

        Bitmap image = DecodeImageFile(path);
        if (!image.Empty()) {
            MACTAB_DIAG("config: theme override for %s", ToUtf8(name).c_str());
            return image;
        }
        MACTAB_WARN("config: could not decode %s", ToUtf8(path).c_str());
    }

    return {};
}

} // namespace mactab::config
