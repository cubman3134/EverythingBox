#include "TraktSync.h"

#include <QByteArray>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QSet>
#include <QTimeZone>

#include <algorithm>

namespace {

constexpr int kListCacheVersion = 1;

// A Trakt timestamp string -> unix seconds, or 0 for anything unparseable. Built on
// trakt::parseTraktTimeUtc so the "a bare string is UTC" guard is applied here too — see TraktRead.h
// for why that is not optional. 0 doubles as "absent", which every caller here treats the same way.
qint64 unixFrom(const QJsonValue& v)
{
    if (!v.isString()) return 0;
    const QDateTime dt = trakt::parseTraktTimeUtc(v.toString());
    if (!dt.isValid()) return 0;
    const qint64 s = dt.toSecsSinceEpoch();
    return s > 0 ? s : 0;   // a pre-1970 watch is not a thing, and 0 is already the absent sentinel
}

// Trakt sends ids as a bag under `ids`; TraktRead's idsFrom is file-private there, so the four fields
// are read here through the same tolerant rules (numbers for trakt/tvdb/tmdb, strings for imdb).
// Only `imdb` is load-bearing — it is the only one the app can key on — so the numeric-id tolerance
// that TraktRead needs for the calendar is not reproduced: a numeric imdb id is not an IMDB id, and
// usableImdb would reject it anyway.
TraktIds idsOf(const QJsonObject& owner)
{
    const QJsonObject o = owner.value(QStringLiteral("ids")).toObject();
    TraktIds ids;
    ids.imdb = o.value(QStringLiteral("imdb")).toString();
    ids.tmdb = o.value(QStringLiteral("tmdb")).isDouble()
                   ? QString::number(qint64(o.value(QStringLiteral("tmdb")).toDouble()))
                   : o.value(QStringLiteral("tmdb")).toString();
    ids.tvdb = o.value(QStringLiteral("tvdb")).isDouble()
                   ? QString::number(qint64(o.value(QStringLiteral("tvdb")).toDouble()))
                   : o.value(QStringLiteral("tvdb")).toString();
    ids.trakt = o.value(QStringLiteral("trakt")).isDouble()
                    ? QString::number(qint64(o.value(QStringLiteral("trakt")).toDouble()))
                    : o.value(QStringLiteral("trakt")).toString();
    return ids;
}

QJsonObject idsJson(const TraktIds& ids)
{
    return { { QStringLiteral("imdb"),  ids.imdb },
             { QStringLiteral("tmdb"),  ids.tmdb },
             { QStringLiteral("tvdb"),  ids.tvdb },
             { QStringLiteral("trakt"), ids.trakt } };
}

TraktIds idsFromCache(const QJsonObject& o)
{
    TraktIds ids;
    ids.imdb  = o.value(QStringLiteral("imdb")).toString();
    ids.tmdb  = o.value(QStringLiteral("tmdb")).toString();
    ids.tvdb  = o.value(QStringLiteral("tvdb")).toString();
    ids.trakt = o.value(QStringLiteral("trakt")).toString();
    return ids;
}

// Which of a row's nested objects this app can render, as a ("movie"|"show", the nested object) pair;
// an empty type means "skip this row".
//
// The two endpoints disagree about whether `type` is even present — /sync/watchlist stamps it on every
// row, /sync/collection/{movies,shows} does not — so BOTH readings are needed and their PRECEDENCE is
// the rule: an EXPLICIT type wins. That matters because a watchlist "episode" or "season" row carries a
// `show` object too, so inferring from the nested objects alone would silently promote "I want to watch
// season 2" into "the whole show is on my watchlist", under the show's own ids, indistinguishably from
// a real show row.
QString rowKind(const QJsonObject& row)
{
    const QString explicitType = row.value(QStringLiteral("type")).toString();
    if (!explicitType.isEmpty())
        return (explicitType == QStringLiteral("movie") || explicitType == QStringLiteral("show"))
                   ? explicitType : QString();
    if (row.value(QStringLiteral("movie")).isObject()) return QStringLiteral("movie");
    if (row.value(QStringLiteral("show")).isObject())  return QStringLiteral("show");
    return QString();
}

int clampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// A header value read as a non-negative int; -1 when absent or not one. Whitespace is tolerated
// because proxies add it; anything else (a float, a list, a token) is not a count.
int headerInt(const QMap<QString, QString>& headers, const QString& lowerName)
{
    const auto it = headers.constFind(lowerName);
    if (it == headers.constEnd()) return -1;
    bool ok = false;
    const int n = it.value().trimmed().toInt(&ok);
    return (ok && n >= 0) ? n : -1;
}

} // namespace

