// Headless check of attract mode (idle screensaver, issue #54): the pure AttractController
// (src/core/AttractController.{h,cpp}) — QtCore-only, no window, no player, no real clock. It pins the three
// things the feature has to get right, each driven with fabricated millisecond times so the whole idle -> fire
// -> input -> restore timeline runs instantly:
//
//   * ART SELECTION / ROTATION — slideFor picks the first PREFERRED role and returns INVALID for a game with
//     none of them; buildSlides drops those; advance() WRAPS. A blank slide is worse than a skipped game.
//   * THE IDLE-FIRE LOGIC — poll() enters only when enabled, not playback-suppressed, idle past the timeout,
//     with slides, and not already active. Each of those five is asserted to BLOCK entry on its own.
//   * THE ENTER -> INPUT -> RESTORE ROUND-TRIP — the #1 correctness property. Entering captures the prior view;
//     the FIRST input dismisses AND is swallowed; the NEXT input is NOT swallowed (navigation flows again) and
//     lands on the restored view. This is the property that proves attract mode never strands the user.
//
// Prints ATTRACT-OK on success; any failure prints ATTRACT-FAIL <cond> (line) and exits non-zero.
#include "AttractController.h"
#include "../src/addons/AddonModels.h"

#include <QCoreApplication>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "ATTRACT-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

static const qint64 MIN = 60 * 1000;   // one minute in ms

// A MediaArt carrying exactly the given role -> a single url.
static MediaArt artWith(const QString& role, const QString& url)
{
    MediaArt a;
    a.addImage(role, url);
    return a;
}

static void testArtSelection()
{
    // The preferred-role contract: the wide screen-filling roles, best first, and a portrait box/poster is
    // NOT among them (a box-only game is skipped, not letterboxed full-screen).
    const QStringList roles = AttractController::preferredRoles();
    CHECK(!roles.isEmpty());
    CHECK(roles.first() == QStringLiteral("fanart"));
    CHECK(roles.contains(QStringLiteral("hero")));
    CHECK(roles.contains(QStringLiteral("screenshot")));
    CHECK(!roles.contains(QStringLiteral("box")));
    CHECK(!roles.contains(QStringLiteral("poster")));

    // slideFor picks the first PREFERRED role the art actually has. With both fanart and screenshot present,
    // fanart (higher preference) wins.
    MediaArt both;
    both.addImage(QStringLiteral("screenshot"), QStringLiteral("shot.png"));
    both.addImage(QStringLiteral("fanart"), QStringLiteral("fan.png"));
    const AttractSlide s = AttractController::slideFor(QStringLiteral("Sonic"), both);
    CHECK(s.isValid());
    CHECK(s.art == QStringLiteral("fan.png"));       // fanart beats screenshot
    CHECK(s.title == QStringLiteral("Sonic"));

    // A game whose ONLY art is a portrait box is skipped: none of the preferred roles are present.
    const AttractSlide boxOnly = AttractController::slideFor(QStringLiteral("Boxed"), artWith(QStringLiteral("box"), QStringLiteral("box.png")));
    CHECK(!boxOnly.isValid());

    // A game with no art at all is skipped.
    const AttractSlide none = AttractController::slideFor(QStringLiteral("Empty"), MediaArt{});
    CHECK(!none.isValid());
}

static void testBuildSlidesSkipsArtless()
{
    QVector<QPair<QString, MediaArt>> lib;
    lib << qMakePair(QStringLiteral("A"), artWith(QStringLiteral("fanart"), QStringLiteral("a.png")));
    lib << qMakePair(QStringLiteral("B"), MediaArt{});                                                  // no art
    lib << qMakePair(QStringLiteral("C"), artWith(QStringLiteral("box"), QStringLiteral("c.png")));     // box only
    lib << qMakePair(QStringLiteral("D"), artWith(QStringLiteral("hero"), QStringLiteral("d.png")));
    const QVector<AttractSlide> slides = AttractController::buildSlides(lib);
    CHECK(slides.size() == 2);                        // only A and D have usable art
    CHECK(slides.at(0).title == QStringLiteral("A"));
    CHECK(slides.at(1).title == QStringLiteral("D"));
}

// A controller pre-loaded with `n` valid slides, enabled, 10-minute timeout, idle seeded at t=0.
static AttractController seeded(int n)
{
    AttractController c;
    QVector<AttractSlide> slides;
    for (int i = 0; i < n; ++i) { AttractSlide s; s.title = QStringLiteral("G%1").arg(i); s.art = QStringLiteral("g%1.png").arg(i); slides << s; }
    c.setSlides(slides);
    c.setEnabled(true);
    c.setTimeoutMs(10 * MIN);
    c.resetIdle(0);
    return c;
}

