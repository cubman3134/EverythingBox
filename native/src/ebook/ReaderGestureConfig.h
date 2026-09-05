#pragma once
#include "ReaderGestures.h"
#include "../core/Settings.h"
#include "../theme2/FormFactor.h"

// The ONE place the reader's pure gesture rules become a Config from stored preferences (issue #147). Split
// out of ReaderGestures.h exactly as #162 split PlayerGestureConfig.h out of PlayerGestures.h: that header
// stays Settings-free so its rules can be reasoned about (and probed) as arithmetic, and everything
// policy-shaped lives here.
//
// The form-factor gate is this function's first line and its whole point, and it is the SAME authority the
// video player's gestures use — FormFactor's touch (Mobile) mode. That is also what "device-class scoped"
// means in practice for issue #147's tap zones: the preset a phone or tablet stores is inert on a TV, so the
// D-pad path keeps its purity by construction rather than by every call site remembering to check, and a
// household with both never has its couch reading rearranged by something it set on a phone. The harness
// forces the touch mode the way a user would — the existing `display/mode` setting FormFactor already
// resolves — so no test-only override exists or is needed.
namespace ReaderGestures
{

inline Config configFromSettings(double topBandPx)
{
    Config c;
    c.enabled   = (FormFactor::instance().mode() == FormFactor::Mode::Mobile);
    c.preset    = presetFromInt(Settings::readerTapZones());
    c.swipe     = Settings::readerSwipePaging();
    // #162's edge band, shared: the reserved strip belongs to the OS, not to whichever of our surfaces is on
    // screen, so the reader reads the player's key rather than growing a second one that could disagree.
    c.edgeInsetPx = Settings::gestureEdgeInset();
    if (topBandPx > 0.0) c.topBandPx = topBandPx;
    return c;
}

} // namespace ReaderGestures
