// Headless check of the input-mode authority (src/input/InputMode) — the ONE answer to "is a controller or
// a mouse driving this app right now", and the object every themed surface reads as `input`. It is plain
// QtCore (no widgets, no scene) and links Gamepad WITHOUT SDL (EVERYTHINGBOX_HAVE_SDL is set only on the
// app target), so the inert Gamepad still serves real per-port BINDINGS out of Settings — which is exactly
// the surface this probe needs. Pins:
//
//   * the app starts in pointer mode with a generic brand, even though a pad may be attached: the mode
//     follows USE, not presence;
//   * notePad()/notePointer() flip the mode and emit changed() exactly once per REAL change — a repeat of
//     the current mode is silent, or every polled controller frame would re-run every QML binding;
//   * chipFor() resolves through the pad's LIVE binding, so a remapped button renders the button the user
//     actually mapped, and an unbound verb falls back to the keyboard text;
//   * with no pad attached at all, chipFor() still answers from the factory bindings rather than blanking;
//   * the guard on notePad() keys on the MODE and the BRAND, never on the port — so reporting a second pad's
//     port is silent (a poll loop must not re-bind the scene per pad per tick). That half is pinned here.
//     The OTHER half of the same guard — a pad swapped onto the SAME port must not be missed — is NOT
//     pinned and CANNOT be; read the note on section 11 before trusting this probe about it;
//   * an out-of-range port is refused outright, rather than published as a help bar that has silently
//     reverted to keyboard text;
//   * setPad() is a real state change (a new pad object carries new bindings), and notifyBindingsChanged()
//     announces a remap, which moves neither the mode nor the brand;
//   * that announcement is COALESCED and is made by Gamepad's own mutators, not by a UI panel: N calls in one
//     turn of the event loop produce exactly ONE changed(), so a 16-row reset-to-defaults sweep re-binds the
//     scene once instead of sixteen times;
//   * the test-channel brand override REFUSES unless EB_UITEST=1 (or --uitest) is in force, so the one hook
//     that can make the app spell a pad the user does not own cannot fire in a normal run.
//
// Prints INPUTMODE-OK on success; any failure prints INPUTMODE-FAIL <cond> (line) and exits non-zero.
//
// Isolation (issue #42): AppPaths::dataDir() is this process's own scratch dir, so the everythingbox.ini
// that Settings opens starts empty — the factory-binding assertions below are only defaults while nothing
// has written the keys.
#include "InputMode.h"
#include "Gamepad.h"
#include "PadGlyphs.h"
#include "Settings.h"

