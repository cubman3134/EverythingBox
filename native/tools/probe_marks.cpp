// Headless check of the ItemMarks store (src/core/ItemMarks) — the per-profile library-management foundation
// (hidden / completion / tags + tag vocabulary + pinned shelves) that later tasks wire hidden-filtering,
// detail actions and shelves onto. QtCore-only (a QSettings wrapper, no Quick/Widgets), so it runs under the
// offscreen QPA in CI and pins the contract those tasks lean on:
//
//   * per-profile isolation — profile A's marks are invisible to profile B (own vocab too);
//   * tag vocabulary unions on setTags; removeTagEverywhere strips the tag from vocab + every item + unpins;
//   * completion round-trips, and an unknown/corrupt completion token reads back as None;
//   * hashed item keys — "a/b", "a//b" and a URL-shaped key resolve independently (no group-path aliasing);
//   * empty key is a no-op on every writer and reads back {}; an unknown key reads back {};
//   * the get() cache is hot (an external ini write is NOT seen until invalidate()), then re-reads after it;
//   * anyHidden() tracks whether the active profile has any hidden item;
//   * clearing an item back to all-default leaves a stamped HUSK rather than removing the row (issue #132) —
//     through both write funnels — while every reader still answers "no marks"; and an item that never
//     carried a mark gets no row at all, so a no-op unhide cannot grow the store.
//
// Prints MARKS-OK on success; any failure prints MARKS-FAIL <cond> (line) and exits non-zero.
//
// Isolation: AppPaths::dataDir() is this process's own scratch directory (issue #42), so the everythingbox.ini
// it reads/writes starts empty, is never shared with a sibling probe or a previous run, and is removed at exit.
// The probe still SEEDS its own profile ids via ProfileStore::setCurrent, because currentId() otherwise
// resolves to the store's default rather than to a profile these asserts name.
#include "ItemMarks.h"
#include "ProfileStore.h"
#include "AppPaths.h"
#include "AppBrand.h"

#include <QCoreApplication>
#include <QSettings>
#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "MARKS-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

using ItemMarks::Completion;

// The same MD5-hex token ItemMarks uses internally, so the probe can address an item's raw ini blob and can
// predict the hashed keys itemKeysWithTag() returns.
static QString hash(const QString& key)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Md5).toHex());
}

