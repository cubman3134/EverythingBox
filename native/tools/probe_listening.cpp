// Headless check of the listening kit's pure cores (issue #140) — the two decisions the player's audiobook
// ergonomics turn on, pulled out of MainWindow so they can be pinned without a window, a QTimer or a running
// file. QtCore-only, so it runs under the offscreen QPA in CI. It pins:
//
//   * SLEEP TIMER (src/media/SleepTimer.h, header-only, mutation-tested):
//       - expiryTime for each mode: Off is never; a minute preset / custom count is nowSec + minutes*60; a
//         non-positive minute count is never; End-of-chapter is the RUNNING chapter's end (the next chapter's
//         start), the LAST chapter's end is the file duration, and sitting before the first chapter ends at
//         chapter 0; an end at/behind the position is never (no spurious immediate fire).
//       - fadeGain is 1 outside the window, ramps linearly 1->0 across it, is 0 at/after expiry, and a
//         non-positive window disables the fade.
//       - resumeNudgeBack steps back by the nudge and clamps at 0 (never a negative seek).
//   * PER-ITEM SPEED (src/core/SpeedStore, mutation-tested resolve + a store round-trip):
//       - speedForItem: an explicit per-item speed wins for any content; else the global default; MUSIC is
//         forced to 1x unless a per-item speed was stored; a non-positive global default falls back to 1x.
//       - the store round-trips a rate by item; an absent item reads back 0.0 (unset); the raw ini leaf lives
//         under speed/items/<md5-10(key)> as {rate,updatedAt}.
//
// Prints LISTENING-OK on success; any failure prints LISTENING-FAIL <cond> (line) and exits non-zero.
//
// FIXTURES ARE COMPUTED INDEPENDENTLY of the code under test: expected expiries/gains/speeds are hand-written
// literals, and the raw speed leaf is addressed by an MD5 taken with QCryptographicHash directly (not via
// SpeedStore::hashFor), so an assertion cannot pass merely because it re-ran the function it is testing.
#include "SleepTimer.h"
#include "SpeedStore.h"
#include "AppPaths.h"
#include "AppBrand.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QVector>
#include <cmath>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "LISTENING-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

// Floating-point equality within a hair — the pure functions return exact arithmetic on the literals below,
// but comparing doubles with == invites a fragile probe.
static bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

// Independent oracle for the store's key hashing (first 10 hex of md5 over UTF-8), so the probe can address a
// book's raw ini blob without calling SpeedStore::hashFor — the discipline probe_launchopts/probe_marks use.
static QString md5hex10(const QString& key)
{
    return QString::fromLatin1(QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Md5).toHex().left(10));
}

