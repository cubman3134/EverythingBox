// Headless check of personal TV channels (issue #179, increment 1): the pure schedule (src/core/Channels),
// the duration index (src/core/MediaDurations), the per-profile channel store (src/core/ChannelStore) and
// the source->candidates seam (src/core/ChannelLineup). QtCore-only — the schedule takes its clock as an
// argument and the stores are QSettings wrappers — so it runs under the offscreen QPA in CI with no window.
//
// EVERY EXPECTED VALUE BELOW IS HAND-COMPUTED FROM THE FIXTURE, never re-derived by calling the function
// under test a second time. The fixture lineup is three items of 100 / 200 / 300 seconds, so a day's slot
// boundaries are arithmetic a human can check: an in-order channel that went on air at midnight airs
// [0,100) [100,300) [300,600) [600,700) … and "what is on at 350" has exactly one answer.
//
// What is pinned:
//   1  the day boundary (local midnight in UTC seconds), incl. west-of-UTC offsets and pre-epoch instants
//   2  the duration gate: an item with no known length is DROPPED and NAMED, never aired at zero length
//   3  in-order vs shuffle, and that shuffle is a permutation (nothing lost, nothing duplicated)
//   4  DETERMINISM: same inputs twice -> identical timeline; a different day -> a different shuffle
//   5  the timeline itself: start epoch honoured, programmes contiguous, the day covered, the slot cap
//   6  what's-on-now + its offset, INCLUDING THE BOUNDARY SECOND (half-open windows)
//   7  the start-from-beginning override
//   8  the frozen-day cache: today's lineup does not move when the source changes; tomorrow's does
//   9  surfing: wrapped up/down, and the ±1 prefetch bound
//   10 the source-agnostic programme bridge (#75's xmltv::Programme)
//   11 the store round trip, id-stable rename, tombstoned delete, per-profile isolation, reserved-enum
//      round trip
//   12 the duration index outliving a cleared resume group
//   13 the lineup seam: an unimplemented source is EMPTY, an installed resolver is used
//
// Prints CHANNELS-OK on success; any failure prints CHANNELS-FAIL <cond> (line) and exits non-zero.
//
// Isolation: AppPaths::dataDir() is this process's own scratch directory (issue #42), so the everythingbox.ini
// the stores read/write starts empty and is removed at exit.
#include "AppBrand.h"
#include "AppPaths.h"
#include "ChannelLineup.h"
#include "ChannelStore.h"
#include "Channels.h"
#include "MediaDurations.h"
#include "ProfileStore.h"
#include "ResumeStore.h"
#include "Tombstones.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QSet>
#include <QSettings>
#include <cstdio>

using namespace channels;

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "CHANNELS-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

// ---- Fixtures --------------------------------------------------------------------------------------------
// Three items with hand-picked lengths, plus a fourth with NO length at all — the one the duration gate must
// drop. Lengths chosen so every boundary in this file can be checked by adding them up: 100, 200, 300.
static QVector<Candidate> fixtureCandidates()
{
    QVector<Candidate> c(4);
    c[0].itemId = QStringLiteral("a"); c[0].title = QStringLiteral("Alpha"); c[0].playKey = QStringLiteral("/lib/a.mkv");
    c[1].itemId = QStringLiteral("b"); c[1].title = QStringLiteral("Beta");  c[1].playKey = QStringLiteral("/lib/b.mkv");
    c[2].itemId = QStringLiteral("c"); c[2].title = QStringLiteral("Gamma"); c[2].playKey = QStringLiteral("/lib/c.mkv");
    c[3].itemId = QStringLiteral("d"); c[3].title = QStringLiteral("Delta"); c[3].playKey = QStringLiteral("/lib/d.mkv");
    return c;
}

// The hand-written duration table. "d" is absent on purpose. Keyed by PLAY KEY here; the identity-keyed
// index the app actually has is exercised in testDurationIndex + testLineupIdentityLookup below.
static int fixtureDuration(const Candidate& c)
{
    if (c.playKey == QStringLiteral("/lib/a.mkv")) return 100;
    if (c.playKey == QStringLiteral("/lib/b.mkv")) return 200;
    if (c.playKey == QStringLiteral("/lib/c.mkv")) return 300;
    return 0;
}

static QVector<LineupItem> fixtureLineup(QStringList* skipped = nullptr)
{
    return withDurations(fixtureCandidates(), fixtureDuration, skipped);
}

static Channel fixtureChannel(Ordering ord, qint64 startEpoch = 0)
{
    Channel ch;
    ch.id         = QStringLiteral("chan-fixture");
    ch.name       = QStringLiteral("Fixture TV");
    ch.sourceKind = SourceKind::Playlist;
    ch.sourceId   = QStringLiteral("pl-1");
    ch.ordering   = ord;
    ch.startEpoch = startEpoch;
    return ch;
}