#include <QCoreApplication>
#include <QSignalSpy>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "INPUTMODE-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    InputMode& im = InputMode::instance();

    // 1. Startup is pointer mode with a generic brand — nothing has been used yet.
    CHECK(im.modeName() == QStringLiteral("pointer"));
    CHECK(im.padMode() == false);
    CHECK(im.brand() == QStringLiteral("generic"));

    // 2. With no pad set, chipFor still answers from the FACTORY bindings (Xbox spelling): a help bar on a
    //    machine whose pad has not been opened yet must not render blanks.
    CHECK(im.chipFor(QStringLiteral("Enter")) == QStringLiteral("A"));
    CHECK(im.chipFor(QStringLiteral("Esc"))   == QStringLiteral("B"));

    // 3. notePad flips the mode and fires changed() ONCE; a second notePad on the same port is silent.
    {
        QSignalSpy spy(&im, &InputMode::changed);
        im.notePad(0);
        CHECK(im.modeName() == QStringLiteral("pad"));
        CHECK(im.padMode() == true);
        CHECK(spy.count() == 1);
        im.notePad(0);
        CHECK(spy.count() == 1);   // still 1: a polled pad must not re-run every QML binding
    }

    // 4. notePointer flips back, once; a repeat is silent.
    {
        QSignalSpy spy(&im, &InputMode::changed);
        im.notePointer();
        CHECK(im.modeName() == QStringLiteral("pointer"));
        CHECK(spy.count() == 1);
        im.notePointer();
        CHECK(spy.count() == 1);
    }

    // 5. With a pad attached, every hint the app owns renders its factory button (Xbox spelling, because a
    //    Gamepad built without SDL reports no controller type). Expected labels are hand-written literals.
    Gamepad pad;
    im.setPad(&pad);
    im.notePad(0);
    CHECK(im.chipFor(QStringLiteral("Enter")) == QStringLiteral("A"));    // RetroPad B  -> SDL 0
    CHECK(im.chipFor(QStringLiteral("Esc"))   == QStringLiteral("B"));    // RetroPad A  -> SDL 1
    CHECK(im.chipFor(QStringLiteral("I"))     == QStringLiteral("Y"));    // RetroPad X  -> SDL 3
    CHECK(im.chipFor(QStringLiteral("/"))     == QStringLiteral("X"));    // RetroPad Y  -> SDL 2
    CHECK(im.chipFor(QStringLiteral("S"))     == QStringLiteral("X"));    // same button, player surface
    CHECK(im.chipFor(QStringLiteral("F"))     == QStringLiteral("LB"));   // RetroPad L  -> SDL 9
    CHECK(im.chipFor(QStringLiteral("P"))     == QStringLiteral("RB"));   // RetroPad R  -> SDL 10
    CHECK(im.chipFor(QStringLiteral("T"))     == QStringLiteral("View")); // RetroPad SELECT -> SDL 4
    // The browse context menu, in the spelling the help bars author. Xbox calls SDL 6 "Menu" too, so this
    // line ALONE would also pass on a chipFor that fell through untranslated — section 7b remaps it for
    // exactly that reason. Both spellings are checked because they are two entries in verbForHint and a
    // surface depends on each: the themed help bar on "Menu", the OSK footer's commit arm on "Start".
    CHECK(im.chipFor(QStringLiteral("Menu"))  == QStringLiteral("Menu")); // RetroPad START -> SDL 6
    CHECK(im.chipFor(QStringLiteral("Start")) == QStringLiteral("Menu")); // the same verb, prose spelling

    // 5b. hintText() is chipFor gated on the mode: buttons on a pad, the caller's own key text on a pointer.
    CHECK(im.hintText(QStringLiteral("I")) == QStringLiteral("Y"));
    im.notePointer();
    CHECK(im.hintText(QStringLiteral("I")) == QStringLiteral("I"));
    CHECK(im.chipFor(QStringLiteral("I")) == QStringLiteral("Y"));   // chipFor is NOT mode-gated
    im.notePad(0);

    // 6. Pass-through survives the live path: arrow chips and third-party text keep the theme's own string.
    CHECK(im.chipFor(QString::fromUtf8("\xe2\x86\x90\xe2\x86\x92"))
          == QString::fromUtf8("\xe2\x86\x90\xe2\x86\x92"));
    CHECK(im.chipFor(QStringLiteral("Ctrl+Q")) == QStringLiteral("Ctrl+Q"));

    // 7. A REMAPPED binding renders the button the user actually mapped. Written through Settings and
    //    reloaded the way the input panel does it, not by poking InputMode.
    Settings::setPadBinding(0, /*RETRO_DEVICE_ID_JOYPAD_B*/ 0, /*SDL Y (north)*/ 3);
    pad.reloadMapping();
    CHECK(im.chipFor(QStringLiteral("Enter")) == QStringLiteral("Y"));

    // 7b. The same, for the context-menu chip — the leg that tells "Menu" translated from "Menu" apart from
    //     "Menu" handed straight back. RetroPad START is SDL 6 out of the box, which Xbox (and therefore the
    //     no-SDL generic brand) spells with the very word the chip is authored as.
    Settings::setPadBinding(0, /*RETRO_DEVICE_ID_JOYPAD_START*/ 3, /*SDL X (west)*/ 2);
    pad.reloadMapping();
    CHECK(im.chipFor(QStringLiteral("Menu"))  == QStringLiteral("X"));
    CHECK(im.chipFor(QStringLiteral("Start")) == QStringLiteral("X"));
    Settings::setPadBinding(0, 3, 6);   // back to the factory button; later sections read this map
    pad.reloadMapping();

    // 8. An UNBOUND verb keeps the keyboard text — the bar never claims a button that does not exist.
    Settings::setPadBinding(0, /*RETRO_DEVICE_ID_JOYPAD_Y (west)*/ 1, Gamepad::kUnbound);
    pad.reloadMapping();
    CHECK(im.chipFor(QStringLiteral("/")) == QStringLiteral("/"));

    // 9. The brand is read from the CACHE — sampled at the last notePad()/setPad()/notifyBindingsChanged(),
    //    never live — and an unrecognised pad (which is every pad in a no-SDL build) is generic rather than
    //    a guess.
    CHECK(im.brand() == QStringLiteral("generic"));

    // 10. The PORT-CHANGE branch. Reporting a different port while already in pad mode moves nothing a QML
    //     binding can see (same mode, same brand), so it MUST be silent: a poll loop that reports every port
    //     with input would otherwise fire changed() once per attached pad per 60Hz tick, and every help chip
    //     in the scene would re-evaluate with it. State here: pad mode, port 0, pad installed.
    {
        QSignalSpy spy(&im, &InputMode::changed);
        im.notePad(1);
        CHECK(im.padMode() == true);
        CHECK(spy.count() == 0);
        im.notePad(0);             // and straight back — this is the two-pad couch, one tick
        CHECK(spy.count() == 0);
    }

    // 11. setPad() is a real state change: a different pad object serves different bindings behind every
    //     chip. Dropping the pad emits and takes the brand back to the no-pad answer; re-installing the SAME
    //     pad is a no-op and stays silent.
    //
    //     NOTE — READ THIS BEFORE TRUSTING THIS PROBE ABOUT notePad()'s GUARD. Exactly one of the guard's
    //     two directions is pinned:
    //       * the EMIT-STORM direction IS pinned. Restore a port-keyed guard and section 10 goes red on
    //         both of its assertions — but the discriminating variable there is the port test that mutant
    //         ADDS, not the brand test it drops.
    //       * the HOT-SWAP direction (same mode, same port, a pad that now reports a DIFFERENT brand must
    //         still emit) is pinned in NEITHER direction, and cannot be. This probe links Gamepad without
    //         SDL, where brand() is a hard-coded "generic" for every port and every device, so brand_ never
    //         leaves "generic" and no assertion in this file can observe the brand comparison happening at
    //         all. Replace the guard with the mode-only form `if (pad_) return;` — which IS the
    //         permanently-stale hot-swap regression the brand cache exists to prevent — and this probe
    //         stays FULLY GREEN. Measured by mutation, not assumed. Section 12 cannot help either: it never
    //         calls notePad, and notifyBindingsChanged() emits unconditionally.
    //     So the brand half of the guard has exactly ONE form of cover: hardware verification with two pads
    //     of different brands, hot-swapped onto the same port. Nothing in CI protects it. A test hook, a
    //     friend declaration or an #ifdef in production code would only buy a fake assertion — an honest
    //     uncovered branch is worth more.
    {
        QSignalSpy spy(&im, &InputMode::changed);
        im.setPad(nullptr);
        CHECK(spy.count() == 1);
        CHECK(im.brand() == QStringLiteral("generic"));
        im.setPad(nullptr);
        CHECK(spy.count() == 1);   // same pad (none), same brand — silent
        im.setPad(&pad);
        CHECK(spy.count() == 2);
        im.setPad(&pad);
        CHECK(spy.count() == 2);
    }

    // 12. A REMAP is invisible to every guard in here — the mode stands still and so does the brand — so the
    //     mutators say so themselves via notifyBindingsChanged(). It emits unconditionally, and the
    //     already-drawn chip re-spells to the button the user just mapped.
    //
    //     The emit is now DEFERRED (section 14 pins why), so each announcement is counted after a turn of
    //     the event loop rather than at the call.
    //
    //     THE FIRST BLOCK PINS THE loadMapping HOOK, AND IT IS THE ONLY THING THAT DOES. There is
    //     deliberately NO explicit notifyBindingsChanged() in it: reloadMapping()'s own hook has to supply
    //     that emit by itself, so deleting the hook from Gamepad::loadMapping turns this red. That hook is
    //     the sole notifier on two real app paths that never call setBinding at all — the game-scope reset
    //     (which clears bindings through Settings and then reloadMapping()s) and the remap dialog's scope
    //     switch — so leaving it unpinned here would mean a silently stale help bar in both. An earlier
    //     draft DID call notify() alongside; that supplied the count on its own and left the hook
    //     invisible, which is the mistake this comment exists to stop repeating.
    {
        QCoreApplication::processEvents();   // drain anything sections 5-8 left pending
        QSignalSpy spy(&im, &InputMode::changed);
        Settings::setPadBinding(0, /*RETRO_DEVICE_ID_JOYPAD_B*/ 0, /*SDL A (south)*/ 0);
        pad.reloadMapping();               // no explicit notify: loadMapping's hook must carry this one
        QCoreApplication::processEvents();
        CHECK(spy.count() == 1);
        CHECK(im.chipFor(QStringLiteral("Enter")) == QStringLiteral("A"));   // undoes section 7's remap
        const QString brandBefore = im.brand();
        im.notifyBindingsChanged();
        QCoreApplication::processEvents();
        CHECK(spy.count() == 2);   // unconditional on purpose: only the caller knows a binding moved
        CHECK(im.modeName() == QStringLiteral("pad"));    // an announcement moves neither the mode...
        CHECK(im.brand() == brandBefore);                 // ...nor the brand
    }

    // 13. An OUT-OF-RANGE port is refused outright, changing neither the mode nor the emit count. Gamepad's
    //     binding() answers kUnbound for every id on a port past kMaxPlayers, so a bar built from one would
    //     silently revert to keyboard text on every chip, and sampleBrand() answers "generic" for that same
    //     bad port, so the brand guard cannot catch it either. Asserted from POINTER mode on purpose: in pad
    //     mode the brand comparison absorbs a bad port anyway, so only the MODE flip can tell a clamped
    //     notePad from an unclamped one.
    {
        im.notePointer();                          // back to pointer mode (this emits; the spy comes after)
        QSignalSpy spy(&im, &InputMode::changed);
        im.notePad(unsigned(Gamepad::kMaxPlayers));   // one past the last real player port
        CHECK(im.modeName() == QStringLiteral("pointer"));
        CHECK(spy.count() == 0);
        im.notePad(99);
        CHECK(im.modeName() == QStringLiteral("pointer"));
        CHECK(spy.count() == 0);
        im.notePad(0);                             // a REAL port still works — the guard is not a blanket
        CHECK(im.modeName() == QStringLiteral("pad"));
        CHECK(spy.count() == 1);
    }

    // 14. The COALESCING, and the two hooks that depend on it. notifyBindingsChanged() is called from inside
    //     Gamepad's map mutators, so the emit count is no longer set by how carefully a UI panel calls it —
    //     it is set by how many rows the user's action rewrites. A reset-to-defaults sweep writes 4 players x
    //     16 rows; without coalescing that is 64 changed()s, and every help chip in the scene re-resolves its
    //     binding on each one. So the emit is marked pending and posted once on a zero-timer.
    //
    //     Counted across a turn of the event loop, which this probe has to pump itself: it never calls exec(),
    //     so the posted callback sits in the queue until something drains it. QCoreApplication::processEvents()
    //     does drain it here — measured, not assumed: with a synchronous emit the last assertion below reads
    //     16, and with the coalescing but no pump it reads 0.
    {
        QCoreApplication::processEvents();   // start from a clean queue
        QSignalSpy spy(&im, &InputMode::changed);
        im.notifyBindingsChanged();
        im.notifyBindingsChanged();
        im.notifyBindingsChanged();
        CHECK(spy.count() == 0);             // nothing yet: the emit is deferred, not dropped
        QCoreApplication::processEvents();
        CHECK(spy.count() == 1);             // three calls, ONE re-bind of the scene
    }

    // 14b. One setBinding is one announcement — the panel does not call anything, Gamepad does.
    {
        QCoreApplication::processEvents();
        QSignalSpy spy(&im, &InputMode::changed);
        pad.setBinding(0, /*RETRO_DEVICE_ID_JOYPAD_B*/ 0, /*SDL Y (north)*/ 3);
        QCoreApplication::processEvents();
        CHECK(spy.count() == 1);
        CHECK(im.chipFor(QStringLiteral("Enter")) == QStringLiteral("Y"));   // and it really took effect
    }

    // 14c. The sweep that motivates all of it: a whole player's profile rewritten row by row, the shape of a
    //      reset-to-defaults. Sixteen writes, ONE changed(). Restore the factory map while we are here, so
    //      the chip assertions above would still hold if anything ran after this.
    {
        QCoreApplication::processEvents();
        QSignalSpy spy(&im, &InputMode::changed);
        for (unsigned id = 0; id < unsigned(Gamepad::kRetroPadButtons); ++id)
            pad.setBinding(0, id, Gamepad::defaultBinding(id));
        CHECK(spy.count() == 0);
        QCoreApplication::processEvents();
        CHECK(spy.count() == 1);
        CHECK(im.chipFor(QStringLiteral("Enter")) == QStringLiteral("A"));
    }

    // 15. The TEST-CHANNEL brand override, and the guard that keeps it out of a normal run. It exists so a
    //     human can look at the PlayStation and Switch glyph columns without owning those pads (the harness
    //     cannot reach SDL, so it cannot make a real pad report its type). The whole safety of that is one
    //     condition: it must refuse unless this process was started with EB_UITEST=1 or --uitest. So pin
    //     BOTH directions — a refusal that stopped refusing would be invisible otherwise, since the feature
    //     it guards is never exercised in a normal run.
    //
    //     The environment is written here rather than assumed: the suite does not set EB_UITEST, but nothing
    //     stops a future runner from exporting it, and this section would then silently stop testing the
    //     refusal at all.
    {
        qunsetenv("EB_UITEST");
        const QString before = im.brand();
        CHECK(im.setBrandOverrideForTest(QStringLiteral("playstation")) == false);
        CHECK(im.brand() == before);                       // and it really did not take

        qputenv("EB_UITEST", "1");
        CHECK(im.setBrandOverrideForTest(QStringLiteral("playstation")) == true);
        CHECK(im.brand() == QStringLiteral("playstation"));
        // The point of the whole exercise: the PS face glyph, U+2715, out of the real translation chain and
        // not the label table directly. fromUtf8, never QStringLiteral — CI builds this probe with GCC.
        CHECK(im.chipFor(QStringLiteral("Enter")) == QString::fromUtf8("\xe2\x9c\x95"));
        // It survives an ordinary emit path, which is why it lives in sampleBrand() and not in brand().
        im.notePad(0);
        CHECK(im.brand() == QStringLiteral("playstation"));

        // Cleared by an empty string, and the real pad's spelling comes back.
        CHECK(im.setBrandOverrideForTest(QString()) == true);
        CHECK(im.brand() == QStringLiteral("generic"));
        CHECK(im.chipFor(QStringLiteral("Enter")) == QStringLiteral("A"));
        qunsetenv("EB_UITEST");
    }

    if (failures == 0) std::printf("INPUTMODE-OK\n");
    else               std::fprintf(stderr, "INPUTMODE: %d check(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}
