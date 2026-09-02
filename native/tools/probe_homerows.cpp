// Headless check of custom home rows (issue #161): the pure planner that orders/hides/caps the home's rows
// (src/core/HomeRows), the per-profile store behind it, and the two-device merge of the synced list. QtCore
// only — the planner takes plain vectors and the store is a QSettings wrapper — so this runs under the
// offscreen QPA in CI with no window, no add-ons and no theme.
//
// THE LOAD-BEARING ASSERTION IS THE FIRST ONE. #161 is opt-in personalisation, not a redesign: a profile
// that never opens the editor must render the home it rendered before this shipped, on BOTH layouts. That is
// pinned three ways here, because one way is not enough:
//   * plan(available, {}) returns `available` VERBATIM — same ids, same order, every cap 0 — for the classic
//     home's shelf list AND for the themed home's category/catalogue rows;
//   * defaultShelfOrder() is exactly what HomeView::renderRecents produced before #161. HomeView iterates
//     THAT list to build what it hands the planner, so the order pinned here is the order drawn on screen;
//   * an opt-in producer (Downloads, a playlist, a preset) is NOT part of the default vocabulary, so the
//     planner's "append a producer the list never heard of" rule cannot grow a row on an untouched profile.
// The mutation that motivated the first of those: a planner written without the empty-list special case —
// mapping every available row through a lookup of the (empty) list — renders an EMPTY home for every
// untouched profile in the world, and every other assertion in this file still passes.
//
// Also pinned: reorder, hide, cap; an unknown rowId kept in the store and skipped at render; a surface that
// cannot produce a listed row ignoring that entry (the "a theme that declares no Favourites shelf shows none
// regardless of the list" rule); the store round-trip and per-profile isolation; and the merge — last-writer
// -wins on order, a UNION of the row set so neither device loses a row, order-independence, and the one
// deliberate exception where a newer EMPTY document (a Reset) clears rather than unioning.
//
// Prints HOMEROWS-OK on success; any failure prints HOMEROWS-FAIL <cond> (line) and exits non-zero.
//
// Isolation: AppPaths::dataDir() is this process's own scratch directory (issue #42), so the everythingbox.ini
// the store reads/writes starts empty and is removed at exit. The probe seeds a profile id via
// ProfileStore::setCurrent, because currentId() otherwise resolves to "default" rather than a named profile.
#include "HomeRows.h"
#include "ProfileStore.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QStringList>
#include <cstdio>

using namespace homerows;

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "HOMEROWS-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

// The plan as a readable string, so a failure names what would have been rendered rather than "false".
static QString spell(const QVector<Planned>& p)
{
    QStringList out;
    for (const Planned& r : p)
        out << (r.cap > 0 ? r.rowId + QStringLiteral("/") + QString::number(r.cap) : r.rowId);
    return out.join(QLatin1Char(' '));
}

static QVector<Available> avail(const QStringList& ids)
{
    QVector<Available> out;
    for (const QString& id : ids) out.push_back({ id, 10 });
    return out;
}

static Row row(const QString& id, bool visible = true, int cap = 0)
{
    Row r; r.rowId = id; r.visible = visible; r.cap = cap; return r;
}

// ---- 1. default == today ----------------------------------------------------------------------------------
static void testDefaultIsToday()
{
    // The CLASSIC home, exactly as HomeView::renderRecents composed it before #161: the recently-played
    // groups, "You Missed", "Airing Soon", "★ Favorites". Written out by hand from the pre-change source,
    // never read back out of the function under test.
    const QStringList classicToday{ QStringLiteral("continue"), QStringLiteral("trakt:missed"),
                                    QStringLiteral("trakt:calendar"), QStringLiteral("favorites") };
    CHECK(defaultShelfOrder() == classicToday);

    // No stored list -> that sequence, in that order, uncapped.
    CHECK(spell(plan(avail(classicToday), {}))
          == QStringLiteral("continue trakt:missed trakt:calendar favorites"));

    // ...and it still holds when the home has fewer rows than the full set (no Trakt account configured),
    // which is the shape most installs actually have.
    CHECK(spell(plan(avail({ QStringLiteral("continue"), QStringLiteral("favorites") }), {}))
          == QStringLiteral("continue favorites"));
    CHECK(plan(avail({}), {}).isEmpty());

    // The THEMED home: the media-type buckets and the catalogue rows, in the order HomeView::categoryItems /
    // systemItems produce them. Same planner, same guarantee, over a vocabulary the classic home never uses.
    const QStringList themedToday{ QStringLiteral("category:video"), QStringLiteral("category:game"),
                                   QStringLiteral("category:audio"), QStringLiteral("source:cinemeta.movie"),
                                   QStringLiteral("source:aio.games") };
    const QVector<Planned> themed = plan(avail(themedToday), {});
    CHECK(themed.size() == themedToday.size());
    for (int i = 0; i < themed.size() && i < themedToday.size(); ++i)
    {
        CHECK(themed[i].rowId == themedToday[i]);
        CHECK(themed[i].cap == 0);
    }

    // Every cap in the default plan is 0. A default that capped anything would silently truncate a shelf that
    // has never been truncated — the same class of regression as reordering one.
    for (const Planned& p : plan(avail(classicToday), {})) CHECK(p.cap == 0);

    // The opt-in producers are NOT part of the default vocabulary, so the planner's append rule cannot put
    // one on an untouched home. (HomeView keeps that promise by only offering them once the list names one;
    // pinned here is WHICH ids that rule covers.)
    CHECK(isOptInShelf(QStringLiteral("downloads")));
    CHECK(isOptInShelf(QStringLiteral("playlist:abc")));
    CHECK(isOptInShelf(QStringLiteral("preset:Unplayed SNES RPGs")));
    CHECK(!isOptInShelf(QStringLiteral("continue")));
    CHECK(!isOptInShelf(QStringLiteral("favorites")));
    CHECK(!isOptInShelf(QStringLiteral("trakt:missed")));
    for (const QString& id : defaultShelfOrder()) CHECK(!isOptInShelf(id));
}

