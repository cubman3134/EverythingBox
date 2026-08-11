// Pad-to-keyboard runtime injector (issue #105) — the OS side of pad2key. While a standalone/PC game WE launched
// holds the foreground, this reads the pad via the existing Gamepad (SDL) layer, runs the pure pad2key::translate
// over each frame's edges, and synthesises OS keyboard events from the result — SendInput on Windows (the primary
// target, implemented); macOS CGEvent / Linux uinput degrade to a logged no-op for now (uinput needs the user in
// the `input` udev group — see the .cpp). It is gated ENTIRELY by the caller: GameLauncher only start()s it when
// Pad2KeyStore says pad2key is enabled for the launched game, so it never runs for an ordinary emulator (whose
// own pad support would double every input) — the focus/scope gate the issue makes a hard requirement.
//
// THE STUCK-KEY FOOTGUN. A key left DOWN when the poller stops is a real bug: the game (or the desktop, once we
// return) sees a key held forever. stop() therefore releases EVERY key this injector still holds before it
// returns, and the injection layer is idempotent (a redundant down for an already-held key, or an up for one we
// don't hold, is dropped) so two controls mapped to the same key can't leak a phantom hold. start() primes its
// edge state from the CURRENT pad so a button already down at launch does not fire a spurious press.
#pragma once
#include <QObject>
#include <QSet>
#include "Pad2Key.h"

class Gamepad;
class QTimer;

class Pad2KeyRuntime : public QObject
{
    Q_OBJECT
public:
    explicit Pad2KeyRuntime(QObject* parent = nullptr);
    ~Pad2KeyRuntime() override;

    // Begin synthesising `profile` from `pad` (BORROWED — not owned; must outlive the run). No-op when the
    // profile is empty or the pad is null/unavailable. Primes edge state from the pad's current frame.
    void start(Gamepad* pad, const pad2key::Profile& profile);

    // Stop polling AND release every key still held. Idempotent — safe to call when not active, and safe to call
    // twice. MUST be called before the launched game exits/loses focus, or a held key is left stuck down.
    void stop();

    bool active() const { return active_; }

private:
    void tick();
    pad2key::PadState sample(const pad2key::PadState& prev) const;   // read the pad -> a PadState frame
    void inject(const QVector<pad2key::KeyEvent>& events);           // OS key events (per platform)
    void releaseAllHeld();                                           // send key-up for everything we hold

    QTimer*          timer_ = nullptr;
    Gamepad*         pad_ = nullptr;        // borrowed
    pad2key::Profile profile_;
    pad2key::PadState prev_;
    QSet<int>        heldVk_;               // OS virtual-keys currently pressed by us (dedup + release-on-stop)
    bool             active_ = false;
    bool             warnedUnsupported_ = false;
};
