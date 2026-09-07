// Headless check of RetroAchievements LEADERBOARDS and RICH PRESENCE (issue #94, increment 2).
//
// NO RETROACHIEVEMENTS ACCOUNT, NO LOGIN AND NO SOCKET ARE INVOLVED. Every case below is driven from a FAKE
// event source: hand-built ra::Event structs, written here by a human from rcheevos' documented event set
// (third_party/rcheevos/include/rc_client.h), fed straight into the real Achievements object exactly as the
// rc_client trampoline would. The Achievements client is constructed (so the real signals, the real dispatch
// and the real accessors are what run) but never logged in, so it never issues a request.
//
// WHAT IT PINS
//   §1 THE EVENT MAP. Each of rcheevos' seven leaderboard event ids resolves to the kind we route on, and an
//      unrelated id resolves to None. Fixture ids are transcribed by hand from rc_client.h, not read back out
//      of the code under test; Achievements.cpp static_asserts the same constants against the real enum, so a
//      rcheevos bump that renumbers them fails the BUILD.
//   §2 THE FOUR ATTEMPT EVENTS -> THE FOUR SIGNALS, on the real Achievements object: started, FAILED (an
//      attempt that earns nothing), submitted, and the server's answer (rank / best / entry count).
//   §3 THE TRACKER LIFECYCLE: appears on show, updates on update, disappears on hide; a SUPERSEDED attempt
//      (a second tracker starting while the first still runs, then the first ending) leaves the overlay up
//      showing the survivor; an update for a tracker that is not showing changes nothing and repaints nothing;
//      and the tracker NEVER OUTLIVES AN UNLOADED GAME (unloadGame clears it and says so).
//   §4 THE SUBMISSION RULE. RetroAchievements accepts leaderboard entries from hardcore sessions only. The
//      softcore state is VISIBLE, READABLE and MARKED - never hidden, never silently attempted - and the
//      hardcore state says it is submitting. Both arms of the rule are pinned on the pure predicate + copy;
//      the wired-up softcore arm is pinned through the real signal (a live hardcore session needs a loaded
//      game and therefore an account, which this probe deliberately does not have).
//   §5 RICH PRESENCE: the accessor returns the current string, and EMPTY for a game with no presence script
//      (here: no game at all, the same no-script path - rc_client's synthetic "Playing <title>" is refused).
//   §6 NO LEADERBOARD OR PRESENCE STRING REACHES ANY LOG. A Qt message handler captures every qDebug/qInfo/
//      qWarning emitted while §2-§5 run, and the transcript is asserted not to contain any of the distinctive
//      fixture strings.
//
// Prints RALEAD-OK on success; any failure prints RALEAD-FAIL <cond> (line) and exits non-zero.
#include "Achievements.h"
#include "Leaderboards.h"

#include <QCoreApplication>
#include <QStringList>
#include <QtGlobal>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "RALEAD-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

// ---- §6's log capture -------------------------------------------------------------------------------------
// Installed for the whole run. Nothing is forwarded to the default handler: a probe that printed the fixtures
// while asserting they are not printed would be asserting nothing.
static QStringList g_log;
static void captureHandler(QtMsgType, const QMessageLogContext&, const QString& msg) { g_log << msg; }

// The distinctive fixture strings. Deliberately unlike anything else the code could print, so a hit is a hit.
static const char* kLbTitle    = "Zzyzx Speedrun Fixture";
static const char* kLbDesc     = "Fixture description: reach the fixture gate";
static const char* kTrackValue = "0'42\"11 FIXTUREVALUE";
static const char* kSubmitted  = "0'41\"08 FIXTURESUBMIT";
static const char* kBest       = "0'39\"00 FIXTUREBEST";

// ---- fixture builders (the fake event source) --------------------------------------------------------------
static ra::Leaderboard fixtureBoard(unsigned id, const QString& value = QString())
{
    ra::Leaderboard lb;
    lb.id = id;
    lb.title = QString::fromUtf8(kLbTitle);
    lb.description = QString::fromUtf8(kLbDesc);
    lb.trackerValue = value;
    lb.lowerIsBetter = true;
    lb.active = true;
    return lb;
}

