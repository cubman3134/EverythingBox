// Headless test for PlaybackSession: queue advance (next/prev/track-end), resume position
// round-trip through a scratch settings file, and the one-shot resume seek. Prints PLAYBACK-OK.
#include <QCoreApplication>
#include <QTemporaryDir>
#include "../src/media/PlaybackSession.h"

static int fails = 0;
#define CHECK(cond, name) do { if (cond) printf("PASS %s\n", name); \
    else { printf("FAIL %s\n", name); ++fails; } } while (0)

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir tmp;
    const QString ini = tmp.filePath("store.ini");

    PlaybackSession s(ini);
    QStringList played;
    QObject::connect(&s, &PlaybackSession::playRequested,
                     [&](const QString& p) { played << p; });
    int finished = 0;
    QObject::connect(&s, &PlaybackSession::queueFinished, [&] { ++finished; });

    s.setQueue({ "a.mp3", "b.mp3", "c.mp3" }, 1);
    CHECK(played == QStringList{ "b.mp3" }, "setQueue starts at startIndex");
    s.next();  CHECK(played.last() == "c.mp3", "next advances");
    s.prev();  CHECK(played.last() == "b.mp3", "prev steps back");
    s.handleTrackEnd();
    CHECK(played.last() == "c.mp3" && finished == 0, "track end auto-advances");
    s.handleTrackEnd();
    CHECK(finished == 1, "track end at the last track emits queueFinished");

    // Resume round-trip: position persists per file and is consumed once on re-open.
    s.beginResume("X:/book.m4b");
    s.setDuration(3600.0);
    s.setPosition(1234.0);
    s.persistResume();
    PlaybackSession s2(ini);
    s2.beginResume("X:/book.m4b");
    CHECK(qFuzzyCompare(s2.takeResumeSeek(), 1234.0), "resume position survives a new session");
    CHECK(qFuzzyCompare(s2.takeResumeSeek() + 1.0, 1.0), "resume seek is consumed once");
    // In-session: a pending (untaken) seek must die with finishResume — a late/duplicate
    // durationChanged after the file finished must never drive a stale seek.
    s2.beginResume("X:/book.m4b"); // re-arms the pending seek (1234) from the store
    s2.finishResume();
    CHECK(qFuzzyCompare(s2.takeResumeSeek() + 1.0, 1.0), "finishResume kills the pending seek in-session");
    PlaybackSession s3(ini);
    s3.beginResume("X:/book.m4b");
    CHECK(qFuzzyCompare(s3.takeResumeSeek() + 1.0, 1.0), "finishResume drops the saved position");

    // setQueue's resumeKey re-keys the starting track by a stable id instead of its file path, so a saved
    // position survives even when the queue's URL changes on re-resolve (folds in the old post-setQueue re-key).
    PlaybackSession s4(ini);
    s4.setQueue({ "a.mp3" }, 0, {}, "stable-id");
    s4.setDuration(1800.0);
    s4.setPosition(567.0);
    s4.persistResume();
    PlaybackSession s5(ini);
    s5.beginResume("stable-id");
    CHECK(qFuzzyCompare(s5.takeResumeSeek(), 567.0), "setQueue resumeKey keys resume by the stable id");

    // ---- the per-track header channel (#59) -------------------------------------------------------------
    // Before it, this class carried urls only, so every queue-driven load — a gated audio stream, an IPTV
    // channel list, and every advance within either — reached the player with no headers and 403'd. What is
    // asserted here is not "the headers arrive" but "TRACK N's headers arrive, and nobody else's": a queue is
    // exactly where a per-source secret is most likely to outlive its source.
    {
        PlaybackSession q(ini);
        QStringList paths;
        QVector<StreamHeaders::Headers> seen;
        QObject::connect(&q, &PlaybackSession::playRequested,
                         [&](const QString& p, const StreamHeaders::Headers& trackHeaders) {
                             paths << p; seen << trackHeaders;
                         });

        StreamHeaders::Headers first, second;
        first.insert("Referer", "https://one.test/");
        second.insert("X-Token", "TWO");
        // Deliberately SHORTER than the track list: a playlist whose third entry sits on another host gets no
        // headers at all, and the caller expresses that by not supplying one.
        q.setQueue({ "http://one.test/a", "http://two.test/b", "http://three.test/c" }, 0, {}, QString(),
                   { first, second });
        CHECK(paths == QStringList{ "http://one.test/a" }, "the queue starts where it was told to");
        CHECK(seen.size() == 1 && seen.at(0) == first, "track 1 is played with track 1's headers");

        q.next();
        CHECK(seen.size() == 2 && seen.at(1) == second, "an advance carries the NEXT track's headers");
        // The assertion this section exists for. An advance that emitted nothing, or emitted the previous
        // track's, is the leak: track 2 is a different host.
        CHECK(seen.size() == 2 && !seen.at(1).contains("Referer"),
              "…and not the previous track's, which belong to a different host");

        q.next();
        CHECK(seen.size() == 3 && seen.at(2).isEmpty(),
              "a track the caller supplied no headers for is played with an EMPTY set");
        // Empty is not "absent": it is what makes the player CLEAR what the last track set (MpvHeaderApply
        // writes all three properties unconditionally), so the signal has to fire for it at all.
        CHECK(paths.size() == 3 && paths.at(2) == QStringLiteral("http://three.test/c"),
              "…and is still played, rather than being skipped for having none");

        // A new queue REPLACES the header list. Appending — or keeping the old one — would index the
        // previous queue's secrets by position into a completely unrelated track list.
        q.setQueue({ "http://four.test/d" }, 0);
        CHECK(seen.size() == 4 && seen.at(3).isEmpty(),
              "a queue that supplies no headers plays with none, whatever the last queue had");

        StreamHeaders::Headers only;
        only.insert("X-Token", "FIVE");
        q.setQueue({ "http://five.test/e" }, 0, {}, QString(), { only });
        CHECK(seen.size() == 5 && seen.at(4) == only, "…and one that does, plays with its own");

        // There is deliberately NO assertion that clearQueue() drops the header list. One was written here
        // ("the same url replayed after a clear carries nothing") and mutation testing showed it inert:
        // deleting the clear from clearQueue left it passing, because trackHeaders_ is only ever READ from
        // playIndex, every route to playIndex goes through setQueue, and setQueue ASSIGNS the list. There is
        // no observable difference, so there is nothing to pin — and an assertion that survives the deletion
        // of the line it names reads as coverage while being none. (Same finding, and the same treatment, as
        // #43's forPlayUrl early return.) The clear itself stays: tracks_ and trackHeaders_ are a parallel
        // pair and clearing one without the other is the state a future reader would be bitten by.
    }

    if (fails == 0) printf("PLAYBACK-OK\n");
    return fails == 0 ? 0 : 1;
}
