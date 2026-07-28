#include "TraktRead.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

namespace {

// Trakt sends `trakt`/`tvdb`/`tmdb` as numbers and `imdb`/`slug` as strings, and the reference marks
// `tvdb`/`imdb`/`tmdb` NULLABLE. Reading a number with toString() silently yields empty and loses the
// id, so normalise both shapes here; anything else (null, a missing key, or the nested `plex` object
// the reference also lists) falls through to an empty QString rather than a bogus one.
QString idString(const QJsonValue& v)
{
    if (v.isString()) return v.toString();
    if (v.isDouble()) return QString::number(qint64(v.toDouble()));
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
        const QDateTime airs =
            QDateTime::fromString(o.value(QStringLiteral("first_aired")).toString(), Qt::ISODateWithMs)
                .toUTC();
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
    if (showIds.imdb.isEmpty()) return QString();
    if (season < 0 || episode < 1) return QString();   // season 0 is valid (specials); episodes are 1-based
    return showIds.imdb + QStringLiteral(":") + QString::number(season)
                        + QStringLiteral(":") + QString::number(episode);
}