static ra::Event attemptEvent(ra::EventKind kind, unsigned id, const QString& value = QString())
{
    ra::Event e;
    e.kind = kind;
    e.leaderboard = fixtureBoard(id, value);
    return e;
}

static ra::Event trackerEvent(ra::EventKind kind, unsigned trackerId, const QString& display)
{
    ra::Event e;
    e.kind = kind;
    e.trackerId = trackerId;
    e.trackerDisplay = display;
    return e;
}

// ---- §1 ---------------------------------------------------------------------------------------------------
static void testEventMap()
{
    using ra::EventKind;
    // Ids hand-transcribed from rc_client.h's RC_CLIENT_EVENT_* enum.
    CHECK(ra::kindForRcEvent(2)  == EventKind::AttemptStarted);
    CHECK(ra::kindForRcEvent(3)  == EventKind::AttemptFailed);
    CHECK(ra::kindForRcEvent(4)  == EventKind::AttemptSubmitted);
    CHECK(ra::kindForRcEvent(13) == EventKind::SubmitResult);
    CHECK(ra::kindForRcEvent(10) == EventKind::TrackerShow);
    CHECK(ra::kindForRcEvent(11) == EventKind::TrackerHide);
    CHECK(ra::kindForRcEvent(12) == EventKind::TrackerUpdate);
    // Events this feature must NOT claim: the achievement unlock (1), the hardcore reset (14), a server error
    // (16) and the "no event" id (0). Without these the map could be a constant and nothing would notice.
    CHECK(ra::kindForRcEvent(0)  == EventKind::None);
    CHECK(ra::kindForRcEvent(1)  == EventKind::None);
    CHECK(ra::kindForRcEvent(14) == EventKind::None);
    CHECK(ra::kindForRcEvent(16) == EventKind::None);
}

// ---- §2 + §3 + §4(softcore arm): the real object, driven by the fake event source --------------------------
namespace {
struct Recorder
{
    int startedCount = 0, failedCount = 0, submittedCount = 0, resultCount = 0, trackerCount = 0;
    unsigned startedId = 0, failedId = 0, submittedId = 0, resultId = 0;
    QString startedTitle, startedDesc, failedTitle, submittedTitle, submittedValue;
    bool startedWillSubmit = true, submittedWillSubmit = true;
    QString resultSubmitted, resultBest;
    unsigned resultRank = 0, resultEntries = 0;
    bool trackerVisible = false;
    QString trackerDisplay;
};
} // namespace

static void wire(Achievements& ach, Recorder& r)
{
    QObject::connect(&ach, &Achievements::leaderboardAttemptStarted,
                     [&r](unsigned id, const QString& t, const QString& d, bool sub) {
        ++r.startedCount; r.startedId = id; r.startedTitle = t; r.startedDesc = d; r.startedWillSubmit = sub; });
    QObject::connect(&ach, &Achievements::leaderboardAttemptFailed,
                     [&r](unsigned id, const QString& t) { ++r.failedCount; r.failedId = id; r.failedTitle = t; });
    QObject::connect(&ach, &Achievements::leaderboardAttemptSubmitted,
                     [&r](unsigned id, const QString& t, const QString& v, bool sub) {
        ++r.submittedCount; r.submittedId = id; r.submittedTitle = t; r.submittedValue = v; r.submittedWillSubmit = sub; });
    QObject::connect(&ach, &Achievements::leaderboardSubmitResult,
                     [&r](unsigned id, const QString& s, const QString& b, unsigned rank, unsigned n) {
        ++r.resultCount; r.resultId = id; r.resultSubmitted = s; r.resultBest = b; r.resultRank = rank; r.resultEntries = n; });
    QObject::connect(&ach, &Achievements::leaderboardTrackerChanged,
                     [&r](bool vis, const QString& d) { ++r.trackerCount; r.trackerVisible = vis; r.trackerDisplay = d; });
}

