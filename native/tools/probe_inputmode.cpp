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
//   * with no pad attached at all, chipFor() still answers from the factory bindings rather than blanking.
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

    // 8. An UNBOUND verb keeps the keyboard text — the bar never claims a button that does not exist.
    Settings::setPadBinding(0, /*RETRO_DEVICE_ID_JOYPAD_Y (west)*/ 1, Gamepad::kUnbound);
    pad.reloadMapping();
    CHECK(im.chipFor(QStringLiteral("/")) == QStringLiteral("/"));

    // 9. The brand is read from the pad on the port that last sent input, and an unrecognised pad (which is
    //    every pad in a no-SDL build) is generic rather than a guess.
    CHECK(im.brand() == QStringLiteral("generic"));

    if (failures == 0) std::printf("INPUTMODE-OK\n");
    else               std::fprintf(stderr, "INPUTMODE: %d check(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}
