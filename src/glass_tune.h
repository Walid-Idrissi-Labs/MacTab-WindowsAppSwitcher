#pragma once

#include <cstddef>
#include <cstring>

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
// One table, used by both sides. A second copy of these names in config.cpp
// would drift, and a key that silently does nothing is worse than no key.

namespace mactab::glass {

// A field of Params, with the range a hand-typed value is held to.
//
// The clamps are not decoration. kBiasFloor and kBiasCeiling were derived for a
// particular gain and a particular band, and a gain of 3 typed into an ini would
// strand the adapted bias against them and land the panel somewhere Adapt did
// not intend. Every range here is one the rest of the material was designed to
// work inside.
struct Field {
    const char* name;
    float Params::* member;
    float lo, hi;
};

// The tint is an array, so it needs its own four entries rather than a pointer
// to member. Handled separately below.
inline constexpr Field kFields[] = {
    { "saturation",   &Params::saturation,   0.0f,  3.0f  },
    { "gain",         &Params::gain,         0.2f,  1.0f  },
    { "bias",         &Params::bias,        -0.3f,  0.6f  },
    { "fallbackalpha",&Params::fallbackAlpha,0.0f,  1.0f  },
    { "rimambient",   &Params::rimAmbient,   0.0f,  0.5f  },
    { "rimlobe",      &Params::rimLobe,      0.0f,  0.5f  },
    { "specline",     &Params::specLine,     0.0f,  0.5f  },
    { "rimenvfloor",  &Params::rimEnvFloor,  0.0f,  2.0f  },
    { "rimenvgain",   &Params::rimEnvGain,   0.0f,  4.0f  },
    { "rimouterdark", &Params::rimOuterDark, 0.0f,  1.0f  },
    { "targetmin",    &Params::targetMin,    0.0f,  1.0f  },
    { "targetmax",    &Params::targetMax,    0.0f,  1.0f  },
    { "kneebelow",    &Params::kneeBelow,    0.0f,  1.0f  },
    { "kneeabove",    &Params::kneeAbove,    0.0f,  1.0f  },
};

struct OpticsField {
    const char* name;
    float Tuning::* member;
    float lo, hi;
};

inline constexpr OpticsField kOpticsFields[] = {
    { "blursigma",       &Tuning::blurSigma,        0.0f, 60.0f },
    { "rimblursigma",    &Tuning::rimBlurSigma,     0.0f, 60.0f },
    { "bezelwidth",      &Tuning::bezelWidth,       0.0f, 60.0f },
    { "glassdepth",      &Tuning::glassDepth,       0.0f, 80.0f },
    { "maxdisplacement", &Tuning::maxDisplacement,  0.0f, 40.0f },
    { "rimspan",         &Tuning::rimSpan,          0.0f, 60.0f },
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
    static const char* kTintNames[4] = { "tintr", "tintg", "tintb", "tinta" };
    for (int i = 0; i < 4; ++i) {
        if (NameMatches(kTintNames[i], name)) {
            p.tint[i] = Clamp(value, 0.0f, 1.0f);
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
    static const char* kTintNames[4] = { "tintr", "tintg", "tintb", "tinta" };
    for (int i = 0; i < 4; ++i) {
        if (NameMatches(kTintNames[i], name)) { out = p.tint[i]; return true; }
    }
    return false;
}

} // namespace mactab::glass