static void testAttemptSignals(Achievements& ach)
{
    Recorder r;
    wire(ach, r);

    // 1. Attempt started. No account is signed in, so this is the honest softcore/signed-out state: the signal
    //    still fires (the attempt is REAL and readable) and carries willSubmit == false rather than pretending.
    ach.handleLeaderboardEvent(attemptEvent(ra::EventKind::AttemptStarted, 4001));
    CHECK(r.startedCount == 1);
    CHECK(r.startedId == 4001u);
    CHECK(r.startedTitle == QString::fromUtf8(kLbTitle));
    CHECK(r.startedDesc == QString::fromUtf8(kLbDesc));
    CHECK(r.startedWillSubmit == false);
    CHECK(r.failedCount == 0 && r.submittedCount == 0 && r.resultCount == 0);

    // 2. An attempt that FAILS. Its own signal, its own id - never confused with a submit.
    ach.handleLeaderboardEvent(attemptEvent(ra::EventKind::AttemptFailed, 4002));
    CHECK(r.failedCount == 1);
    CHECK(r.failedId == 4002u);
    CHECK(r.failedTitle == QString::fromUtf8(kLbTitle));
    CHECK(r.startedCount == 1 && r.submittedCount == 0 && r.resultCount == 0);

    // 3. Attempt submitted, carrying the value rc_client formatted for it. rcheevos raises this event in
    //    SOFTCORE TOO and simply sends nothing, so willSubmit is what tells the UI the truth.
    ach.handleLeaderboardEvent(attemptEvent(ra::EventKind::AttemptSubmitted, 4003, QString::fromUtf8(kTrackValue)));
    CHECK(r.submittedCount == 1);
    CHECK(r.submittedId == 4003u);
    CHECK(r.submittedValue == QString::fromUtf8(kTrackValue));
    CHECK(r.submittedWillSubmit == false);
    CHECK(r.resultCount == 0);

    // 4. The submit RESULT (the scoreboard the server sends back).
    ra::Event res;
    res.kind = ra::EventKind::SubmitResult;
    res.scoreboard.id = 4003;
    res.scoreboard.submitted = QString::fromUtf8(kSubmitted);
    res.scoreboard.best = QString::fromUtf8(kBest);
    res.scoreboard.newRank = 7;
    res.scoreboard.numEntries = 913;
    ach.handleLeaderboardEvent(res);
    CHECK(r.resultCount == 1);
    CHECK(r.resultId == 4003u);
    CHECK(r.resultSubmitted == QString::fromUtf8(kSubmitted));
    CHECK(r.resultBest == QString::fromUtf8(kBest));
    CHECK(r.resultRank == 7u);
    CHECK(r.resultEntries == 913u);

    // None of the four attempt events touches the tracker overlay.
    CHECK(r.trackerCount == 0);
    CHECK(ach.leaderboardTracker().isEmpty());

    ach.disconnect();
}

