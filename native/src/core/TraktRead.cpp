#include "TraktRead.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QTimeZone>

#include <cmath>
#include <limits>

namespace {

// Trakt sends `trakt`/`tvdb`/`tmdb` as numbers and `imdb`/`slug` as strings, and the reference marks
// `tvdb`/`imdb`/`tmdb` NULLABLE. Reading a number with toString() silently yields empty and loses the
// id, so normalise both shapes here; anything else (null, a missing key, or the nested `plex` object
// the reference also lists) falls through to an empty QString rather than a bogus one.
QString idString(const QJsonValue& v)
{
    if (v.isString()) return v.toString();
    if (v.isDouble())
    {
        // QJsonValue keeps every number as a double, so a hostile or corrupted body can hand us
        // one no qint64 can hold. Narrowing that is UNDEFINED — MSVC lands 1e300 on
        // -9223372036854775808 — and a bogus id is worse than none, because later joins would
        // happily match on it. Anything unrepresentable is no id at all.
        const double d = v.toDouble();
        constexpr double kMin = -9223372036854775808.0;   // exactly qint64 min
        constexpr double kMax =  9223372036854775808.0;   // one past qint64 max (max is not exact)
        if (!std::isfinite(d) || d < kMin || d >= kMax) return QString();
        return QString::number(qint64(d));
    }
    return QString();
}

TraktIds idsFrom(const QJsonObject& owner)
{
    const QJsonObject o = owner.value(QStringLiteral("ids")).toObject();
    TraktIds ids;
    ids.imdb  = idString(o.value(QStringLiteral("imdb")));
    ids.tmdb  = idString(o.value(QStringLiteral("tmdb")));
    ids.tvdb  = idString(o.value(QStringLiteral("tvdb")));
    ids.trakt = idString(o.value(QStringLiteral("trakt")));
    return ids;
}

// The ONE air-time reader, shared by the wire parser and the cache reader. Both face the same hazard —
// a string with no zone designator, which Qt reads as LOCAL time — and if only one of them applied the
// UTC guard, a cached entry would drift by the machine's offset relative to the fetch that wrote it.
// Returns an invalid QDateTime for anything unparseable; both callers drop such an entry.
QDateTime airTimeUtc(const QString& s)
{
    QDateTime dt = QDateTime::fromString(s, Qt::ISODateWithMs);
    if (!dt.isValid()) return QDateTime();
    if (dt.timeSpec() == Qt::LocalTime) dt.setTimeZone(QTimeZone::UTC);
    return dt.toUTC();
}

// The cache's own id shape. Deliberately NOT idsFrom(): that one is tolerant of Trakt's wire types
// (numbers for trakt/tvdb/tmdb, nulls anywhere) because it has to be. The cache is written by
// serializeCalendar and every id in it is already a normalised string, so reading one as anything else
// means the file is not ours — toString() yields "" and the id is simply absent, which is a shape the
// whole read layer already handles.
TraktIds cachedIds(const QJsonObject& o)
{
    TraktIds ids;
    ids.imdb  = o.value(QStringLiteral("imdb")).toString();
    ids.tmdb  = o.value(QStringLiteral("tmdb")).toString();
    ids.tvdb  = o.value(QStringLiteral("tvdb")).toString();
    ids.trakt = o.value(QStringLiteral("trakt")).toString();
    return ids;
}

QJsonObject idsJson(const TraktIds& ids)
{
    return { { QStringLiteral("imdb"),  ids.imdb },
             { QStringLiteral("tmdb"),  ids.tmdb },
             { QStringLiteral("tvdb"),  ids.tvdb },
             { QStringLiteral("trakt"), ids.trakt } };
}

constexpr int kCacheVersion = 1;

} // namespace

QVector<CalendarEntry> trakt::parseMyShowsCalendar(const QByteArray& json)
{
    QVector<CalendarEntry> out;
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isArray()) return out;                 // non-JSON, an object, or null -> empty

    for (const QJsonValue& v : doc.array())
    {
        if (!v.isObject()) continue;                // a bare string where an object belongs
        const QJsonObject o = v.toObject();

        // An entry with no usable air time is dropped: the calendar is ordered by it, and an entry
        // that cannot be placed on a date has nothing to say. The reference pins these as UTC ISO 8601
        // strings but does not pin the fractional-second part, so parse the with-ms form — Qt's ISO
        // parser treats the fraction as optional, which probe §6 holds it to.
        //
        // The zone handling lives in airTimeUtc(): Qt reads a string with NO zone designator as LOCAL
        // time, so converting it to UTC would shift it by this machine's offset — enough to move a
        // late-evening episode onto the wrong calendar day, differently in summer and winter. The
        // reference pins first_aired as nothing more than {"type":"string"}, so a dropped Z (or a
        // reformatting proxy) is a shape we have to survive; Trakt states the times ARE UTC, so that
        // is what a bare string means. An EXPLICIT offset, by contrast, is real information — Qt gives
        // it Qt::OffsetFromUTC, not Qt::LocalTime, so it is converted rather than stamped over. Probe §7.
        const QDateTime airs = airTimeUtc(o.value(QStringLiteral("first_aired")).toString());
        if (!airs.isValid()) continue;

        const QJsonObject show = o.value(QStringLiteral("show")).toObject();
        const QJsonObject ep   = o.value(QStringLiteral("episode")).toObject();

        CalendarEntry e;
        e.airsAtUtc    = airs;
        e.showTitle    = show.value(QStringLiteral("title")).toString();
        e.showIds      = idsFrom(show);
        e.season       = ep.value(QStringLiteral("season")).toInt(-1);
        e.episode      = ep.value(QStringLiteral("number")).toInt(-1);
        e.episodeTitle = ep.value(QStringLiteral("title")).toString();
        e.episodeIds   = idsFrom(ep);
        out.push_back(e);
    }
    return out;
}

