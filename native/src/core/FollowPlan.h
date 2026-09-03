// The PURE layer behind "Follow a series" (issue #155, increment 1). Header-only, QtCore-only, no store, no
// network, no clock of its own — every function here is a total function of its arguments, which is what lets
// probe_follow drive the whole feature (the schedule, the politeness, the diff, the shelf) against a FAKE
// CLOCK with nothing running.
//
// The four rules this file owns, each stated once so the scheduler, the shelf and the probe cannot drift:
//
// 1. WHAT CAN BE FOLLOWED. A series-shaped row and nothing else. The verb appears on a container the user
//    can drill into (`expandable`) whose type is not one of the leaf/structural types listed below — an
//    episode, a track, a chapter, a film, a console folder or a playlist folder is not a thing that grows
//    new children, and offering "Follow" on one is offering a promise the refresh can never keep. The test is
//    deliberately a DENY list over `expandable` rather than an allow list of known series types: an addon may
//    define its own container type (the bundled podcasts addon defines "podcast"), and an allow list would
//    silently exclude every source we have not heard of — which is the opposite of "any series-shaped item
//    from any source".
//
// 2. WHEN A CYCLE IS DUE. lastRun + interval + jitter, where the jitter is DETERMINISTIC in its seed and
//    bounded by min(interval/10, 15 min). Deterministic because a probe has to be able to assert the bound
//    and the exact due second; bounded because the jitter exists to stop a fleet of installs waking on the
//    same second, not to make the schedule vague. Interval 0 = MANUAL: never due, "Check now" only.
//
// 3. WHEN A REQUEST MAY BE SENT. Per SOURCE, never globally: one request in flight at a time and a minimum
//    gap between two requests to the same source, and a source that FAILED is skipped for the rest of the
//    cycle rather than retried inside it. "Retried next cycle only" is the whole politeness claim of this
//    feature — a source that is down must cost it exactly one request per cycle, no matter how many followed
//    series it holds.
//
// 4. WHAT COUNTS AS NEW. Children present now that were not in the last snapshot. A child that DISAPPEARED
//    is not news (a podcast feed that only publishes its last 60 episodes drops old ones constantly), so the
//    seen-set only ever GROWS — intersecting it with the current list would make an episode that fell out and
//    came back read as new. A source that does not give its children stable ids cannot be diffed per child at
//    all; that degrades to ONE row saying the series changed, decided here (coarseChanged) rather than by the
//    caller, so the degrade is pinned by the same probe as the precise path.
#pragma once
#include <QCryptographicHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>
#include <algorithm>

namespace follow
{
    // ---- 1. What can be followed --------------------------------------------------------------------

    // Types that are never followable however the source flags them. Leaves first (an episode/track/chapter
    // is the THING a follow delivers, not a thing to follow), then the structural containers the browse tree
    // builds itself — a console platform, a playlist folder, a Live TV source, an album (a fixed track list
    // that does not grow) and the synthetic marker rows.
    inline bool isNeverFollowable(const QString& type)
    {
        static const QSet<QString> kNever = {
            QStringLiteral("episode"),   QStringLiteral("movie"),    QStringLiteral("track"),
            QStringLiteral("chapter"),   QStringLiteral("book"),     QStringLiteral("photo"),
            QStringLiteral("game"),      QStringLiteral("video"),    QStringLiteral("audio"),
            QStringLiteral("link"),      QStringLiteral("info"),     QStringLiteral("rechdr"),
            QStringLiteral("podcast_episode"),
            QStringLiteral("platform"),  QStringLiteral("album"),    QStringLiteral("livetv"),
            QStringLiteral("playlist"),  QStringLiteral("folder"),
        };
        return kNever.contains(type);
    }

    // The verb's gate, on both layouts. `expandable` is the source's own claim that the row has children;
    // a row that has none can never produce a new one. A synthetic row (type starting '_') carries no
    // identity to file a follow under, so it is refused before anything else looks at it.
    inline bool isFollowable(const QString& type, bool expandable)
    {
        if (type.isEmpty() || type.startsWith(QLatin1Char('_'))) return false;
        if (isNeverFollowable(type)) return false;
        return expandable;
    }

