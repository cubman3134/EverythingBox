// Headless check of touch reading, issue #147 increment 1 — the reader's tap-zone presets, swipe paging, the
// dual-page landscape geometry and the keep-screen-awake refcount.
//
// Every rule worth getting wrong here is a coordinate rule, a threshold rule or a lifetime rule, and none of
// the three can be driven reliably through a live QTouchEvent stream on a machine with no touchscreen. So all
// four units are pure and this probe drives them directly:
//
//   * src/ebook/ReaderGestures.h      — the three presets, the top band, the OS edge band, the swipe
//                                       threshold and axis test, and the legacy (pre-#147) behaviour the
//                                       form-factor gate falls through to;
//   * src/ebook/ReaderGestureConfig.h — the one place stored preferences become a Config, and the FORM-FACTOR
//                                       gate that keeps a phone's preset out of the TV profile;
//   * src/ebook/ReaderSpread.h        — the wide-viewport predicate and its boundary, and which side of a
//                                       spread the first column is drawn on;
//   * src/core/KeepAwake.h            — acquire on open, release on close, and release by DESTRUCTION, which
//                                       is the teardown nobody wrote a close() handler for.
//
// What this probe deliberately also asserts is that #147 did not invent a second gesture vocabulary: the tap
// slop, the swipe travel and the edge band are read off PlayerGestures::Config (#162), and section 2 pins
// them to it by equality rather than to a number of their own.
//
// Prints READERGESTURES-OK on success; any failure prints READERGESTURES-FAIL <cond> and exits non-zero.
//
// Isolation: AppPaths::dataDir() is this process's own scratch directory (issue #42), so the everythingbox.ini
// Settings opens starts empty — which is what lets section 7 assert the DEFAULTS of the new reader keys.
#include "ReaderGestures.h"
#include "ReaderGestureConfig.h"
#include "ReaderSpread.h"
#include "KeepAwake.h"

#include <QCoreApplication>
#include <QVector>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "READERGESTURES-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

using namespace ReaderGestures;

// A 1000 x 1400 reading page — a tablet held upright. Thirds fall at 333.3 and 666.7.
static const double kW = 1000.0;
static const double kH = 1400.0;
static const double kBand = 56.0;          // the reader's declared chrome inset (EbookView::kMenuHeight)

