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

    if (fails == 0) printf("PLAYBACK-OK\n");
    return fails == 0 ? 0 : 1;
}
