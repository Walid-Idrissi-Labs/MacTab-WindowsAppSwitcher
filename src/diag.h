#pragma once

#include "pch.h"

// Diagnostics logging.
//
// This exists because the project is developed on a machine that cannot run it.
// When something misbehaves on a real Windows box, the log is the only channel
// back — so it is deliberately chatty about decisions (which backdrop tier won,
// why a window was filtered out) and about timings, and the log file is opened
// with full sharing so it can be tailed while the app runs.
//
// Disabled by default: Enabled() is a plain bool read, and the DIAG macros
// short-circuit before evaluating their arguments, so logging costs nothing in
// a normal session.

namespace mactab::diag {

// `enabled` comes from the --diag command line flag. Writes to
// %LOCALAPPDATA%\MacTab\diag.log, truncating any previous run's log.
void Init(bool enabled);
void Shutdown();

bool Enabled();

// Full path to the active log file, or empty when logging is off.
const std::wstring& LogPath();

// printf-style. `level` is a short tag such as "INFO" / "WARN" / "FAIL".
void Writef(const char* level, const char* fmt, ...);

// Logs "<label>: N.NN ms" on destruction. Use via the MACTAB_DIAG_TIMER macro.
class ScopedTimer {
public:
    explicit ScopedTimer(const char* label);
    ~ScopedTimer();

    ScopedTimer(const ScopedTimer&)            = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;

private:
    const char* m_label;
    double      m_startMs;
};

} // namespace mactab::diag

// __VA_OPT__ requires the conforming preprocessor; CMakeLists passes
// /Zc:preprocessor. The Enabled() check is first so that argument expressions
// are never evaluated when logging is off.
#define MACTAB_DIAG(fmt, ...)                                                        \
    do {                                                                             \
        if (::mactab::diag::Enabled())                                               \
            ::mactab::diag::Writef("INFO", fmt __VA_OPT__(,) __VA_ARGS__);           \
    } while (0)

#define MACTAB_WARN(fmt, ...)                                                        \
    do {                                                                             \
        if (::mactab::diag::Enabled())                                               \
            ::mactab::diag::Writef("WARN", fmt __VA_OPT__(,) __VA_ARGS__);           \
    } while (0)

// Failures are worth recording with the OS error code attached at the call site.
#define MACTAB_FAIL(fmt, ...)                                                        \
    do {                                                                             \
        if (::mactab::diag::Enabled())                                               \
            ::mactab::diag::Writef("FAIL", fmt __VA_OPT__(,) __VA_ARGS__);           \
    } while (0)

#define MACTAB_DIAG_TIMER_CAT2(a, b) a##b
#define MACTAB_DIAG_TIMER_CAT(a, b)  MACTAB_DIAG_TIMER_CAT2(a, b)
#define MACTAB_DIAG_TIMER(label)                                                     \
    ::mactab::diag::ScopedTimer MACTAB_DIAG_TIMER_CAT(mactabTimer_, __LINE__)(label)
