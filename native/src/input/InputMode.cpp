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

QString InputMode::sampleBrand() const
{
    if (!gamepad_) return QStringLiteral("generic");
    return QString::fromStdString(gamepad_->brand(port_));
}

void InputMode::setPad(Gamepad* pad)
{
    // A different pad object means different bindings behind every chip, so that alone is a real change even
    // when the brand happens to match. Re-installing the same pad is a no-op and stays silent.
    const bool padChanged = (pad != gamepad_);
    gamepad_ = pad;
    const QString b = sampleBrand();
    if (!padChanged && b == brand_) return;
    brand_ = b;
    emit changed();
}

void InputMode::notifyBindingsChanged()
{
    // Unconditional: a remap changes neither the mode nor the brand, so there is nothing here to compare.
    brand_ = sampleBrand();
    emit changed();
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
    // The port is where the brand is READ FROM, but it is not itself a published fact — guarding on it would
    // both storm on a two-pad couch and miss a hot-swap onto the same port. Guard on what a binding can see.
    port_ = port;
    const QString b = sampleBrand();
    if (pad_ && b == brand_) return;    // already in pad mode, same spelling — stay silent (see the header)
    pad_ = true;
    brand_ = b;
    emit changed();
}

void InputMode::notePointer()
{
    if (!pad_) return;
    pad_ = false;
    emit changed();
}
