#include "InputMode.h"
#include "Gamepad.h"
#include "PadGlyphs.h"

InputMode& InputMode::instance()
{
    static InputMode s;
    return s;
}

QString InputMode::modeName() const
{
    return pad_ ? QStringLiteral("pad") : QStringLiteral("pointer");
}

QString InputMode::brand() const
{
    if (!gamepad_) return QStringLiteral("generic");
    return QString::fromStdString(gamepad_->brand(port_));
}

QString InputMode::chipFor(const QString& hintKey) const
{
    const padglyphs::Verb v = padglyphs::verbForHint(hintKey);
    if (v == padglyphs::Verb::None) return hintKey;
    const int retroId = padglyphs::retroIdForVerb(v);
    // retroIdForVerb answers -1 for a verb it has no button for, and both binding() and defaultBinding()
    // take an UNSIGNED id — passing -1 there would wrap to 4294967295 and only be caught by a range guard
    // that exists for other reasons. Refuse it here, where the sign is still visible.
    if (retroId < 0) return hintKey;
    // With no pad opened yet, the factory mapping is still the honest answer: it is what that button will do
    // the moment a controller is plugged in, and blanking the chip would be worse than being one remap stale.
    const int sdlCode = gamepad_ ? gamepad_->binding(port_, unsigned(retroId))
                                 : Gamepad::defaultBinding(unsigned(retroId));
    return padglyphs::chip(hintKey, padglyphs::brandFromName(brand()), sdlCode);
}

QString InputMode::hintText(const QString& hintKey) const
{
    return pad_ ? chipFor(hintKey) : hintKey;
}

void InputMode::notePad(unsigned port)
{
    // The port matters even when the mode does not change: the brand is read from whichever pad is driving.
    const bool portChanged = (port != port_);
    port_ = port;
    if (pad_ && !portChanged) return;   // already in pad mode on this port — stay silent (see the header)
    pad_ = true;
    emit changed();
}

void InputMode::notePointer()
{
    if (!pad_) return;
    pad_ = false;
    emit changed();
}
