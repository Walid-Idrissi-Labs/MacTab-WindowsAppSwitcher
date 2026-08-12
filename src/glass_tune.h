#pragma once

#include <cstddef>
#include <cstring>
#include <cwchar>
#include <string>

#include "glass.h"

// Every number in the material, addressable by name.
//
// This exists because of one constraint that shapes the whole project: MacTab is
// written on a machine that cannot build it or run it, and the material is the
// part of it that can only be judged by looking. Four releases have now gone
// round the loop of change a number, push, wait for CI, install, look, report.
// That loop is twenty minutes long and it is the reason the glass has been wrong
// for so long, not any one number in it.
//
// So the numbers come out of the binary. Put them in settings.ini, reload from
// the tray, and the loop is seconds. Then the values that looked right can be
// read back off the same names and replayed here with
//
//     ./build-preview/preview out/ --set dark.gain=0.74
//
// which is the whole point: the tuning happens where it can be seen, and the
// verification happens where it can be measured, against identical names.
//
// One table, used by both sides, and now by settings.ini itself: IniBlock()
// below writes the file's glass section out of these same rows. A second copy of
// these names in config.cpp would drift, and a key that silently does nothing is
// worse than no key.

namespace mactab::glass {

// A field of Params, with the range a hand-typed value is held to and one line
// saying what it does.
//
// The clamps are not decoration. kBiasFloor and kBiasCeiling were derived for a
// particular gain and a particular band, and a gain of 3 typed into an ini would
// strand the adapted bias against them and land the panel somewhere Adapt did
// not intend. Every range here is one the rest of the material was designed to
// work inside.
//
// `doc` is here rather than in config.cpp because config.cpp GENERATES the glass
// section of settings.ini from this table: the key names, the ranges, the
// shipped defaults and the descriptions all come from one place, so the file the
// user edits cannot describe a material the binary does not have. A comment that
// lies about a default is worse than no comment, because somebody would tune
// against it.
struct Field {
    const char* name;
    float Params::* member;
    float lo, hi;
    const char* doc;
};

// Names are spelled the way settings.ini spells them, because that file is now
// generated from this table. Every lookup on both sides goes through
// NameMatches, which is case-insensitive, so the casing is presentation only and
// `--set dark.gain=0.74` on the preview's command line still works.
inline constexpr Field kFields[] = {
    { "Saturation",   &Params::saturation,   0.0f,  3.0f,
      "Colour push. Above 1 boosts, 1 leaves the desktop's colours alone." },
    { "Gain",         &Params::gain,         0.2f,  1.0f,
      "How much of the desktop's contrast survives the material." },
    { "Bias",         &Params::bias,        -0.3f,  0.6f,
      "Black point lift, added after the gain. The panel moves this one per "
      "gesture to suit the wallpaper behind it, so it is a starting point "
      "rather than a final value." },
    { "FallbackAlpha",&Params::fallbackAlpha,0.0f,  1.0f,
      "The base coat used when the desktop grab fails. Nearly opaque on "
      "purpose: a degraded state should look deliberate rather than broken." },
    { "RimAmbient",   &Params::rimAmbient,   0.0f,  0.5f,
      "The lit edge, all the way round, whichever way the surface faces." },
    { "RimLobe",      &Params::rimLobe,      0.0f,  0.5f,
      "Extra light for the part of the rim facing up, as if lit from overhead." },
    { "SpecLine",     &Params::specLine,     0.0f,  0.5f,
      "The thin bright filament along the top run." },
    { "RimEnvFloor",  &Params::rimEnvFloor,  0.0f,  2.0f,
      "How much of the lit edge is left over a black backdrop." },
    { "RimEnvGain",   &Params::rimEnvGain,   0.0f,  4.0f,
      "How much brighter the lit edge gets as the backdrop under it brightens. "
      "A rim that ignores its backdrop is a painted-on border rather than a "
      "highlight." },
    { "RimOuterDark", &Params::rimOuterDark, 0.0f,  1.0f,
      "The dark line on the outermost pixel, which is what stops a dark panel "
      "dissolving into a dark wallpaper." },
    { "TargetMin",    &Params::targetMin,    0.0f,  1.0f,
      "Bottom of the band the finished panel's brightness is steered into." },
    { "TargetMax",    &Params::targetMax,    0.0f,  1.0f,
      "Top of that band." },
    { "KneeBelow",    &Params::kneeBelow,    0.0f,  1.0f,
      "How much of an excursion below the band survives it. 0 is a hard clamp, "
      "1 is no steering at all." },
    { "KneeAbove",    &Params::kneeAbove,    0.0f,  1.0f,
      "The same, above the band. Light keeps this low on purpose: soften it and "
      "a white desktop drives the panel to white." },
};

// The tint, which is an array on Params rather than four members. Same three
// jobs as the table above: parsing, generating the ini, reading back for the
// log.
struct TintField {
    const char* name;
    int index;
    const char* doc;
};

inline constexpr TintField kTintFields[] = {
    { "TintR", 0, "Red of the tint laid over the treated backdrop, 0 to 1." },
    { "TintG", 1, "Green of it." },
    { "TintB", 2, "Blue of it." },
    { "TintA", 3, "How much of that tint there is at all. Lower is more "
                  "see-through, and after the blur this is the number that "
                  "decides most of how the panel reads." },
};

struct OpticsField {
    const char* name;
    float Tuning::* member;
    float lo, hi;
    const char* doc;
};

// "Depth", not "GlassDepth". The ini key is this name with Glass in front of it,
// so the old spelling asked for GlassGlassDepth, which is not the key the
// shipped settings file and the README have always documented and is not one
// anybody could have set successfully. Renaming it here is what makes the
// documented GlassDepth work; on the preview's command line it is now
// --set depth=24.
inline constexpr OpticsField kOpticsFields[] = {
    { "BlurSigma",       &Tuning::blurSigma,        0.0f, 60.0f,
      "How soft the backdrop goes. The single most decisive number in the "
      "material: it decides whether you can see the desktop through the panel "
      "or only that something is behind it." },
    { "RimBlurSigma",    &Tuning::rimBlurSigma,     0.0f, 60.0f,
      "The rim's own, much sharper blur. Bending a crisper copy of the desktop "
      "at the edge than in the middle is what makes it read as a lens rather "
      "than as frost." },
    { "BezelWidth",      &Tuning::bezelWidth,       0.0f, 60.0f,
      "How far in from the edge the surface curves rather than lying flat." },
    { "Depth",           &Tuning::glassDepth,       0.0f, 80.0f,
      "How thick the pane is, which is what decides how hard the rim bends "
      "what is behind it." },
    { "MaxDisplacement", &Tuning::maxDisplacement,  0.0f, 40.0f,
      "Ceiling on that bending, in pixels." },
    { "RimSpan",         &Tuning::rimSpan,          0.0f, 60.0f,
      "How far in the lit edge reaches before it is gone." },
};

inline bool NameMatches(const char* a, const char* b) {
    for (;; ++a, ++b) {
        const char ca = (*a >= 'A' && *a <= 'Z') ? static_cast<char>(*a + 32) : *a;
        const char cb = (*b >= 'A' && *b <= 'Z') ? static_cast<char>(*b + 32) : *b;
        if (ca != cb) return false;
        if (ca == '\0') return true;
    }
}

inline float Clamp(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// Set one field of a theme by name. Returns false for a name that is not a
// field, so a typo in an ini can be reported rather than ignored.
inline bool SetField(Params& p, const char* name, float value) {
    for (const Field& f : kFields) {
        if (NameMatches(f.name, name)) {
            p.*f.member = Clamp(value, f.lo, f.hi);
            return true;
        }
    }
    // tint.r / tint.g / tint.b / tint.a, held to 0..1 like every other colour.
    for (const TintField& t : kTintFields) {
        if (NameMatches(t.name, name)) {
            p.tint[t.index] = Clamp(value, 0.0f, 1.0f);
            return true;
        }
    }
    return false;
}

inline bool SetOptic(Tuning& t, const char* name, float value) {
    for (const OpticsField& f : kOpticsFields) {
        if (NameMatches(f.name, name)) {
            t.*f.member = Clamp(value, f.lo, f.hi);
            return true;
        }
    }
    return false;
}

// Read one field back, for logging what a running copy is actually using. A
// screenshot arriving without its numbers is a screenshot nobody can act on.
inline bool GetField(const Params& p, const char* name, float& out) {
    for (const Field& f : kFields) {
        if (NameMatches(f.name, name)) { out = p.*f.member; return true; }
    }
    for (const TintField& t : kTintFields) {
        if (NameMatches(t.name, name)) { out = p.tint[t.index]; return true; }
    }
    return false;
}

inline bool GetOptic(const Tuning& t, const char* name, float& out) {
    for (const OpticsField& f : kOpticsFields) {
        if (NameMatches(f.name, name)) { out = t.*f.member; return true; }
    }
    return false;
}

// --- The settings.ini section -----------------------------------------------
//
// The glass part of settings.ini is generated from the tables above rather than
// typed out by hand, because a hand-written copy is a second list of names,
// ranges and default values, and a second list is the one thing this header
// exists to prevent. The file it replaces had exactly that problem: it described
// eleven of the forty-odd keys, named one of them wrongly, and quoted a tint
// alpha from two releases earlier.
//
// Here rather than in config.cpp so tools/preview can generate the same text on
// the development machine and check that every line in it parses back to the
// material the binary actually holds. That check is the only way the author of a
// program that cannot be run on the machine it is written on finds out that the
// file it ships is telling the truth.

// The line the migration looks for to decide whether an existing settings.ini
// already has this section. Must appear verbatim in the generated text.
inline constexpr wchar_t kIniMarker[] =
    L"; --- The glass material, every number in it";

namespace detail {

// Field names and descriptions are ASCII, so widening them is a copy.
inline std::wstring Widen(const char* ascii) {
    std::wstring out;
    for (const char* c = ascii; *c; ++c) out.push_back(static_cast<wchar_t>(*c));
    return out;
}

// A value as the file should carry it.
//
// %g rather than %f, so 8 is written as "8" and not "8.000000", and with enough
// digits that the shipped 0.0654 and 0.2199 survive as themselves: a file
// documenting the default bias as 0.07 would defeat the whole point of writing
// the defaults down at all. The C locale is in force, since nothing in MacTab
// calls setlocale, so the decimal point is a point; the reader is wcstod, which
// obeys the same locale, so the two cannot disagree.
inline std::wstring FormatValue(float v) {
    wchar_t buffer[32] = L"";
    std::swprintf(buffer, sizeof(buffer) / sizeof(buffer[0]), L"%.6g",
                  static_cast<double>(v));
    return buffer;
}

// One paragraph as ini comment lines, wrapped so the file reads in Notepad at
// its default width.
inline void AppendComment(std::wstring& out, const std::wstring& text) {
    constexpr size_t kWidth = 74;

    size_t start = 0;
    while (start < text.size()) {
        size_t take = text.size() - start;
        if (take > kWidth) {
            const size_t space = text.rfind(L' ', start + kWidth);
            take = (space != std::wstring::npos && space > start) ? space - start
                                                                 : kWidth;
        }
        out += L"; ";
        out.append(text, start, take);
        out += L"\r\n";

        start += take;
        while (start < text.size() && text[start] == L' ') ++start;
    }
}

inline std::wstring Range(float lo, float hi) {
    return L" Range " + FormatValue(lo) + L" to " + FormatValue(hi) + L".";
}

// ";GlassDarkGain=0.789", which is a default sitting there commented out.
inline void AppendDefault(std::wstring& out, const wchar_t* prefix,
                          const char* name, float value) {
    out += L";";
    out += prefix;
    out += Widen(name);
    out += L"=";
    out += FormatValue(value);
    out += L"\r\n";
}

} // namespace detail

// The whole glass section, CRLF-terminated, ready to be written or appended.
inline std::wstring IniBlock() {
    using namespace detail;

    const Tuning optics{};   // the shipped shape, whatever the running copy has

    std::wstring out;
    out += kIniMarker;
    out += L" ---------------\r\n";
    out += L";\r\n";
    AppendComment(out, L"Every line in this section is commented out and holds the "
                       L"value MacTab ships with. To change one, copy the line, take "
                       L"the semicolon off the copy, and edit that. The original "
                       L"stays above it, so whatever you are trying always sits next "
                       L"to the default it replaced.");
    out += L";\r\n";
    AppendComment(out, L"Saving this file re-reads them, and there is a Reload glass "
                       L"from settings.ini in the tray menu that says out loud that "
                       L"it worked. Alt+Tab to see the result.");
    out += L";\r\n";
    AppendComment(out, L"Each one gives the range it is held to. A number outside "
                       L"that range is pulled back into it rather than refused, and "
                       L"one that is not a number at all is ignored and written to "
                       L"the diagnostics log.");
    out += L";\r\n";
    AppendComment(out, L"If it gets away from you, Reset settings.ini in the tray "
                       L"menu puts this file back exactly as it is written here and "
                       L"keeps what you had as settings.ini.bak.");
    out += L";\r\n";
    out += L"\r\n";

    AppendComment(out, L"--- The shape of the pane. Both appearances share it, in "
                       L"logical pixels at 100% scaling.");
    out += L"\r\n";

    for (const OpticsField& f : kOpticsFields) {
        float value = 0.0f;
        GetOptic(optics, f.name, value);

        AppendComment(out, Widen(f.doc) + Range(f.lo, f.hi));
        AppendDefault(out, L"Glass", f.name, value);
        out += L"\r\n";
    }

    AppendComment(out, L"--- The two appearances. Every number below comes in a dark "
                       L"and a light version and they are listed as a pair, so a "
                       L"change made to one is a change the other one still needs.");
    out += L"\r\n";

    AppendComment(out, L"The tint laid over the treated desktop, and how much of it "
                       L"there is. Start here: this and the blur above are what "
                       L"decide whether the panel reads as glass or as a card.");
    out += L"\r\n";

    struct Appearance { const wchar_t* prefix; const Params& params; };
    const Appearance appearances[] = {
        { L"GlassDark",  kDark  },
        { L"GlassLight", kLight },
    };

    for (const TintField& t : kTintFields) {
        AppendComment(out, Widen(t.doc));
        for (const Appearance& a : appearances)
            AppendDefault(out, a.prefix, t.name, a.params.tint[t.index]);
        out += L"\r\n";
    }

    for (const Field& f : kFields) {
        AppendComment(out, Widen(f.doc) + Range(f.lo, f.hi));
        for (const Appearance& a : appearances)
            AppendDefault(out, a.prefix, f.name, a.params.*f.member);
        out += L"\r\n";
    }

    return out;
}

} // namespace mactab::glass