// ================================================================================================
// the lists
// ================================================================================================

QVector<TraktListEntry> trakt::parseListPayload(const QByteArray& json)
{
    QVector<TraktListEntry> out;
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isArray()) return out;

    for (const QJsonValue& v : doc.array())
    {
        if (!v.isObject()) continue;
        const QJsonObject row = v.toObject();

        const QString kind = rowKind(row);
        if (kind.isEmpty()) continue;              // a season/episode/person row, or a shapeless one
        // `type` may say "movie" with no `movie` object beside it; toObject() then yields an empty one and
        // every field below reads as absent. There is deliberately NO separate guard for that: a row with
        // no title and no id is already dropped by the admissibility test at the bottom, and an empty body
        // cannot produce either, so a guard here would be unreachable code with no behaviour to defend.
        const QJsonObject body = row.value(kind).toObject();

        TraktListEntry e;
        e.type  = kind;
        e.title = body.value(QStringLiteral("title")).toString();
        e.year  = body.value(QStringLiteral("year")).toInt(0);
        e.ids   = idsOf(body);
        // Whichever timestamp this endpoint uses. /sync/collection/shows uses `last_collected_at`, so
        // all three are read; the first one that parses wins, and none of them is required.
        e.addedAt = unixFrom(row.value(QStringLiteral("listed_at")));
        if (e.addedAt == 0) e.addedAt = unixFrom(row.value(QStringLiteral("collected_at")));
        if (e.addedAt == 0) e.addedAt = unixFrom(row.value(QStringLiteral("last_collected_at")));
        // A row with NEITHER a title NOR an imdb id is not something a surface can draw or a user can
        // recognise, so it is dropped rather than rendered as a blank tile.
        if (e.title.trimmed().isEmpty() && trakt::imdbMovieStreamIdFor(e.ids).isEmpty()) continue;
        out.push_back(e);
    }
    return out;
}

bool trakt::looksLikeListPayload(const QByteArray& json)
{
    // Array-ness, and nothing about the row count — see the header: an emptied watchlist is a real
    // answer that must be allowed to replace the cache.
    return QJsonDocument::fromJson(json).isArray();
}

QByteArray trakt::serializeList(const QVector<TraktListEntry>& entries)
{
    QJsonArray arr;
    for (const TraktListEntry& e : entries)
        arr.append(QJsonObject{
            { QStringLiteral("type"),    e.type },
            { QStringLiteral("title"),   e.title },
            { QStringLiteral("year"),    e.year },
            { QStringLiteral("ids"),     idsJson(e.ids) },
            { QStringLiteral("addedAt"), e.addedAt },
        });
    return QJsonDocument(QJsonObject{ { QStringLiteral("v"), kListCacheVersion },
                                      { QStringLiteral("entries"), arr } })
        .toJson(QJsonDocument::Compact);
}

QVector<TraktListEntry> trakt::deserializeList(const QByteArray& json)
{
    QVector<TraktListEntry> out;
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject()) return out;
    const QJsonObject root = doc.object();
    // toInt(0), never toInt(kListCacheVersion): a MISSING version must not read as the current one.
    if (root.value(QStringLiteral("v")).toInt(0) != kListCacheVersion) return out;
    const QJsonValue entriesVal = root.value(QStringLiteral("entries"));
    if (!entriesVal.isArray()) return out;

    for (const QJsonValue& v : entriesVal.toArray())
    {
        if (!v.isObject()) continue;
        const QJsonObject o = v.toObject();
        TraktListEntry e;
        e.type = o.value(QStringLiteral("type")).toString();
        // The SAME admissibility rule the wire parser applies, so a round trip is value-identical and a
        // hand-edited or truncated cache cannot smuggle in a row type no surface renders.
        if (e.type != QStringLiteral("movie") && e.type != QStringLiteral("show")) continue;
        e.title   = o.value(QStringLiteral("title")).toString();
        e.year    = o.value(QStringLiteral("year")).toInt(0);
        e.ids     = idsFromCache(o.value(QStringLiteral("ids")).toObject());
        e.addedAt = qint64(o.value(QStringLiteral("addedAt")).toDouble(0));
        out.push_back(e);
    }
    return out;
}