    // ---- 2. When a cycle is due ---------------------------------------------------------------------

    constexpr qint64 kHourSecs = 3600;
    constexpr qint64 kMaxJitterSecs = 15 * 60;   // the jitter ceiling, whatever the interval

    // The intervals offered in Settings, in hours, in the order they are shown. 0 = manual (never due).
    inline QVector<int> intervalChoicesHours() { return { 6, 12, 24, 24 * 7, 0 }; }

    // Clamp a stored interval to one of the offered choices; anything unrecognised reads as the default
    // (daily). A stored 0 is a real choice (manual) and survives.
    inline qint64 clampIntervalHours(int hours)
    {
        for (int h : intervalChoicesHours()) if (h == hours) return h;
        return 24;
    }

    // Deterministic per-install jitter, in [0, min(interval/10, 15 min)]. The seed is the install's device
    // id hashed to an int by the caller — same box, same offset every cycle, different boxes spread out.
    // Deterministic rather than random so the probe can assert the exact due second AND the bound.
    inline qint64 jitterSecs(qint64 intervalSecs, quint32 seed)
    {
        if (intervalSecs <= 0) return 0;
        const qint64 span = qMin<qint64>(intervalSecs / 10, kMaxJitterSecs);
        return span <= 0 ? 0 : qint64(seed % quint32(span + 1));
    }

    // The second at which the next scheduled cycle becomes due. A never-run install (lastRun <= 0) is due
    // IMMEDIATELY rather than one interval from install: the first thing a user does after following
    // something is look for what is new, and making them wait a day for the first pass reads as broken.
    inline qint64 nextDueAt(qint64 lastRun, qint64 intervalSecs, qint64 jitter)
    {
        if (intervalSecs <= 0) return -1;         // manual: never due
        if (lastRun <= 0)      return 0;          // never run -> due now
        return lastRun + intervalSecs + qMax<qint64>(0, jitter);
    }

    inline bool dueNow(qint64 now, qint64 lastRun, qint64 intervalSecs, qint64 jitter)
    {
        const qint64 due = nextDueAt(lastRun, intervalSecs, jitter);
        return due >= 0 && now >= due;
    }

    // ---- 3. When a request may be sent --------------------------------------------------------------

    // The minimum gap between two requests to the SAME source. Five seconds is not a network limit — it is
    // the claim "a background pass is never mistakable for a scrape" made concrete: a user with forty
    // followed series on one addon costs that addon one request every five seconds for three minutes, once
    // a day, instead of forty at once.
    constexpr qint64 kSourceGapSecs = 5;

    enum class Admit
    {
        Send,          // go
        WaitInFlight,  // this source already has a request out — one at a time, per source
        WaitGap,       // too soon after that source's last request
        SourceFailed,  // this source failed earlier in THIS cycle: skipped until the next one
    };

    inline Admit admit(qint64 now, qint64 lastSentToSource, bool sourceBusy,
                       bool sourceFailedThisCycle, qint64 gapSecs = kSourceGapSecs)
    {
        if (sourceFailedThisCycle) return Admit::SourceFailed;
        if (sourceBusy)            return Admit::WaitInFlight;
        // lastSentToSource <= 0 means "not yet asked this run", which must not be read as "asked at the
        // epoch and therefore long enough ago" — it is, but stating it keeps the branch honest if the
        // sentinel ever changes.
        if (lastSentToSource > 0 && now - lastSentToSource < gapSecs) return Admit::WaitGap;
        return Admit::Send;
    }

    // ---- 4. What counts as new ----------------------------------------------------------------------

    // One child of a followed series as the source reported it. `id` is the source's own stable item id;
    // EMPTY means this source does not key its children, which is what triggers the coarse degrade.
    struct Child
    {
        QString id;
        QString title;
        QString subtitle;
        QString thumbnailUrl;
        QString type;
        QString url;      // a direct playable (the podcasts addon puts the audio stream here)
        QString mime;
    };

    // The most seen-ids a snapshot keeps. A podcast that has run weekly for twenty years is ~1000 episodes;
    // past the cap the OLDEST are dropped, which can at worst re-announce an episode a decade after it fell
    // out of both the cap and the feed. Unbounded growth in a store that is written on every cycle is the
    // worse failure.
    constexpr int kMaxSeenIds = 2000;