static void testTrackerLifecycle(Achievements& ach)
{
    Recorder r;
    wire(ach, r);

    // Appears.
    ach.handleLeaderboardEvent(trackerEvent(ra::EventKind::TrackerShow, 11, QStringLiteral("0'01\"00")));
    CHECK(r.trackerCount == 1);
    CHECK(r.trackerVisible == true);
    CHECK(r.trackerDisplay == QStringLiteral("0'01\"00"));
    CHECK(ach.leaderboardTracker() == QStringLiteral("0'01\"00"));

    // Updates.
    ach.handleLeaderboardEvent(trackerEvent(ra::EventKind::TrackerUpdate, 11, QStringLiteral("0'02\"00")));
    CHECK(r.trackerCount == 2);
    CHECK(r.trackerDisplay == QStringLiteral("0'02\"00"));

    // An update with the SAME value is not a change: no repaint is asked for.
    ach.handleLeaderboardEvent(trackerEvent(ra::EventKind::TrackerUpdate, 11, QStringLiteral("0'02\"00")));
    CHECK(r.trackerCount == 2);

    // An update for a tracker that is not showing must not conjure one up.
    ach.handleLeaderboardEvent(trackerEvent(ra::EventKind::TrackerUpdate, 99, QStringLiteral("ghost")));
    CHECK(r.trackerCount == 2);
    CHECK(!ach.leaderboardTracker().contains(QStringLiteral("ghost")));

    // A SUPERSEDED attempt: a second tracker starts while the first still runs...
    ach.handleLeaderboardEvent(trackerEvent(ra::EventKind::TrackerShow, 12, QStringLiteral("SCORE 1200")));
    CHECK(r.trackerCount == 3);
    CHECK(r.trackerVisible == true);
    CHECK(ach.leaderboardTracker().contains(QStringLiteral("0'02\"00")));
    CHECK(ach.leaderboardTracker().contains(QStringLiteral("SCORE 1200")));

    // ...the first one ends, and the overlay stays up showing the survivor rather than vanishing.
    ach.handleLeaderboardEvent(trackerEvent(ra::EventKind::TrackerHide, 11, QString()));
    CHECK(r.trackerCount == 4);
    CHECK(r.trackerVisible == true);
    CHECK(r.trackerDisplay == QStringLiteral("SCORE 1200"));
    CHECK(!ach.leaderboardTracker().contains(QStringLiteral("0'02\"00")));

    // A hide for an id that was never shown changes nothing.
    ach.handleLeaderboardEvent(trackerEvent(ra::EventKind::TrackerHide, 99, QString()));
    CHECK(r.trackerCount == 4);

    // Disappears when the last attempt ends.
    ach.handleLeaderboardEvent(trackerEvent(ra::EventKind::TrackerHide, 12, QString()));
    CHECK(r.trackerCount == 5);
    CHECK(r.trackerVisible == false);
    CHECK(ach.leaderboardTracker().isEmpty());

    // NEVER OUTLIVES AN UNLOADED GAME. rc_client raises no hide when a game goes away, so unloading with a
    // tracker up has to clear it and say so - otherwise a stale running value sits over the next game.
    ach.handleLeaderboardEvent(trackerEvent(ra::EventKind::TrackerShow, 13, QString::fromUtf8(kTrackValue)));
    CHECK(r.trackerVisible == true);
    CHECK(!ach.leaderboardTracker().isEmpty());
    ach.unloadGame();
    CHECK(r.trackerVisible == false);
    CHECK(ach.leaderboardTracker().isEmpty());

    // ...and unloading again is silent (nothing on screen, nothing to repaint).
    const int quiet = r.trackerCount;
    ach.unloadGame();
    CHECK(r.trackerCount == quiet);

    ach.disconnect();
}