using MediaSegments::Chapter;
namespace ST = SleepTimer;

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const QString iniPath = AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile);

    // A four-chapter book: chapters start at 0, 600, 1500 and 2400 s; the file is 3000 s long. Hand-written, so
    // the expiry assertions do not track any production chapter parser.
    const QVector<Chapter> chapters = {
        { 0.0,    QStringLiteral("Ch1") },
        { 600.0,  QStringLiteral("Ch2") },
        { 1500.0, QStringLiteral("Ch3") },
        { 2400.0, QStringLiteral("Ch4 (last)") },
    };
    const double duration = 3000.0;

    // ---- 1. expiryTime: Off is never (a negative sentinel), regardless of position/chapters ---------------
    {
        CHECK(ST::expiryTime({ ST::Mode::Off, 0.0 }, 100.0, chapters, duration) < 0.0);
    }

    // ---- 2. expiryTime: a minute preset / custom count is nowSec + minutes*60 ------------------------------
    {
        // 30-minute preset from position 100 -> 100 + 1800 = 1900 (hand-computed).
        CHECK(near(ST::expiryTime({ ST::Mode::Minutes, 30.0 }, 100.0, chapters, duration), 1900.0));
        // 15-minute preset from position 0 -> 900.
        CHECK(near(ST::expiryTime({ ST::Mode::Minutes, 15.0 }, 0.0, chapters, duration), 900.0));
        // A custom 7.5 minutes from 200 -> 200 + 450 = 650.
        CHECK(near(ST::expiryTime({ ST::Mode::Minutes, 7.5 }, 200.0, chapters, duration), 650.0));
        // A non-positive count is not a timer -> never.
        CHECK(ST::expiryTime({ ST::Mode::Minutes, 0.0 },  100.0, chapters, duration) < 0.0);
        CHECK(ST::expiryTime({ ST::Mode::Minutes, -5.0 }, 100.0, chapters, duration) < 0.0);
    }

    // ---- 3. expiryTime, End-of-chapter: the RUNNING chapter's end (the next chapter's start) --------------
    {
        // Sitting at 700 (inside Ch2, 600..1500) -> ends at Ch3's start, 1500.
        CHECK(near(ST::expiryTime({ ST::Mode::EndOfChapter, 0.0 }, 700.0, chapters, duration), 1500.0));
        // Sitting at 100 (inside Ch1, 0..600) -> ends at Ch2's start, 600.
        CHECK(near(ST::expiryTime({ ST::Mode::EndOfChapter, 0.0 }, 100.0, chapters, duration), 600.0));
        // Exactly ON a chapter boundary (1500 = Ch3 start) is inside Ch3 (0..duration span) -> ends at Ch4, 2400.
        CHECK(near(ST::expiryTime({ ST::Mode::EndOfChapter, 0.0 }, 1500.0, chapters, duration), 2400.0));
    }

    // ---- 4. expiryTime, End-of-chapter: the LAST chapter ends at the file DURATION ------------------------
    {
        // Sitting at 2500 (inside Ch4, the last, 2400..end) -> ends at duration, 3000.
        CHECK(near(ST::expiryTime({ ST::Mode::EndOfChapter, 0.0 }, 2500.0, chapters, duration), 3000.0));
        // With NO chapters at all the whole file is one region ending at duration.
        CHECK(near(ST::expiryTime({ ST::Mode::EndOfChapter, 0.0 }, 500.0, QVector<Chapter>{}, duration), 3000.0));
    }

    // ---- 5. expiryTime, End-of-chapter: an end at/behind the position is "never" (no immediate fire) ------
    {
        // Inside the last chapter but the position is already past the (unknown) duration -> never.
        CHECK(ST::expiryTime({ ST::Mode::EndOfChapter, 0.0 }, 2500.0, chapters, 0.0) < 0.0);
        // Position exactly at duration inside the last chapter -> end == pos, cannot fire -> never.
        CHECK(ST::expiryTime({ ST::Mode::EndOfChapter, 0.0 }, 3000.0, chapters, duration) < 0.0);
    }

    // ---- 6. fadeGain: 1 outside the window, linear 1->0 inside, 0 at/after expiry, disabled by a <=0 window
    {
        // 40 s remaining with a 20 s window: outside the window -> full volume.
        CHECK(near(ST::fadeGain(40.0, 20.0), 1.0));
        // Exactly at the window edge is still full (>=, no premature dip).
        CHECK(near(ST::fadeGain(20.0, 20.0), 1.0));
        // Halfway through the window (10 of 20 s left) -> 0.5, hand-computed.
        CHECK(near(ST::fadeGain(10.0, 20.0), 0.5));
        // A quarter left (5 of 20) -> 0.25.
        CHECK(near(ST::fadeGain(5.0, 20.0), 0.25));
        // At expiry and past it -> silence, clamped at 0 (never negative).
        CHECK(near(ST::fadeGain(0.0, 20.0), 0.0));
        CHECK(near(ST::fadeGain(-3.0, 20.0), 0.0));
        // A non-positive window disables the ramp: full volume right up to the hard stop.
        CHECK(near(ST::fadeGain(1.0, 0.0), 1.0));
        CHECK(near(ST::fadeGain(1.0, -5.0), 1.0));
    }

    // ---- 7. resumeNudgeBack: steps back by the nudge, clamps at 0 -----------------------------------------
    {
        // 500 s in, default 30 s nudge -> 470 (hand-computed).
        CHECK(near(ST::resumeNudgeBack(500.0), 470.0));
        // A custom nudge.
        CHECK(near(ST::resumeNudgeBack(500.0, 15.0), 485.0));
        // Near the very start, the nudge would go negative -> clamped to 0, never a negative seek.
        CHECK(near(ST::resumeNudgeBack(10.0), 0.0));
        CHECK(near(ST::resumeNudgeBack(0.0), 0.0));
        // Exactly at the nudge distance -> 0, not a tiny negative.
        CHECK(near(ST::resumeNudgeBack(30.0), 0.0));
    }

    // ---- 8. speedForItem: stored wins for any content; else default; music forced to 1x ------------------
    {
        // A stored per-item speed wins for a BOOK (isMusic=false) over the global default.
        CHECK(near(SpeedStore::speedForItem(1.5, 1.25, /*isMusic*/false), 1.5));
        // ...and wins for MUSIC too — an explicit choice is honoured regardless of type.
        CHECK(near(SpeedStore::speedForItem(1.5, 1.25, /*isMusic*/true), 1.5));
        // No stored speed, a book -> the global default.
        CHECK(near(SpeedStore::speedForItem(0.0, 1.25, /*isMusic*/false), 1.25));
        // No stored speed, MUSIC -> forced 1x, IGNORING the (non-1x) global default. This is the crux rule.
        CHECK(near(SpeedStore::speedForItem(0.0, 1.25, /*isMusic*/true), 1.0));
        // A corrupt/absent global default (<=0) falls back to 1x rather than yielding a zero rate.
        CHECK(near(SpeedStore::speedForItem(0.0, 0.0,  /*isMusic*/false), 1.0));
        CHECK(near(SpeedStore::speedForItem(0.0, -2.0, /*isMusic*/false), 1.0));
    }

    // ---- 9. Store: a rate round-trips by item; an absent item reads back 0.0 (unset) ---------------------
    {
        const QString book = QStringLiteral("audiobook:The Way of Kings");
        CHECK(near(SpeedStore::storedForItem(book), 0.0));       // nothing stored yet -> unset
        SpeedStore::setForItem(book, 1.75);
        CHECK(near(SpeedStore::storedForItem(book), 1.75));      // round-trips

        // An unrelated item carries nothing.
        CHECK(near(SpeedStore::storedForItem(QStringLiteral("audiobook:Nothing")), 0.0));

        // The raw ini leaf lives under speed/items/<md5-10(key)> as {rate,updatedAt} — addressed via the
        // independent oracle so the store's own hashing is not the thing under test.
        QSettings s(iniPath, QSettings::IniFormat);
        const QString leaf = QStringLiteral("speed/items/") + md5hex10(book);
        const QJsonObject blob = QJsonDocument::fromJson(s.value(leaf).toString().toUtf8()).object();
        CHECK(near(blob.value(QStringLiteral("rate")).toDouble(), 1.75));
        CHECK(static_cast<qint64>(blob.value(QStringLiteral("updatedAt")).toDouble()) > 0);

        // A re-save replaces the rate (upsert by item, not a second row).
        SpeedStore::setForItem(book, 1.25);
        CHECK(near(SpeedStore::storedForItem(book), 1.25));

        // Guards: an empty key and a non-positive rate write nothing / stay unset.
        SpeedStore::setForItem(QString(), 2.0);
        CHECK(near(SpeedStore::storedForItem(QString()), 0.0));
        SpeedStore::setForItem(QStringLiteral("audiobook:Bad"), 0.0);
        CHECK(near(SpeedStore::storedForItem(QStringLiteral("audiobook:Bad")), 0.0));
    }

    if (failures == 0) std::printf("LISTENING-OK\n");
    else               std::fprintf(stderr, "LISTENING: %d check(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}