// ================================================================================================
// the watched history
// ================================================================================================

trakt::WatchedParse trakt::parseWatchedPayload(const QByteArray& json)
{
    WatchedParse out;
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isArray()) return out;

    for (const QJsonValue& v : doc.array())
    {
        if (!v.isObject()) continue;
        const QJsonObject row = v.toObject();

        // ---- a movie row: one mark ------------------------------------------------------------
        if (row.value(QStringLiteral("movie")).isObject())
        {
            const QJsonObject m = row.value(QStringLiteral("movie")).toObject();
            const QString id = trakt::imdbMovieStreamIdFor(idsOf(m));
            if (id.isEmpty()) { ++out.droppedNoKey; continue; }
            const qint64 ts = unixFrom(row.value(QStringLiteral("last_watched_at")));
            if (ts <= 0) { ++out.droppedNoTimestamp; continue; }
            out.marks.push_back(WatchedMark{ id, ts });
            continue;
        }

        // ---- a show row: one mark per listed episode --------------------------------------------
        if (!row.value(QStringLiteral("show")).isObject()) continue;   // neither shape; nothing to read
        const TraktIds showIds = idsOf(row.value(QStringLiteral("show")).toObject());
        const QJsonArray seasons = row.value(QStringLiteral("seasons")).toArray();
        for (const QJsonValue& sv : seasons)
        {
            if (!sv.isObject()) continue;
            const QJsonObject s = sv.toObject();
            // -1, not 0: season 0 is Trakt's specials season and is perfectly valid, so the "missing"
            // sentinel has to be a number the mapping rejects. imdbStreamIdFor rejects negatives.
            const int seasonNo = s.value(QStringLiteral("number")).toInt(-1);
            for (const QJsonValue& ev : s.value(QStringLiteral("episodes")).toArray())
            {
                if (!ev.isObject()) continue;
                const QJsonObject e = ev.toObject();
                const int epNo = e.value(QStringLiteral("number")).toInt(-1);
                const QString id = trakt::imdbStreamIdFor(showIds, seasonNo, epNo);
                if (id.isEmpty()) { ++out.droppedNoKey; continue; }
                // The EPISODE's own timestamp, and no fallback to the show-level one. See the header:
                // the show-level value advances whenever any episode is watched, so inheriting it would
                // re-clear the watermark for this episode on every later run and revert the user's
                // unmark for ever. Dropping costs one episode and is REPORTED.
                const qint64 ts = unixFrom(e.value(QStringLiteral("last_watched_at")));
                if (ts <= 0) { ++out.droppedNoTimestamp; continue; }
                out.marks.push_back(WatchedMark{ id, ts });
            }
        }
    }
    return out;
}

trakt::BackfillPlan trakt::planWatchedBackfill(const QVector<WatchedMark>& marks, qint64 watermark,
                                               const std::function<LocalState(const QString&)>& localState)
{
    BackfillPlan p;
    QSet<QString> eligibleSeen;

    for (const WatchedMark& m : marks)
    {
        // OBSERVED first, before any bucket decision. The watermark advances over everything this run
        // SAW, not everything it MARKED — that is the step the convergence argument in the header turns
        // on, so it happens before every `continue` below.
        if (m.lastWatchedAt > p.newWatermark) p.newWatermark = m.lastWatchedAt;

        if (m.streamId.isEmpty() || m.lastWatchedAt <= 0) { ++p.unusable; continue; }
        // STRICT >. With >=, the newest entry of the previous run is exactly equal to the watermark and
        // would be re-marked on every run for ever — reverting whatever the user did to it in between.
        if (m.lastWatchedAt <= watermark) { ++p.skippedByWatermark; continue; }
        if (eligibleSeen.contains(m.streamId)) { ++p.duplicates; continue; }
        eligibleSeen.insert(m.streamId);

        const LocalState st = localState ? localState(m.streamId) : LocalState::Unmarked;
        switch (st)
        {
        case LocalState::Watched:       ++p.alreadyWatched; break;   // no write => no sync churn
        case LocalState::OtherExplicit: ++p.keptLocal;      break;   // the user said something on purpose
        case LocalState::Unmarked:      p.toMark.push_back(m.streamId); break;
        }
    }
    return p;
}

// ================================================================================================
// where a run's progress is stored, and what it says afterwards
// ================================================================================================