// Switch the active profile the store keys off, exactly as the app would (setCurrent + invalidate the cache).
static void useProfile(const QString& id)
{
    ProfileStore::setCurrent(id);
    ItemMarks::invalidate();
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    const QString iniPath = AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile);

    // ---- 1. Per-profile isolation --------------------------------------------------------------------------
    useProfile(QStringLiteral("probeA"));
    ItemMarks::setHidden(QStringLiteral("game:doom"), true);
    ItemMarks::setCompletion(QStringLiteral("game:doom"), Completion::Finished);
    ItemMarks::setTags(QStringLiteral("game:doom"), QStringList{QStringLiteral("fps"), QStringLiteral("classic")});
    {
        const ItemMarks::Marks m = ItemMarks::get(QStringLiteral("game:doom"));
        CHECK(m.hidden);
        CHECK(m.completion == Completion::Finished);
        CHECK(m.tags.contains(QStringLiteral("fps")) && m.tags.contains(QStringLiteral("classic")));
    }

    // Profile B is a clean slate — sees none of A's marks or vocabulary.
    useProfile(QStringLiteral("probeB"));
    {
        const ItemMarks::Marks m = ItemMarks::get(QStringLiteral("game:doom"));
        CHECK(!m.hidden);
        CHECK(m.completion == Completion::None);
        CHECK(m.tags.isEmpty());
        CHECK(ItemMarks::tagVocab().isEmpty());
        CHECK(!ItemMarks::anyHidden());
    }

    // Switching back to A restores A's marks intact.
    useProfile(QStringLiteral("probeA"));
    {
        const ItemMarks::Marks m = ItemMarks::get(QStringLiteral("game:doom"));
        CHECK(m.hidden);
        CHECK(m.completion == Completion::Finished);
        CHECK(m.tags.contains(QStringLiteral("fps")));
    }

    // ---- 2. Tag vocabulary union + removeTagEverywhere (strips + unpins) ------------------------------------
    {
        const QStringList v = ItemMarks::tagVocab();
        CHECK(v.contains(QStringLiteral("fps")) && v.contains(QStringLiteral("classic")));
    }
    // A second item adds a shared and a new tag; vocab unions (no dupes).
    ItemMarks::setTags(QStringLiteral("game:quake"), QStringList{QStringLiteral("fps"), QStringLiteral("id")});
    {
        const QStringList v = ItemMarks::tagVocab();
        CHECK(v.count(QStringLiteral("fps")) == 1);   // unioned, not duplicated
        CHECK(v.contains(QStringLiteral("id")));
        CHECK(v.contains(QStringLiteral("classic")));
    }
    // itemKeysWithTag returns HASHED keys — "fps" is on both doom and quake.
    {
        const QVector<QString> keys = ItemMarks::itemKeysWithTag(QStringLiteral("fps"));
        CHECK(keys.size() == 2);
        CHECK(keys.contains(hash(QStringLiteral("game:doom"))));
        CHECK(keys.contains(hash(QStringLiteral("game:quake"))));
    }
    // Pin two tags; removeTagEverywhere("fps") must drop it from vocab, from every item, and from pinned.
    ItemMarks::setPinned(QStringLiteral("fps"), true);
    ItemMarks::setPinned(QStringLiteral("classic"), true);
    CHECK(ItemMarks::pinnedTags().contains(QStringLiteral("fps")));
    ItemMarks::removeTagEverywhere(QStringLiteral("fps"));
    {
        CHECK(!ItemMarks::tagVocab().contains(QStringLiteral("fps")));       // gone from vocab
        CHECK(!ItemMarks::pinnedTags().contains(QStringLiteral("fps")));     // unpinned
        CHECK(ItemMarks::pinnedTags().contains(QStringLiteral("classic")));  // other pin survives
        CHECK(!ItemMarks::get(QStringLiteral("game:doom")).tags.contains(QStringLiteral("fps")));
        CHECK(ItemMarks::get(QStringLiteral("game:doom")).tags.contains(QStringLiteral("classic")));
        CHECK(!ItemMarks::get(QStringLiteral("game:quake")).tags.contains(QStringLiteral("fps")));
        CHECK(ItemMarks::get(QStringLiteral("game:quake")).tags.contains(QStringLiteral("id")));
        CHECK(ItemMarks::itemKeysWithTag(QStringLiteral("fps")).isEmpty());  // nobody carries it now
    }

    // ---- 3. Completion round-trip, incl. unknown-token -> None ----------------------------------------------
    ItemMarks::setCompletion(QStringLiteral("game:quake"), Completion::Planned);
    CHECK(ItemMarks::get(QStringLiteral("game:quake")).completion == Completion::Planned);
    ItemMarks::setCompletion(QStringLiteral("game:quake"), Completion::None);
    CHECK(ItemMarks::get(QStringLiteral("game:quake")).completion == Completion::None);
    {
        // Inject a blob with an unknown completion token straight into the ini; it must read back as None
        // (forward-compat: a value written by a newer build is not misread).
        const QString ik = QStringLiteral("marks/probeA/items/") + hash(QStringLiteral("game:doom"));
        QSettings raw(iniPath, QSettings::IniFormat);
        raw.setValue(ik, QStringLiteral("{\"hidden\":true,\"completion\":\"warp-speed\",\"tags\":[\"classic\"]}"));
        raw.sync();
        ItemMarks::invalidate();
        CHECK(ItemMarks::get(QStringLiteral("game:doom")).completion == Completion::None);
        CHECK(ItemMarks::get(QStringLiteral("game:doom")).hidden); // rest of the blob still parses
    }

    // ---- 4. Hashed-key collision independence --------------------------------------------------------------
    // Verbatim group nesting would alias "a/b" and "a//b"; the store hashes first so they (and a URL) are
    // independent entries.
    ItemMarks::setTags(QStringLiteral("a/b"),  QStringList{QStringLiteral("tA")});
    ItemMarks::setTags(QStringLiteral("a//b"), QStringList{QStringLiteral("tB")});
    ItemMarks::setTags(QStringLiteral("https://x/y.mkv"), QStringList{QStringLiteral("tU")});
    CHECK(ItemMarks::get(QStringLiteral("a/b")).tags == QStringList{QStringLiteral("tA")});
    CHECK(ItemMarks::get(QStringLiteral("a//b")).tags == QStringList{QStringLiteral("tB")});
    CHECK(ItemMarks::get(QStringLiteral("https://x/y.mkv")).tags == QStringList{QStringLiteral("tU")});

    // ---- 5. Empty-key no-op + unknown-key absence ---------------------------------------------------------
    {
        const QStringList vocabBefore = ItemMarks::tagVocab();
        ItemMarks::setHidden(QString(), true);
        ItemMarks::setCompletion(QString(), Completion::Finished);
        ItemMarks::setTags(QString(), QStringList{QStringLiteral("ghost")});
        const ItemMarks::Marks m = ItemMarks::get(QString());
        CHECK(!m.hidden && m.completion == Completion::None && m.tags.isEmpty()); // empty key -> {}
        CHECK(ItemMarks::tagVocab() == vocabBefore);            // empty-key setTags added no vocab entry
        CHECK(!ItemMarks::tagVocab().contains(QStringLiteral("ghost")));
        // An unknown natural key reads back default and creates nothing.
        const ItemMarks::Marks u = ItemMarks::get(QStringLiteral("no-such-item"));
        CHECK(!u.hidden && u.completion == Completion::None && u.tags.isEmpty());
        // No empty-hash junk item key was written.
        QSettings ini(iniPath, QSettings::IniFormat);
        ini.beginGroup(QStringLiteral("marks/probeA/items"));
        const QStringList itemKeys = ini.childKeys();
        ini.endGroup();
        CHECK(!itemKeys.contains(QString()));
    }

    // ---- 6. Cache is hot; invalidate() re-reads -----------------------------------------------------------
    {
        useProfile(QStringLiteral("probeCache"));
        ItemMarks::setTags(QStringLiteral("k"), QStringList{QStringLiteral("v1")}); // writer invalidated
        CHECK(ItemMarks::get(QStringLiteral("k")).tags == QStringList{QStringLiteral("v1")}); // primes cache
        // External write straight to the ini (bypasses ItemMarks, so no invalidate happens).
        {
            const QString ik = QStringLiteral("marks/probeCache/items/") + hash(QStringLiteral("k"));
            QSettings raw(iniPath, QSettings::IniFormat);
            raw.setValue(ik, QStringLiteral("{\"hidden\":false,\"completion\":\"none\",\"tags\":[\"v2\"]}"));
            raw.sync();
        }
        CHECK(ItemMarks::get(QStringLiteral("k")).tags == QStringList{QStringLiteral("v1")}); // still cached (hot)
        ItemMarks::invalidate();
        CHECK(ItemMarks::get(QStringLiteral("k")).tags == QStringList{QStringLiteral("v2")}); // now re-read
    }

    // ---- 7. anyHidden() fast-path truth -------------------------------------------------------------------
    {
        useProfile(QStringLiteral("probeHidden"));
        CHECK(!ItemMarks::anyHidden());                       // fresh profile: nothing hidden
        ItemMarks::setHidden(QStringLiteral("h1"), true);
        CHECK(ItemMarks::anyHidden());                        // now true
        ItemMarks::setHidden(QStringLiteral("h1"), false);
        CHECK(!ItemMarks::anyHidden());                       // back to false (the husk reads as no marks)
    }

    // ---- 8. Clearing leaves a stamped HUSK, not an empty slot (issue #132) --------------------------------
    //
    // The store half of the fix. A row cleared back to all-default is REWRITTEN with a fresh updatedAt rather
    // than removed, because a removed row is indistinguishable from "this device never saw that item" and the
    // next merge with a peer still holding the marks would put them straight back. The cross-device half —
    // that the husk actually wins that merge — is probe_cloudmerge section 25; here the contract is: the row
    // survives, it carries a fresh stamp, and every reader still answers "no marks".
    {
        useProfile(QStringLiteral("probeHusk"));
        const QString grp = QStringLiteral("marks/probeHusk/items/");
        auto rawBlob = [&](const QString& key) {
            QSettings raw(iniPath, QSettings::IniFormat); raw.sync();
            return QJsonDocument::fromJson(raw.value(grp + hash(key)).toString().toUtf8()).object();
        };
        auto rowPresent = [&](const QString& key) {
            QSettings raw(iniPath, QSettings::IniFormat); raw.sync();
            return raw.contains(grp + hash(key));
        };

        const qint64 before = QDateTime::currentSecsSinceEpoch();
        ItemMarks::setHidden(QStringLiteral("hk"), true);
        ItemMarks::setCompletion(QStringLiteral("hk"), Completion::Finished);
        ItemMarks::setTags(QStringLiteral("hk"), QStringList{QStringLiteral("t")});
        CHECK(ItemMarks::get(QStringLiteral("hk")).hidden);

        // Clear every mark, one verb at a time — the last of them is what takes the record to all-default.
        ItemMarks::setHidden(QStringLiteral("hk"), false);
        ItemMarks::setCompletion(QStringLiteral("hk"), Completion::None);
        ItemMarks::setTags(QStringLiteral("hk"), QStringList{});

        // The row is STILL THERE — a clear with a time on it, which is the whole point.
        CHECK(rowPresent(QStringLiteral("hk")));
        {
            const QJsonObject o = rawBlob(QStringLiteral("hk"));
            const qint64 ts = qint64(o.value(QStringLiteral("updatedAt")).toDouble());
            CHECK(ts >= before && ts <= QDateTime::currentSecsSinceEpoch()); // freshly stamped, not carried
            CHECK(o.value(QStringLiteral("hidden")).toBool() == false);
            CHECK(o.value(QStringLiteral("completion")).toString() == QStringLiteral("none"));
            CHECK(o.value(QStringLiteral("tags")).toArray().isEmpty());
        }
        // ...and every reader answers "no marks" for it, exactly as if the row were absent.
        {
            const ItemMarks::Marks m = ItemMarks::get(QStringLiteral("hk"));
            CHECK(!m.hidden && m.completion == Completion::None && m.tags.isEmpty());
            CHECK(!ItemMarks::anyHidden());
            CHECK(ItemMarks::itemKeysWithTag(QStringLiteral("t")).isEmpty());
        }

        // A husk records a CLEAR, so an item that never carried a mark gets no row at all: unhiding something
        // never hidden, or saving an empty tag list on an untagged item, must not grow the store.
        ItemMarks::setHidden(QStringLiteral("neverMarked"), false);
        ItemMarks::setCompletion(QStringLiteral("neverMarked"), Completion::None);
        ItemMarks::setTags(QStringLiteral("neverMarked"), QStringList{});
        CHECK(!rowPresent(QStringLiteral("neverMarked")));

        // removeTagEverywhere's direct-rewrite path is the second write funnel and obeys the same rule: an
        // item left all-default once its only tag is stripped keeps a stamped husk, not a hole.
        ItemMarks::setTags(QStringLiteral("solo"), QStringList{QStringLiteral("only")});
        const qint64 beforeStrip = QDateTime::currentSecsSinceEpoch();
        ItemMarks::removeTagEverywhere(QStringLiteral("only"));
        CHECK(rowPresent(QStringLiteral("solo")));
        {
            const qint64 ts = qint64(rawBlob(QStringLiteral("solo")).value(QStringLiteral("updatedAt")).toDouble());
            CHECK(ts >= beforeStrip && ts <= QDateTime::currentSecsSinceEpoch());
        }
        CHECK(ItemMarks::get(QStringLiteral("solo")).tags.isEmpty());
    }

    if (failures == 0) { std::puts("MARKS-OK"); return 0; }
    std::fprintf(stderr, "MARKS: %d check(s) failed\n", failures);
    return 1;
}
