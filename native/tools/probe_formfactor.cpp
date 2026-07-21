// Headless check of the FormFactor core (src/theme2/FormFactor) — the ONE form-factor authority every
// later subsystem-D task consumes. FormFactor is a plain QtCore QObject (no Quick/Widgets), so this runs
// under the offscreen QPA in CI and pins the contract the adaptive UI leans on:
//
//   * defaults: with the stored "display/mode" resolving to "auto", a desktop build resolves Desktop, and
//     Desktop tokens are IDENTITY (uiScale 1.0, minHitPx 0, safeAreaFrac 0.0, density 1.0) — so every
//     consumer that multiplies/insets by these is a pixel-for-pixel no-op in Desktop mode;
//   * an explicit "display/mode" override beats auto, and the full token table per mode holds
//     (tv: 1.3/0/0.05/1.15 ; mobile: 1.15/44/0.0/1.1);
//   * changed() fires exactly once per REAL mode change, never on a same-mode refresh();
//   * an unknown / corrupt stored string falls back to auto -> Desktop (corrupt-ini safety).
//
// Prints FORMFACTOR-OK on success; any failure prints FORMFACTOR-FAIL <cond> and exits non-zero.
//
// Isolation: like the other core probes (see probe_meta), AppPaths::dataDir() is the probe exe's own folder
// in the build tree (portable app), so the mymediavault.ini it reads/writes is next to the probe and never
// touches a deployed install. Every assert sets "display/mode" explicitly, so a leftover ini can't skew it.
#include "FormFactor.h"
#include "Settings.h"

