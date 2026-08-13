#pragma once

#include "pch.h"

// Most-recently-used window order.
//
// macOS Cmd-Tab is MRU-ordered, not Z-ordered, so the list has to be maintained
// continuously rather than derived when the switcher opens. This does it with a
// single EVENT_SYSTEM_FOREGROUND WinEvent hook whose callback appends an HWND
// to a deque, a few instructions per focus change, and nothing at all when the
// user is not switching windows. That is the whole idle cost of the feature.
//
// All functions are UI-thread only: WinEvent callbacks are delivered on the
// thread that installed the hook, so there is no shared state to lock.
//
// M2 replaces the shallow filtering here with real Alt-Tab eligibility rules
// and app grouping; this is the minimum needed to make M1 verifiable.

namespace mactab::foreground {

bool Start();
void Stop();

// Most recent first. Dead windows are pruned lazily at call time rather than by
// subscribing to EVENT_OBJECT_DESTROY, which fires for every menu, tooltip and
// transient UI object in the session and would be a constant CPU drip.
std::vector<HWND> Snapshot();

} // namespace mactab::foreground
