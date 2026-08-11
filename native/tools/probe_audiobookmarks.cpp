// probe_audiobookmarks — the pure layers of the #140 audio-bookmark work: the per-item AudioBookmarkStore (a
// TIME-anchored twin of #136's reading bookmarks) and the jump-interval Settings default. Neither needs a
// window, so both are asserted headless here; the on-screen add / jump / list in a running audio transport is
// not headlessly drivable (the QMenu on bookmarkBtn_) and is verified by hand — this probe pins the store + the
// setting, and probe_cloudmerge pins the sync classification + the merge.
//
// Isolation: AppPaths::dataDir() is this process's own scratch directory (issue #42), so AudioBookmarkStore and
// Settings open an everythingbox.ini that starts empty and is removed at exit. The probe seeds its own profile
// id — the per-profile namespace fixture, not a defence against a previous run.
//
// FIXTURES ARE INDEPENDENT of the code under test: positions, the expected playback order and the expected
// default are hand-written; the id's exact bytes are recomputed with QCryptographicHash DIRECTLY (not via
// AudioBookmarkStore::idFor), and the raw ini leaf is read straight off the ini — so no assertion passes merely
// because it re-ran the function it is testing. Prints AUDIOBM-OK on success; any failure prints
// AUDIOBM-FAIL <cond> (line) and exits non-zero.
#include "AudioBookmarkStore.h"
#include "Tombstones.h"
#include "ProfileStore.h"
#include "Settings.h"
#include "AppPaths.h"
#include "AppBrand.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QSettings>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QStringList>
#include <QVector>
#include <QtGlobal>
#include <cmath>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "AUDIOBM-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

static bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