// ---- 2. reorder / hide / cap ------------------------------------------------------------------------------
static void testReorderHideCap()
{
    const QVector<Available> a = avail({ QStringLiteral("continue"), QStringLiteral("trakt:missed"),
                                         QStringLiteral("trakt:calendar"), QStringLiteral("favorites") });

    // The issue's own example: Continue first, a row hidden, Favourites capped to 6.
    const QVector<Row> list{ row(QStringLiteral("continue")),
                             row(QStringLiteral("favorites"), true, 6),
                             row(QStringLiteral("trakt:calendar"), false),
                             row(QStringLiteral("trakt:missed")) };
    CHECK(spell(plan(a, list)) == QStringLiteral("continue favorites/6 trakt:missed"));

    // A hidden row is not resurrected by the append pass. That pass keys on "the list has never MENTIONED
    // this id", not on "the plan does not contain it" — the latter would put every hidden row back at the
    // bottom of the home, which is the most obvious way this feature could fail while looking correct.
    const QVector<Row> allHidden{ row(QStringLiteral("continue"), false),
                                  row(QStringLiteral("trakt:missed"), false),
                                  row(QStringLiteral("trakt:calendar"), false),
                                  row(QStringLiteral("favorites"), false) };
    CHECK(plan(a, allHidden).isEmpty());

    // A producer the list has never heard of is APPENDED, never dropped: the list was written before this
    // profile connected Trakt, and "Airing Soon" must still reach the screen.
    const QVector<Row> old{ row(QStringLiteral("favorites")), row(QStringLiteral("continue")) };
    CHECK(spell(plan(a, old)) == QStringLiteral("favorites continue trakt:missed trakt:calendar"));

    // A cap of 0 (and a negative one, which the store clamps but the planner must not trust) is "no cap".
    CHECK(spell(plan(avail({ QStringLiteral("continue") }), { row(QStringLiteral("continue"), true, 0) }))
          == QStringLiteral("continue"));
    CHECK(spell(plan(avail({ QStringLiteral("continue") }), { row(QStringLiteral("continue"), true, -4) }))
          == QStringLiteral("continue"));

    // A duplicated id renders once, at its FIRST position (a merge can hand the planner one).
    const QVector<Row> dupes{ row(QStringLiteral("favorites"), true, 3), row(QStringLiteral("continue")),
                              row(QStringLiteral("favorites"), true, 9) };
    CHECK(spell(plan(avail({ QStringLiteral("continue"), QStringLiteral("favorites") }), dupes))
          == QStringLiteral("favorites/3 continue"));
}

