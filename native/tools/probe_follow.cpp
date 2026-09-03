// Headless check of "Follow a series" (issue #155, increment 1): the mark, the device-local snapshot, the
// scheduled refresh and the New shelf. QtCore-only (QSettings wrappers + a QObject with no timer running),
// so it runs under the offscreen QPA in CI with no event loop, no network and NO WALL CLOCK — every schedule
// assertion below is driven by a fake clock the probe advances by hand, which is the only way to state "six
// hours later" in a test that must finish in milliseconds and never flake.
//
// What it pins:
//
//   * WHO CAN BE FOLLOWED — a series-shaped row and nothing else, on both layouts, through the one oracle
//     both surfaces call (follow::isFollowable). A leaf is refused however its source flags it.
//   * THE MARK — FollowStore round-trips per profile, de-dupes a re-follow, stamps at the mutation site,
//     tombstones an unfollow, and ERASES that tombstone on a re-follow (without which the next merge would
//     suppress the row the user just re-created — the resurrection guard read backwards).
//   * THE SNAPSHOT — device-local, per series: seen ids only ever GROW, a pending child keeps the foundAt it
//     was first given, "mark all seen" empties pending without forgetting what was seen, and unfollowing
//     forgets the series so a re-follow starts from a baseline instead of announcing five years of back
//     catalogue.
//   * THE DIFF — first check is a silent BASELINE; a new child is news; a REMOVED child is not (and is not
//     forgotten either); an unchanged list produces nothing; and a source with no stable child ids degrades
//     to one "something changed" row rather than to a false per-child claim.
//   * THE SCHEDULE — the interval choices, the clamp, the jitter's determinism AND its bound, "never run =
//     due now", and manual (0) never being due.
//   * THE POLITENESS, which is the part of this feature that can hurt somebody else's server, so it is
//     asserted as behaviour of the running scheduler and not only of the pure predicate: one request in
//     flight per source, a five-second gap between two requests to the same source, a different source not
//     blocked by a busy one, a failing source costing exactly ONE request per cycle however many series it
//     holds, that same source being asked again on the NEXT cycle, a fetch that never answers being reaped,
//     playback and a metered link each deferring the pass WITHOUT consuming it, and "Check now" bypassing
//     both of those gates but none of the per-source ones.
//   * THE NEW SHELF — newest-first ordering, the dealt-with filter (the existing completion states), the
//     badge counted through that same filter, and the union with #25's rows deduplicated by item id.
//
// Prints FOLLOW-OK on success; any failure prints FOLLOW-FAIL <cond> (line) and exits non-zero.
//
// Isolation: AppPaths::dataDir() is this process's own scratch directory (issue #42), so the everythingbox.ini
// it reads and writes starts empty, is never shared with a sibling probe or a previous run, and is removed at
// exit. Profile ids are seeded explicitly because currentId() otherwise resolves to the store's default.
#include "FollowPlan.h"
#include "FollowScheduler.h"
#include "FollowSnapshot.h"
#include "FollowStore.h"
#include "ProfileStore.h"
#include "Tombstones.h"
#include "AppPaths.h"
#include "AppBrand.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QSettings>
#include <QStringList>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "FOLLOW-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

static QSettings& raw()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                       QSettings::IniFormat);
    return s;
}

static QString md5(const QString& k)
{
    return QString::fromLatin1(QCryptographicHash::hash(k.toUtf8(), QCryptographicHash::Md5).toHex());
}

static void useProfile(const QString& id) { ProfileStore::setCurrent(id); }

static FollowItem mkFollow(const QString& id, const QString& addon, const QString& type = QStringLiteral("series"))
{
    FollowItem f;
    f.itemId = id; f.addonId = addon; f.type = type; f.title = id;
    return f;
}

static follow::Child mkChild(const QString& id, const QString& title = QString())
{
    follow::Child c; c.id = id; c.title = title.isEmpty() ? id : title; c.type = QStringLiteral("episode");
    return c;
}