    // A coarse fingerprint for a source that gives no stable child ids: the count plus a digest of the
    // titles, in order. Not an identity — only ever compared with the previous one for equality.
    inline QString coarseFingerprint(const QVector<Child>& children)
    {
        QCryptographicHash h(QCryptographicHash::Md5);
        h.addData(QByteArray::number(children.size()));
        for (const Child& c : children) { h.addData("\x1f"); h.addData(c.title.toUtf8()); }
        return QString::fromLatin1(h.result().toHex());
    }

    // True when the child list cannot be diffed per child — ANY child without an id poisons it, because a
    // partial diff would announce the keyed half and silently drop the rest. An EMPTY list is reliable
    // (a series with no children yet is a fact, not an unreliable source).
    inline bool childrenAreKeyed(const QVector<Child>& children)
    {
        for (const Child& c : children) if (c.id.isEmpty()) return false;
        return true;
    }

    struct Diff
    {
        QVector<Child> newChildren;   // present now, absent from the snapshot (keyed sources)
        bool coarseChanged = false;   // unkeyed source whose fingerprint moved (the "something changed" row)
        QStringList seenAfter;        // the seen-set to persist: the old set UNION the current ids, capped
        QString fingerprintAfter;     // the fingerprint to persist (unkeyed sources)
    };

    // The diff. `firstEver` is the snapshot store's "this series has never been checked" answer, and it is a
    // SEPARATE argument rather than "seen is empty" on purpose: a series that genuinely has no children yet
    // also has an empty seen-set, and conflating the two would either announce a whole back catalogue on the
    // first check or never announce the first ever child.
    inline Diff diffChildren(const QStringList& seen, const QString& seenFingerprint,
                             const QVector<Child>& current, bool firstEver)
    {
        Diff d;
        const bool keyed = childrenAreKeyed(current);
        d.fingerprintAfter = keyed ? QString() : coarseFingerprint(current);

        // The seen-set only grows: current ids first (so the cap keeps the NEWEST), then the old ones.
        QSet<QString> had;
        for (const QString& s : seen) had.insert(s);
        QStringList after;
        QSet<QString> put;
        for (const Child& c : current)
            if (!c.id.isEmpty() && !put.contains(c.id)) { after << c.id; put.insert(c.id); }
        for (const QString& s : seen)
            if (!s.isEmpty() && !put.contains(s)) { after << s; put.insert(s); }
        if (after.size() > kMaxSeenIds) after = after.mid(0, kMaxSeenIds);
        d.seenAfter = after;

        if (firstEver) return d;   // baseline only: what is already there is not news

        if (keyed)
        {
            for (const Child& c : current)
                if (!c.id.isEmpty() && !had.contains(c.id)) d.newChildren << c;
        }
        else
        {
            d.coarseChanged = !seenFingerprint.isEmpty() && seenFingerprint != d.fingerprintAfter;
        }
        return d;
    }

    // ---- The New shelf ------------------------------------------------------------------------------

    // How many rows the New shelf shows. #25's shelf cap (trakt::kMissedShelfMax, 8) is the precedent and the
    // reasoning is identical — a home strip you have to scroll has stopped being a glance — but this shelf
    // carries TWO producers' rows, so it is a little longer rather than making one of them invisible behind
    // the other. The full list lives behind the series itself.
    constexpr int kNewShelfMax = 12;

    // One row on the New shelf, from either producer. Deliberately not a MediaItem: the pure layer must stay
    // QtCore-only so probe_follow links without the addon models, and the two surfaces (classic list rows and
    // the themed column) both build their own row objects from this anyway.
    struct NewRow
    {
        QString id;         // the row's identity — dedupe key across producers
        QString seriesKey;  // the followed series this belongs to ("" for a #25 row)
        QString title;
        QString subtitle;
        QString thumbnailUrl;
        QString type;
        QString addonId;
        QString url;
        QString mime;
        qint64  foundAt = 0;  // when this row became new (newest first)
        int     count = 1;    // rows that stand for several children ("3 new episodes")
    };

