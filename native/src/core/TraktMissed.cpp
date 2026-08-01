#include "TraktMissed.h"

#include <QHash>
#include <QSet>
#include <QTimeZone>
#include <algorithm>

namespace {

// The TOTAL order inside one show's group. Air time decides, and season/episode break a tie — a double-bill
// really does arrive on one tick, and "the oldest" has to mean something when it does. Without the tie-break
// the group's first element would depend on the order Trakt happened to return, which would make the row's
// play target, its episode code and its poster all silently input-ordered.
bool episodeBefore(const CalendarEntry& a, const CalendarEntry& b)
{
    if (a.airsAtUtc != b.airsAtUtc) return a.airsAtUtc < b.airsAtUtc;
    if (a.season != b.season)       return a.season < b.season;
    return a.episode < b.episode;
}

struct Group
{
    QVector<CalendarEntry> eps;
    QSet<QString>          seen;   // stream ids already taken — a calendar that lists one twice must not double the count
};

} // namespace

QString trakt::missedShowKey(const TraktIds& showIds)
{
    // The SHOW half of an episode stream id, which is exactly what the movie mapping already computes: both
    // apply the one usability predicate (a "tt…" id carrying no ':'), and expressing it as a call rather than
    // as a second copy of that test is what keeps the dismissal key and the stream ids in the same group from
    // ever disagreeing. probe_trakt asserts the equality against imdbStreamIdFor's own prefix.
    return imdbMovieStreamIdFor(showIds);
}

bool trakt::missedDismissExpired(qint64 dismissedAt, qint64 nowUnix)
{
    // A non-positive stamp is not a record — 0 is the store's "never dismissed" — so it is collectable by
    // definition. A stamp in the FUTURE needs no separate case: `nowUnix - dismissedAt` is then negative and
    // cannot exceed the TTL, so it reads as live, which is what a peer with a fast clock deserves.
    if (dismissedAt <= 0) return true;
    return nowUnix - dismissedAt > static_cast<qint64>(kMissedDismissTtlDays) * 86400;
}

