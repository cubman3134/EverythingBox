#pragma once
#include "PlayerGestures.h"
#include "../core/Settings.h"
#include "../theme2/FormFactor.h"

// The ONE place the pure recogniser's Config is built from stored preferences (issue #162). Split out of
// PlayerGestures.h on purpose: that header must stay Settings-free so the recogniser can be reasoned about
// (and probed) as arithmetic. Everything policy-shaped lives here.
//
// The form-factor gate is this function's first line and its whole point: `enabled` is true ONLY for the
// touch (Mobile) mode of the ONE form-factor authority. A desktop or TV build gets an inert recogniser, so
// the D-pad/nav path is untouched by construction rather than by every call site remembering to check. The
// harness forces the touch mode the same way a user would — the existing `display/mode` setting, which
// FormFactor already resolves — so no test-only override exists or is needed.
namespace PlayerGestures
{

inline Config configFromSettings()
{
    Config c;
    c.enabled     = (FormFactor::instance().mode() == FormFactor::Mode::Mobile);
    c.volume      = Settings::gestureVolume();
    c.brightness  = Settings::gestureBrightness();
    c.seek        = Settings::gestureSeek();
    c.doubleTap   = Settings::gestureDoubleTap();
    c.longPress   = Settings::gestureLongPress();
    c.pinch       = Settings::gesturePinch();
    c.edgeInsetPx = Settings::gestureEdgeInset();
    // #140's interval, shared — issue #162 is explicit that the skip must NOT grow a second knob of its own.
    c.jumpSeconds = Settings::audioJumpSeconds();
    return c;
}

} // namespace PlayerGestures
