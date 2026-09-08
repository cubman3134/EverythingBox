#include "Jellyseerr.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <algorithm>

namespace {

// Jellyseerr's (Overseerr's) media status integers, mapped onto our vocabulary. Named rather than inlined
// because the numbers appear in two shapes — the title's own `mediaInfo.status` and each season's
// `status` — and a second copy of the mapping is a second chance to disagree with the first.
requests::Availability availabilityForCode(int code)
{
    switch (code)
    {
    case 1:  return requests::Availability::NotRequested;       // UNKNOWN: nothing has been asked for
    case 2:  return requests::Availability::Pending;            // awaiting approval
    case 3:  return requests::Availability::Processing;         // approved and being fetched
    case 4:  return requests::Availability::PartiallyAvailable;
    case 5:  return requests::Availability::Available;
    default: return requests::Availability::Unknown;            // a code this build has never seen
    }
}

// A REQUEST's own status, which is a different enum from the media status above and is the only place a
// decline is reported. 1 PENDING, 2 APPROVED, 3 DECLINED, 4 FAILED.
requests::Availability availabilityForRequestCode(int code)
{
    switch (code)
    {
    case 1:  return requests::Availability::Pending;
    case 2:  return requests::Availability::Approved;
    case 3:  return requests::Availability::Declined;
    case 4:  return requests::Availability::Failed;
    default: return requests::Availability::Unknown;
    }
}

// How bad an answer is, for picking one when a title carries several. A DECLINE or a FAILURE outranks
// everything: it is the one state the user has to act on, and folding it under "pending" (which the media
// status would still say) hides the only thing on the page worth reading.
int severity(requests::Availability a)
{
    switch (a)
    {
    case requests::Availability::Declined:
    case requests::Availability::Failed:    return 3;
    case requests::Availability::Available: return 2;
    case requests::Availability::Unknown:   return 0;
    default:                                return 1;
    }
}

} // namespace

