#pragma once

#include "pch.h"

namespace mactab {

// Bring `target` to the foreground and give it focus.
//
// `altVirtualKey` is the Alt key whose release the hook swallowed; it is
// released synthetically before the switch. Pass 0 if no Alt is outstanding.
//
// Returns false if the window is gone or Windows refused the foreground change.
bool ActivateWindow(HWND target, WORD altVirtualKey);

} // namespace mactab
