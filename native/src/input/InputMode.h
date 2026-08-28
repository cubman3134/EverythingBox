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
// notePad() is called from the controller poll timer, so it MUST be silent when the mode is already pad, or
// the whole scene re-binds sixty times a second.
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
    QString brand() const;                // "xbox" | "playstation" | "switch" | "generic"

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
    // then answers from the factory bindings.
    void setPad(Gamepad* pad) { gamepad_ = pad; }

    void notePad(unsigned port);   // a controller press happened on this port
    void notePointer();            // a real mouse movement happened

signals:
    void changed();

private:
    InputMode() = default;
    Gamepad* gamepad_ = nullptr;   // borrowed
    bool     pad_ = false;
    unsigned port_ = 0;
};
