#pragma once

// WinRT/Composition prelude.
//
// Included ONLY by the rendering layer (panel, backdrop, text). Everything else
// includes plain "pch.h" so it stays parseable by a non-MSVC compiler for
// cross-platform syntax checking — see the note in pch.h.

#include "pch.h"

// unknwn.h must precede winrt/base.h so C++/WinRT enables classic COM support
// (winrt::implements deriving from IUnknown, and the ...Interop interfaces we
// need for DesktopWindowTarget and composition surfaces).
#include <unknwn.h>
#include <winrt/base.h>