QString trakt::backfillKeyPrefix() { return QStringLiteral("trakt/backfill/"); }

namespace {
// "default" for the no-profile-selected case, byte for byte the rule ItemMarks::profileGroup applies to
// the marks themselves — the cursor and the marks must agree about which bucket that is, or the very
// first run on a fresh install writes its marks in one place and its watermark in another.
QString profileSlot(const QString& profileId)
{
    return profileId.isEmpty() ? QStringLiteral("default") : profileId;
}
} // namespace

QString trakt::backfillThroughKey(const QString& profileId)
{ return backfillKeyPrefix() + profileSlot(profileId) + QStringLiteral("/through"); }

QString trakt::backfillDoneKey(const QString& profileId)
{ return backfillKeyPrefix() + profileSlot(profileId) + QStringLiteral("/done"); }

trakt::BackfillHeadline trakt::backfillHeadlineFor(bool complete, bool abandoned, int marked,
                                                   int skippedByWatermark, int alreadyKnown)
{
    // Abandoned first: it is the only case where the counters describe a plan that was never applied.
    if (abandoned)  return BackfillHeadline::Abandoned;
    if (!complete)  return BackfillHeadline::Incomplete;
    if (marked > 0) return BackfillHeadline::Marked;
    // The three ways to mark nothing, which the user must be able to tell apart. NothingNew is the one
    // that has an action attached (re-import); the other two do not, and offering it for them would be
    // advice to re-do work that would change nothing.
    if (skippedByWatermark > 0) return BackfillHeadline::NothingNew;
    if (alreadyKnown > 0)       return BackfillHeadline::AlreadyKnown;
    return BackfillHeadline::NothingToImport;
}

QString trakt::importStatusLine(qint64 watchlistAt, qint64 collectionAt,
                                bool everImported, qint64 importedThrough, qint64 nowUnix)
{
    // A date, not an age: "3 hours ago" needs a clock the caller would have to keep re-reading, and the
    // question a user actually has about a cache is "is this from before or after I changed something".
    // UTC throughout, like every other time this feature handles.
    const auto when = [nowUnix](qint64 stamp) {
        QString s = QDateTime::fromSecsSinceEpoch(stamp, QTimeZone::UTC).date().toString(Qt::ISODate);
        // A stamp in the future is a clock disagreement, not a fetch that has not happened; it is shown
        // rather than hidden, because silently normalising it is how a wrong clock stays invisible.
        if (stamp > nowUnix) s += QStringLiteral(" (?)");
        return s;
    };
    const QString wl = watchlistAt  > 0 ? when(watchlistAt)  : QObject::tr("never fetched");
    const QString co = collectionAt > 0 ? when(collectionAt) : QObject::tr("never fetched");
    const QString hi = !everImported     ? QObject::tr("never imported")
                     : importedThrough > 0 ? QObject::tr("imported through %1").arg(when(importedThrough))
                                           : QObject::tr("imported; nothing was dated");
    return QObject::tr("Watchlist: %1 · Collection: %2 · Watched history: %3").arg(wl, co, hi);
}

// ================================================================================================
// paging + failure
// ================================================================================================

trakt::PageInfo trakt::parsePageInfo(const QMap<QString, QString>& headers)
{
    PageInfo info;
    const int page  = headerInt(headers, QStringLiteral("x-pagination-page"));
    const int count = headerInt(headers, QStringLiteral("x-pagination-page-count"));
    const int items = headerInt(headers, QStringLiteral("x-pagination-item-count"));
    info.page      = page  < 0 ? 0  : page;
    info.pageCount = count < 0 ? 0  : count;
    info.itemCount = items;    // stays -1 when absent; it is reported, never used to steer the loop
    return info;
}

