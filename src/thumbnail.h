#pragma once

#include "pch.h"
#include "image.h"

// Getting a window's contents onto the Mission Control overlay.
//
// There is no good documented way to do this, so there is a ladder, probed at
// startup and recorded in the diagnostics log:
//
//  1. SharedVisual   DwmpCreateSharedThumbnailVisual, dwmapi ordinal 147.
//                    Undocumented. Hands back a composition visual that drops
//                    straight into our own tree, which means it can be clipped
//                    to a rounded shape and ANIMATED BY THE COMPOSITOR at no
//                    CPU cost. That last part is why this is first: it is the
//                    only option that can fly a window from where it really is
//                    to where the arrangement puts it, which is most of what
//                    makes the gesture read as Mission Control.
//
//                    The ordinal has been at 147 since Windows 8.1 and is in
//                    shipping use elsewhere. The churn in this family of
//                    exports hit the multi-window ordinals, not this one.
//
//  2. Snapshot       PrintWindow with PW_RENDERFULLCONTENT, once per window,
//                    into a bitmap. Fully documented, works everywhere,
//                    completely static, and costs a full-size readback per
//                    window. Renders black for a few DRM-protected windows and
//                    fails outright on elevated ones.
//
//  3. IconOnly       The app's icon on a card the size and shape the window
//                    would have been. Never a hole and never a black rectangle:
//                    a black rectangle reads as a bug, a card reads as a design.
//
// Rejected: Windows.Graphics.Capture. Its window path needs Windows 10 1903,
// above this project's floor; it draws a yellow border around every captured
// window on every Windows 10 build, and the property that turns that off does
// not exist before build 20348; and thirty simultaneous frame pools is hundreds
// of megabytes of video memory. Three independent disqualifications.

namespace mactab::thumbnail {

enum class Tier {
    None,           // nothing has been probed yet
    SharedVisual,
    Snapshot,
    IconOnly,
};

const char* TierName(Tier tier);

// Probe once, hosting on `destination` and asking for `source`. Safe to call
// again; the answer is cached for the session.
//
// The probe actually calls the export and throws the result away, rather than
// only checking that it resolves. An export that resolves and then fails is the
// failure mode that matters, and it cannot be detected any other way. Pass two
// different windows of your own: whether DWM will compose a window's thumbnail
// into that same window is undocumented, and a refusal for that reason would
// say nothing about whether the export works.
Tier Probe(HWND destination, HWND source);

Tier Current();

// Force a tier, for the settings.ini escape hatch. Passing None restores
// whatever the probe decided.
void Force(Tier tier);

// The size DWM believes the source window's thumbnail is, which is not always
// its window rect. Falls back to the extended frame bounds.
bool SourceSize(HWND source, SIZE& out);

// What a thumbnail's pixels actually cover, and what part of that is the window
// you can see.
//
// These are not the same rect and the difference is why an outline drawn around
// a tile does not line up with the window inside it. A thumbnail contains the
// WHOLE window, including the invisible resize border Windows has put around
// every resizable window since Vista, which is around seven pixels a side at
// 100% scaling and draws nothing. The arrangement is built from the visible
// frame, so fitting the thumbnail to the tile by its own size leaves the
// visible window smaller than the tile and pushed up and left inside it.
bool SourceGeometry(HWND source, RECT& window, RECT& frame);

// Create a shared thumbnail visual for `source`, parented to `destination`.
//
// `device` is an IDCompositionDevice obtained by QueryInterface on the
// Windows.UI.Composition compositor, passed as void* so this header does not
// drag dcomp.h into every translation unit that mentions a thumbnail.
// `outVisual` receives an IUnknown* the caller must Release, and which casts to
// a composition visual.
//
// Returns false if the tier is unavailable or DWM refused this particular
// window, which happens for windows that are cloaked onto another desktop.
// `render` is the size DWM is asked to produce the thumbnail at.
//
// `render` is what DWM is asked to map the thumbnail into. Every public user of
// this export passes the source's own size, from ordinal 162, and displays at
// roughly 1:1, so that is what is passed here.
//
// It is NOT a quality control, whatever an earlier version of this comment
// claimed. A thumbnail is a live connection to the source's surfaces rather than
// a render into an intermediate, so the destination rectangle is a mapping and
// the sampling still happens once, at the leaf, with whatever scale the whole
// visual tree composes to. That is why asking for a smaller destination and
// asking for a larger one both looked the same.
//
// What actually decides how a downscaled thumbnail looks is the interpolation
// mode below.
bool CreateSharedVisual(void* device, HWND destination, HWND source, SIZE render,
                        void** outVisual, HTHUMBNAIL* outHandle);

// Ask the compositor to sample this visual's subtree bilinearly.
//
// Worth more than everything else in this file put together, because of one
// sentence in the documentation for IDCompositionVisual::SetBitmapInterpolationMode:
// if no visual sets the mode, the default for the ENTIRE visual tree is nearest
// neighbour. A 4K window mapped into a 400 pixel tile with nearest-neighbour
// sampling is not a downscale, it is every tenth pixel of the window and nothing
// else, which is exactly what "the previews look terrible" describes.
//
// The mode inherits down the subtree, so setting it on the visual DWM hands back
// reaches the children DWM built inside it, which we do not own and could not
// reach otherwise.
//
// Reached by QueryInterface, because the WinRT projection exposes no such
// property at any SDK version: the object handed back by an interop compositor
// is dual-natured and answers to IDCompositionVisual as well. False means it did
// not, which is worth knowing and not worth failing over.
bool SetSmoothScaling(void* visual);

void ReleaseSharedVisual(HTHUMBNAIL handle);

// One static snapshot of a window, scaled to fit inside the given box.
//
// Guarded against a hung application: PrintWindow drives the target's own paint
// path, so an unresponsive window can block the caller indefinitely. The window
// is pinged with a timeout first and skipped if it does not answer.
//
// Safe to call from a worker thread, and it should be: the guard is a fifty
// millisecond wait per window and thirty of those do not belong on the thread
// that has to produce a frame.
Bitmap Snapshot(HWND source, int maxWidth, int maxHeight);

} // namespace mactab::thumbnail