namespace jellyseerr {

UrlVerdict checkUrl(const QString& url, bool allowPlainHttp)
{
    const QString trimmed = url.trimmed();
    if (trimmed.isEmpty()) return UrlVerdict::Malformed;
    const QUrl u(trimmed, QUrl::StrictMode);
    if (!u.isValid() || u.host().isEmpty()) return UrlVerdict::Malformed;
    const QString scheme = u.scheme().toLower();
    if (scheme == QLatin1String("https")) return UrlVerdict::Ok;
    if (scheme != QLatin1String("http"))  return UrlVerdict::NotHttp;
    // Plain HTTP puts the API key on the wire in clear on every single call, so it is refused unless the
    // user has explicitly said otherwise — and that question is asked before the key is ever stored.
    return allowPlainHttp ? UrlVerdict::Ok : UrlVerdict::InsecureRefused;
}

QString normalizeRoot(const QString& url, bool allowPlainHttp)
{
    if (checkUrl(url, allowPlainHttp) != UrlVerdict::Ok) return QString();
    QString root = url.trimmed();
    while (root.endsWith(QLatin1Char('/'))) root.chop(1);
    return root;
}

QString mediaPath(const QString& mediaType, const QString& tmdbId)
{
    if (tmdbId.isEmpty()) return QString();
    const QString kind = (mediaType == requests::kMovie()) ? QStringLiteral("movie") : QStringLiteral("tv");
    return QStringLiteral("/api/v1/") + kind + QLatin1Char('/') + tmdbId;
}

QString searchPath()  { return QStringLiteral("/api/v1/search"); }
QString requestPath() { return QStringLiteral("/api/v1/request"); }
QString statusPath()  { return QStringLiteral("/api/v1/status"); }

MediaStatus readMediaStatus(const QByteArray& body, const QString& mediaType)
{
    MediaStatus out;
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(body, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return out;   // ok stays false
    const QJsonObject o = doc.object();
    // The envelope is identified by its own `id`. A body without one is not a title's details, however
    // well-formed it is as JSON — a proxy's JSON error page parses perfectly and means nothing.
    if (!o.contains(QStringLiteral("id"))) return out;
    out.ok = true;
    const QJsonValue idv = o.value(QStringLiteral("id"));
    out.tmdbId = idv.isDouble() ? QString::number(qint64(idv.toDouble())) : idv.toString();

    // NO mediaInfo AT ALL is the ordinary case for a title nobody has asked for. It is an ANSWER, not a
    // gap: the service knows this title and has nothing on it.
    const QJsonObject info = o.value(QStringLiteral("mediaInfo")).toObject();
    if (info.isEmpty())
    {
        out.availability = requests::Availability::NotRequested;
    }
    else
    {
        out.availability = availabilityForCode(info.value(QStringLiteral("status")).toInt());
        // Jellyfin's own id for the copy on the linked server, when there is one. Left UNQUALIFIED here;
        // requests::libraryRefFor decides whether it can be named at all.
        out.serverItemId = info.value(QStringLiteral("jellyfinMediaId")).toString();

        // A decline or a failure lives on the REQUEST, not on the media, and it outranks the media status.
        for (const QJsonValue& rv : info.value(QStringLiteral("requests")).toArray())
        {
            if (!rv.isObject()) continue;
            const requests::Availability a =
                availabilityForRequestCode(rv.toObject().value(QStringLiteral("status")).toInt());
            if (severity(a) > severity(out.availability)) out.availability = a;
        }
    }

    if (mediaType == requests::kTv())
    {
        // The SERIES' own season list says how many there are; specials (season 0) are excluded because
        // nobody requests them and counting them would offer a picker row that fetches nothing.
        for (const QJsonValue& sv : o.value(QStringLiteral("seasons")).toArray())
        {
            if (!sv.isObject()) continue;
            if (sv.toObject().value(QStringLiteral("seasonNumber")).toInt() > 0) ++out.seasonCount;
        }
        // ...and mediaInfo's season list says which of them are already there. "Already there" is
        // deliberately AVAILABLE only: a season being fetched is not one the user has, but it is also not
        // one they should be asked to request again, so the picker treats in-flight seasons separately.
        for (const QJsonValue& sv : info.value(QStringLiteral("seasons")).toArray())
        {
            if (!sv.isObject()) continue;
            const QJsonObject so = sv.toObject();
            const int n = so.value(QStringLiteral("seasonNumber")).toInt();
            if (n <= 0) continue;
            if (availabilityForCode(so.value(QStringLiteral("status")).toInt())
                    != requests::Availability::NotRequested)
                out.availableSeasons.push_back(n);
        }
        std::sort(out.availableSeasons.begin(), out.availableSeasons.end());
    }
    return out;
}

SearchHit readSearchHit(const QByteArray& body, const QString& wantType)
{
    SearchHit out;
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(body, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return out;
    const QJsonArray results = doc.object().value(QStringLiteral("results")).toArray();
    for (const QJsonValue& rv : results)
    {
        if (!rv.isObject()) continue;
        const QJsonObject r = rv.toObject();
        const QString mt = r.value(QStringLiteral("mediaType")).toString();
        // "person" is a real result type from this endpoint and is not a thing anybody can request; the
        // type gate drops it along with anything else that is not the shape we asked about.
        if (mt != QLatin1String("movie") && mt != QLatin1String("tv")) continue;
        const QString ours = (mt == QLatin1String("movie")) ? requests::kMovie() : requests::kTv();
        if (!wantType.isEmpty() && ours != wantType) continue;
        const QJsonValue idv = r.value(QStringLiteral("id"));
        const QString id = idv.isDouble() ? QString::number(qint64(idv.toDouble())) : idv.toString();
        if (id.isEmpty()) continue;
        out.ok = true;
        out.tmdbId = id;
        out.mediaType = ours;
        return out;
    }
    return out;
}

Ack readRequestAck(const QByteArray& body)
{
    Ack out;
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(body, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return out;   // ok stays false
    const QJsonObject o = doc.object();
    // The created request's own id is what makes this an acknowledgement rather than a 200 with an opinion
    // in it. Without one we do not claim the request exists.
    if (!o.contains(QStringLiteral("id"))) return out;
    out.ok = true;
    if (o.contains(QStringLiteral("status")))
    {
        const requests::Availability a = availabilityForRequestCode(o.value(QStringLiteral("status")).toInt());
        if (a != requests::Availability::Unknown) out.availability = a;
    }
    return out;
}

QByteArray requestBody(const QString& mediaType, const QString& tmdbId, const QVector<int>& seasons)
{
    bool numeric = false;
    const qint64 id = tmdbId.toLongLong(&numeric);
    if (!numeric || id <= 0) return QByteArray();   // "do not send" — see the header

    QJsonObject o;
    o.insert(QStringLiteral("mediaType"),
             (mediaType == requests::kMovie()) ? QStringLiteral("movie") : QStringLiteral("tv"));
    o.insert(QStringLiteral("mediaId"), double(id));
    if (mediaType == requests::kTv())
    {
        if (seasons.isEmpty())
        {
            o.insert(QStringLiteral("seasons"), QStringLiteral("all"));
        }
        else
        {
            QJsonArray arr;
            QVector<int> sorted = seasons;
            std::sort(sorted.begin(), sorted.end());
            sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
            for (int s : sorted) if (s > 0) arr.append(s);
            if (arr.isEmpty()) return QByteArray();   // a season list that names nothing sends nothing
            o.insert(QStringLiteral("seasons"), arr);
        }
    }
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

requests::Failure failureForHttp(int status, const QByteArray& body)
{
    if (status == 401) return requests::Failure::Unauthorized;
    if (status == 403) return requests::Failure::Forbidden;
    if (status == 404) return requests::Failure::NotFound;
    if (status == 409) return requests::Failure::Duplicate;
    if (status == 429) return requests::Failure::RateLimited;
    if (status >= 500)
    {
        // The one marker this file reads out of an error body, and it is read as a FLAG rather than
        // rendered: some builds answer a duplicate with a 500. Everything else about the body is dropped.
        const QByteArray lower = body.left(512).toLower();
        if (lower.contains("already exists")) return requests::Failure::Duplicate;
        return requests::Failure::ServerError;
    }
    if (status >= 400) return requests::Failure::Malformed;
    return requests::Failure::None;
}

} // namespace jellyseerr
