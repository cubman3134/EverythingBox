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
        QDateTime airs =
            QDateTime::fromString(o.value(QStringLiteral("first_aired")).toString(), Qt::ISODateWithMs);
        if (!airs.isValid()) continue;

        // Qt reads a string with NO zone designator as LOCAL time, so converting it to UTC would
        // shift it by this machine's offset — enough to move a late-evening episode onto the wrong
        // calendar day, differently in summer and winter. The reference pins first_aired as nothing
        // more than {"type":"string"}, so a dropped Z (or a reformatting proxy) is a shape we have
        // to survive; Trakt states the times ARE UTC, so that is what a bare string means. An
        // EXPLICIT offset, by contrast, is real information — Qt gives it Qt::OffsetFromUTC, not
        // Qt::LocalTime, so it falls past this and is converted properly below. Probe §7.
        if (airs.timeSpec() == Qt::LocalTime) airs.setTimeZone(QTimeZone::UTC);
        airs = airs.toUTC();

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