static void testIdleFireGating()
{
    // Fires once idle passes the timeout.
    {
        AttractController c = seeded(3);
        CHECK(!c.poll(9 * MIN, QStringLiteral("home"), 0));   // 9 min idle < 10 min timeout: no fire
        CHECK(!c.active());
        CHECK(c.poll(10 * MIN, QStringLiteral("home"), 0));   // 10 min idle >= timeout: fires
        CHECK(c.active());
        CHECK(!c.poll(20 * MIN, QStringLiteral("home"), 0));  // already active: poll does not re-fire
    }
    // DISABLED blocks entry even when long idle.
    {
        AttractController c = seeded(3);
        c.setEnabled(false);
        CHECK(!c.poll(30 * MIN, QStringLiteral("home"), 0));
        CHECK(!c.active());
    }
    // PLAYBACK ACTIVE blocks entry AND holds the idle clock: even after playback ends the full timeout must
    // elapse again.
    {
        AttractController c = seeded(3);
        c.setPlaybackActive(true);
        CHECK(!c.poll(30 * MIN, QStringLiteral("home"), 0));  // suppressed
        CHECK(!c.active());
        c.setPlaybackActive(false);                           // playback ends at t=30min
        CHECK(!c.poll(39 * MIN, QStringLiteral("home"), 0));  // only 9 min since the clock was released
        CHECK(c.poll(40 * MIN, QStringLiteral("home"), 0));   // a fresh 10 min has now passed
        CHECK(c.active());
    }
    // NO SLIDES blocks entry.
    {
        AttractController c = seeded(0);
        CHECK(!c.poll(60 * MIN, QStringLiteral("home"), 0));
        CHECK(!c.active());
    }
    // ANY input resets the idle clock, so the timeout is measured from the last input.
    {
        AttractController c = seeded(3);
        c.noteInput(8 * MIN);                                 // input at 8 min re-arms the clock
        CHECK(!c.poll(17 * MIN, QStringLiteral("home"), 0));  // only 9 min since that input
        CHECK(c.poll(18 * MIN, QStringLiteral("home"), 0));   // 10 min since the input
    }
}

static void testRoundTrip()
{
    AttractController c = seeded(3);
    // Enter attract mode, capturing the prior view "browse:games".
    CHECK(c.poll(10 * MIN, QStringLiteral("browse:games"), 0));
    CHECK(c.active());

    // FIRST input after entering: dismissed AND swallowed, restoring exactly the captured view.
    const AttractController::InputResult first = c.noteInput(11 * MIN);
    CHECK(first.dismissed);
    CHECK(first.swallow);                                     // the wake press is consumed, not delivered
    CHECK(first.restoreToken == QStringLiteral("browse:games"));
    CHECK(!c.active());

    // NEXT input: NOT swallowed and NOT a dismiss — ordinary navigation flows to the restored view again.
    const AttractController::InputResult second = c.noteInput(12 * MIN);
    CHECK(!second.swallow);
    CHECK(!second.dismissed);

    // The captured token is per-entry: a second enter can restore a DIFFERENT view.
    CHECK(c.poll(30 * MIN, QStringLiteral("home"), 0));
    const AttractController::InputResult third = c.noteInput(31 * MIN);
    CHECK(third.dismissed);
    CHECK(third.restoreToken == QStringLiteral("home"));
}

static void testAdvanceWraps()
{
    AttractController c = seeded(3);
    CHECK(c.enter(QStringLiteral("home"), 0));
    CHECK(c.currentIndex() == 0);
    CHECK(c.currentSlide().art == QStringLiteral("g0.png"));
    CHECK(c.advance() == 1);
    CHECK(c.currentSlide().art == QStringLiteral("g1.png"));
    CHECK(c.advance() == 2);
    CHECK(c.advance() == 0);                                  // wraps back to the start
    CHECK(c.currentSlide().art == QStringLiteral("g0.png"));

    // enter's startIndex wraps too, so a caller can randomise the starting slide with any int.
    AttractController d = seeded(3);
    CHECK(d.enter(QStringLiteral("home"), 5));                // 5 % 3 == 2
    CHECK(d.currentIndex() == 2);
}

static void testStateSuppressionDismisses()
{
    // Playback coming on screen while attract is showing dismisses it immediately.
    {
        AttractController c = seeded(3);
        CHECK(c.enter(QStringLiteral("home"), 0));
        CHECK(c.active());
        c.setPlaybackActive(true);
        CHECK(!c.active());
    }
    // Turning the feature off while it is showing dismisses it too.
    {
        AttractController c = seeded(3);
        CHECK(c.enter(QStringLiteral("home"), 0));
        c.setEnabled(false);
        CHECK(!c.active());
    }
    // Clearing the library while showing dismisses it.
    {
        AttractController c = seeded(3);
        CHECK(c.enter(QStringLiteral("home"), 0));
        c.setSlides({});
        CHECK(!c.active());
        CHECK(c.currentSlide().art.isEmpty());
    }
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    testArtSelection();
    testBuildSlidesSkipsArtless();
    testIdleFireGating();
    testRoundTrip();
    testAdvanceWraps();
    testStateSuppressionDismisses();
    if (failures == 0) std::printf("ATTRACT-OK\n");
    return failures == 0 ? 0 : 1;
}
