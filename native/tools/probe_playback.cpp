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
    s2.finishResume();
    PlaybackSession s3(ini);
    s3.beginResume("X:/book.m4b");
    CHECK(qFuzzyCompare(s3.takeResumeSeek() + 1.0, 1.0), "finishResume drops the saved position");

    if (fails == 0) printf("PLAYBACK-OK\n");
    return fails == 0 ? 0 : 1;
}
