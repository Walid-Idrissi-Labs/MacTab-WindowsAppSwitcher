#pragma once

#include "pch.h"

// Watching settings.ini so an edit takes effect the moment it is saved.
//
// The glass material is tuned by hand in a text editor, by someone who cannot
// build or run this program at all: the loop is type a number, save, look. The
// tray item that re-reads the file removed the build and the restart from that
// loop. This removes the last manual step in it, so saving the file IS the
// reload.
//
// Two structural rules, both load-bearing:
//
//  1. The watch runs on a DEDICATED thread, blocked in WaitForMultipleObjects.
//     A change notification has no message-queue form, so somebody has to sit
//     in a wait for it, and that somebody cannot be the UI thread.
//
//  2. The watch thread NEVER re-reads the settings itself. glass::g_tuning and
//     the two glass::Params are UI-thread-only by construction, see the note on
//     the Tuning declaration in glass.h; everything that draws reads them
//     without a lock because nothing else writes them. So this thread's whole
//     job is to post the message below and go back to waiting, and the reload
//     happens on the UI thread where every other write to the material already
//     happens.

namespace mactab::settings_watch {

// Posted to the UI window passed to Start() when anything in the settings
// folder changes.
//
// Fire-and-forget, and deliberately NOT one message per changed file. A single
// save produces several notifications (editors write a temporary file and
// rename it over the target, and a write plus a metadata update are two events
// either way), so the handler is expected to coalesce a burst rather than treat
// each one as a separate edit. See kSettingsReloadTimer in main.cpp.
//
// WM_APP + 20 keeps it clear of the switcher's block at WM_APP + 10..17 in
// hotkey.h and the panel's at WM_APP + 1..8 in main.cpp.
enum : UINT {
    WM_MACTAB_SETTINGS_CHANGED = WM_APP + 20,
};

// Opens the notification handle and spawns the watch thread. Returns false if
// the folder cannot be watched, which costs the live reload and nothing else:
// the tray item still works, so this is never a reason to fail startup.
bool Start(HWND uiWindow);

// Signals the watch thread and joins it.
void Stop();

} // namespace mactab::settings_watch
