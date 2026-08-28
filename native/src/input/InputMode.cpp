#include "InputMode.h"
#include "Gamepad.h"
#include "PadGlyphs.h"

#include <QTimer>

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
    // COALESCED, because the callers are Gamepad's map mutators themselves: a reset-to-defaults sweep writes
    // 4 players x 16 rows through setBinding and every one of those writes lands here. Emitting synchronously
    // would re-run every help chip's binding 64 times for one user action — the same emit-storm the notePad()
    // guard exists to stop, arriving by a different door. So the first call marks the map dirty and posts ONE
    // zero-timer; every further call before the event loop comes back folds into it.
    //
    // Unconditional on purpose, unlike every other emit path in this file: this path deliberately emits even
    // when the resolved chips might be unchanged, because it cannot know what a rewritten binding map
    // resolved to without re-resolving every chip in the scene. A spurious re-bind is far cheaper than a
    // stale button.
    if (emitPending_) return;
    emitPending_ = true;
    // QTimer::singleShot is QtCore, so this keeps the file free of QtGui/QtWidgets (see the header). Bound to
    // `this` as the context object: a queued callback on a destroyed receiver is cancelled rather than run.
    // InputMode is a function-local static and outlives the loop in practice; the context argument makes that
    // correct by construction rather than by luck.
    QTimer::singleShot(0, this, [this] {
        emitPending_ = false;
        // Re-sample like every other emit path: changed() is the NOTIFY for the `brand` property too, and a
        // pad can be swapped between the call that set the flag and this turn of the loop.
        brand_ = sampleBrand();
        emit changed();
    });
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
    // A port past the last real player is not survivable further in, and it degrades SILENTLY: Gamepad's
    // binding() answers kUnbound for every id on it, so every chip falls back and the whole help bar quietly
    // reverts to keyboard text — while sampleBrand() answers "generic" for that same bad port, so the brand
    // comparison below cannot notice it either. Refuse it here, where the port is still visible.
    if (port >= unsigned(Gamepad::kMaxPlayers)) return;
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
    // changed() is the NOTIFY for the brand property too, so emitting without re-sampling would re-notify
    // every `input.brand` binding with a value this call deliberately did not refresh: swap the pad while
    // the user is on the mouse and the chips would re-evaluate still spelling the OLD brand. Every other
    // emit path here re-samples first; this one is not the exception.
    brand_ = sampleBrand();
    emit changed();
}