// ---- §4: the hardcore-only submission rule, both arms ------------------------------------------------------
static void testSubmissionRule(Achievements& ach)
{
    // The predicate. Only a hardcore session by a signed-in player submits; every other combination does not.
    // Asserted per combination so a mutant that drops either half is killed by its own line.
    CHECK(ra::willSubmit(true,  true)  == true);
    CHECK(ra::willSubmit(false, true)  == false);
    CHECK(ra::willSubmit(true,  false) == false);
    CHECK(ra::willSubmit(false, false) == false);

    // The marking. Never empty - a softcore board must READ as not submitting, and a hardcore one must read as
    // submitting, so the two are told apart at a glance rather than by their absence.
    const QString soft = ra::submissionNote(false, true);
    const QString hard = ra::submissionNote(true,  true);
    const QString out  = ra::submissionNote(false, false);
    CHECK(!soft.isEmpty() && !hard.isEmpty() && !out.isEmpty());
    CHECK(soft != hard);
    CHECK(soft.contains(QStringLiteral("Not submitting")));
    CHECK(soft.contains(QStringLiteral("hardcore")));      // it says WHY, not merely that it will not
    CHECK(out.contains(QStringLiteral("Not submitting")));
    CHECK(out.contains(QStringLiteral("sign in")));
    CHECK(!hard.contains(QStringLiteral("Not submitting")));
    CHECK(hard.contains(QStringLiteral("Submitting")));

    // The notice the overlay shows. The softcore submit is the load-bearing one: rcheevos raises SUBMITTED in
    // softcore and sends nothing, so a notice that just said "submitted" would be a lie.
    const ra::Leaderboard lb = fixtureBoard(4004, QString::fromUtf8(kTrackValue));
    const QString softSubmit = ra::attemptNotice(ra::EventKind::AttemptSubmitted, lb, false);
    const QString hardSubmit = ra::attemptNotice(ra::EventKind::AttemptSubmitted, lb, true);
    CHECK(softSubmit.contains(QStringLiteral("not submitted")));
    CHECK(softSubmit.contains(QString::fromUtf8(kTrackValue)));   // still READABLE, just not sent
    CHECK(!hardSubmit.contains(QStringLiteral("not submitted")));
    // The notice is the STATUS only. It must not repeat the board's name: the notice card draws the name on
    // its own (independently elided) line, and a combined string was long enough in the live drive that the
    // elide cut the "not submitting" clause off the end - the one part that may never be lost.
    CHECK(!softSubmit.contains(QString::fromUtf8(kLbTitle)));
    CHECK(!ra::attemptNotice(ra::EventKind::AttemptStarted, lb, false).contains(QString::fromUtf8(kLbTitle)));
    CHECK(!ra::attemptNotice(ra::EventKind::AttemptFailed, lb, true).contains(QString::fromUtf8(kLbTitle)));
    CHECK(ra::attemptNotice(ra::EventKind::AttemptStarted, lb, ra::willSubmit(false, true)).contains(QStringLiteral("not submitting")));
    CHECK(!ra::attemptNotice(ra::EventKind::AttemptStarted, lb, ra::willSubmit(true, true)).contains(QStringLiteral("not submitting")));
    CHECK(ra::attemptNotice(ra::EventKind::AttemptFailed, lb, true).contains(QStringLiteral("failed")));
    // The tracker lifecycle events announce nothing on their own.
    CHECK(ra::attemptNotice(ra::EventKind::TrackerShow, lb, true).isEmpty());

    // The scoreboard line carries rank, entry count and the player's best.
    ra::Scoreboard sb;
    sb.id = 4004; sb.submitted = QString::fromUtf8(kSubmitted); sb.best = QString::fromUtf8(kBest);
    sb.newRank = 7; sb.numEntries = 913;
    const QString line = ra::scoreboardNotice(sb);
    CHECK(line.contains(QString::fromUtf8(kSubmitted)));
    CHECK(line.contains(QStringLiteral("7")));
    CHECK(line.contains(QStringLiteral("913")));
    CHECK(line.contains(QString::fromUtf8(kBest)));

    // The live client with no game loaded and nobody signed in: it does not submit, and it does not pretend to
    // have leaderboards it never fetched.
    CHECK(ach.leaderboardsSubmit() == false);
    CHECK(ach.hasLeaderboards() == false);
    CHECK(ach.leaderboards().isEmpty());
}

// ---- §5 ----------------------------------------------------------------------------------------------------
static void testRichPresence(Achievements& ach)
{
    // A game with no rich-presence script (here: no game at all - the same no-script branch) reads EMPTY. This
    // is the assertion that keeps rc_client's synthetic "Playing <title>" fallback out of the UI.
    CHECK(ach.hasRichPresence() == false);
    CHECK(ach.richPresence().isEmpty());
    // The accessor is a pull, not a stored value: asking twice in a row is legal and stays consistent.
    CHECK(ach.richPresence() == ach.richPresence());
}

// ---- §6 ----------------------------------------------------------------------------------------------------
static void testNothingLogged()
{
    const QStringList secrets{
        QString::fromUtf8(kLbTitle), QString::fromUtf8(kLbDesc), QString::fromUtf8(kTrackValue),
        QString::fromUtf8(kSubmitted), QString::fromUtf8(kBest),
        QStringLiteral("SCORE 1200"), QStringLiteral("0'02\"00"),
    };
    for (const QString& line : g_log)
        for (const QString& s : secrets)
            if (line.contains(s))
            {
                // Print the OFFENDING CALL SITE'S shape, never the line itself - reproducing it here would be
                // the same leak this section exists to forbid.
                std::fprintf(stderr, "RALEAD-FAIL a leaderboard/presence string reached the log (line %d)\n", __LINE__);
                ++failures;
                return;
            }
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    qInstallMessageHandler(captureHandler);

    testEventMap();

    {
        Achievements ach;          // real client, never logged in -> issues nothing
        testAttemptSignals(ach);
        testTrackerLifecycle(ach);
        testSubmissionRule(ach);
        testRichPresence(ach);
    }

    testNothingLogged();

    qInstallMessageHandler(nullptr);
    if (failures) { std::fprintf(stderr, "RALEAD: %d failure(s)\n", failures); return 1; }
    std::printf("RALEAD-OK\n");
    return 0;
}