#include <QCoreApplication>
#include <QSignalSpy>
#include <QtGlobal>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "FORMFACTOR-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    FormFactor& ff = FormFactor::instance();

    // 1. Defaults: stored mode "auto" -> Desktop identity tokens.
    Settings::setDisplayMode(QStringLiteral("auto"));
    ff.refresh();
    CHECK(ff.mode() == FormFactor::Mode::Desktop);
    CHECK(ff.modeName() == QStringLiteral("desktop"));
    CHECK(qFuzzyCompare(ff.uiScale(), 1.0));
    CHECK(ff.minHitPx() == 0);
    CHECK(qFuzzyCompare(ff.safeAreaFrac() + 1.0, 1.0)); // == 0.0
    CHECK(qFuzzyCompare(ff.density(), 1.0));

    // 2. Explicit override beats auto; full token table per mode.
    Settings::setDisplayMode(QStringLiteral("tv"));
    ff.refresh();
    CHECK(ff.mode() == FormFactor::Mode::Tv);
    CHECK(ff.modeName() == QStringLiteral("tv"));
    CHECK(qFuzzyCompare(ff.uiScale(), 1.3));
    CHECK(ff.minHitPx() == 0);
    CHECK(qFuzzyCompare(ff.safeAreaFrac(), 0.05));
    CHECK(qFuzzyCompare(ff.density(), 1.15));

    Settings::setDisplayMode(QStringLiteral("mobile"));
    ff.refresh();
    CHECK(ff.mode() == FormFactor::Mode::Mobile);
    CHECK(ff.modeName() == QStringLiteral("mobile"));
    CHECK(qFuzzyCompare(ff.uiScale(), 1.15));
    CHECK(ff.minHitPx() == 44);
    CHECK(qFuzzyCompare(ff.safeAreaFrac() + 1.0, 1.0)); // == 0.0
    CHECK(qFuzzyCompare(ff.density(), 1.1));

    Settings::setDisplayMode(QStringLiteral("desktop"));
    ff.refresh();
    CHECK(ff.mode() == FormFactor::Mode::Desktop);
    CHECK(qFuzzyCompare(ff.uiScale(), 1.0));
    CHECK(ff.minHitPx() == 0);
    CHECK(qFuzzyCompare(ff.safeAreaFrac() + 1.0, 1.0)); // == 0.0
    CHECK(qFuzzyCompare(ff.density(), 1.0));

    // 3. changed() emitted exactly once per REAL change, and NOT on a same-mode refresh.
    // Baseline: we are in Desktop from the step above.
    {
        QSignalSpy spy(&ff, &FormFactor::changed);
        Settings::setDisplayMode(QStringLiteral("tv"));
        ff.refresh();
        CHECK(spy.count() == 1);         // Desktop -> Tv is a real change: one emit
        ff.refresh();                    // still "tv": no re-resolve difference
        CHECK(spy.count() == 1);         // same-mode refresh must NOT emit
        Settings::setDisplayMode(QStringLiteral("mobile"));
        ff.refresh();
        CHECK(spy.count() == 2);         // Tv -> Mobile: second emit
    }

    // 4. Unknown / corrupt stored string falls back to auto -> Desktop.
    Settings::setDisplayMode(QStringLiteral("banana"));
    ff.refresh();
    CHECK(ff.mode() == FormFactor::Mode::Desktop);
    CHECK(qFuzzyCompare(ff.uiScale(), 1.0));

    // 5. hitClamp(base) = qMax(int(base*uiScale), minHitPx) — the ONE size-derivation helper the widget-side
    // chokepoint (applyFormFactorWidgets) routes OSK key/bar sizing through. Pinning it here pins the real
    // pixel math. Desktop is identity (hitClamp(n)==n); tv scales by 1.3 (no floor); mobile scales by 1.15
    // then floors to 44 (so small bases snap UP to the touch target while the OSK keys stay above it).
    Settings::setDisplayMode(QStringLiteral("desktop"));
    ff.refresh();
    CHECK(ff.hitClamp(46) == 46);   // identity
    CHECK(ff.hitClamp(40) == 40);
    CHECK(ff.hitClamp(20) == 20);
    Settings::setDisplayMode(QStringLiteral("tv"));
    ff.refresh();
    CHECK(ff.hitClamp(46) == 59);   // int(46*1.3)=59
    CHECK(ff.hitClamp(40) == 52);   // int(40*1.3)=52
    CHECK(ff.hitClamp(20) == 26);   // int(20*1.3)=26, no floor
    Settings::setDisplayMode(QStringLiteral("mobile"));
    ff.refresh();
    CHECK(ff.hitClamp(46) == 52);   // int(46*1.15)=52 (> floor 44)
    CHECK(ff.hitClamp(40) == 46);   // int(40*1.15)=46 (> floor 44)
    CHECK(ff.hitClamp(20) == 44);   // int(20*1.15)=23 -> floored up to 44

    // 6. Virtual gamepad visibility resolver (D1 Task 6). resolveInput's OR-in and the overlay widget need a
    // live GL RetroView (not headless-probeable — see the report), but visibility is decided by the ONE
    // resolver Settings::virtualPadEnabled(), which RetroView::virtualPadShouldShow() delegates to — so pinning
    // it here pins the live emulator path. It resolves "auto" against the FormFactor authority (mode()), so we
    // drive FormFactor via setDisplayMode + refresh(), exactly as the running app does.
    Settings::setVirtualPad(QStringLiteral("auto"));
    Settings::setDisplayMode(QStringLiteral("desktop"));
    ff.refresh();
    CHECK(!Settings::virtualPadEnabled());                 // auto + FormFactor Desktop => hidden
    // Phase 1 resolveAuto() is always Desktop, so an explicit "mobile" override is how FormFactor reaches Mobile
    // in this probe. (Phase 2: a real mobile device under stored "auto" resolves Mobile and shows it the same
    // way, since virtualPadEnabled() reads the resolved mode() and not the raw display/mode string.)
    Settings::setDisplayMode(QStringLiteral("mobile"));
    ff.refresh();
    CHECK(Settings::virtualPadEnabled());                  // auto + FormFactor Mobile => shown
    Settings::setVirtualPad(QStringLiteral("off"));
    CHECK(!Settings::virtualPadEnabled());                 // explicit off beats Mobile
    Settings::setVirtualPad(QStringLiteral("on"));
    Settings::setDisplayMode(QStringLiteral("desktop"));
    ff.refresh();
    CHECK(Settings::virtualPadEnabled());                  // explicit on beats non-Mobile
    CHECK(Settings::virtualPadOpacity() == 45);            // default
    Settings::setVirtualPadOpacity(200);
    CHECK(Settings::virtualPadOpacity() == 100);           // clamped to 0..100
    Settings::setVirtualPadOpacity(45);
    Settings::setVirtualPad(QStringLiteral("auto"));       // restore defaults so a leftover ini can't skew later
    Settings::setDisplayMode(QStringLiteral("auto"));
    ff.refresh();

    if (failures == 0) { std::puts("FORMFACTOR-OK"); return 0; }
    std::fprintf(stderr, "FORMFACTOR: %d check(s) failed\n", failures);
    return 1;
}
