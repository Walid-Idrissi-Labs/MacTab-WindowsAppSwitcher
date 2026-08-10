#pragma once

#include "pch.h"
#include "image.h"

// Win32 -> Bitmap conversion.
//
// Split out from image.h so that the image processing itself stays free of
// windows.h and can be run natively for visual verification.

namespace mactab {

// Convert a shell/GDI bitmap.
//
// Shell icons come back in inconsistent shapes: some HBITMAPs carry a real
// alpha channel, some are 32-bit with every alpha byte zero (which read
// literally is a fully transparent icon). This detects and repairs that.
Bitmap FromHBitmap(HBITMAP bitmap);

// Convert an icon, honouring the AND mask when the colour bitmap has no real
// alpha channel of its own.
Bitmap FromHIcon(HICON icon);

} // namespace mactab