// ---- 1. the day boundary ---------------------------------------------------------------------------------
static void testDayBoundary()
{
    // UTC. 1970-01-02 00:00:00 UTC is 86400; anything in that day floors to it.
    CHECK(dayStartUtc(86400, 0) == 86400);
    CHECK(dayStartUtc(86400 + 1, 0) == 86400);
    CHECK(dayStartUtc(86400 + 86399, 0) == 86400);
    CHECK(dayStartUtc(86400 + 86400, 0) == 172800);   // the boundary second belongs to the NEW day

    // UTC+2 (7200). Local midnight of 1970-01-02 is 1970-01-01 22:00 UTC = 79200.
    CHECK(dayStartUtc(86400, 7200) == 79200);
    CHECK(dayStartUtc(79200, 7200) == 79200);
    CHECK(dayStartUtc(79199, 7200) == 79200 - 86400);

    // UTC-5 (-18000). Local midnight of 1970-01-01 is 05:00 UTC = 18000.
    CHECK(dayStartUtc(18000, -18000) == 18000);
    CHECK(dayStartUtc(17999, -18000) == 18000 - 86400);

    // PRE-EPOCH. Truncating division would put -1 in the same day as 0; flooring does not.
    CHECK(dayStartUtc(-1, 0) == -86400);
    CHECK(dayStartUtc(-86400, 0) == -86400);
    CHECK(dayStartUtc(-86401, 0) == -172800);
}

// ---- 2. the duration gate --------------------------------------------------------------------------------
static void testDurationGate()
{
    QStringList skipped;
    const QVector<LineupItem> lineup = fixtureLineup(&skipped);
    CHECK(lineup.size() == 3);                                   // "d" is gone
    CHECK(skipped.size() == 1 && skipped.first() == QStringLiteral("d"));  // …and NAMED, once
    CHECK(lineup[0].itemId == QStringLiteral("a") && lineup[0].durationSec == 100);
    CHECK(lineup[1].itemId == QStringLiteral("b") && lineup[1].durationSec == 200);
    CHECK(lineup[2].itemId == QStringLiteral("c") && lineup[2].durationSec == 300);
    // Input order is preserved by the gate — the ORDERING decides play order, not the gate.
    CHECK(lineup[0].title == QStringLiteral("Alpha"));

    // A negative length is unknown, not a length.
    QVector<Candidate> one(1);
    one[0].itemId = QStringLiteral("neg"); one[0].playKey = QStringLiteral("k");
    QStringList sk2;
    CHECK(withDurations(one, [](const Candidate&) { return -5; }, &sk2).isEmpty());
    CHECK(sk2.size() == 1);

    // NO LOOKUP AT ALL means nothing qualifies — not "everything does". A caller that forgot the index gets
    // an empty channel, never a day divided into zero-length programmes.
    QStringList sk3;
    CHECK(withDurations(fixtureCandidates(), nullptr, &sk3).isEmpty());
    CHECK(sk3.size() == 4);

    // The skip list is OPTIONAL: passing nullptr must not crash and must gate identically.
    CHECK(withDurations(fixtureCandidates(), fixtureDuration, nullptr).size() == 3);
}

