#pragma once

#include "pch.h"
#include "image.h"

// Icon extraction, squircle processing and caching.
//
// The switcher must appear within one frame of the reveal decision, and
// extracting a 256px shell icon plus rasterising a tile is far too slow to do
// on that path. So nothing here ever blocks the UI thread:
//
//   Acquire() answers from the in-memory cache, or returns false and queues the
//   work on a background thread. The panel draws a neutral placeholder for that
//   app and is told to redraw when the real tile lands.
//
// Two cache layers. The memory cache holds finished tiles at display size. The
// disk cache holds the same finished pixels, keyed by a hash that includes the
// source executable's timestamp, so from the second launch onward, an app that
// has not been updated costs a file read rather than shell round-trip plus
// resampling plus mask.

namespace mactab::icons {

struct Request {
    std::wstring key;       // app grouping key, from AppIdentity
    std::wstring exePath;
    std::wstring aumid;
    bool         packaged = false;

    // Last-resort source if the shell yields nothing: the window's own icon.
    // Usually only 32px and visibly soft, so it is genuinely a fallback.
    HWND fallbackWindow = nullptr;

    int size = 128;         // final tile size in physical pixels
};

// `notifyWindow` receives `notifyMessage` (posted, never sent) whenever one or
// more tiles finish, so the panel can refresh.
bool Start(HWND notifyWindow, UINT notifyMessage);
void Stop();

// Fills `out` and returns true when the tile is ready. Otherwise queues
// extraction and returns false. Never blocks.
bool Acquire(const Request& request, Bitmap& out);

// Friendly name from a package manifest, for packaged apps whose name cannot be
// read without the shell. Empty until resolved.
std::wstring DisplayName(const std::wstring& key);

// Drop cached tiles. Used when DPI changes make every cached size wrong.
void ClearMemoryCache();

} // namespace mactab::icons
