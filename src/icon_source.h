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
//
// `alphaMissing`, when supplied, reports that repair. It matters to the caller
// because the repair is lossy in a way nothing downstream can undo: the icon's
// transparency has been replaced by whatever colour it was flattened onto, and
// an edge that was antialiased against that colour stays flattened. A caller
// with another source available should prefer it over a repaired bitmap.
Bitmap FromHBitmap(HBITMAP bitmap, bool* alphaMissing = nullptr);

// Convert an icon, honouring the AND mask when the colour bitmap has no real
// alpha channel of its own.
Bitmap FromHIcon(HICON icon);

// The largest icon in an executable's own resources.
//
// This is the same icon group Explorer and the taskbar draw from, read straight
// out of the file rather than through the shell, which matters for two reasons.
// The shell will not enlarge past the largest frame an app actually ships, so
// asking it for 256 from an app whose best frame is 48 returns a 48px mark
// adrift in a 256 canvas and no way to tell that apart from an icon that is
// meant to look like that. And a Vista-era 256 frame is stored as PNG, decoded
// here through WIC with its alpha intact, where the shell's route to the same
// bytes goes through a GDI bitmap that regularly loses it.
//
// `nativePixels` reports the size the icon was actually authored at, which is
// what decides whether it can be enlarged to fill a tile.
Bitmap FromExecutableResource(const std::wstring& path, int* nativePixels = nullptr);

// Decode an image file, and the same from a block of memory.
//
// WIC rather than a minimal decoder of our own: it is in the OS, and it handles
// colour profiles, interlacing and the 16-bit and palettised PNGs that turn up
// inside icon resources.
Bitmap DecodeImageFile(const std::wstring& path);
Bitmap DecodeImageMemory(const void* data, size_t bytes);

} // namespace mactab