// ---- 3/4. ordering + determinism -------------------------------------------------------------------------
static void testOrderingAndDeterminism()
{
    const QVector<LineupItem> lineup = fixtureLineup();
    const qint64 day = dayStartUtc(1'700'000'000, 0);

    // In order: the identity permutation, always.
    const QVector<int> inOrder = orderFor(fixtureChannel(Ordering::InOrder), day, lineup.size());
    CHECK(inOrder == QVector<int>({0, 1, 2}));

    // Reserved TimeBlocked degrades to the identity rather than inventing a timeline.
    CHECK(orderFor(fixtureChannel(Ordering::TimeBlocked), day, 3) == QVector<int>({0, 1, 2}));

    // Shuffle is a PERMUTATION: every index exactly once. Checked as a set, so this assertion does not
    // silently encode whatever the current mixer happens to emit.
    const QVector<int> shuf = orderFor(fixtureChannel(Ordering::Shuffle), day, 3);
    CHECK(shuf.size() == 3);
    CHECK(QSet<int>(shuf.begin(), shuf.end()) == QSet<int>({0, 1, 2}));

    // DETERMINISM. Same channel, same day -> the same permutation, every time.
    CHECK(orderFor(fixtureChannel(Ordering::Shuffle), day, 3) == shuf);
    CHECK(orderFor(fixtureChannel(Ordering::Shuffle), day, 3) == shuf);

    // A DIFFERENT DAY re-seeds. Over a wide span the odds of a 3-element shuffle agreeing every day are
    // vanishing, so "at least one of the next 40 days differs" is a real assertion and not a flake: it fails
    // if and only if the day stopped feeding the seed.
    bool anyDiffers = false;
    for (int d = 1; d <= 40 && !anyDiffers; ++d)
        anyDiffers = orderFor(fixtureChannel(Ordering::Shuffle), day + d * 86400, 3) != shuf;
    CHECK(anyDiffers);

    // A DIFFERENT CHANNEL ID re-seeds too (two channels over the same playlist are not the same channel).
    Channel other = fixtureChannel(Ordering::Shuffle);
    other.id = QStringLiteral("chan-other");
    bool idMatters = false;
    for (int d = 0; d <= 40 && !idMatters; ++d)
        idMatters = orderFor(other, day + d * 86400, 3)
                    != orderFor(fixtureChannel(Ordering::Shuffle), day + d * 86400, 3);
    CHECK(idMatters);

    // The seed itself is stable and id/day sensitive.
    CHECK(seedFor(QStringLiteral("x"), 0) == seedFor(QStringLiteral("x"), 0));
    CHECK(seedFor(QStringLiteral("x"), 0) != seedFor(QStringLiteral("y"), 0));
    CHECK(seedFor(QStringLiteral("x"), 0) != seedFor(QStringLiteral("x"), 86400));

    // n == 0 / n < 0 are total.
    CHECK(orderFor(fixtureChannel(Ordering::Shuffle), day, 0).isEmpty());
    CHECK(orderFor(fixtureChannel(Ordering::Shuffle), day, -3).isEmpty());
}

// ---- 5. the timeline -------------------------------------------------------------------------------------
static void testBuildDay()
{
    const QVector<LineupItem> lineup = fixtureLineup();
    const qint64 day = 86400 * 100;   // an arbitrary UTC midnight

    const Schedule s = buildDay(fixtureChannel(Ordering::InOrder), day, lineup);
    CHECK(s.channelId == QStringLiteral("chan-fixture"));
    CHECK(s.dayStartUtc == day);
    CHECK(!s.isEmpty());
    // HAND-COMPUTED boundaries: 100 + 200 + 300 = 600 per cycle, starting at the day's midnight.
    CHECK(s.programmes[0].startUtc == day        && s.programmes[0].durationSec == 100 && s.programmes[0].itemId == QStringLiteral("a"));
    CHECK(s.programmes[1].startUtc == day + 100  && s.programmes[1].durationSec == 200 && s.programmes[1].itemId == QStringLiteral("b"));
    CHECK(s.programmes[2].startUtc == day + 300  && s.programmes[2].durationSec == 300 && s.programmes[2].itemId == QStringLiteral("c"));
    CHECK(s.programmes[3].startUtc == day + 600  && s.programmes[3].itemId == QStringLiteral("a"));   // the cycle repeats
    // CONTIGUOUS: no gaps, no overlaps, anywhere in the day.
    for (int i = 1; i < s.programmes.size(); ++i)
        CHECK(s.programmes[i].startUtc == s.programmes[i - 1].endUtc());
    // THE WHOLE DAY IS COVERED: the last slot ends at or past midnight, and no slot starts past it.
    CHECK(s.programmes.last().endUtc() >= s.dayEndUtc());
    CHECK(s.programmes.last().startUtc < s.dayEndUtc());
    // 86400 / 600 = 144 cycles of 3 -> 432 programmes exactly.
    CHECK(s.programmes.size() == 432);

    // AN EMPTY LINEUP IS AN EMPTY DAY, not a crash and not a day of nothing repeating.
    CHECK(buildDay(fixtureChannel(Ordering::InOrder), day, {}).isEmpty());

    // THE START EPOCH. A channel that went on air at day+5000 is off air before then: its first slot starts
    // AT the epoch, not at midnight.
    const Schedule late = buildDay(fixtureChannel(Ordering::InOrder, day + 5000), day, lineup);
    CHECK(!late.isEmpty());
    CHECK(late.programmes[0].startUtc == day + 5000);
    // …and a day BEFORE it went on air has no programmes at all.
    CHECK(buildDay(fixtureChannel(Ordering::InOrder, day + 5000), day - 86400, lineup).isEmpty());
    // A channel that goes on air exactly at the next midnight has nothing today (half-open day).
    CHECK(buildDay(fixtureChannel(Ordering::InOrder, day + 86400), day, lineup).isEmpty());

    // THE SLOT CAP. One-second items would otherwise lay 86 400 programmes; the day truncates instead of looping.
    QVector<LineupItem> tiny(1);
    tiny[0].itemId = QStringLiteral("t"); tiny[0].playKey = QStringLiteral("t"); tiny[0].durationSec = 1;
    const Schedule capped = buildDay(fixtureChannel(Ordering::InOrder), day, tiny);
    CHECK(capped.programmes.size() == kMaxSlotsPerDay);

    // The inputs hash moves with the inputs and is stable for identical ones.
    CHECK(hashOfLineup(fixtureChannel(Ordering::InOrder), day, lineup)
          == hashOfLineup(fixtureChannel(Ordering::InOrder), day, lineup));
    QVector<LineupItem> plus = lineup;
    plus.push_back(lineup[0]);
    CHECK(hashOfLineup(fixtureChannel(Ordering::InOrder), day, plus)
          != hashOfLineup(fixtureChannel(Ordering::InOrder), day, lineup));
    CHECK(s.inputsHash == hashOfLineup(fixtureChannel(Ordering::InOrder), day, lineup));

    // A whole day, built twice, is IDENTICAL slot for slot — the property every device relies on.
    const Schedule again = buildDay(fixtureChannel(Ordering::Shuffle), day, lineup);
    const Schedule third = buildDay(fixtureChannel(Ordering::Shuffle), day, lineup);
    CHECK(again.programmes.size() == third.programmes.size());
    bool identical = again.programmes.size() == third.programmes.size();
    for (int i = 0; identical && i < again.programmes.size(); ++i)
        identical = again.programmes[i].itemId == third.programmes[i].itemId
                    && again.programmes[i].startUtc == third.programmes[i].startUtc;
    CHECK(identical);
}

// ---- 6/7. what's on now, the boundary second, the override -----------------------------------------------
static void testWhatsOnNow()
{
    const QVector<LineupItem> lineup = fixtureLineup();
    const qint64 day = 86400 * 100;
    const Schedule s = buildDay(fixtureChannel(Ordering::InOrder), day, lineup);

    // Mid-programme: at day+350 we are 50 s into "c" (which runs [300, 600)), 250 s remain, "a" is next.
    const Airing mid = whatsOn(s, day + 350);
    CHECK(mid.valid);
    CHECK(mid.current.itemId == QStringLiteral("c"));
    CHECK(mid.offsetSec == 50);
    CHECK(mid.remainingSec == 250);
    CHECK(mid.hasNext && mid.next.itemId == QStringLiteral("a"));

    // THE BOUNDARY SECOND. The window is HALF-OPEN: at exactly day+100, "b" has started (offset 0) and "a"
    // is over. One second earlier "a" is still on, at its very last second.
    const Airing atBoundary = whatsOn(s, day + 100);
    CHECK(atBoundary.valid && atBoundary.current.itemId == QStringLiteral("b") && atBoundary.offsetSec == 0);
    const Airing justBefore = whatsOn(s, day + 99);
    CHECK(justBefore.valid && justBefore.current.itemId == QStringLiteral("a") && justBefore.offsetSec == 99);
    CHECK(justBefore.remainingSec == 1);
    // The day's very first second.
    const Airing atOpen = whatsOn(s, day);
    CHECK(atOpen.valid && atOpen.current.itemId == QStringLiteral("a") && atOpen.offsetSec == 0);

    // OUTSIDE the day, either side.
    CHECK(!whatsOn(s, day - 1).valid);
    CHECK(!whatsOn(s, day + 86400 * 2).valid);
    // …and an empty schedule answers "nothing", not a default-constructed programme claimed as valid.
    CHECK(!whatsOn(Schedule{}, day).valid);

    // BEFORE THE CHANNEL GOES ON AIR, on the day it does.
    const Schedule late = buildDay(fixtureChannel(Ordering::InOrder, day + 5000), day, lineup);
    CHECK(!whatsOn(late, day + 4999).valid);
    CHECK(whatsOn(late, day + 5000).valid);

    // THE OVERRIDE. Join-in-progress is the default; start-from-beginning is 0 for the SAME airing.
    CHECK(joinOffsetSec(mid, /*startFromBeginning=*/false) == 50);
    CHECK(joinOffsetSec(mid, /*startFromBeginning=*/true) == 0);
    // An invalid airing has no offset either way (a caller must not seek into nothing).
    CHECK(joinOffsetSec(Airing{}, false) == 0);
    CHECK(joinOffsetSec(Airing{}, true) == 0);

    // The last slot of the day has no `next` — the caller re-cuts tomorrow rather than being told a lie.
    const Airing last = whatsOn(s, s.programmes.last().startUtc);
    CHECK(last.valid && !last.hasNext);
}

// ---- 8. the frozen-day cache -----------------------------------------------------------------------------
static void testFrozenDayCache()
{
    ScheduleCache cache;
    const Channel ch = fixtureChannel(Ordering::InOrder);
    const qint64 day = 86400 * 100;
    const qint64 noon = day + 43200;

    const QVector<LineupItem> lineup = fixtureLineup();
    const Schedule first = cache.dayFor(ch, noon, 0, lineup);
    CHECK(!cache.servedFrozen());          // the first cut is fresh
    CHECK(first.programmes.size() == 432);

    // THE SAME DAY, WITH A LONGER LINEUP: today does not move. This is the rule that stops an episode added
    // at 20:15 re-cutting the evening under a viewer who is halfway through something.
    QVector<LineupItem> grown = lineup;
    LineupItem extra; extra.itemId = QStringLiteral("e"); extra.playKey = QStringLiteral("/lib/e.mkv");
    extra.title = QStringLiteral("Epsilon"); extra.durationSec = 600;
    grown.push_back(extra);
    const Schedule frozen = cache.dayFor(ch, noon + 3600, 0, grown);
    CHECK(cache.servedFrozen());
    CHECK(frozen.programmes.size() == first.programmes.size());
    CHECK(frozen.inputsHash == first.inputsHash);
    bool same = true;
    for (int i = 0; same && i < frozen.programmes.size(); ++i)
        same = frozen.programmes[i].itemId == first.programmes[i].itemId
               && frozen.programmes[i].startUtc == first.programmes[i].startUtc;
    CHECK(same);
    // …and the caller can SEE that the live lineup has drifted from the airing one.
    CHECK(cache.driftedFrom(ch, noon + 3600, 0, grown));
    CHECK(!cache.driftedFrom(ch, noon + 3600, 0, lineup));

    // THE NEXT DAY re-cuts, and the new item is in it.
    const Schedule tomorrow = cache.dayFor(ch, noon + 86400, 0, grown);
    CHECK(!cache.servedFrozen());
    CHECK(tomorrow.dayStartUtc == day + 86400);
    CHECK(tomorrow.inputsHash != first.inputsHash);
    bool sawExtra = false;
    for (const Slot& sl : tomorrow.programmes) if (sl.itemId == QStringLiteral("e")) { sawExtra = true; break; }
    CHECK(sawExtra);

    // forget() drops the freeze; clear() drops all of it.
    cache.forget(ch.id);
    CHECK(cache.size() == 0);
    cache.dayFor(ch, noon, 0, lineup);
    CHECK(cache.size() == 1);
    cache.clear();
    CHECK(cache.size() == 0);
    // Nothing frozen -> nothing to have drifted from.
    CHECK(!cache.driftedFrom(ch, noon, 0, grown));

    // TWO CHANNELS DO NOT SHARE A FREEZE.
    Channel two = fixtureChannel(Ordering::InOrder);
    two.id = QStringLiteral("chan-two");
    cache.dayFor(ch, noon, 0, lineup);
    cache.dayFor(two, noon, 0, grown);
    CHECK(cache.size() == 2);
    CHECK(cache.dayFor(ch, noon, 0, grown).inputsHash == first.inputsHash);   // ch is still frozen on `lineup`
}

// ---- 9. surfing ------------------------------------------------------------------------------------------
static void testSurfing()
{
    CHECK(surfIndex(3, 0, +1) == 1);
    CHECK(surfIndex(3, 2, +1) == 0);      // wraps
    CHECK(surfIndex(3, 0, -1) == 2);      // wraps the other way
    CHECK(surfIndex(1, 0, +1) == 0);      // a single channel surfs to itself
    CHECK(surfIndex(0, 0, +1) == -1);     // an empty list yields no index to dereference
    CHECK(surfIndex(3, 1, +7) == 2);      // any positive delta is ONE step
    CHECK(surfIndex(3, 1, -7) == 0);
    CHECK(surfIndex(3, 1, 0) == 1);

    const QStringList ids{QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c"), QStringLiteral("d")};
    // ±1 AND NO MORE, on either side of the tuned channel.
    CHECK(prefetchNeighbours(ids, QStringLiteral("b")) == QStringList({QStringLiteral("a"), QStringLiteral("c")}));
    // …wrapping at both ends.
    CHECK(prefetchNeighbours(ids, QStringLiteral("a")) == QStringList({QStringLiteral("d"), QStringLiteral("b")}));
    CHECK(prefetchNeighbours(ids, QStringLiteral("d")) == QStringList({QStringLiteral("c"), QStringLiteral("a")}));
    // Two channels: each has ONE neighbour, not the same one twice.
    const QStringList pair{QStringLiteral("x"), QStringLiteral("y")};
    CHECK(prefetchNeighbours(pair, QStringLiteral("x")) == QStringList({QStringLiteral("y")}));
    // One channel: its wrapped neighbour is ITSELF, which is not a neighbour — nothing to prefetch.
    CHECK(prefetchNeighbours({QStringLiteral("x")}, QStringLiteral("x")).isEmpty());
    // A tuned id that is not in the list prefetches nothing (rather than the first two).
    CHECK(prefetchNeighbours(ids, QStringLiteral("zz")).isEmpty());
    CHECK(prefetchNeighbours({}, QStringLiteral("a")).isEmpty());
    // THE BOUND ITSELF, over a long list: never more than two, never the tuned channel.
    QStringList many;
    for (int i = 0; i < 50; ++i) many << QStringLiteral("c%1").arg(i);
    const QStringList pf = prefetchNeighbours(many, QStringLiteral("c25"));
    CHECK(pf.size() == 2);
    CHECK(!pf.contains(QStringLiteral("c25")));
    CHECK(pf == QStringList({QStringLiteral("c24"), QStringLiteral("c26")}));
}

// ---- 10. the source-agnostic programme bridge ------------------------------------------------------------
static void testProgrammeBridge()
{
    const qint64 day = 86400 * 100;
    const Schedule s = buildDay(fixtureChannel(Ordering::InOrder), day, fixtureLineup());
    const QVector<xmltv::Programme> progs = toProgrammes(s);
    CHECK(progs.size() == s.programmes.size());
    CHECK(progs[0].title == QStringLiteral("Alpha"));
    CHECK(progs[0].channelId == QStringLiteral("channel:chan-fixture"));   // the #161 row-producer key
    CHECK(progs[0].startUtc == QDateTime::fromSecsSinceEpoch(day, Qt::UTC));
    CHECK(progs[0].stopUtc == QDateTime::fromSecsSinceEpoch(day + 100, Qt::UTC));
    CHECK(progs[1].startUtc == progs[0].stopUtc);
    // The programme model is #75's, so its own now/next reader answers the same question the schedule does.
    const xmltv::NowNext nn = xmltv::nowNext(progs, QDateTime::fromSecsSinceEpoch(day + 350, Qt::UTC));
    CHECK(nn.hasCurrent && nn.current.title == QStringLiteral("Gamma"));
    CHECK(nn.hasNext && nn.next.title == QStringLiteral("Alpha"));
    // An empty schedule bridges to no programmes.
    CHECK(toProgrammes(Schedule{}).isEmpty());
}

// ---- the row-producer name (#161 coordinates on this string, by NAME only) --------------------------------
static void testRowProducerKey()
{
    CHECK(rowProducerKey(QStringLiteral("abc")) == QStringLiteral("channel:abc"));
    CHECK(rowProducerKey(QString()).isEmpty());
    CHECK(isRowProducerKey(QStringLiteral("channel:abc")));
    CHECK(!isRowProducerKey(QStringLiteral("channel:")));       // a prefix with no id is not a key
    CHECK(!isRowProducerKey(QStringLiteral("livetv:abc")));
    CHECK(!isRowProducerKey(QString()));
    CHECK(channelIdFromKey(QStringLiteral("channel:abc")) == QStringLiteral("abc"));
    CHECK(channelIdFromKey(QStringLiteral("livetv:abc")).isEmpty());
    // Round trip, including an id with a colon in it (a uuid never has one, but the parse must not split).
    CHECK(channelIdFromKey(rowProducerKey(QStringLiteral("a:b"))) == QStringLiteral("a:b"));
}

// ---- enums: the RESERVED values must round-trip ----------------------------------------------------------
static void testEnums()
{
    for (int v : { 0, 1, 2, 3, 4 })
        CHECK(toInt(sourceKindFromInt(v)) == v);
    for (int v : { 0, 1, 2 })
        CHECK(toInt(orderingFromInt(v)) == v);
    // An UNKNOWN number falls back to the DEFAULT, never to a neighbour: a channel written by a newer build
    // must not silently become a playlist channel pointing at somebody else's id.
    CHECK(sourceKindFromInt(99) == SourceKind::Playlist);
    CHECK(sourceKindFromInt(-1) == SourceKind::Playlist);
    CHECK(orderingFromInt(99) == Ordering::Shuffle);
    // What increment 1 actually implements.
    CHECK(isImplemented(SourceKind::Playlist));
    CHECK(isImplemented(SourceKind::FilterPreset));
    CHECK(isImplemented(SourceKind::LocalFolder));
    CHECK(!isImplemented(SourceKind::AddonCatalog));
    CHECK(!isImplemented(SourceKind::ServerItems));
    CHECK(isImplemented(Ordering::InOrder));
    CHECK(isImplemented(Ordering::Shuffle));
    CHECK(!isImplemented(Ordering::TimeBlocked));
    CHECK(!label(SourceKind::Playlist).isEmpty() && !label(Ordering::Shuffle).isEmpty());
}

// ---- 12. the duration index ------------------------------------------------------------------------------
static void testDurationIndex()
{
    QSettings s(AppPaths::dataDir() + QStringLiteral("/durprobe.ini"), QSettings::IniFormat);
    const QString key = QStringLiteral("/lib/a.mkv");
    CHECK(MediaDurations::secondsIn(s, key) == 0);           // unknown reads as 0
    MediaDurations::noteIn(s, key, 1234);
    CHECK(MediaDurations::secondsIn(s, key) == 1234);
    // 0 and negatives are NOT stored — "unknown" has exactly one spelling.
    MediaDurations::noteIn(s, QStringLiteral("/lib/z.mkv"), 0);
    CHECK(MediaDurations::secondsIn(s, QStringLiteral("/lib/z.mkv")) == 0);
    MediaDurations::noteIn(s, QStringLiteral("/lib/z.mkv"), -7);
    CHECK(MediaDurations::secondsIn(s, QStringLiteral("/lib/z.mkv")) == 0);
    // A later measurement wins.
    MediaDurations::noteIn(s, key, 1300);
    CHECK(MediaDurations::secondsIn(s, key) == 1300);
    // An empty key is total.
    CHECK(MediaDurations::keyFor(QString()).isEmpty());
    CHECK(MediaDurations::secondsIn(s, QString()) == 0);

    // THE POINT OF THE STORE: the length OUTLIVES the resume group. ResumeStore::clear removes the whole
    // group (that is what finishing a file does), and before this index existed that threw away the only
    // measurement the app ever took — so the items most likely to be in a channel, the ones you have
    // watched, were exactly the ones with no length.
    s.setValue(ResumeStore::groupFor(key) + QStringLiteral("/pos"), 12.0);
    s.setValue(ResumeStore::groupFor(key) + QStringLiteral("/dur"), 1300.0);
    ResumeStore::clear(s, key);
    CHECK(!s.contains(ResumeStore::groupFor(key) + QStringLiteral("/dur")));
    CHECK(MediaDurations::secondsIn(s, key) == 1300);   // still known
    // It is keyed the same way the resume group is, so one item is one row in both.
    CHECK(MediaDurations::keyFor(key) == QStringLiteral("mediadur/") + ResumeStore::tombKey(key));
}

// ---- 12b. the gate asks the index by IDENTITY, not by path ------------------------------------------------
// THE DEFECT A LIVE DRIVE FOUND. The duration index is keyed by whatever identity PlaybackSession resumed the
// item under, and for a local video that is its MediaItem id ("local:<path>"), not its path. The first cut of
// the gate asked with the path alone, so a channel over three files the app had already played reported that
// none of them had a known length — indistinguishable, on screen, from a channel with nothing to air.
static void testLineupIdentityLookup()
{
    // Driven through the REAL ChannelLineup::knownDurationSec and the REAL MediaDurations store (the probe's
    // data dir is its own scratch ini, so "the app's store" here is this process's), NOT through a lambda that
    // re-states the rule — a test that spells the lookup itself would have passed against the broken code.
    const QString path  = QStringLiteral("C:/lib/The Show S01E01.mkv");
    const QString ident = QStringLiteral("local:") + path;
    // The two keys hash DIFFERENTLY — which is the whole reason asking with the wrong one silently failed.
    CHECK(MediaDurations::keyFor(ident) != MediaDurations::keyFor(path));

    Candidate c; c.itemId = ident; c.title = QStringLiteral("Ep 1"); c.playKey = path;
    CHECK(ChannelLineup::knownDurationSec(c) == 0);          // nothing known yet

    // Filed under the IDENTITY — which is what the app writes (MainWindow::onDuration hands
    // PlaybackSession::resumePath(), and for a local video that is its MediaItem id, not its path).
    MediaDurations::note(ident, 600);
    CHECK(MediaDurations::seconds(path) == 0);               // …and NOT under the path
    CHECK(ChannelLineup::knownDurationSec(c) == 600);        // the lookup finds it anyway

    // …so the gate keeps the item, where the path-only lookup that shipped first would have skipped it.
    QStringList skipped;
    const QVector<LineupItem> kept =
        withDurations({ c }, [](const Candidate& x) { return ChannelLineup::knownDurationSec(x); }, &skipped);
    CHECK(kept.size() == 1 && kept[0].durationSec == 600);
    CHECK(skipped.isEmpty());

    // THE FALLBACK ARM: a candidate with no id at all is looked up by its play key, and found.
    MediaDurations::note(path, 480);
    Candidate bare; bare.playKey = path;
    CHECK(ChannelLineup::knownDurationSec(bare) == 480);
    // …and the id still WINS when both are known (the identity is the more specific fact).
    CHECK(ChannelLineup::knownDurationSec(c) == 600);
    // A candidate that names nothing is 0, not a crash.
    CHECK(ChannelLineup::knownDurationSec(Candidate{}) == 0);
}

// ---- 11. the store ---------------------------------------------------------------------------------------
static void testStore()
{
    ProfileStore::setCurrent(QStringLiteral("alpha"));
    CHECK(ChannelStore::list().isEmpty());

    Channel c;
    c.name       = QStringLiteral("90s Saturday");
    c.sourceKind = SourceKind::LocalFolder;
    c.sourceId   = QStringLiteral("name:the show");
    c.ordering   = Ordering::Shuffle;
    c.startFromBeginning = true;
    const QString id = ChannelStore::add(c);
    CHECK(!id.isEmpty());

    QVector<Channel> all = ChannelStore::list();
    CHECK(all.size() == 1);
    CHECK(all[0].id == id);
    CHECK(all[0].name == QStringLiteral("90s Saturday"));
    CHECK(all[0].sourceKind == SourceKind::LocalFolder);
    CHECK(all[0].sourceId == QStringLiteral("name:the show"));
    CHECK(all[0].ordering == Ordering::Shuffle);
    CHECK(all[0].startFromBeginning);
    CHECK(all[0].ts > 0);
    // A channel with no explicit start epoch goes on air NOW, not at the epoch — otherwise it would claim to
    // have been broadcasting since 1970 and drop a viewer at a random point of a schedule that never aired.
    CHECK(all[0].startEpoch > 0);
    const qint64 airedFrom = all[0].startEpoch;

    Channel got;
    CHECK(ChannelStore::get(id, got) && got.name == QStringLiteral("90s Saturday"));
    CHECK(!ChannelStore::get(QStringLiteral("nope"), got));
    CHECK(ChannelStore::add(Channel{}).isEmpty());   // a nameless channel is not stored

    // A RENAME IS AN EDIT ON A STABLE ID, not a delete+add — the whole reason identity is the id.
    got.name = QStringLiteral("Saturday Morning");
    got.ordering = Ordering::InOrder;
    CHECK(ChannelStore::update(got));
    all = ChannelStore::list();
    CHECK(all.size() == 1 && all[0].id == id);
    CHECK(all[0].name == QStringLiteral("Saturday Morning"));
    CHECK(all[0].ordering == Ordering::InOrder);
    // …and the edit did NOT re-date when the channel went on air.
    CHECK(all[0].startEpoch == airedFrom);
    Channel unknown; unknown.id = QStringLiteral("nope"); unknown.name = QStringLiteral("x");
    CHECK(!ChannelStore::update(unknown));

    // RESERVED enum values round-trip through the store, so a channel written by a newer build survives here.
    Channel reserved;
    reserved.name = QStringLiteral("Future");
    reserved.sourceKind = SourceKind::ServerItems;
    reserved.ordering   = Ordering::TimeBlocked;
    const QString rid = ChannelStore::add(reserved);
    CHECK(ChannelStore::get(rid, got));
    CHECK(got.sourceKind == SourceKind::ServerItems && got.ordering == Ordering::TimeBlocked);
    ChannelStore::remove(rid);

    // A DELETE LEAVES A DATED TOMBSTONE, so a peer's stale copy cannot walk back in on the next merge.
    ChannelStore::remove(id);
    CHECK(ChannelStore::list().isEmpty());
    bool tombed = false;
    for (const Tombstones::Entry& t : Tombstones::all(QStringLiteral("channels/alpha")))
        if (t.key == id && t.ts > 0) { tombed = true; break; }
    CHECK(tombed);
    // Removing something that is not there tombstones nothing (no husk per stray call).
    const int before = Tombstones::all(QStringLiteral("channels/alpha")).size();
    ChannelStore::remove(QStringLiteral("never-existed"));
    CHECK(Tombstones::all(QStringLiteral("channels/alpha")).size() == before);

    // PER-PROFILE ISOLATION: another profile's channels are not this one's.
    ProfileStore::setCurrent(QStringLiteral("beta"));
    CHECK(ChannelStore::list().isEmpty());
    Channel b; b.name = QStringLiteral("Beta TV");
    const QString bid = ChannelStore::add(b);
    CHECK(ChannelStore::list().size() == 1);
    ProfileStore::setCurrent(QStringLiteral("alpha"));
    CHECK(ChannelStore::list().isEmpty());
    CHECK(!ChannelStore::get(bid, got));
    ProfileStore::setCurrent(QStringLiteral("beta"));
    CHECK(ChannelStore::list().size() == 1);
    ChannelStore::remove(bid);
}

// ---- 13. the lineup seam ---------------------------------------------------------------------------------
static void testLineupSeam()
{
    ChannelLineup::clearSourceResolvers();

    // An UNIMPLEMENTED source is EMPTY — never "everything", never a guess.
    Channel preset = fixtureChannel(Ordering::InOrder);
    preset.sourceKind = SourceKind::FilterPreset;
    preset.sourceId   = QStringLiteral("Unwatched 90s sitcoms");
    CHECK(ChannelLineup::candidatesFor(preset).isEmpty());
    QStringList skipped;
    CHECK(ChannelLineup::build(preset, &skipped).isEmpty());
    CHECK(skipped.isEmpty());   // nothing was enumerated, so nothing was SKIPPED for want of a length

    // A playlist channel over a playlist that does not exist is empty too (not a crash).
    Channel pl = fixtureChannel(Ordering::InOrder);
    pl.sourceId = QStringLiteral("no-such-playlist");
    CHECK(ChannelLineup::candidatesFor(pl).isEmpty());

    // AN INSTALLED RESOLVER IS USED — the seam increment 2's sources (and a preset resolver) arrive through.
    ChannelLineup::setSourceResolver(SourceKind::FilterPreset, [](const Channel& ch) {
        QVector<Candidate> out;
        Candidate c; c.itemId = ch.sourceId; c.title = QStringLiteral("Resolved"); c.playKey = QStringLiteral("/lib/a.mkv");
        out.push_back(c);
        return out;
    });
    const QVector<Candidate> resolved = ChannelLineup::candidatesFor(preset);
    CHECK(resolved.size() == 1 && resolved[0].title == QStringLiteral("Resolved"));
    // …and it does NOT bleed into another kind.
    CHECK(ChannelLineup::candidatesFor(pl).isEmpty());
    ChannelLineup::clearSourceResolvers();
    CHECK(ChannelLineup::candidatesFor(preset).isEmpty());
}

// ---- end to end: one channel, one clock, one join ---------------------------------------------------------
// The question a tuner asks, answered the way MainWindow::tuneChannel asks it, so the live drive has a number
// to compare against: "at this exact second, what plays and how far in?"
static void testJoinInProgress()
{
    const qint64 day = 86400 * 19000;          // an arbitrary UTC midnight
    const qint64 now = day + 3 * 3600 + 17;    // 03:00:17 local (tz 0)
    ScheduleCache cache;
    const Channel ch = fixtureChannel(Ordering::InOrder);
    const Schedule s = cache.dayFor(ch, now, 0, fixtureLineup());
    const Airing a = whatsOn(s, now);
    CHECK(a.valid);
    // 10817 s into a 600 s cycle: 10817 = 18*600 + 17, so we are 17 s into the cycle -> item "a" [0,100).
    CHECK(a.current.itemId == QStringLiteral("a"));
    CHECK(a.offsetSec == 17);
    CHECK(a.remainingSec == 83);
    CHECK(joinOffsetSec(a, ch.startFromBeginning) == 17);
    Channel fromTop = ch; fromTop.startFromBeginning = true;
    CHECK(joinOffsetSec(a, fromTop.startFromBeginning) == 0);
    // Re-asking the SAME clock gives the SAME answer (the frozen day is what guarantees it).
    const Airing again = whatsOn(cache.dayFor(ch, now, 0, fixtureLineup()), now);
    CHECK(again.current.itemId == a.current.itemId && again.offsetSec == a.offsetSec);
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    testDayBoundary();
    testDurationGate();
    testOrderingAndDeterminism();
    testBuildDay();
    testWhatsOnNow();
    testFrozenDayCache();
    testSurfing();
    testProgrammeBridge();
    testRowProducerKey();
    testEnums();
    testDurationIndex();
    testLineupIdentityLookup();
    testStore();
    testLineupSeam();
    testJoinInProgress();
    if (failures == 0) std::printf("CHANNELS-OK\n");
    return failures == 0 ? 0 : 1;
}
