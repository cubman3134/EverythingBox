// The ONE authority on which device is driving the app right now, and the object every themed surface reads
// as `input` (registered next to `form`, exactly like FormFactor). Two facts live here:
//
//   * mode — "pointer" or "pad". A controller press puts it in pad mode; a REAL mouse movement puts it back.
//     A keypress changes nothing: a keyboard on a couch is not a mouse. Startup is pointer mode even with a
//     pad attached, because the mode follows USE, not presence.
//   * brand — how the pad on the port that last sent input spells its buttons, which is what chipFor() needs.
//
// QtCore only, on purpose: no cursor code and no widgets live here, so probe_inputmode can pin the whole
// contract headlessly the way probe_formfactor pins FormFactor's. MainWindow reacts to changed() and owns
// the cursor; this object only states the fact.
//
// SIGNAL ECONOMY MATTERS. changed() is a QML binding's NOTIFY: every themed help chip re-evaluates on it.
// notePad() is called from the controller poll timer, so it MUST be silent unless something a binding can
// SEE actually moved. The guard therefore keys on the two facts this object publishes — the mode and the
// brand — and NOT on the port:
//
//   * keying on the port alone would emit twice per poll tick on a two-pad couch (notePad(1) then notePad(0)
//     each look like a change), which is the sixty-re-binds-a-second catastrophe this comment exists to stop
//     — BUT only for pads that SPELL THE SAME. Two pads of DIFFERENT brands (an Xbox pad on port 0 and a
//     DualSense on port 1, an ordinary couch here) make sampleBrand() differ from the cache on every
//     alternating call, so changed() fires every tick anyway. Each of those emits is individually correct
//     — the driving pad's brand really did change — so it is inherent to last-pad-wins, not a bug to fix
//     here. The CONSEQUENCE is a hard rule on the caller: notePad() may only be driven from a real input
//     EDGE (a button that just went down), never from every poll tick that still sees a held button;
//   * keying on the mode alone would go permanently stale on a HOT-SWAP: openControllers() hands a
//     replacement pad the lowest free port, so unplugging an Xbox pad and plugging in a DualSense calls
//     notePad(0) again with pad_ already true — the brand would keep spelling "xbox" for the rest of the
//     session, which is the exact failure this whole surface exists to prevent.
//
// So the brand is CACHED in brand_ and re-sampled on every notePad()/setPad() (SDL_GameControllerGetType is
// a struct-field read, so sampling costs nothing), and changed() fires only when the mode flipped or the
// cached brand really differs. brand() then answers from the cache, which also keeps chipFor() — re-run by
// every help chip on every changed() — off SDL entirely.
//
// A REMAP is invisible to both facts: chipFor() reads the pad's live binding, but nothing about a rewritten
// binding changes the mode or the brand. The input settings panel therefore has to say so itself, via
// notifyBindingsChanged().
#pragma once
#include <QObject>
#include <QString>

class Gamepad;

class InputMode : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString mode  READ modeName NOTIFY changed)
    Q_PROPERTY(QString brand READ brand    NOTIFY changed)
public:
    static InputMode& instance();

    QString modeName() const;             // "pointer" | "pad"
    bool    padMode() const { return pad_; }
    // The CACHED brand of the pad on the port that last sent input — re-sampled by notePad()/setPad()/
    // notifyBindingsChanged(), never read live, so a help bar full of chips costs no SDL calls at all.
    QString brand() const { return brand_; }   // "xbox" | "playstation" | "switch" | "generic"

    // Translate one help-bar chip. Resolves the hint's verb to a RetroPad id, asks the pad for that id's
    // LIVE binding (so a remap shows the button the user actually mapped), and spells it for the brand.
    // Returns `hintKey` unchanged when the hint is not one of ours or nothing is bound to it. Always
    // translates — the CALLER decides when to ask (HelpSystem only asks in pad mode).
    Q_INVOKABLE QString chipFor(const QString& hintKey) const;

    // The same translation, but only while a pad is driving: in pointer mode the caller's own key text comes
    // back. This is what prose copy calls ("Press %1 again at the end"), so the mode test is written ONCE
    // here rather than repeated at every string. HelpSystem does NOT use it — a QML binding has to read
    // `input.mode` itself to subscribe to changed(), which a plain function call would not do.
    Q_INVOKABLE QString hintText(const QString& hintKey) const;

    // The app's one Gamepad, BORROWED — not owned; must outlive this object's use. Null is fine: chipFor
    // then answers from the factory bindings. Installing a DIFFERENT pad (including null) re-samples the
    // brand and emits changed(), because every chip's binding is read out of that object; re-installing the
    // same pad with the same brand is silent.
    void setPad(Gamepad* pad);

    // "A binding was rewritten underneath you." Re-samples and emits changed() UNCONDITIONALLY, because a
    // remap moves neither the mode nor the brand and so is invisible to every other guard here.
    // OBLIGATION: the input settings panel MUST call this after Gamepad::setBinding (and after the
    // reloadMapping that follows it), or every already-drawn help chip keeps spelling the OLD button.
    void notifyBindingsChanged();

    void notePad(unsigned port);   // a controller press happened on this port
    void notePointer();            // a real mouse movement happened

signals:
    void changed();

private:
    InputMode() = default;
    QString  sampleBrand() const;  // the pad's brand right now, "generic" with no pad

    Gamepad* gamepad_ = nullptr;   // borrowed
    bool     pad_ = false;
    unsigned port_ = 0;
    // Primed to the no-pad answer so brand() is honest before anything has ever been set.
    QString  brand_ = QStringLiteral("generic");
};