QVector<trakt::MissedRow> trakt::planMissed(
    const QVector<CalendarEntry>& entries, const QDateTime& nowUtc, int lookbackDays,
    const std::function<LocalState(const QString&)>& localState,
    const std::function<qint64(const QString&)>& dismissedThrough)
{
    QVector<MissedRow> out;
    // An empty window has no rows in it. Answered BEFORE the boundary arithmetic, because at zero days the
    // two boundaries collapse onto the same instant and an episode airing exactly then would satisfy both.
    //
    // The clock check beside it is the same deliberate not-killable-by-construction line as the air-time
    // one below, for the same reason: with an invalid `nowUtc` every comparison in the loop is against an
    // invalid QDateTime, whose ordering Qt does not define, so the emptiness of the result would rest on
    // an implementation detail rather than on this rule. It is stated rather than inferred.
    if (lookbackDays <= 0 || !nowUtc.isValid()) return out;
    const QDateTime oldest = nowUtc.addDays(-lookbackDays);

    QHash<QString, Group> groups;
    for (const CalendarEntry& e : entries)
    {
        // The parser's own rule: an entry with no air time cannot be placed on a calendar, so it cannot be
        // said to have aired either.
        //
        // NO MUTATION KILLS THIS LINE, and it stays anyway — deliberately, not by oversight. Deleting it
        // leaves the entry to be thrown out by clause 2 instead, because THIS Qt happens to order an
        // invalid QDateTime below every valid one. Qt does not promise that: comparing against an invalid
        // QDateTime is documented as giving an unspecified result, so the only thing standing between a
        // parse failure and a row on the user's shelf would be an accident of the implementation. The
        // exclusion is written here so it is a property of this rule rather than of Qt's comparison
        // operators. (probe_trakt §28 asserts the outcome; it cannot distinguish the two routes to it.)
        if (!e.airsAtUtc.isValid()) continue;
        // Clause 1, the partition against "Airing Soon": strictly-later belongs there, this tick belongs here.
        if (e.airsAtUtc > nowUtc) continue;
        // Clause 2, re-applied at RENDER time — the cache outlives the window the fetch asked for.
        if (e.airsAtUtc < oldest) continue;
        // Clause 3a: no key, no claim. Dropped rather than carried unplayable (see the header's asymmetry note).
        const QString sid = imdbStreamIdFor(e.showIds, e.season, e.episode);
        if (sid.isEmpty()) continue;
        // Clause 3b: any explicit mark at all — watched, in progress, abandoned, planned, hidden — means the
        // user already knows about this episode, so it is not a surprise and this surface stays quiet.
        if (localState(sid) != LocalState::Unmarked) continue;

        // Non-empty by construction: sid is non-empty only when the show's IMDB id passed the same usability
        // test this key applies, so there is deliberately no guard here for a case that cannot arise.
        Group& g = groups[missedShowKey(e.showIds)];
        if (g.seen.contains(sid)) continue;
        g.seen.insert(sid);
        g.eps.push_back(e);
    }

    for (auto it = groups.begin(); it != groups.end(); ++it)
    {
        QVector<CalendarEntry> eps = it.value().eps;
        std::sort(eps.begin(), eps.end(), episodeBefore);

        // Clause 4. Asked once per show, and only for a show that got this far. CLOSED: a dismissal stamped
        // with the newest air time the row was speaking for covers that episode too, which is the whole point
        // — the row must not come straight back with one entry still in it.
        //
        // "Never dismissed" (0, and anything else non-positive) needs no branch of its own and deliberately
        // does not get one: it becomes a cut at or before the epoch, and clause 2 has already thrown out
        // everything older than the window, so no surviving episode can fall under it. A guard here would be
        // a line no mutation could distinguish from its absence.
        const QDateTime cut = QDateTime::fromSecsSinceEpoch(dismissedThrough(it.key()), QTimeZone::UTC);
        eps.erase(std::remove_if(eps.begin(), eps.end(),
                                 [&cut](const CalendarEntry& e) { return e.airsAtUtc <= cut; }),
                  eps.end());
        if (eps.isEmpty()) continue;   // wholly dismissed: no row, no header, no trace

        const CalendarEntry& first = eps.front();
        MissedRow r;
        r.showKey        = it.key();
        // Taken from the OLDEST episode rather than from whichever row arrived first, so two entries that
        // spell the show differently cannot make the row depend on Trakt's array order.
        r.showTitle      = first.showTitle;
        r.showIds        = first.showIds;
        r.season         = first.season;
        r.episode        = first.episode;
        r.episodeTitle   = first.episodeTitle;
        r.streamId       = imdbStreamIdFor(first.showIds, first.season, first.episode);
        r.airedAtUtc     = first.airsAtUtc;
        r.latestAiredUtc = eps.back().airsAtUtc;
        r.count          = static_cast<int>(eps.size());
        // The first poster in air order, so a group where only some episodes carry art still gets some, and
        // gets the SAME one on every run.
        for (const CalendarEntry& e : eps)
            if (!e.posterUrl.isEmpty()) { r.posterUrl = e.posterUrl; break; }
        out.push_back(r);
    }

    // Most-recent first, then a total tie-break. Total is not tidiness here: the groups came out of a QHash,
    // whose iteration order is neither the input order nor stable across runs, so anything this comparator
    // leaves undecided is decided by the hash seed.
    std::sort(out.begin(), out.end(), [](const MissedRow& a, const MissedRow& b) {
        if (a.latestAiredUtc != b.latestAiredUtc) return a.latestAiredUtc > b.latestAiredUtc;
        const int c = QString::compare(a.showTitle, b.showTitle, Qt::CaseInsensitive);
        if (c != 0) return c < 0;
        return a.showKey < b.showKey;
    });
    return out;
}
