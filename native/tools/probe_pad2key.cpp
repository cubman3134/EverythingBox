// Headless check of the pad-to-keyboard core (src/core/Pad2Key.h) and its per-game store
// (src/core/Pad2KeyStore, issue #105) — the profiles that let a keyboard-only game (an MS-DOS title) be played
// from the couch by synthesising keystrokes from pad input. Pad2Key.h is header-only QtCore, so it runs under
// the offscreen QPA in CI and pins:
//
//   * translate() — the load-bearing edge detector: a button going up→down emits its key DOWN exactly once and
//     nothing more while held; down→up emits it UP; an unmapped control emits nothing; a combo (Y→Ctrl+Enter)
//     emits both keys down on press and both up on release, release in reverse order.
//   * resolveStick() — the analog hysteresis: engages at/over ENTER, releases only at/under EXIT, and holds its
//     state in the band between so a stick resting near the line does NOT chatter. Threaded frame-by-frame
//     through translate() exactly as the runtime does, so the stuck-key footgun (a key left DOWN when the stick
//     falls straight through the band) is proven released.
//   * defaultProfile("msdos") — the DOS default: d-pad→arrows, A→Enter, B→Esc.
//   * Profile <-> JSON round-trips (single key as a bare string, a combo as an array).
//   * Pad2KeyStore — a record round-trips by key (enabled + custom profile); an absent key reads empty; a clear
//     leaves a HUSK that still reads as "no override"; effectiveProfile falls back to the per-system default.
//
// Prints PAD2KEY-OK on success; any failure prints PAD2KEY-FAIL <cond> (line) and exits non-zero.
//
// FIXTURES ARE COMPUTED INDEPENDENTLY of the code under test: expected key sequences are hand-written literals,
// the raw ini leaf is addressed by an MD5 taken with QCryptographicHash directly (not via Pad2KeyStore::hashKey),
// and the expected JSON tokens are spelled out by hand — so an assertion cannot pass merely by re-running the
// function it is meant to check. Isolation (issue #42): AppPaths::dataDir() is this process's own scratch dir, so
// the ini starts empty and is removed at exit.
#include "Pad2Key.h"
#include "Pad2KeyStore.h"
#include "AppPaths.h"
#include "AppBrand.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <cstdio>

using namespace pad2key;

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "PAD2KEY-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

static QString md5hex(const QString& key)
{
    return QString::fromLatin1(QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Md5).toHex());
}

