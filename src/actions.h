#pragma once

#include "pch.h"
#include "window_model.h"

// The macOS quick-action keys.
//
// One deliberate omission: nothing here ever force-terminates a process.
// TerminateProcess would lose unsaved work with no chance to prompt, and macOS's
// Cmd-Q does not do that either; it asks the app to quit and lets it refuse.

namespace mactab {

// Q  quits the application: WM_CLOSE to every top-level window it owns.
// Apps with unsaved work will put up their own save prompt, which is correct.
void QuitApp(const SwitcherApp& app);

// W  closes one window of the application: the frontmost from the app row, or
// the highlighted one when the app has been expanded into its windows, which is
// the one the user is actually looking at. Out-of-range indices do nothing.
void CloseWindow(const SwitcherApp& app, size_t index);

// H  macOS hides the application. Windows has no equivalent concept, so this
// minimises every window instead. Documented as an approximation rather than
// pretending it is the same thing.
void HideApp(const SwitcherApp& app);

} // namespace mactab