// ---- 3. a row this surface cannot produce -----------------------------------------------------------------
static void testUnproducibleRowsAreKeptAndSkipped()
{
    // A preset the user deleted, an add-on that was removed, and a row that belongs to the OTHER layout's
    // home: all one rule. Each is skipped at render...
    const QVector<Available> classic = avail({ QStringLiteral("continue"), QStringLiteral("favorites") });
    const QVector<Row> list{ row(QStringLiteral("preset:gone")),
                             row(QStringLiteral("continue")),
                             row(QStringLiteral("source:removed.addon")),
                             row(QStringLiteral("category:video")),   // the themed home's vocabulary
                             row(QStringLiteral("favorites")) };
    CHECK(spell(plan(classic, list)) == QStringLiteral("continue favorites"));

    // ...and the SAME list on the themed home renders that home's rows and skips the classic ones. This is
    // the "the theme still owns what it can show" rule: a home that has no Favourites shelf shows none no
    // matter what the list says, and the entry survives for the layout that can honour it.
    const QVector<Available> themed = avail({ QStringLiteral("category:video"),
                                              QStringLiteral("source:removed.addon") });
    CHECK(spell(plan(themed, list)) == QStringLiteral("source:removed.addon category:video"));

    // ...and NOTHING was dropped from the list itself. Kept-and-skipped is a claim about the STORE, so it is
    // checked against the store rather than against the planner's output.
    ProfileStore::setCurrent(QStringLiteral("keepskip"));
    HomeRowStore::save(list);
    const QVector<Row> back = HomeRowStore::list();
    CHECK(back.size() == list.size());
    QSet<QString> ids;
    for (const Row& r : back) ids.insert(r.rowId);
    CHECK(ids.contains(QStringLiteral("preset:gone")));
    CHECK(ids.contains(QStringLiteral("source:removed.addon")));
    CHECK(ids.contains(QStringLiteral("category:video")));
}

// ---- 4. the store -----------------------------------------------------------------------------------------
static void testStore()
{
    ProfileStore::setCurrent(QStringLiteral("alice"));
    CHECK(HomeRowStore::list().isEmpty());     // a fresh profile is on the default layout
    CHECK(!HomeRowStore::isCustomised());

    const QVector<Row> mine{ row(QStringLiteral("continue")), row(QStringLiteral("favorites"), true, 6),
                             row(QStringLiteral("trakt:calendar"), false) };
    HomeRowStore::save(mine);
    CHECK(HomeRowStore::isCustomised());
    const QVector<Row> back = HomeRowStore::list();
    CHECK(back.size() == 3);
    CHECK(back.size() == 3 && back[0].rowId == QStringLiteral("continue") && back[0].visible && back[0].cap == 0);
    CHECK(back.size() == 3 && back[1].rowId == QStringLiteral("favorites") && back[1].cap == 6);
    CHECK(back.size() == 3 && back[2].rowId == QStringLiteral("trakt:calendar") && !back[2].visible);

    // Per profile, like every other small store here.
    ProfileStore::setCurrent(QStringLiteral("bob"));
    CHECK(HomeRowStore::list().isEmpty());
    ProfileStore::setCurrent(QStringLiteral("alice"));
    CHECK(HomeRowStore::list().size() == 3);

    // Reset puts the profile back on the default layout — and plan() then renders today's home again.
    HomeRowStore::reset();
    CHECK(HomeRowStore::list().isEmpty());
    CHECK(!HomeRowStore::isCustomised());
    CHECK(spell(plan(avail(defaultShelfOrder()), HomeRowStore::list()))
          == QStringLiteral("continue trakt:missed trakt:calendar favorites"));

    // A negative cap never reaches a caller.
    HomeRowStore::save({ row(QStringLiteral("continue"), true, -3) });
    CHECK(HomeRowStore::list().size() == 1 && HomeRowStore::list()[0].cap == 0);
    HomeRowStore::reset();
}

// ---- 5. JSON round-trip -----------------------------------------------------------------------------------
static void testJson()
{
    Doc d;
    d.updatedAt = 1700000000;
    d.rows = { row(QStringLiteral("continue")), row(QStringLiteral("favorites"), false, 6),
               row(QStringLiteral("preset:Unplayed SNES RPGs"), true, 12) };
    const Doc rt = fromJson(toJson(d));
    CHECK(rt.updatedAt == d.updatedAt);
    CHECK(rt.rows.size() == 3);
    CHECK(rt.rows.size() == 3 && rt.rows[1].rowId == QStringLiteral("favorites")
          && !rt.rows[1].visible && rt.rows[1].cap == 6);
    CHECK(rt.rows.size() == 3 && rt.rows[2].rowId == QStringLiteral("preset:Unplayed SNES RPGs")
          && rt.rows[2].cap == 12);

    // A row written by a build that lacked the fields defaults to VISIBLE and uncapped — what that row did
    // there. Defaulting `visible` to false would make an upgrade silently remove content.
    const QJsonObject legacy = QJsonDocument::fromJson(
        QByteArray("{\"updatedAt\":5,\"rows\":[{\"rowId\":\"continue\"},{\"rowId\":\"\"},{\"cap\":3}]}")).object();
    const Doc leg = fromJson(legacy);
    CHECK(leg.rows.size() == 1);           // the id-less entries are not rows
    CHECK(leg.rows.size() == 1 && leg.rows[0].visible && leg.rows[0].cap == 0);

    // Vocabulary.
    CHECK(isKnownRowId(QStringLiteral("continue")));
    CHECK(isKnownRowId(QStringLiteral("favorites")));
    CHECK(isKnownRowId(QStringLiteral("downloads")));
    CHECK(isKnownRowId(QStringLiteral("trakt:missed")));
    CHECK(isKnownRowId(QStringLiteral("preset:x")));
    CHECK(isKnownRowId(QStringLiteral("playlist:x")));
    CHECK(isKnownRowId(QStringLiteral("category:video")));
    CHECK(isKnownRowId(QStringLiteral("source:aio.games")));
    CHECK(!isKnownRowId(QString()));
    CHECK(!isKnownRowId(QStringLiteral("nonsense")));
    CHECK(!isKnownRowId(QStringLiteral("nonsense:x")));
    CHECK(!isKnownRowId(QStringLiteral(":x")));
    CHECK(!isKnownRowId(QStringLiteral("preset:")));
}