// A one-control-changed frame helper: start from `base`, flip one control, return the new state. Independent of
// translate — it only touches PadState.down, which the probe reasons about directly.
static PadState withControl(const PadState& base, Control c, bool v)
{
    PadState s = base; s.set(c, v); return s;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const QString iniPath = AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile);

    // A hand-authored profile — NOT defaultProfile — so the translate assertions don't track a default change.
    Profile prof;
    prof.name = QStringLiteral("test");
    prof.map.insert(Control::A, { Key::Enter });
    prof.map.insert(Control::B, { Key::Esc });
    prof.map.insert(Control::Y, { Key::Ctrl, Key::Enter });   // a combo
    prof.map.insert(Control::LStickRight, { Key::Right });     // an analog direction
    // Control::X is deliberately UNMAPPED.

    // ---- 1. A press emits the mapped key DOWN once; holding emits nothing more --------------------------------
    {
        PadState none;                                   // all released
        PadState aDown = withControl(none, Control::A, true);
        const QVector<KeyEvent> ev = translate(none, aDown, prof);
        CHECK(ev.size() == 1);
        CHECK(ev.size() == 1 && ev[0].key == Key::Enter && ev[0].down == true);

        // Held: prev==cur (A still down) -> no edge -> nothing.
        const QVector<KeyEvent> held = translate(aDown, aDown, prof);
        CHECK(held.isEmpty());
    }

    // ---- 2. A release emits the mapped key UP ----------------------------------------------------------------
    {
        PadState none;
        PadState bDown = withControl(none, Control::B, true);
        const QVector<KeyEvent> up = translate(bDown, none, prof);   // B released
        CHECK(up.size() == 1);
        CHECK(up.size() == 1 && up[0].key == Key::Esc && up[0].down == false);
    }

    // ---- 3. An unmapped control emits nothing -----------------------------------------------------------------
    {
        PadState none;
        PadState xDown = withControl(none, Control::X, true);       // X is not in the profile
        CHECK(translate(none, xDown, prof).isEmpty());
        CHECK(translate(xDown, none, prof).isEmpty());
    }

    // ---- 4. A combo emits BOTH keys down on press, BOTH up on release (release reversed) ----------------------
    {
        PadState none;
        PadState yDown = withControl(none, Control::Y, true);
        const QVector<KeyEvent> down = translate(none, yDown, prof);
        CHECK(down.size() == 2);
        // Hand-authored order: press forward = Ctrl then Enter.
        CHECK(down.size() == 2 && down[0].key == Key::Ctrl && down[0].down == true);
        CHECK(down.size() == 2 && down[1].key == Key::Enter && down[1].down == true);

        const QVector<KeyEvent> up = translate(yDown, none, prof);
        CHECK(up.size() == 2);
        // Release reversed = Enter then Ctrl, both up.
        CHECK(up.size() == 2 && up[0].key == Key::Enter && up[0].down == false);
        CHECK(up.size() == 2 && up[1].key == Key::Ctrl && up[1].down == false);
    }

    // ---- 5. resolveStick hysteresis at the exact thresholds (hand-authored boundaries) -----------------------
    {
        // Not engaged: engages only at/over ENTER.
        CHECK(resolveStick(kAxisEnter - 1, /*prevEngaged*/ false) == false);
        CHECK(resolveStick(kAxisEnter,     /*prevEngaged*/ false) == true);
        // Engaged: stays until it drops to/under EXIT.
        CHECK(resolveStick(kAxisExit + 1, /*prevEngaged*/ true) == true);
        CHECK(resolveStick(kAxisExit,     /*prevEngaged*/ true) == false);
        // In the band while NOT engaged: does not spuriously engage (pins the two distinct thresholds).
        CHECK(resolveStick(kAxisExit + 1, /*prevEngaged*/ false) == false);
    }

    // ---- 6. Analog axis through translate: down past ENTER, up only under EXIT, no chatter in the band --------
    // The runtime threads resolveStick into PadState.down[stickControl] each frame; the probe does the same, then
    // asserts translate's output. axisMag is |axisLX| in the +X (Right) direction.
    {
        auto frame = [&](const PadState& prev, int axisMag) {
            PadState cur = prev;   // carry every other control unchanged
            cur.set(Control::LStickRight, resolveStick(axisMag, prev.get(Control::LStickRight)));
            return cur;
        };

        PadState f0;                          // neutral, stick released
        PadState f1 = frame(f0, 30000);       // hard right, past ENTER -> engages
        CHECK(f1.get(Control::LStickRight) == true);
        const QVector<KeyEvent> e1 = translate(f0, f1, prof);
        CHECK(e1.size() == 1 && e1[0].key == Key::Right && e1[0].down == true);

        PadState f2 = frame(f1, 12000);       // eased into the band (EXIT < 12000 < ENTER) -> stays engaged
        CHECK(f2.get(Control::LStickRight) == true);
        CHECK(translate(f1, f2, prof).isEmpty());   // no chatter

        PadState f3 = frame(f2, 10000);       // still in the band -> still engaged, still nothing
        CHECK(f3.get(Control::LStickRight) == true);
        CHECK(translate(f2, f3, prof).isEmpty());

        PadState f4 = frame(f3, 5000);        // dropped under EXIT -> releases
        CHECK(f4.get(Control::LStickRight) == false);
        const QVector<KeyEvent> e4 = translate(f3, f4, prof);
        CHECK(e4.size() == 1 && e4[0].key == Key::Right && e4[0].down == false);

        // The footgun: a stick that rises to a band value WITHOUT first crossing ENTER never engages, so it can
        // never leave a key stuck down.
        PadState g0;
        PadState g1 = frame(g0, 10000);       // straight to a band value, never reached ENTER
        CHECK(g1.get(Control::LStickRight) == false);
        CHECK(translate(g0, g1, prof).isEmpty());
    }

    // ---- 7. defaultProfile("msdos"): the DOS default — d-pad→arrows, A→Enter, B→Esc --------------------------
    {
        const Profile dos = defaultProfile(QStringLiteral("msdos"));
        CHECK(!dos.isEmpty());
        auto mapsTo = [&](Control c, Key k) {
            const auto it = dos.map.constFind(c);
            return it != dos.map.constEnd() && it.value().size() == 1 && it.value().first() == k;
        };
        CHECK(mapsTo(Control::DpadUp,    Key::Up));
        CHECK(mapsTo(Control::DpadDown,  Key::Down));
        CHECK(mapsTo(Control::DpadLeft,  Key::Left));
        CHECK(mapsTo(Control::DpadRight, Key::Right));
        CHECK(mapsTo(Control::A,         Key::Enter));
        CHECK(mapsTo(Control::B,         Key::Esc));
        // An unknown system has no default -> pad2key synthesises nothing.
        CHECK(defaultProfile(QStringLiteral("snes")).isEmpty());
    }

    // ---- 8. Profile <-> JSON round-trips (single key as a bare string, combo as an array) --------------------
    {
        const QJsonObject j = toJson(prof);
        // Hand-authored expectations of the on-disk spelling, NOT produced by re-running toJson.
        const QJsonObject m = j.value(QStringLiteral("map")).toObject();
        CHECK(m.value(QStringLiteral("A")).toString() == QStringLiteral("Enter"));   // single key -> bare string
        CHECK(m.value(QStringLiteral("B")).toString() == QStringLiteral("Esc"));
        CHECK(m.value(QStringLiteral("Y")).isArray());                                // combo -> array
        const QJsonArray combo = m.value(QStringLiteral("Y")).toArray();
        CHECK(combo.size() == 2 && combo[0].toString() == QStringLiteral("Ctrl")
                                && combo[1].toString() == QStringLiteral("Enter"));
        CHECK(m.value(QStringLiteral("LStickRight")).toString() == QStringLiteral("Right"));

        const Profile back = fromJson(j);
        CHECK(back.name == prof.name);
        CHECK(back.map.value(Control::A) == KeyChord{ Key::Enter });
        CHECK(back.map.value(Control::Y) == (KeyChord{ Key::Ctrl, Key::Enter }));
        CHECK(back.map.value(Control::LStickRight) == KeyChord{ Key::Right });
        CHECK(!back.map.contains(Control::X));       // never authored -> still absent after a round-trip
    }

    // ---- 9. Store: a record round-trips by key; an absent key reads empty ------------------------------------
    {
        const QString key = QStringLiteral("romlib:C:/dos/Prince of Persia.dosz");
        Pad2KeyStore::Entry e; e.enabled = true; e.profile = prof;
        Pad2KeyStore::set(key, e);

        const Pad2KeyStore::Entry got = Pad2KeyStore::get(key);
        CHECK(got.enabled == true);
        CHECK(Pad2KeyStore::enabled(key));
        CHECK(got.profile.map.value(Control::A) == KeyChord{ Key::Enter });
        CHECK(Pad2KeyStore::has(key));

        const Pad2KeyStore::Entry none = Pad2KeyStore::get(QStringLiteral("romlib:C:/dos/Nothing.dosz"));
        CHECK(none.isEmpty());
        CHECK(!Pad2KeyStore::has(QStringLiteral("romlib:C:/dos/Nothing.dosz")));

        // Raw ini leaf under pad2key/items/<md5(key)>, addressed by the independent oracle.
        QSettings s(iniPath, QSettings::IniFormat);
        const QString leaf = QStringLiteral("pad2key/items/") + md5hex(key);
        const QJsonObject blob = QJsonDocument::fromJson(s.value(leaf).toString().toUtf8()).object();
        CHECK(blob.value(QStringLiteral("enabled")).toBool() == true);
        CHECK(blob.value(QStringLiteral("profile")).toObject()
                  .value(QStringLiteral("map")).toObject()
                  .value(QStringLiteral("A")).toString() == QStringLiteral("Enter"));
        CHECK(static_cast<qint64>(blob.value(QStringLiteral("updatedAt")).toDouble()) > 0);
    }

    // ---- 10. Clear leaves a HUSK that reads as "no override"; effectiveProfile falls back to the system default
    {
        const QString key = QStringLiteral("romlib:C:/dos/Doom.dosz");
        Pad2KeyStore::setEnabled(key, true);          // enabled, no custom profile
        CHECK(Pad2KeyStore::enabled(key));
        // No custom profile -> effectiveProfile is the per-system DOS default.
        const Profile eff = Pad2KeyStore::effectiveProfile(key, QStringLiteral("msdos"));
        CHECK(eff.map.value(Control::A) == KeyChord{ Key::Enter });

        Pad2KeyStore::reset(key);
        CHECK(Pad2KeyStore::get(key).isEmpty());
        CHECK(!Pad2KeyStore::enabled(key));

        QSettings s(iniPath, QSettings::IniFormat);
        const QString leaf = QStringLiteral("pad2key/items/") + md5hex(key);
        CHECK(s.contains(leaf));                       // the husk row survives for the merge
        const QJsonObject husk = QJsonDocument::fromJson(s.value(leaf).toString().toUtf8()).object();
        CHECK(husk.value(QStringLiteral("enabled")).toBool(false) == false);
        CHECK(static_cast<qint64>(husk.value(QStringLiteral("updatedAt")).toDouble()) > 0);
    }

    // ---- 11. Clearing a game that never carried a record writes NOTHING (no phantom husk) --------------------
    {
        const QString key = QStringLiteral("romlib:C:/dos/NeverTouched.dosz");
        Pad2KeyStore::reset(key);
        QSettings s(iniPath, QSettings::IniFormat);
        const QString leaf = QStringLiteral("pad2key/items/") + md5hex(key);
        CHECK(!s.contains(leaf));
    }

    if (failures == 0) std::printf("PAD2KEY-OK\n");
    else               std::fprintf(stderr, "PAD2KEY: %d check(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}