    // WHAT TAKES A ROW OFF THE SHELF. The completion states that already exist, and nothing new: the issue
    // says "marking as read/watched clears it — reusing the completion states that already exist", and this is
    // the same rule #25's shelf lives under (TraktMissed.h: BOTH non-Unmarked states clear the row). Hidden
    // counts too, because every other surface in the app already drops a hidden item and a New shelf that did
    // not would be the one place a hidden item still shows up.
    //
    // Stated over two BOOLS rather than over ItemMarks::Marks so the pure layer keeps no dependency on the
    // marks store; the one caller passes `marks.hidden` and `marks.completion == None`.
    inline bool isDealtWith(bool hidden, bool completionIsNone) { return hidden || !completionIsNone; }

    // The pending children of ONE series as shelf rows, newest first, with the dealt-with ones dropped.
    // `dealtWith` is the caller's marks lookup (a probe passes a fixture set).
    template <typename Pending, typename DealtWithFn>
    QVector<NewRow> rowsForSeries(const QString& seriesKey, const QString& addonId,
                                  const QVector<Pending>& pending, DealtWithFn dealtWith)
    {
        QVector<NewRow> out;
        for (const Pending& p : pending)
        {
            if (p.id.isEmpty() || dealtWith(p.id)) continue;
            NewRow r;
            r.id = p.id; r.seriesKey = seriesKey; r.title = p.title; r.subtitle = p.subtitle;
            r.thumbnailUrl = p.thumbnailUrl; r.type = p.type; r.addonId = addonId;
            r.url = p.url; r.mime = p.mime; r.foundAt = p.foundAt; r.count = p.count;
            out << r;
        }
        std::stable_sort(out.begin(), out.end(),
                         [](const NewRow& a, const NewRow& b) { return a.foundAt > b.foundAt; });
        return out;
    }

    // The unread badge on a series tile: how many of its pending children are still undealt-with. Counted
    // through the SAME filter the shelf uses, so a badge can never disagree with the rows behind it.
    template <typename Pending, typename DealtWithFn>
    int unreadCount(const QVector<Pending>& pending, DealtWithFn dealtWith)
    {
        int n = 0;
        for (const Pending& p : pending)
            if (!p.id.isEmpty() && !dealtWith(p.id)) ++n;
        return n;
    }

    // The union of the two producers, newest first, deduplicated by id.
    //
    // #25 ("You missed") and this feature answer the same question from two directions — Trakt's calendar
    // says an episode of a show you follow THERE aired and you have not watched it; a follow says a source
    // grew a child you have not seen. Two shelves saying that would be two shelves, so they share one, and
    // the dedupe is by `id` because #25 keys its row on the episode's stream id and a followed series' child
    // carries the source's own item id: where those coincide (the same episode reached both ways) the row
    // must appear ONCE. First writer wins the row's content and the larger count survives, so a #25 row that
    // stands for three episodes does not lose its "3" to a single-child follow row.
    inline QVector<NewRow> mergeNewShelf(const QVector<NewRow>& followRows,
                                         const QVector<NewRow>& missedRows, int cap = kNewShelfMax)
    {
        QVector<NewRow> all;
        all.reserve(followRows.size() + missedRows.size());
        QSet<QString> seenId;
        auto ingest = [&](const QVector<NewRow>& src) {
            for (const NewRow& r : src)
            {
                if (r.id.isEmpty()) continue;
                bool merged = false;
                for (NewRow& have : all)
                    if (have.id == r.id) { have.count = qMax(have.count, r.count); merged = true; break; }
                if (merged) continue;
                seenId.insert(r.id);
                all << r;
            }
        };
        ingest(followRows);
        ingest(missedRows);
        // Newest first. std::stable_sort so rows stamped in the same second keep producer order (follow
        // rows before #25's), which is what makes the order reproducible in a probe that stamps everything
        // from one fake clock.
        std::stable_sort(all.begin(), all.end(),
                         [](const NewRow& a, const NewRow& b) { return a.foundAt > b.foundAt; });
        if (cap > 0 && all.size() > cap) all.resize(cap);
        return all;
    }
}