// A Config wired the way a touch host wires it: the form-factor gate open, the band declared.
static Config touchConfig(TapPreset p)
{
    Config c;
    c.enabled = true;
    c.preset = p;
    c.topBandPx = kBand;
    return c;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ---- 1. The three presets, at the three zones. The whole of issue #147's "presets over matrices". ----
    {
        const double y = 700.0;                        // well below the band, vertically central
        const Config def = touchConfig(TapPreset::RightForward);
        CHECK(tapAction(def, 100.0, y, kW, kH) == Kind::Prev);   // left third goes back
        CHECK(tapAction(def, 500.0, y, kW, kH) == Kind::Menu);   // centre opens the menu
        CHECK(tapAction(def, 900.0, y, kW, kH) == Kind::Next);   // right third goes forward

        // The MIRROR, for the left-thumb grip: every paging zone is the other one, the menu stays put.
        const Config mir = touchConfig(TapPreset::LeftForward);
        CHECK(tapAction(mir, 100.0, y, kW, kH) == Kind::Next);
        CHECK(tapAction(mir, 500.0, y, kW, kH) == Kind::Menu);
        CHECK(tapAction(mir, 900.0, y, kW, kH) == Kind::Prev);

        // It really is the mirror and not some third arrangement: at every x the two presets swap Prev/Next.
        for (double x = 40.0; x < kW; x += 37.0)
        {
            const Kind a = tapAction(def, x, y, kW, kH);
            const Kind b = tapAction(mir, x, y, kW, kH);
            if (a == Kind::Prev)      CHECK(b == Kind::Next);
            else if (a == Kind::Next) CHECK(b == Kind::Prev);
            else                      CHECK(b == a);
        }

        // Menu-only: no paging zone anywhere, so a tap can never lose your place. Swipe is the only turn.
        const Config only = touchConfig(TapPreset::MenuOnly);
        for (double x = 40.0; x < kW - 40.0; x += 23.0)   // inside the OS edge band, which section 4 owns
            CHECK(tapAction(only, x, 700.0, kW, kH) == Kind::Menu);

        // The zone boundary itself: the thirds are the player's thirds (#162), so 333 is left and 334 is not.
        CHECK(zoneOf(333.0, kW) == -1);
        CHECK(zoneOf(334.0, kW) == 0);
        CHECK(zoneOf(666.0, kW) == 0);
        CHECK(zoneOf(667.0, kW) == 1);

        // The band across the top opens the menu WHATEVER column it falls in, in every preset — the gesture
        // every e-reader trains people to expect, and nobody qualifies "tap the top for the menu" by column.
        for (TapPreset p : { TapPreset::RightForward, TapPreset::LeftForward, TapPreset::MenuOnly })
        {
            const Config c = touchConfig(p);
            CHECK(tapAction(c, 100.0, 40.0, kW, kH) == Kind::Menu);
            CHECK(tapAction(c, 900.0, 40.0, kW, kH) == Kind::Menu);
            CHECK(tapAction(c, 900.0, kBand, kW, kH) == Kind::Menu);        // the band is inclusive…
        }
        // …and one pixel below it the paging presets have the page back. (Menu-only has no zone to hand it
        // to, which is the point of that preset, so it is not in this loop.)
        for (TapPreset p : { TapPreset::RightForward, TapPreset::LeftForward })
            CHECK(tapAction(touchConfig(p), 900.0, kBand + 1.0, kW, kH) != Kind::Menu);

        // An unknown stored preset reads as the default rather than as no zones at all.
        CHECK(presetFromInt(0) == TapPreset::RightForward);
        CHECK(presetFromInt(1) == TapPreset::LeftForward);
        CHECK(presetFromInt(2) == TapPreset::MenuOnly);
        CHECK(presetFromInt(3) == TapPreset::RightForward);
        CHECK(presetFromInt(-1) == TapPreset::RightForward);
        CHECK(presetToInt(TapPreset::MenuOnly) == 2);       // the enum's values are the on-disk format
    }

    // ---- 2. ONE vocabulary, not two (the #162 reuse the brief is explicit about). ------------------------
    {
        const PlayerGestures::Config player;
        Config r;
        CHECK(r.tapSlopPx    == player.tapSlopPx);
        CHECK(r.swipeStartPx == player.swipeStartPx);
        CHECK(r.edgeInsetPx  == player.edgeInsetPx);
        // And the zone split is literally the same arithmetic the player's double-tap uses.
        PlayerGestures::Recognizer pg;
        pg.setViewport(QSizeF(kW, kH));
        for (double x = 5.0; x < kW; x += 13.0) CHECK(zoneOf(x, kW) == pg.zoneOf(x));
    }

    // ---- 3. Swipe paging: direction, threshold at and either side of the boundary, and the axis test. ----
    {
        Config c = touchConfig(TapPreset::RightForward);
        const double t = double(c.swipeStartPx);

        CHECK(swipeAction(c, -120.0, 0.0) == Kind::Next);   // leftward = next, the page-flip convention
        CHECK(swipeAction(c,  120.0, 0.0) == Kind::Prev);

        CHECK(swipeAction(c, -(t - 1.0), 0.0) == Kind::None);   // one pixel short of the threshold: nothing
        CHECK(swipeAction(c, -t,         0.0) == Kind::Next);   // exactly the threshold: a swipe
        CHECK(swipeAction(c,  t,         0.0) == Kind::Prev);

        // Mostly vertical travel is not a page turn (the reader must not fight a scroll or a pull-down).
        CHECK(swipeAction(c, -100.0, 200.0) == Kind::None);
        CHECK(swipeAction(c, -100.0, 100.0) == Kind::None);     // a tie is not horizontal
        CHECK(swipeAction(c, -100.0,  99.0) == Kind::Next);

        // The mirror preset mirrors the TAPS, never the swipe: dragging the page leftward is "next" in both,
        // or a left-thumb reader would have taps and swipes disagreeing about which way the book runs.
        Config m = touchConfig(TapPreset::LeftForward);
        CHECK(swipeAction(m, -120.0, 0.0) == Kind::Next);
        CHECK(swipeAction(m,  120.0, 0.0) == Kind::Prev);
        // Menu-only leaves the swipe as the ONLY way to turn a page, so it had better still work.
        Config o = touchConfig(TapPreset::MenuOnly);
        CHECK(swipeAction(o, -120.0, 0.0) == Kind::Next);

        c.swipe = false;                                        // the row that switches swipe paging off
        CHECK(swipeAction(c, -400.0, 0.0) == Kind::None);
        CHECK(swipeAction(c,  400.0, 0.0) == Kind::None);
        c.swipe = true;

        // A tap is a tap only inside the slop, and the dead band between slop and swipe start belongs to
        // neither — exactly as it does in the player.
        CHECK(isTap(c, 0.0, 0.0));
        CHECK(isTap(c, double(c.tapSlopPx) - 1.0, 0.0));
        CHECK(!isTap(c, double(c.tapSlopPx), 0.0));
        CHECK(!isTap(c, 28.0, 0.0));
        CHECK(swipeAction(c, -28.0, 0.0) == Kind::None);
    }

    // ---- 4. The OS's edge band: a touch that STARTS in it is inert, for taps and swipes alike. -----------
    {
        const Config c = touchConfig(TapPreset::RightForward);
        const double i = double(c.edgeInsetPx);
        CHECK(inertStart(c, i - 1.0, 700.0, kW, kH));            // left edge — the system back swipe
        CHECK(inertStart(c, kW - i + 1.0, 700.0, kW, kH));       // right edge
        CHECK(inertStart(c, 500.0, i - 1.0, kW, kH));            // top — notifications
        CHECK(inertStart(c, 500.0, kH - i + 1.0, kW, kH));       // bottom — the home gesture
        CHECK(!inertStart(c, i, i, kW, kH));                     // the inset itself is inside the page
        CHECK(!inertStart(c, 500.0, 700.0, kW, kH));

        CHECK(tapAction(c, 4.0, 700.0, kW, kH) == Kind::None);   // and it really does refuse to act
        CHECK(tapAction(c, 4.0, 4.0, kW, kH) == Kind::None);     // even where the menu band would have won

        Config wide = c;
        wide.edgeInsetPx = 0;                                    // a kiosk with no system gestures
        CHECK(!inertStart(wide, 0.0, 0.0, kW, kH));
        CHECK(tapAction(wide, 4.0, 700.0, kW, kH) == Kind::Prev);
    }

    // ---- 5. The FORM-FACTOR gate. With it shut, the reader behaves exactly as it did before #147: the ----
    //         fixed thirds and the 80 px swipe, NOT the presets with the numbers turned down.
    {
        Config off;                                              // enabled defaults false
        off.topBandPx = kBand;
        off.preset = TapPreset::LeftForward;                     // a stored phone preset…
        off.swipe = false;                                       // …and a stored phone switch
        // …neither of which reaches a desktop or a television. This is the "device-class scoped" guarantee.
        CHECK(tapAction(off, 100.0, 700.0, kW, kH) == Kind::Prev);
        CHECK(tapAction(off, 900.0, 700.0, kW, kH) == Kind::Next);
        CHECK(tapAction(off, 500.0, 700.0, kW, kH) == Kind::Menu);
        CHECK(tapAction(off, 900.0, 20.0, kW, kH) == Kind::Menu);
        CHECK(tapAction(off, 4.0, 700.0, kW, kH) == Kind::Prev);  // no edge band existed before, and none starts now
        CHECK(swipeAction(off, -79.0, 0.0) == Kind::None);        // the 80 px it always asked for
        CHECK(swipeAction(off, -80.0, 0.0) == Kind::Next);
        CHECK(swipeAction(off,  80.0, 0.0) == Kind::Prev);
        CHECK(swipeAction(off, -32.0, 0.0) == Kind::None);        // the player's threshold does NOT apply here

        // And the legacy functions are what it fell through to, stated directly.
        CHECK(legacyTapAction(100.0, 700.0, kW, kBand) == Kind::Prev);
        CHECK(legacyTapAction(500.0, 700.0, kW, kBand) == Kind::Menu);
        CHECK(legacySwipeAction(-100.0, 0.0) == Kind::Next);
    }

    // ---- 6. Dual-page landscape: the wide-viewport predicate and its OFF-BY-ONE at the boundary. ---------
    {
        const int minW = ReaderSpread::minWideWidthPx();
        CHECK(ReaderSpread::active(true, minW, 700));             // wide and landscape: a spread
        CHECK(!ReaderSpread::active(true, minW - 1, 700));        // one pixel narrower: one column
        CHECK(ReaderSpread::active(true, minW + 1, 700));
        CHECK(!ReaderSpread::active(true, 1400, 1400));           // square is not landscape
        CHECK(!ReaderSpread::active(true, 1399, 1400));           // portrait, however wide
        CHECK(ReaderSpread::active(true, 1401, 1400));
        CHECK(!ReaderSpread::active(true, 800, 400));             // landscape but too narrow to read in two
        CHECK(!ReaderSpread::active(false, 1600, 900));           // the preference is off

        CHECK(ReaderSpread::columns(true, 1600, 900) == 2);
        CHECK(ReaderSpread::columns(true, 600, 900) == 1);
        CHECK(ReaderSpread::columns(false, 1600, 900) == 1);

        // The column box. Two columns and a gutter fill the content width exactly, and one column is the
        // whole of it — so turning the spread off cannot change the text width by a rounding error.
        const double contentW = 1400.0, left = 100.0;
        const double g = ReaderSpread::gutterFor(contentW);
        CHECK(g >= 24.0 && g <= 96.0);
        const double cw2 = ReaderSpread::columnWidth(contentW, 2, g);
        CHECK(qAbs(cw2 * 2.0 + g - contentW) < 0.001);
        CHECK(qAbs(ReaderSpread::columnWidth(contentW, 1, g) - contentW) < 0.001);
        CHECK(ReaderSpread::columnWidth(10.0, 2, 400.0) >= 1.0);   // never 0 or negative, however cramped

        // Left to right: column 0 is on the left. Right to left (#152's rule, the one ComicView's two-up
        // follows): the EARLIER column is on the RIGHT. Only the rectangles move — see ReaderSpread.h.
        CHECK(qAbs(ReaderSpread::columnLeft(left, cw2, g, 0, 2, false) - left) < 0.001);
        CHECK(qAbs(ReaderSpread::columnLeft(left, cw2, g, 1, 2, false) - (left + cw2 + g)) < 0.001);
        CHECK(qAbs(ReaderSpread::columnLeft(left, cw2, g, 0, 2, true) - (left + cw2 + g)) < 0.001);
        CHECK(qAbs(ReaderSpread::columnLeft(left, cw2, g, 1, 2, true) - left) < 0.001);
        // A single column has no sides to swap, so direction cannot move it.
        CHECK(qAbs(ReaderSpread::columnLeft(left, contentW, g, 0, 1, true) - left) < 0.001);
    }

    // ---- 7. Config from the stored settings: the defaults, and the FORM-FACTOR gate. ---------------------
    {
        Settings::setDisplayMode(QStringLiteral("desktop"));
        FormFactor::instance().refresh();
        Config c = configFromSettings(kBand);
        CHECK(!c.enabled);                                        // desktop: the presets are inert
        CHECK(c.preset == TapPreset::RightForward);               // the default arrangement
        CHECK(c.swipe);                                           // swipe paging on by default
        CHECK(c.edgeInsetPx == 24);                               // #162's key, shared — not a second one
        CHECK(qAbs(c.topBandPx - kBand) < 0.001);                 // the host's declared band, not a guess
        CHECK(!Settings::readerKeepAwake());                      // keep-awake OFF by default
        CHECK(Settings::readerDualPage());                        // dual-page landscape ON by default

        Settings::setDisplayMode(QStringLiteral("tv"));
        FormFactor::instance().refresh();
        CHECK(!configFromSettings(kBand).enabled);                // TV: inert, which is the D-pad guarantee

        Settings::setDisplayMode(QStringLiteral("mobile"));
        FormFactor::instance().refresh();
        CHECK(configFromSettings(kBand).enabled);                 // touch: the one mode with tap zones

        Settings::setReaderTapZones(1);
        CHECK(configFromSettings(kBand).preset == TapPreset::LeftForward);
        Settings::setReaderTapZones(2);
        CHECK(configFromSettings(kBand).preset == TapPreset::MenuOnly);
        Settings::setReaderTapZones(9);                           // clamped on write, like every bounded key
        CHECK(Settings::readerTapZones() == 2);
        Settings::setReaderTapZones(-3);
        CHECK(Settings::readerTapZones() == 0);

        Settings::setReaderSwipePaging(false);
        CHECK(!configFromSettings(kBand).swipe);
        CHECK(configFromSettings(kBand).preset == TapPreset::RightForward);  // one row does not disturb its neighbour
        Settings::setReaderSwipePaging(true);

        Settings::setReaderKeepAwake(true);
        CHECK(Settings::readerKeepAwake());
        Settings::setReaderKeepAwake(false);
        Settings::setReaderDualPage(false);
        CHECK(!Settings::readerDualPage());
        Settings::setReaderDualPage(true);

        Settings::setDisplayMode(QStringLiteral("auto"));
        FormFactor::instance().refresh();
    }

    // ---- 8. Keep the screen awake: acquired on open, released on close, and released by DESTRUCTION. -----
    {
        int ons = 0, offs = 0;
        bool state = false;
        KeepAwake::Registry& reg = KeepAwake::Registry::instance();
        reg.setApplier([&ons, &offs, &state](bool on) { state = on; if (on) ++ons; else ++offs; });
        CHECK(!state);                                            // the incoming applier inherits the state…
        CHECK(offs == 1);                                         // …and is TOLD it, rather than assuming
        CHECK(!reg.active());
        ons = 0; offs = 0;                                        // count the transitions, not the priming

        {
            KeepAwake::Guard g(true);                             // the reader opened with the toggle on
            CHECK(g.held());
            CHECK(reg.active());
            CHECK(state);
            CHECK(ons == 1);
            g.release();                                          // the reader closed
            CHECK(!g.held());
            CHECK(!reg.active());
            CHECK(!state);
            CHECK(offs == 1);
            g.release();                                          // idempotent: a second close is not a bug
            CHECK(offs == 1);
        }
        CHECK(!reg.active());                                     // and the destructor of a released guard is inert
        CHECK(offs == 1);

        // The teardown nobody wrote a handler for: the guard is destroyed without anyone calling release,
        // which is the reader being torn down rather than closed. The lock must not survive it.
        {
            KeepAwake::Guard g(true);
            CHECK(reg.active());
        }
        CHECK(!reg.active());
        CHECK(!state);
        CHECK(ons == 2 && offs == 2);

        // The toggle OFF holds nothing at all — no acquire, no release, no platform call either way.
        {
            KeepAwake::Guard g(false);
            CHECK(!g.held());
            CHECK(!reg.active());
        }
        CHECK(ons == 2 && offs == 2);

        // Two holders: the platform call happens on the 0->1 and the 1->0 transitions only, so one reader
        // closing cannot switch the lock off under another that is still open.
        {
            KeepAwake::Guard a(true);
            CHECK(ons == 3);
            {
                KeepAwake::Guard b(true);
                CHECK(reg.holders() == 2);
                CHECK(ons == 3);                                  // no second platform call
            }
            CHECK(reg.holders() == 1);
            CHECK(reg.active());
            CHECK(offs == 2);                                     // and none on the way back down
            CHECK(state);
        }
        CHECK(!reg.active());
        CHECK(offs == 3);

        // An unbalanced release cannot drive the count negative and strand the next acquire.
        reg.release();
        reg.release();
        CHECK(reg.holders() == 0);
        CHECK(offs == 3);
        {
            KeepAwake::Guard g(true);
            CHECK(reg.active());
        }
        CHECK(!reg.active());
    }

    if (failures) { std::fprintf(stderr, "READERGESTURES-FAIL %d check(s)\n", failures); return 1; }
    std::printf("READERGESTURES-OK\n");
    return 0;
}