// Independent oracle for the store's id (md5(itemKey | whole-second), 16 hex chars) — hand-built from the SAME
// definition the header documents, with QCryptographicHash directly, so idFor's derivation is the thing tested
// rather than the thing trusted.
static QString expectId(const QString& itemKey, double posSec)
{
    const qint64 sec = qRound64(qMax(0.0, posSec));
    QByteArray seed = itemKey.toUtf8();
    seed.append('|');
    seed.append(QByteArray::number(sec));
    return QString::fromLatin1(QCryptographicHash::hash(seed, QCryptographicHash::Md5).toHex().left(16));
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const QString iniPath = AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile);

    // ---- 1. idFor: deterministic, whole-second (rounds), item-sensitive, empty item -> empty ----------------
    {
        const QString A = QStringLiteral("audiobook:Dune");
        const QString B = QStringLiteral("podcast:Neuromancer");

        // Matches the independent oracle byte-for-byte (kills any change to the seed / hash / truncation).
        CHECK(AudioBookmarkStore::idFor(A, 100.0) == expectId(A, 100.0));
        CHECK(AudioBookmarkStore::idFor(A, 100.0).size() == 16);

        // Deterministic + rounds to a whole second: 100.2 and 100.4 fold to the SAME id, 100.4 and 101.0 do not.
        CHECK(AudioBookmarkStore::idFor(A, 100.0) == AudioBookmarkStore::idFor(A, 100.0));
        CHECK(AudioBookmarkStore::idFor(A, 100.2) == AudioBookmarkStore::idFor(A, 100.4));  // same second
        CHECK(AudioBookmarkStore::idFor(A, 100.4) != AudioBookmarkStore::idFor(A, 101.0));  // a second apart
        // Item-sensitive: the same second in a different item is a different id.
        CHECK(AudioBookmarkStore::idFor(A, 100.0) != AudioBookmarkStore::idFor(B, 100.0));
        // Empty item -> empty id.
        CHECK(AudioBookmarkStore::idFor(QString(), 100.0).isEmpty());
    }

    // ---- 2. Store: add / list sorted by position, per-item filtering, no leakage, raw leaf ------------------
    {
        ProfileStore::setCurrent(QStringLiteral("abmtest"));
        const QString A = QStringLiteral("audiobook:Dune");
        const QString B = QStringLiteral("podcast:Ep42");

        // Add three to A OUT of position order + one to B.
        AudioBookmarkStore::add(A, 900.0, QStringLiteral("late"));
        AudioBookmarkStore::add(A, 60.0,  QStringLiteral("early"));
        AudioBookmarkStore::add(A, 305.5, QStringLiteral("mid"));
        AudioBookmarkStore::add(B, 12.0,  QStringLiteral("bstart"));

        // list(A) returns ONLY A's, sorted ascending by position (early @60, mid @305.5, late @900).
        const QVector<AudioBookmarkStore::Bookmark> la = AudioBookmarkStore::list(A);
        CHECK(la.size() == 3);
        CHECK(la[0].label == QStringLiteral("early") && near(la[0].posSec, 60.0));
        CHECK(la[1].label == QStringLiteral("mid")   && near(la[1].posSec, 305.5));  // the precise double survives
        CHECK(la[2].label == QStringLiteral("late")  && near(la[2].posSec, 900.0));
        for (const AudioBookmarkStore::Bookmark& b : la) CHECK(b.itemKey == A);       // no item-B leakage
        CHECK(AudioBookmarkStore::list(B).size() == 1);
        CHECK(AudioBookmarkStore::list(QString()).isEmpty());                          // empty key -> empty

        // An empty item key adds nothing (no identity).
        AudioBookmarkStore::add(QString(), 10.0, QStringLiteral("nope"));
        CHECK(AudioBookmarkStore::all().size() == 4);                                  // 3 in A + 1 in B, unchanged

        // The raw ini leaf lives under audiobookmarks/<profile>/items as a JSON array carrying posSec + label —
        // addressed straight off the ini (independent of the store's reader).
        QSettings raw(iniPath, QSettings::IniFormat);
        const QString leaf = QStringLiteral("audiobookmarks/abmtest/items");
        const QJsonArray arr = QJsonDocument::fromJson(raw.value(leaf).toString().toUtf8()).array();
        int inA = 0; bool sawMid = false;
        for (const QJsonValue& v : arr)
        {
            const QJsonObject o = v.toObject();
            if (o.value(QStringLiteral("itemKey")).toString() == A) ++inA;
            if (near(o.value(QStringLiteral("posSec")).toDouble(), 305.5)
                && o.value(QStringLiteral("label")).toString() == QStringLiteral("mid")) sawMid = true;
        }
        CHECK(inA == 3);
        CHECK(sawMid);

        // Idempotent by rounded position: re-adding within the same second folds onto the one id (updates
        // label/ts), never a duplicate row. 305.5 rounds to 306 -> re-adding 305.9 (also 306) hits the same id.
        const int before = AudioBookmarkStore::list(A).size();
        AudioBookmarkStore::add(A, 305.9, QStringLiteral("mid (renamed)"));
        const QVector<AudioBookmarkStore::Bookmark> la2 = AudioBookmarkStore::list(A);
        CHECK(la2.size() == before);                                                  // no new row
        bool renamed = false;
        for (const AudioBookmarkStore::Bookmark& b : la2)
            if (AudioBookmarkStore::idFor(A, b.posSec) == expectId(A, 305.5))
                renamed = (b.label == QStringLiteral("mid (renamed)"));
        CHECK(renamed);                                                               // the fold refreshed the label
    }

    // ---- 3. remove deletes the row AND leaves a delete tombstone (survivor carries none) -------------------
    {
        ProfileStore::setCurrent(QStringLiteral("abmrm"));
        const QString A = QStringLiteral("audiobook:Hyperion");
        const AudioBookmarkStore::Bookmark keep = AudioBookmarkStore::add(A, 10.0, QStringLiteral("keep"));
        const AudioBookmarkStore::Bookmark gone = AudioBookmarkStore::add(A, 200.0, QStringLiteral("gone"));
        CHECK(AudioBookmarkStore::list(A).size() == 2);

        AudioBookmarkStore::remove(gone.id);
        const QVector<AudioBookmarkStore::Bookmark> after = AudioBookmarkStore::list(A);
        CHECK(after.size() == 1 && after[0].id == keep.id);                           // the row is gone
        // A delete TOMBSTONE was recorded for the removed id (so a peer cannot resurrect it on merge)...
        bool tombed = false, keepTombed = false;
        for (const Tombstones::Entry& e : Tombstones::all(AudioBookmarkStore::tombstoneStore()))
        {
            if (e.key == gone.id) tombed = true;
            if (e.key == keep.id) keepTombed = true;
        }
        CHECK(tombed);
        CHECK(!keepTombed);                                                           // ...and only for that id

        // Re-adding the removed spot resurrects it AND clears its tombstone (the "deletion undone" path).
        const AudioBookmarkStore::Bookmark back = AudioBookmarkStore::add(A, 200.0, QStringLiteral("back"));
        CHECK(back.id == gone.id);                                                    // same second -> same id
        bool stillTombed = false;
        for (const Tombstones::Entry& e : Tombstones::all(AudioBookmarkStore::tombstoneStore()))
            if (e.key == gone.id) stillTombed = true;
        CHECK(!stillTombed);                                                          // the re-add cleared it
        CHECK(AudioBookmarkStore::list(A).size() == 2);
    }

    // ---- 4. Settings::audioJumpSeconds: default 30, clamped 5..120 on write, valid round-trip ---------------
    {
        // A fresh key reads the 30 s default (hand-written, not read from the code).
        QSettings raw(iniPath, QSettings::IniFormat);
        raw.remove(QStringLiteral("playback/jumpSeconds"));
        raw.sync();
        CHECK(Settings::audioJumpSeconds() == 30);

        // A valid value round-trips.
        Settings::setAudioJumpSeconds(45);
        CHECK(Settings::audioJumpSeconds() == 45);

        // Clamp on write: below the floor -> 5, above the ceiling -> 120.
        Settings::setAudioJumpSeconds(1);
        CHECK(Settings::audioJumpSeconds() == 5);
        Settings::setAudioJumpSeconds(999);
        CHECK(Settings::audioJumpSeconds() == 120);
    }

    if (failures == 0) { std::puts("AUDIOBM-OK"); return 0; }
    std::fprintf(stderr, "AUDIOBM: %d check(s) failed\n", failures);
    return 1;
}