// ---- 6. the two-device merge -------------------------------------------------------------------------------
static void testMerge()
{
    auto doc = [](qint64 ts, const QVector<Row>& rows) { Doc d; d.updatedAt = ts; d.rows = rows; return d; };
    auto spellRows = [](const Doc& d) {
        QStringList out;
        for (const Row& r : d.rows)
            out << r.rowId + (r.visible ? QString() : QStringLiteral("(hidden)"))
                   + (r.cap > 0 ? QStringLiteral("/") + QString::number(r.cap) : QString());
        return out.join(QLatin1Char(' '));
    };

    // Last-writer-wins on the ORDER: the newer device's arrangement is what both end up with.
    const Doc oldD = doc(100, { row(QStringLiteral("continue")), row(QStringLiteral("favorites")) });
    const Doc newD = doc(200, { row(QStringLiteral("favorites"), true, 6), row(QStringLiteral("continue")) });
    CHECK(spellRows(merge(oldD, newD)) == QStringLiteral("favorites/6 continue"));
    CHECK(spellRows(merge(newD, oldD)) == QStringLiteral("favorites/6 continue"));   // order-independent
    CHECK(merge(oldD, newD).updatedAt == 200);

    // NEVER A LOST ROW ON EITHER SIDE. Each device added a row the other has never seen; both survive, the
    // winner's arrangement first, the loser's extras appended in the loser's own order.
    const Doc a = doc(300, { row(QStringLiteral("continue")), row(QStringLiteral("preset:aaa")) });
    const Doc b = doc(200, { row(QStringLiteral("favorites"), true, 4), row(QStringLiteral("playlist:xyz"), false) });
    CHECK(spellRows(merge(a, b)) == QStringLiteral("continue preset:aaa favorites/4 playlist:xyz(hidden)"));
    CHECK(spellRows(merge(b, a)) == spellRows(merge(a, b)));
    CHECK(merge(a, b).rows.size() == 4);
    // ...and the loser's own settings ride along with its row: one that arrives hidden stays hidden.

    // An equal stamp is not "undecided": both merge orders must agree. (Which of the two wins is an
    // implementation detail of the byte comparator; that BOTH orders answer the same is the property.)
    const Doc t1 = doc(400, { row(QStringLiteral("continue")), row(QStringLiteral("favorites")) });
    const Doc t2 = doc(400, { row(QStringLiteral("favorites")), row(QStringLiteral("continue")) });
    CHECK(spellRows(merge(t1, t2)) == spellRows(merge(t2, t1)));
    CHECK(merge(t1, t2).rows.size() == 2);

    // RESET PROPAGATES. A newer EMPTY document is the user pressing "Reset to default"; unioning there would
    // let the peer's stale copy put the list straight back and the reset could never travel at all.
    const Doc resetHusk = doc(500, {});
    CHECK(merge(a, resetHusk).rows.isEmpty());
    CHECK(merge(resetHusk, a).rows.isEmpty());
    CHECK(merge(a, resetHusk).updatedAt == 500);
    // An OLDER empty document is not a reset of anything — it loses, and contributes nothing.
    CHECK(spellRows(merge(a, doc(1, {}))) == QStringLiteral("continue preset:aaa"));
    // A device that has never written a list (ts 0, no rows) simply adopts the peer's.
    CHECK(spellRows(merge(doc(0, {}), a)) == QStringLiteral("continue preset:aaa"));

    // Merging twice changes nothing (a repeated pull must not reshuffle the home).
    const Doc once = merge(a, b);
    CHECK(spellRows(merge(once, b)) == spellRows(once));
    CHECK(spellRows(merge(once, once)) == spellRows(once));
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("EverythingBoxProbe"));
    QCoreApplication::setApplicationName(QStringLiteral("probe_homerows"));

    testDefaultIsToday();
    testReorderHideCap();
    testUnproducibleRowsAreKeptAndSkipped();
    testStore();
    testJson();
    testMerge();

    if (failures) { std::fprintf(stderr, "HOMEROWS-FAIL %d assertion(s)\n", failures); return 1; }
    std::printf("HOMEROWS-OK\n");
    return 0;
}
