#pragma once

#include "pch.h"

// Finding the icon file a packaged app actually ships.
//
// The obvious route to a Store app's icon, binding shell:AppsFolder\<AUMID> and
// asking IShellItemImageFactory for an image, returns the icon already
// composited onto the background colour from the app's manifest. Windows does
// not draw it that way anywhere the user sees it: the taskbar and the built-in
// switcher use the unplated asset. So the obvious route produces exactly the
// thing that looks wrong, a small mark centred in a big coloured square, and it
// produces it at whatever size the shell felt like.
//
// Packages ship the assets themselves, next to the manifest, in every size and
// variant the app was built with. Reading the file is both higher resolution
// and closer to what Windows shows.
//
// All plain Win32: the package APIs are in kernel32, the manifest goes through
// XmlLite, and the asset is a PNG that the existing WIC decoder handles. No
// WinRT, so this stays inside the off-Windows syntax check.

namespace mactab::packages {

struct Logo {
    // The asset file to decode.
    std::wstring path;

    // BackgroundColor from the manifest, opaque, or 0 when the manifest does
    // not give one or gives "transparent". This is the colour Windows plates
    // the app with, which makes it the right colour for the tile behind it.
    uint32_t background = 0;
};

// Resolve an AUMID to the best asset file the package holds. False when the
// app is not packaged, is not installed for this user, or ships nothing usable.
bool FindLogo(const std::wstring& aumid, Logo& out);

// Something that changes when the app is updated, for the disk cache to key on.
//
// A package's full name carries its version, so the concatenated full names of
// every package in the family change on any update to any of them, including
// the resource packages the assets often live in. The equivalent for an
// ordinary app is its executable's write time, which packaged apps have no
// usable version of: their exe is under a path nobody should be reaching into,
// and for a good few of them there is no single exe at all.
std::wstring VersionTag(const std::wstring& aumid);

} // namespace mactab::packages