// A pending-shaped row for the pure shelf helpers, so §10 can exercise them without the store.
struct FakePending { QString id, title, subtitle, thumbnailUrl, type, url, mime; qint64 foundAt = 0; int count = 1; };

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ---- 1. Who can be followed ------------------------------------------------------------------------
    {
        // Series-shaped containers, including an addon-defined one the app has never heard of. This is the
        // clause that makes "any series-shaped item from any source" true: the podcasts addon's "podcast"
        // type is not in any list here, and it must still be followable.
        CHECK(follow::isFollowable(QStringLiteral("series"), true));
        CHECK(follow::isFollowable(QStringLiteral("podcast"), true));
        CHECK(follow::isFollowable(QStringLiteral("manga"), true));
        CHECK(follow::isFollowable(QStringLiteral("comic"), true));
        CHECK(follow::isFollowable(QStringLiteral("some_addon_defined_thing"), true));

        // Leaves. A leaf is refused EVEN IF its source claims it is expandable — an episode that drills into
        // its own versions is still an episode, and following one promises a refresh that can never deliver.
        CHECK(!follow::isFollowable(QStringLiteral("episode"), true));
        CHECK(!follow::isFollowable(QStringLiteral("podcast_episode"), true));
        CHECK(!follow::isFollowable(QStringLiteral("movie"), true));
        CHECK(!follow::isFollowable(QStringLiteral("track"), true));
        CHECK(!follow::isFollowable(QStringLiteral("chapter"), true));
        CHECK(!follow::isFollowable(QStringLiteral("game"), true));

        // Structural containers the browse tree builds itself, and an album (a fixed track list).
        CHECK(!follow::isFollowable(QStringLiteral("platform"), true));
        CHECK(!follow::isFollowable(QStringLiteral("album"), true));
        CHECK(!follow::isFollowable(QStringLiteral("playlist"), true));
        CHECK(!follow::isFollowable(QStringLiteral("livetv"), true));

        // Synthetic marker rows carry no identity to file a follow under.
        CHECK(!follow::isFollowable(QStringLiteral("_traktmissed"), true));
        CHECK(!follow::isFollowable(QStringLiteral("_playlists"), true));
        CHECK(!follow::isFollowable(QString(), true));

        // Not expandable = no children = nothing to follow, whatever the type says.
        CHECK(!follow::isFollowable(QStringLiteral("series"), false));
        CHECK(!follow::isFollowable(QStringLiteral("podcast"), false));
    }

    // ---- 2. The mark: round trip, de-dupe, per-profile isolation ---------------------------------------
    {
        useProfile(QStringLiteral("fA"));
        CHECK(FollowStore::list().isEmpty());
        CHECK(!FollowStore::isFollowed(QStringLiteral("s1")));

        FollowStore::add(mkFollow(QStringLiteral("s1"), QStringLiteral("addonA")));
        FollowStore::add(mkFollow(QStringLiteral("s2"), QStringLiteral("addonB")));
        CHECK(FollowStore::count() == 2);
        CHECK(FollowStore::isFollowed(QStringLiteral("s1")));
        CHECK(FollowStore::list().first().itemId == QStringLiteral("s2"));   // newest first

        // A re-follow is one row, not two, and it re-dates rather than duplicating.
        FollowStore::add(mkFollow(QStringLiteral("s1"), QStringLiteral("addonA")));
        CHECK(FollowStore::count() == 2);

        // The stamp is set by the store, not by the caller: mkFollow leaves ts at 0.
        CHECK(FollowStore::list().first().ts > 0);

        // An empty id is a no-op on both writers.
        FollowStore::add(mkFollow(QString(), QStringLiteral("addonA")));
        FollowStore::remove(QString());
        CHECK(FollowStore::count() == 2);

        // Another profile follows its own shows and cannot see this one's.
        useProfile(QStringLiteral("fB"));
        CHECK(FollowStore::list().isEmpty());
        FollowStore::add(mkFollow(QStringLiteral("s9"), QStringLiteral("addonA")));
        CHECK(FollowStore::count() == 1);
        useProfile(QStringLiteral("fA"));
        CHECK(FollowStore::count() == 2);
        CHECK(!FollowStore::isFollowed(QStringLiteral("s9")));
    }

    // ---- 3. Unfollow tombstones; a re-follow erases the tombstone --------------------------------------
    {
        useProfile(QStringLiteral("fA"));
        FollowStore::remove(QStringLiteral("s2"));
        CHECK(!FollowStore::isFollowed(QStringLiteral("s2")));
        const QVector<Tombstones::Entry> tombs = Tombstones::all(QStringLiteral("follow/fA"));
        bool tombed = false;
        for (const Tombstones::Entry& e : tombs) if (e.key == QStringLiteral("s2")) tombed = true;
        CHECK(tombed);

        // THE RESURRECTION GUARD, READ BACKWARDS. The merge rule is "a tombstone at-or-after the item's ts
        // suppresses it"; a re-follow in the SAME SECOND as the unfollow is exactly that tie, so a re-follow
        // that only re-stamped would be silently suppressed on the next merge. add() erases the record.
        FollowStore::add(mkFollow(QStringLiteral("s2"), QStringLiteral("addonB")));
        bool stillTombed = false;
        for (const Tombstones::Entry& e : Tombstones::all(QStringLiteral("follow/fA")))
            if (e.key == QStringLiteral("s2")) stillTombed = true;
        CHECK(!stillTombed);
        CHECK(FollowStore::isFollowed(QStringLiteral("s2")));

        // Unfollowing something that is not followed writes nothing at all — no rewrite, no tombstone, no
        // sync churn from a press that changed nothing.
        const int tombsBefore = int(Tombstones::all(QStringLiteral("follow/fA")).size());
        FollowStore::remove(QStringLiteral("neverFollowed"));
        CHECK(int(Tombstones::all(QStringLiteral("follow/fA")).size()) == tombsBefore);
    }

    // ---- 4. The snapshot store: device-local shape, growth, pending lifecycle ---------------------------
    {
        useProfile(QStringLiteral("fA"));
        const QString sid = QStringLiteral("snap/series?x=1");   // a key that is NOT ini-leaf-safe
        CHECK(FollowSnapshot::get(sid).neverChecked());

        FollowSnapshot::record(sid, QStringList{ QStringLiteral("c1"), QStringLiteral("c2") },
                               QString(), {}, 1000);
        FollowSnapshot::Snapshot s = FollowSnapshot::get(sid);
        CHECK(!s.neverChecked());
        CHECK(s.checked == 1000);
        CHECK(s.seen.size() == 2);
        CHECK(s.pending.isEmpty());

        // The key is HASHED before use as an ini leaf, so a URL-shaped or slash-carrying id cannot fold into
        // a colliding group path (the ItemMarks lesson).
        CHECK(raw().contains(QStringLiteral("followsnap/fA/") + md5(sid)));

        // A found child becomes pending, with the time it was found.
        FollowSnapshot::Pending p;
        p.id = QStringLiteral("c3"); p.title = QStringLiteral("Ep 3"); p.foundAt = 2000;
        FollowSnapshot::record(sid, QStringList{ QStringLiteral("c1"), QStringLiteral("c2"), QStringLiteral("c3") },
                               QString(), { p }, 2000);
        s = FollowSnapshot::get(sid);
        CHECK(s.pending.size() == 1);
        CHECK(s.pending.first().foundAt == 2000);

        // A LATER check that finds the same child again does not re-list it and does not move it up the
        // shelf: a pending row keeps the foundAt it was FIRST given.
        FollowSnapshot::record(sid, QStringList{ QStringLiteral("c1"), QStringLiteral("c3") },
                               QString(), { p }, 9000);
        s = FollowSnapshot::get(sid);
        CHECK(s.pending.size() == 1);
        CHECK(s.pending.first().foundAt == 2000);
        CHECK(s.checked == 9000);

        // Clearing one child, then all.
        FollowSnapshot::clearPending(sid, QStringLiteral("nosuch"));
        CHECK(FollowSnapshot::get(sid).pending.size() == 1);
        FollowSnapshot::clearPending(sid, QStringLiteral("c3"));
        CHECK(FollowSnapshot::get(sid).pending.isEmpty());

        FollowSnapshot::record(sid, QStringList{ QStringLiteral("c4") }, QString(), { p }, 9100);
        CHECK(FollowSnapshot::get(sid).pending.size() == 1);
        FollowSnapshot::markAllSeen(sid);
        s = FollowSnapshot::get(sid);
        CHECK(s.pending.isEmpty());
        CHECK(!s.seen.isEmpty());          // "mark all seen" forgets nothing; it only stops showing them
        CHECK(!s.neverChecked());

        // Unfollowing forgets the series, so a re-follow years later starts from a baseline rather than
        // announcing everything published in between.
        FollowSnapshot::forget(sid);
        CHECK(FollowSnapshot::get(sid).neverChecked());
        CHECK(!raw().contains(QStringLiteral("followsnap/fA/") + md5(sid)));

        // The snapshots are per profile, like everything else keyed on a viewer.
        FollowSnapshot::record(sid, QStringList{ QStringLiteral("z") }, QString(), {}, 5000);
        useProfile(QStringLiteral("fB"));
        CHECK(FollowSnapshot::get(sid).neverChecked());
        useProfile(QStringLiteral("fA"));
        CHECK(!FollowSnapshot::get(sid).neverChecked());
    }

    // ---- 5. The diff -----------------------------------------------------------------------------------
    {
        const QVector<follow::Child> two{ mkChild(QStringLiteral("a")), mkChild(QStringLiteral("b")) };

        // FIRST CHECK IS A BASELINE. Two children exist; neither is news. This is the clause that stops
        // following a twenty-year-old podcast from dumping a thousand rows on the home screen.
        follow::Diff d = follow::diffChildren({}, QString(), two, /*firstEver*/ true);
        CHECK(d.newChildren.isEmpty());
        CHECK(!d.coarseChanged);
        CHECK(d.seenAfter.size() == 2);

        // Unchanged: nothing.
        d = follow::diffChildren(d.seenAfter, QString(), two, false);
        CHECK(d.newChildren.isEmpty());

        // A new child.
        QVector<follow::Child> three = two; three << mkChild(QStringLiteral("c"));
        d = follow::diffChildren(QStringList{ QStringLiteral("a"), QStringLiteral("b") }, QString(), three, false);
        CHECK(d.newChildren.size() == 1);
        CHECK(d.newChildren.first().id == QStringLiteral("c"));
        CHECK(d.seenAfter.size() == 3);

        // A REMOVED child is not news, and is not forgotten either — the seen-set only grows. A podcast that
        // publishes only its last 60 episodes drops old ones constantly; intersecting would make one that
        // fell out and came back read as brand new.
        const QVector<follow::Child> onlyB{ mkChild(QStringLiteral("b")) };
        d = follow::diffChildren(QStringList{ QStringLiteral("a"), QStringLiteral("b") }, QString(), onlyB, false);
        CHECK(d.newChildren.isEmpty());
        CHECK(d.seenAfter.contains(QStringLiteral("a")));
        CHECK(d.seenAfter.contains(QStringLiteral("b")));

        // An empty list from a keyed source is a fact, not an unreliable source: no coarse row.
        d = follow::diffChildren(QStringList{ QStringLiteral("a") }, QString(), {}, false);
        CHECK(d.newChildren.isEmpty());
        CHECK(!d.coarseChanged);

        // The seen-set is bounded, keeping the NEWEST (the current list is written first).
        {
            QVector<follow::Child> many;
            for (int i = 0; i < follow::kMaxSeenIds + 50; ++i)
                many << mkChild(QStringLiteral("n") + QString::number(i));
            const follow::Diff big = follow::diffChildren({}, QString(), many, true);
            CHECK(big.seenAfter.size() == follow::kMaxSeenIds);
            CHECK(big.seenAfter.first() == QStringLiteral("n0"));
        }
    }

    // ---- 6. The degrade: a source with no stable child ids ----------------------------------------------
    {
        QVector<follow::Child> unkeyed;
        follow::Child a; a.title = QStringLiteral("One"); unkeyed << a;
        follow::Child b; b.title = QStringLiteral("Two"); unkeyed << b;
        CHECK(!follow::childrenAreKeyed(unkeyed));
        CHECK(follow::childrenAreKeyed({ mkChild(QStringLiteral("x")) }));

        // ONE unkeyed child poisons the whole list: a partial diff would announce the keyed half and silently
        // drop the rest, which is worse than saying "something changed".
        QVector<follow::Child> mixed{ mkChild(QStringLiteral("x")), a };
        CHECK(!follow::childrenAreKeyed(mixed));

        follow::Diff d = follow::diffChildren({}, QString(), unkeyed, true);
        CHECK(!d.coarseChanged);                       // baseline
        CHECK(!d.fingerprintAfter.isEmpty());
        const QString fp1 = d.fingerprintAfter;

        d = follow::diffChildren({}, fp1, unkeyed, false);
        CHECK(!d.coarseChanged);                       // unchanged list, unchanged fingerprint

        QVector<follow::Child> grown = unkeyed;
        follow::Child c; c.title = QStringLiteral("Three"); grown << c;
        d = follow::diffChildren({}, fp1, grown, false);
        CHECK(d.coarseChanged);
        CHECK(d.newChildren.isEmpty());                // never a per-child claim from an unkeyed source
    }

    // ---- 7. The schedule, on a fake clock ---------------------------------------------------------------
    {
        CHECK(follow::intervalChoicesHours().contains(6));
        CHECK(follow::intervalChoicesHours().contains(24 * 7));
        CHECK(follow::intervalChoicesHours().contains(0));
        CHECK(follow::clampIntervalHours(6) == 6);
        CHECK(follow::clampIntervalHours(0) == 0);       // manual is a real choice and survives the clamp
        CHECK(follow::clampIntervalHours(37) == 24);     // anything unrecognised reads as the daily default
        CHECK(follow::clampIntervalHours(-5) == 24);

        const qint64 day = 24 * follow::kHourSecs;

        // NEVER RUN = DUE NOW. Following something and then being told to come back tomorrow reads as broken.
        CHECK(follow::nextDueAt(0, day, 0) == 0);
        CHECK(follow::dueNow(1, 0, day, 0));

        // MANUAL never comes due, however long you leave it.
        CHECK(follow::nextDueAt(1000, 0, 0) == -1);
        CHECK(!follow::dueNow(qint64(1) << 40, 1000, 0, 0));

        // The boundary is exact: one second before is not due, the second itself is.
        CHECK(!follow::dueNow(1000 + day - 1, 1000, day, 0));
        CHECK(follow::dueNow(1000 + day, 1000, day, 0));
        CHECK(!follow::dueNow(1000 + day + 100, 1000, day, 200));    // the jitter really does hold it back
        CHECK(follow::dueNow(1000 + day + 200, 1000, day, 200));

        // The jitter is DETERMINISTIC in its seed, and BOUNDED by min(interval/10, 15 min). Both halves
        // matter: deterministic so a probe can assert a due second, bounded so "jittered" never means "some
        // time next week".
        for (quint32 seed = 0; seed < 5000; seed += 37)
        {
            const qint64 j = follow::jitterSecs(day, seed);
            CHECK(j >= 0 && j <= follow::kMaxJitterSecs);
            CHECK(j == follow::jitterSecs(day, seed));                 // same seed, same offset, every cycle
        }
        // The bound is the SMALLER of interval/10 and the ceiling, and both arms are exercised: six hours
        // would give 36 min by the ratio, so the CEILING binds it; a week would give 16.8 hours by the
        // ratio, so the ceiling binds that too. Asserting the min() explicitly is what stops it being
        // silently flipped to a max().
        const qint64 sixH = 6 * follow::kHourSecs;
        for (quint32 seed = 0; seed < 3000; seed += 17)
            CHECK(follow::jitterSecs(sixH, seed) <= qMin<qint64>(sixH / 10, follow::kMaxJitterSecs));
        CHECK(qMin<qint64>(sixH / 10, follow::kMaxJitterSecs) == follow::kMaxJitterSecs);
        // A week's interval is capped by the ceiling rather than by interval/10 (which would be 16.8 hours).
        for (quint32 seed = 0; seed < 3000; seed += 17)
            CHECK(follow::jitterSecs(7 * day, seed) <= follow::kMaxJitterSecs);
        CHECK(follow::jitterSecs(0, 12345) == 0);        // manual has no jitter to speak of
    }

    // ---- 8. The politeness predicate --------------------------------------------------------------------
    {
        using follow::Admit;
        CHECK(follow::admit(100, 0, false, false) == Admit::Send);
        CHECK(follow::admit(100, 0, true,  false) == Admit::WaitInFlight);
        CHECK(follow::admit(100, 98, false, false) == Admit::WaitGap);
        CHECK(follow::admit(100, 95, false, false) == Admit::Send);     // exactly the gap is enough
        CHECK(follow::admit(100, 0, false, true)  == Admit::SourceFailed);
        // A failed source outranks everything: it is not "wait", it is "not this cycle".
        CHECK(follow::admit(100, 99, true, true) == Admit::SourceFailed);
    }

    // ---- 9. The scheduler, driven by hand ---------------------------------------------------------------
    {
        useProfile(QStringLiteral("fSched"));
        qint64 clock = 1'000'000;

        // The fixture source: a map of series id -> its current children, which the probe GROWS between
        // checks. This is the deterministic stand-in for a podcast feed publishing an episode.
        QHash<QString, QVector<follow::Child>> world;
        world.insert(QStringLiteral("A1"), { mkChild(QStringLiteral("a-e1")) });

        QSet<QString> deadSources;          // sources whose fetch fails
        QSet<QString> silentSources;        // sources whose fetch never calls back at all
        QVector<QString> asked;             // every series asked, in order

        QVector<FollowItem> follows{ mkFollow(QStringLiteral("A1"), QStringLiteral("srcA")) };

        FollowScheduler sched;
        sched.setPeriodic(false);
        sched.setClock([&clock] { return clock; });
        sched.setListSource([&follows] { return follows; });
        sched.setIntervalHours(24);
        sched.setJitterSeed(0);            // no jitter, so the due seconds below are the interval exactly
        sched.setFetcher([&](const FollowItem& it, FollowScheduler::FetchDone done) {
            asked << it.itemId;
            if (silentSources.contains(it.addonId)) return;             // never answers
            if (deadSources.contains(it.addonId)) { done(false, {}); return; }
            done(true, world.value(it.itemId));
        });

        int lastNewSeries = 0;
        QString lastNewId;
        QObject::connect(&sched, &FollowScheduler::newItemsFound, [&](const QString& id, int n) {
            lastNewId = id; lastNewSeries = n;
        });
        int finished = 0;
        QObject::connect(&sched, &FollowScheduler::cycleFinished, [&](int, int) { ++finished; });

        // 9a. The first pass is a BASELINE: the source is asked, nothing is announced.
        sched.tick();
        CHECK(sched.issued() == 1);
        CHECK(sched.newFound() == 0);
        CHECK(finished == 1);
        CHECK(!sched.cycleActive());
        CHECK(FollowSnapshot::get(QStringLiteral("A1")).pending.isEmpty());

        // 9b. Nothing is due again until the interval has passed.
        clock += 60;
        sched.tick();
        CHECK(sched.issued() == 1);

        // 9c. The fixture grows; a day later the pass finds exactly the new child.
        world[QStringLiteral("A1")] << mkChild(QStringLiteral("a-e2"));
        clock += 24 * follow::kHourSecs;
        sched.tick();
        CHECK(sched.issued() == 2);
        CHECK(sched.newFound() == 1);
        CHECK(lastNewId == QStringLiteral("A1"));      // the increment-2 notifier seam fired, with the count
        CHECK(lastNewSeries == 1);
        {
            const FollowSnapshot::Snapshot s = FollowSnapshot::get(QStringLiteral("A1"));
            CHECK(s.pending.size() == 1);
            CHECK(s.pending.first().id == QStringLiteral("a-e2"));
        }

        // 9d. SKIPPED WHILE PLAYING — and the pass is DEFERRED, not consumed: the moment playback stops, the
        // same tick that would have been too late runs it.
        bool playing = true;
        sched.setIsPlaying([&playing] { return playing; });
        clock += 24 * follow::kHourSecs;
        sched.tick();
        CHECK(sched.skippedPlaying() == 1);
        CHECK(sched.issued() == 2);                     // nothing went out
        sched.tick();
        CHECK(sched.skippedPlaying() == 2);             // still held, and still not consumed
        playing = false;
        sched.tick();
        CHECK(sched.issued() == 3);                     // the day's check was not lost

        // 9e. SKIPPED ON A METERED LINK, by default; allowed when the user says so.
        bool metered = true;
        sched.setIsMetered([&metered] { return metered; });
        clock += 24 * follow::kHourSecs;
        sched.tick();
        CHECK(sched.skippedMetered() == 1);
        CHECK(sched.issued() == 3);
        sched.setAllowMetered(true);
        sched.tick();
        CHECK(sched.issued() == 4);
        sched.setAllowMetered(false);

        // 9f. "CHECK NOW" bypasses both gates — it is a deliberate press, not a background pass.
        playing = true; metered = true;
        const int before = sched.issued();
        sched.checkNow();
        CHECK(sched.issued() == before + 1);
        playing = false; metered = false;

        // 9g. THE PER-SOURCE GAP. Three series on ONE source: the first goes out, the other two wait, and
        // each five seconds releases exactly one more. This is the claim that a user with forty followed
        // shows on one addon never floods it.
        follows = { mkFollow(QStringLiteral("A1"), QStringLiteral("srcA")),
                    mkFollow(QStringLiteral("A2"), QStringLiteral("srcA")),
                    mkFollow(QStringLiteral("A3"), QStringLiteral("srcA")) };
        world.insert(QStringLiteral("A2"), { mkChild(QStringLiteral("b-e1")) });
        world.insert(QStringLiteral("A3"), { mkChild(QStringLiteral("c-e1")) });
        clock += 24 * follow::kHourSecs;
        const int base = sched.issued();
        sched.tick();
        CHECK(sched.issued() == base + 1);
        CHECK(sched.cycleActive());
        CHECK(sched.queued() == 2);
        sched.tick();                                    // same second: still too soon
        CHECK(sched.issued() == base + 1);
        clock += follow::kSourceGapSecs;
        sched.tick();
        CHECK(sched.issued() == base + 2);
        clock += follow::kSourceGapSecs - 1;
        sched.tick();
        CHECK(sched.issued() == base + 2);               // one second short
        clock += 1;
        sched.tick();
        CHECK(sched.issued() == base + 3);
        CHECK(!sched.cycleActive());                     // the cycle closes when its queue drains

        // 9h. ONE IN FLIGHT PER SOURCE, and a DIFFERENT source is not blocked by a busy one.
        follows = { mkFollow(QStringLiteral("A1"), QStringLiteral("srcSilent")),
                    mkFollow(QStringLiteral("A2"), QStringLiteral("srcSilent")),
                    mkFollow(QStringLiteral("A3"), QStringLiteral("srcOther")) };
        silentSources.insert(QStringLiteral("srcSilent"));
        clock += 24 * follow::kHourSecs;
        asked.clear();
        sched.tick();
        CHECK(asked.count(QStringLiteral("A1")) == 1);
        CHECK(asked.count(QStringLiteral("A2")) == 0);   // its source already has a request out
        CHECK(asked.count(QStringLiteral("A3")) == 1);   // a different source went out in the same pump
        CHECK(sched.inFlight() == 1);

        // 9i. THE WATCHDOG. A fetch that never answers must not wedge the cycle for ever; past the timeout
        // its source is written off exactly as a failing one is, and the cycle can close.
        clock += 120;
        sched.tick();
        CHECK(sched.inFlight() == 0);
        CHECK(!sched.cycleActive());
        silentSources.clear();

        // 9j. A FAILING SOURCE COSTS IT ONE REQUEST PER CYCLE, however many series it holds — and it IS
        // asked again on the next cycle. This is the whole "retried next cycle, never within one" claim.
        follows = { mkFollow(QStringLiteral("D1"), QStringLiteral("srcDead")),
                    mkFollow(QStringLiteral("D2"), QStringLiteral("srcDead")),
                    mkFollow(QStringLiteral("D3"), QStringLiteral("srcDead")) };
        deadSources.insert(QStringLiteral("srcDead"));
        clock += 24 * follow::kHourSecs;
        asked.clear();
        sched.tick();
        CHECK(asked.size() == 1);                        // one request, not three
        CHECK(sched.deferred() >= 2);
        CHECK(!sched.cycleActive());

        clock += 24 * follow::kHourSecs;
        asked.clear();
        sched.tick();
        CHECK(asked.size() == 1);                        // next cycle: asked again, still exactly once
        deadSources.clear();

        // 9k. A source that FAILED must not have LEARNED anything — in particular it must not have recorded
        // an empty child list, which would make the next successful check announce the whole catalogue.
        CHECK(FollowSnapshot::get(QStringLiteral("D2")).neverChecked());
        CHECK(FollowSnapshot::get(QStringLiteral("D1")).neverChecked());
    }

    // ---- 10. The New shelf ------------------------------------------------------------------------------
    {
        // The dealt-with rule, stated over the two bools the caller reads off ItemMarks.
        CHECK(!follow::isDealtWith(/*hidden*/ false, /*completionIsNone*/ true));
        CHECK(follow::isDealtWith(true, true));      // hidden clears it
        CHECK(follow::isDealtWith(false, false));    // ANY explicit completion state clears it

        QVector<FakePending> pend;
        FakePending p1; p1.id = QStringLiteral("e1"); p1.title = QStringLiteral("Ep 1"); p1.foundAt = 100;
        FakePending p2; p2.id = QStringLiteral("e2"); p2.title = QStringLiteral("Ep 2"); p2.foundAt = 300;
        FakePending p3; p3.id = QStringLiteral("e3"); p3.title = QStringLiteral("Ep 3"); p3.foundAt = 200;
        pend << p1 << p2 << p3;

        QSet<QString> done;
        auto dealt = [&done](const QString& id) { return done.contains(id); };

        QVector<follow::NewRow> rows =
            follow::rowsForSeries(QStringLiteral("S"), QStringLiteral("addonA"), pend, dealt);
        CHECK(rows.size() == 3);
        CHECK(rows[0].id == QStringLiteral("e2"));   // newest first
        CHECK(rows[1].id == QStringLiteral("e3"));
        CHECK(rows[2].id == QStringLiteral("e1"));
        CHECK(rows[0].seriesKey == QStringLiteral("S"));
        CHECK(follow::unreadCount(pend, dealt) == 3);

        // Marking one watched takes it off the shelf AND off the badge — one filter, so they cannot disagree.
        done.insert(QStringLiteral("e2"));
        rows = follow::rowsForSeries(QStringLiteral("S"), QStringLiteral("addonA"), pend, dealt);
        CHECK(rows.size() == 2);
        CHECK(rows[0].id == QStringLiteral("e3"));
        CHECK(follow::unreadCount(pend, dealt) == 2);

        // THE UNION WITH #25. Two producers, one shelf, deduplicated by item id, newest first.
        QVector<follow::NewRow> followRows = rows;
        follow::NewRow m1; m1.id = QStringLiteral("tt1"); m1.title = QStringLiteral("Missed show");
        m1.foundAt = 400; m1.count = 3;                 // #25's rows stand for several episodes
        follow::NewRow m2; m2.id = QStringLiteral("e3"); m2.title = QStringLiteral("Same episode, other way");
        m2.foundAt = 150; m2.count = 2;
        const QVector<follow::NewRow> merged = follow::mergeNewShelf(followRows, { m1, m2 });
        CHECK(merged.size() == 3);                      // e3 appears ONCE, not twice
        CHECK(merged[0].id == QStringLiteral("tt1"));   // newest of the union leads
        int e3Count = -1;
        for (const follow::NewRow& r : merged) if (r.id == QStringLiteral("e3")) e3Count = r.count;
        CHECK(e3Count == 2);                            // the larger count survives the dedupe

        // The cap bounds the strip; an empty union is empty (so the shelf's header is skipped).
        QVector<follow::NewRow> many;
        for (int i = 0; i < follow::kNewShelfMax + 7; ++i)
        {
            follow::NewRow r; r.id = QStringLiteral("m") + QString::number(i); r.foundAt = i;
            many << r;
        }
        CHECK(follow::mergeNewShelf(many, {}).size() == follow::kNewShelfMax);
        CHECK(follow::mergeNewShelf(many, {}, 0).size() == many.size());   // <= 0 = uncapped (the folder)
        CHECK(follow::mergeNewShelf({}, {}).isEmpty());

        // A row with no id cannot be deduplicated and is not a row.
        follow::NewRow anon;
        CHECK(follow::mergeNewShelf({ anon }, {}).isEmpty());
    }

    if (failures == 0) { std::puts("FOLLOW-OK"); return 0; }
    std::fprintf(stderr, "FOLLOW: %d check(s) failed\n", failures);
    return 1;
}