QString trakt::imdbStreamIdFor(const TraktIds& showIds, int season, int episode)
{
    // No imdb id -> "", the "not playable" signal. Never fall back to tmdb/tvdb: nothing downstream
    // can resolve those, so a substituted id would produce an item that LOOKS playable and is not.
    //
    // The id itself gets the same scrutiny the season/episode numbers do, and for the same reason.
    // A ':' inside it would emit MORE than the three fields this format has (a "tt1:9:9" show id
    // yields "tt1:9:9:1:1"); a value that is not an IMDB title id at all — a bare number that
    // reached us as a JSON number, an "nm…" person id — yields a shapely id nothing can resolve.
    // Both look playable to the caller and fail only at play time, which is the outcome the guard
    // below already exists to prevent.
    const QString imdb = showIds.imdb.trimmed();
    if (!imdb.startsWith(QStringLiteral("tt"))) return QString();
    if (imdb.contains(QLatin1Char(':'))) return QString();
    if (season < 0 || episode < 1) return QString();   // season 0 is valid (specials); episodes are 1-based
    return imdb + QStringLiteral(":") + QString::number(season)
                + QStringLiteral(":") + QString::number(episode);
}

QByteArray trakt::serializeCalendar(const QVector<CalendarEntry>& entries)
{
    QJsonArray arr;
    for (const CalendarEntry& e : entries)
    {
        // The air time is written in the SAME form the wire uses (UTC ISO 8601 with milliseconds, so
        // the trailing Z is explicit) — which means the reader's zone guard never has to fire on our
        // own output, and a cache file is readable by eye against a raw Trakt response.
        arr.append(QJsonObject{
            { QStringLiteral("airsAtUtc"),    e.airsAtUtc.toUTC().toString(Qt::ISODateWithMs) },
            { QStringLiteral("showTitle"),    e.showTitle },
            { QStringLiteral("showIds"),      idsJson(e.showIds) },
            { QStringLiteral("season"),       e.season },
            { QStringLiteral("episode"),      e.episode },
            { QStringLiteral("episodeTitle"), e.episodeTitle },
            { QStringLiteral("episodeIds"),   idsJson(e.episodeIds) },
            { QStringLiteral("posterUrl"),    e.posterUrl },
        });
    }
    // Versioned so a future format change is a clean miss (deserializeCalendar returns empty and the
    // caller re-fetches) rather than a silent half-read of fields that no longer mean what they did.
    const QJsonObject doc{ { QStringLiteral("v"), kCacheVersion },
                           { QStringLiteral("entries"), arr } };
    return QJsonDocument(doc).toJson(QJsonDocument::Compact);
}

QVector<CalendarEntry> trakt::deserializeCalendar(const QByteArray& json)
{
    QVector<CalendarEntry> out;
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject()) return out;                          // empty, truncated, non-JSON, or a bare array
    const QJsonObject root = doc.object();
    // toInt(0) rather than toInt(kCacheVersion): a MISSING version must not be read as the current one.
    if (root.value(QStringLiteral("v")).toInt(0) != kCacheVersion) return out;
    const QJsonValue entriesVal = root.value(QStringLiteral("entries"));
    if (!entriesVal.isArray()) return out;

    for (const QJsonValue& v : entriesVal.toArray())
    {
        if (!v.isObject()) continue;
        const QJsonObject o = v.toObject();
        const QDateTime airs = airTimeUtc(o.value(QStringLiteral("airsAtUtc")).toString());
        if (!airs.isValid()) continue;   // same rule as the parser: an unplaceable entry is dropped

        CalendarEntry e;
        e.airsAtUtc    = airs;
        e.showTitle    = o.value(QStringLiteral("showTitle")).toString();
        e.showIds      = cachedIds(o.value(QStringLiteral("showIds")).toObject());
        // -1, not 0. The header pins -1 as the "missing" sentinel precisely because a 0 read back for a
        // missing season is indistinguishable from a real special, so the default here must BE that
        // sentinel or a truncated cache would resurrect entries as season-0 specials.
        e.season       = o.value(QStringLiteral("season")).toInt(-1);
        e.episode      = o.value(QStringLiteral("episode")).toInt(-1);
        e.episodeTitle = o.value(QStringLiteral("episodeTitle")).toString();
        e.episodeIds   = cachedIds(o.value(QStringLiteral("episodeIds")).toObject());
        e.posterUrl    = o.value(QStringLiteral("posterUrl")).toString();
        out.push_back(e);
    }
    return out;
}