trakt::PageVerdict trakt::classifyPage(int httpStatus, const QMap<QString, QString>& headers,
                                       const QByteArray& body)
{
    PageVerdict v;

    // No HTTP response at all — a dropped connection, a DNS failure, a TLS error. Retryable, with no
    // server hint to honour.
    if (httpStatus <= 0) { v.outcome = PageOutcome::Retryable; return v; }

    if (httpStatus == 429)
    {
        v.outcome = PageOutcome::Retryable;
        const int hinted = headerInt(headers, QStringLiteral("retry-after"));
        // Clamped, not trusted. "Retry-After: 86400" from a proxy must not park a background import on
        // a day-long timer, and "0" must not spin the loop; both are honoured only within the bound.
        if (hinted > 0) v.retryAfterSec = clampInt(hinted, 1, kMaxBackoffSec);
        return v;
    }
    if (httpStatus == 401 || httpStatus == 403) { v.outcome = PageOutcome::AuthFailed; return v; }
    if (httpStatus >= 500 && httpStatus <= 599) { v.outcome = PageOutcome::Retryable; return v; }
    if (httpStatus >= 200 && httpStatus <= 299)
    {
        // The transport succeeded, so the ONLY remaining question is whether the body is the thing we
        // asked for. A captive portal answers 200 with HTML; so does a TLS-intercepting proxy's error
        // page. Malformed is deliberately NOT Retryable: asking again gets the same page back, and the
        // run must stop and say so rather than burn its attempts.
        v.outcome = looksLikeListPayload(body) ? PageOutcome::Ok : PageOutcome::Malformed;
        return v;
    }
    // Everything else — 3xx we did not follow, 4xx that is not an auth problem. A bad request stays bad.
    v.outcome = PageOutcome::Fatal;
    return v;
}

trakt::NextPage trakt::nextPageAfter(const PageInfo& info, int fetchedPage)
{
    // Nonsense in. NOT "done": nothing was established about how much of the list was read, and a
    // caller that treated it as done would cache whatever it happened to have as the whole thing.
    if (fetchedPage < 1) return NextPage{ PageStep::Unusable, 0 };
    // The last page, AND the no-pagination-headers case, in one test. pageCount is 0 when the headers
    // were absent, which for Trakt means the endpoint answered in one body — the /sync/watched
    // endpoints do exactly this — and `fetchedPage >= 0` then makes the run COMPLETE, not broken.
    //
    // There was a separate `if (info.pageCount <= 0) return 0;` above this line. It is gone because it
    // was unreachable-in-effect: every input it caught, this test catches identically, so no mutation
    // of it could change any behaviour. A guard nothing can distinguish from its absence is a guard
    // that documents a rule while defending nothing, and the rule is better stated here, once.
    if (fetchedPage >= info.pageCount) return NextPage{ PageStep::LastPage, 0 };
    // The outright bound, checked against the page WE fetched. A `page_count` of a billion — hostile,
    // or simply a bug at the other end — costs kMaxPages requests instead of an unbounded run that
    // rate-limits the account into the ground.
    //
    // Reached only when the test above did NOT fire, i.e. the server says there are more pages. So this
    // is a TRUNCATION and says so, rather than sharing an answer with the last page: the caller reports
    // the run incomplete, keeps the previous cache, and — for the backfill — leaves the watermark where
    // it was rather than advancing it over a tail nothing ever observed.
    if (fetchedPage >= kMaxPages) return NextPage{ PageStep::BoundHit, 0 };
    // fetchedPage, NOT info.page. A server echoing "1" on every page would otherwise hold the loop on
    // page 1 until the bound; one echoing a page ahead would skip real rows.
    return NextPage{ PageStep::Next, fetchedPage + 1 };
}

bool trakt::shouldRetryAttempt(int attempt)
{
    // 1-based: attempt 1 is the first try. So attempts 1..kMaxPageAttempts-1 may be retried, giving
    // kMaxPageAttempts tries in total.
    return attempt >= 1 && attempt < kMaxPageAttempts;
}

int trakt::backoffSecFor(int attempt, int retryAfterSec)
{
    // A server hint wins outright — it is the only party that knows when the window reopens — but only
    // inside the same bound classifyPage applies, because this is also reachable from a caller that did
    // not go through it.
    if (retryAfterSec > 0) return clampInt(retryAfterSec, 1, kMaxBackoffSec);
    // No `if (attempt < 1) attempt = 1;` here. It was, and it could not change any answer: the loop
    // below runs while `i < attempt` from i = 1, so attempt 0 and attempt -7 already skip it exactly as
    // attempt 1 does, and all three return kBaseBackoffSec. Removed on the same rule the dead guard in
    // nextPageAfter was removed on — a clamp that reads as protection while defending nothing. The
    // totality it was pretending to provide is real and is asserted directly instead (probe §21).
    qint64 s = kBaseBackoffSec;
    for (int i = 1; i < attempt && s < kMaxBackoffSec; ++i) s *= 2;
    return clampInt(int(std::min<qint64>(s, kMaxBackoffSec)), 1, kMaxBackoffSec);
}
