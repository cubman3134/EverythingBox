#include "TrackerRules.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>
#include <QUrl>
#include <QUrlQuery>

using namespace tracker;

// ================= the AniList wire =====================================================================

QString anilist::authorizeUrl(const QString& authBase, const QString& clientId, const QString& redirectUri)
{
    QUrl u(authBase + QStringLiteral("/authorize"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("client_id"), clientId);
    q.addQueryItem(QStringLiteral("redirect_uri"), redirectUri);
    q.addQueryItem(QStringLiteral("response_type"), QStringLiteral("code"));
    u.setQuery(q);
    return u.toString();
}

QByteArray anilist::tokenExchangeBody(const QString& clientId, const QString& clientSecret,
                                      const QString& redirectUri, const QString& code)
{
    QJsonObject o;
    o.insert(QStringLiteral("grant_type"), QStringLiteral("authorization_code"));
    o.insert(QStringLiteral("client_id"), clientId);
    o.insert(QStringLiteral("client_secret"), clientSecret);
    o.insert(QStringLiteral("redirect_uri"), redirectUri);
    o.insert(QStringLiteral("code"), code);
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

QByteArray anilist::tokenRefreshBody(const QString& clientId, const QString& clientSecret,
                                     const QString& refreshToken)
{
    QJsonObject o;
    o.insert(QStringLiteral("grant_type"), QStringLiteral("refresh_token"));
    o.insert(QStringLiteral("client_id"), clientId);
    o.insert(QStringLiteral("client_secret"), clientSecret);
    o.insert(QStringLiteral("refresh_token"), refreshToken);
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

anilist::TokenReply anilist::parseTokenReply(const QByteArray& json)
{
    TokenReply r;
    const QJsonDocument d = QJsonDocument::fromJson(json);
    if (!d.isObject()) return r;          // HTML, an array, a truncated read — not a token reply
    const QJsonObject o = d.object();
    const QString access = o.value(QStringLiteral("access_token")).toString();
    // The ONE gate. An error object ({"error":"invalid_grant"}) parses as an object and would otherwise
    // return ok=true with empty strings, which a caller would then store OVER the live tokens.
    if (access.isEmpty()) return r;
    r.ok = true;
    r.accessToken = access;
    r.refreshToken = o.value(QStringLiteral("refresh_token")).toString();
    // AniList sends expires_in as a NUMBER; some proxies stringify it. Both read.
    const QJsonValue exp = o.value(QStringLiteral("expires_in"));
    r.expiresInSec = exp.isString() ? exp.toString().toLongLong() : static_cast<qint64>(exp.toDouble());
    return r;
}

// The GraphQL documents. Kept as one string each rather than assembled, so what goes on the wire is
// readable here and a probe can assert the variables rather than the whitespace.
static const char* kSearchQuery =
    "query ($search: String, $type: MediaType, $year: FuzzyDateInt) {"
    " Page(page: 1, perPage: 8) {"
    "  media(search: $search, type: $type, startDate_greater: $year, sort: SEARCH_MATCH) {"
    "   id title { romaji english } startDate { year } episodes chapters coverImage { large } } } }";

static const char* kEntryQuery =
    "query ($mediaId: Int) {"
    " Media(id: $mediaId) {"
    "  id episodes chapters"
    "  mediaListEntry { id progress status score(format: POINT_100) } } }";

static const char* kSaveMutation =
    "mutation ($mediaId: Int, $progress: Int, $status: MediaListStatus, $scoreRaw: Int) {"
    " SaveMediaListEntry(mediaId: $mediaId, progress: $progress, status: $status, scoreRaw: $scoreRaw) {"
    "  id progress status } }";

static QByteArray graphql(const char* query, const QJsonObject& vars)
{
    QJsonObject o;
    o.insert(QStringLiteral("query"), QLatin1String(query));
    o.insert(QStringLiteral("variables"), vars);
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

QByteArray anilist::searchBody(const QString& title, int year, Kind kind)
{
    QJsonObject v;
    v.insert(QStringLiteral("search"), title.trimmed());
    v.insert(QStringLiteral("type"), kind == Kind::Manga ? QStringLiteral("MANGA") : QStringLiteral("ANIME"));
    // OMITTED when unknown. AniList's FuzzyDateInt is yyyymmdd; "greater than the start of that year" is the
    // narrowing the issue asks for, and a 0 here would filter to nothing rather than to everything.
    if (year > 0) v.insert(QStringLiteral("year"), year * 10000);
    return graphql(kSearchQuery, v);
}

QVector<Match> anilist::parseSearch(const QByteArray& json)
{
    QVector<Match> out;
    const QJsonDocument d = QJsonDocument::fromJson(json);
    if (!d.isObject()) return out;
    // A GraphQL error reply is {"errors":[…]} with data null — handled by the value chain below coming back
    // empty, so no special case is needed and none can be forgotten.
    const QJsonArray media = d.object().value(QStringLiteral("data")).toObject()
                              .value(QStringLiteral("Page")).toObject()
                              .value(QStringLiteral("media")).toArray();
    for (const QJsonValue& mv : media)
    {
        const QJsonObject m = mv.toObject();
        const int id = m.value(QStringLiteral("id")).toInt();
        if (id <= 0) continue;                          // a row with no id names nothing linkable
        Match x;
        x.mediaId = QString::number(id);
        const QJsonObject t = m.value(QStringLiteral("title")).toObject();
        const QString eng = t.value(QStringLiteral("english")).toString();
        const QString rom = t.value(QStringLiteral("romaji")).toString();
        x.title = !eng.isEmpty() ? eng : rom;
        x.altTitle = (!eng.isEmpty() && !rom.isEmpty() && eng != rom) ? rom : QString();
        if (x.title.isEmpty()) continue;                // nor does one with no title
        x.year = m.value(QStringLiteral("startDate")).toObject().value(QStringLiteral("year")).toInt();
        // Which count is present IS the media type, and it is the count the COMPLETED rule needs. A row
        // carrying `chapters` is manga; one carrying `episodes` is anime.
        const int eps = m.value(QStringLiteral("episodes")).toInt();
        const int chs = m.value(QStringLiteral("chapters")).toInt();
        x.kind = (chs > 0 && eps <= 0) ? Kind::Manga : Kind::Anime;
        x.totalUnits = (x.kind == Kind::Manga) ? chs : eps;
        x.coverUrl = m.value(QStringLiteral("coverImage")).toObject().value(QStringLiteral("large")).toString();
        out.push_back(x);
    }
    return out;
}

QByteArray anilist::entryBody(const QString& mediaId)
{
    QJsonObject v;
    v.insert(QStringLiteral("mediaId"), mediaId.toInt());
    return graphql(kEntryQuery, v);
}

bool anilist::parseEntry(const QByteArray& json, const QString& mediaId, Entry& out)
{
    const QJsonDocument d = QJsonDocument::fromJson(json);
    if (!d.isObject()) return false;
    const QJsonObject data = d.object().value(QStringLiteral("data")).toObject();
    if (!data.contains(QStringLiteral("Media"))) return false;   // not an entry reply at all
    const QJsonValue mv = data.value(QStringLiteral("Media"));
    if (!mv.isObject()) return false;
    const QJsonObject m = mv.toObject();
    out = Entry{};
    out.mediaId = mediaId;
    const int eps = m.value(QStringLiteral("episodes")).toInt();
    const int chs = m.value(QStringLiteral("chapters")).toInt();
    out.totalUnits = eps > 0 ? eps : chs;
    const QJsonValue ev = m.value(QStringLiteral("mediaListEntry"));
    if (!ev.isObject()) return true;    // asked, answered: the account has no row for this media
    const QJsonObject e = ev.toObject();
    out.exists = true;
    out.progress = e.value(QStringLiteral("progress")).toInt();
    out.status = statusFromToken(e.value(QStringLiteral("status")).toString());
    out.score = e.value(QStringLiteral("score")).toInt();
    return true;
}

QByteArray anilist::saveBody(const Update& u, int totalUnits)
{
    QJsonObject v;
    v.insert(QStringLiteral("mediaId"), u.mediaId.toInt());
    // Never negative, and a completion event is at least unit 1: a 0 here would tell the account you have
    // read nothing, which is a regression dressed as an update.
    v.insert(QStringLiteral("progress"), qMax(1, u.unit));
    // COMPLETED needs BOTH the caller's claim and the tracker's own count to agree, when the tracker has a
    // count. A provider listing that is missing the final chapters would otherwise mark a running series
    // finished — the one push that cannot be undone by simply pushing again.
    const bool completed = u.completes && (totalUnits <= 0 || u.unit >= totalUnits);
    v.insert(QStringLiteral("status"), completed ? QStringLiteral("COMPLETED") : QStringLiteral("CURRENT"));
    // ABSENT unless the app really has a rating. AniList reads scoreRaw 0 as "rated zero", so sending it
    // unconditionally would erase a score the user set on their own list.
    if (u.hasScore) v.insert(QStringLiteral("scoreRaw"), qBound(0, u.score, 100));
    return graphql(kSaveMutation, v);
}

QString anilist::statusToken(Status s)
{
    switch (s)
    {
        case Status::Current:   return QStringLiteral("CURRENT");
        case Status::Planning:  return QStringLiteral("PLANNING");
        case Status::Completed: return QStringLiteral("COMPLETED");
        case Status::Dropped:   return QStringLiteral("DROPPED");
        case Status::Paused:    return QStringLiteral("PAUSED");
        case Status::Repeating: return QStringLiteral("REPEATING");
    }
    return QStringLiteral("CURRENT");
}

Status anilist::statusFromToken(const QString& token)
{
    const QString t = token.toUpper();
    if (t == QLatin1String("PLANNING"))  return Status::Planning;
    if (t == QLatin1String("COMPLETED")) return Status::Completed;
    if (t == QLatin1String("DROPPED"))   return Status::Dropped;
    if (t == QLatin1String("PAUSED"))    return Status::Paused;
    if (t == QLatin1String("REPEATING")) return Status::Repeating;
    return Status::Current;
}

// ================= the push machinery ===================================================================

bool tracker::debounceAllows(qint64 lastSentMs, qint64 nowMs)
{
    if (lastSentMs <= 0) return true;        // nothing sent yet — never delay the first push
    if (nowMs < lastSentMs) return true;     // the clock moved backwards; do not suspend pushing for hours
    return nowMs - lastSentMs >= kDebounceMs;
}

// Is `b` further along than `a`? Unit first; at the same unit, the one that COMPLETES wins, because it
// carries a status transition the other does not.
static bool furtherThan(const Update& b, const Update& a)
{
    if (b.unit != a.unit) return b.unit > a.unit;
    return b.completes && !a.completes;
}

bool tracker::coalesce(QVector<Update>& q, const Update& u)
{
    if (u.mediaId.isEmpty() || u.itemKey.isEmpty()) return false;  // no link, nothing to push
    for (int i = 0; i < q.size(); ++i)
    {
        if (q[i].itemKey != u.itemKey || q[i].mediaId != u.mediaId) continue;
        if (!furtherThan(u, q[i])) return false;    // an earlier chapter arriving late changes nothing
        // The FURTHEST wins, and it inherits nothing from the row it replaces: a rating cleared between the
        // two events must not be resurrected by the one still sitting in the queue.
        q[i] = u;
        return true;
    }
    q.push_back(u);
    return true;
}

int tracker::applyQueueCap(QVector<Update>& q)
{
    if (q.size() <= kMaxQueued) return 0;
    const int drop = q.size() - kMaxQueued;
    q.erase(q.begin(), q.begin() + drop);   // from the FRONT: the newest progress is what still matters
    return drop;
}

QByteArray tracker::encodeQueue(const QVector<Update>& q)
{
    QJsonArray a;
    for (const Update& u : q)
    {
        QJsonObject o;
        o.insert(QStringLiteral("key"), u.itemKey);
        o.insert(QStringLiteral("media"), u.mediaId);
        o.insert(QStringLiteral("kind"), kindToken(u.kind));
        o.insert(QStringLiteral("unit"), u.unit);
        o.insert(QStringLiteral("done"), u.completes);
        // The rating rides as a PRESENCE, not as a number with a sentinel: "no score" and "score 0" are
        // different pushes and a -1 convention on disk would be one edit away from being sent as a score.
        if (u.hasScore) o.insert(QStringLiteral("score"), u.score);
        o.insert(QStringLiteral("at"), double(u.atMs));
        a.push_back(o);
    }
    return QJsonDocument(a).toJson(QJsonDocument::Compact);
}

QVector<Update> tracker::decodeQueue(const QByteArray& json)
{
    QVector<Update> out;
    const QJsonDocument d = QJsonDocument::fromJson(json);
    if (!d.isArray()) return out;
    for (const QJsonValue& rv : d.array())
    {
        if (!rv.isObject()) continue;
        const QJsonObject o = rv.toObject();
        Update u;
        u.itemKey = o.value(QStringLiteral("key")).toString();
        u.mediaId = o.value(QStringLiteral("media")).toString();
        if (u.itemKey.isEmpty() || u.mediaId.isEmpty()) continue;  // unpushable; drop the row, keep the rest
        u.kind = o.value(QStringLiteral("kind")).toString() == QLatin1String("manga") ? Kind::Manga
                                                                                     : Kind::Anime;
        u.unit = o.value(QStringLiteral("unit")).toInt();
        u.completes = o.value(QStringLiteral("done")).toBool();
        u.hasScore = o.contains(QStringLiteral("score"));
        u.score = o.value(QStringLiteral("score")).toInt();
        u.atMs = static_cast<qint64>(o.value(QStringLiteral("at")).toDouble());
        out.push_back(u);
    }
    return out;
}

// ================= the pull machinery ===================================================================

Reconcile tracker::reconcile(int localUnits, int remoteUnits)
{
    const int l = qMax(0, localUnits);
    const int r = qMax(0, remoteUnits);
    if (r > l) return Reconcile::AdvanceLocal;
    if (l > r) return Reconcile::PushRemote;
    return Reconcile::Nothing;
}

// ================= identity helpers =====================================================================

int tracker::chapterNumberFromTitle(const QString& title, int fallback)
{
    if (title.isEmpty()) return fallback;
    // A "ch"-marked number FIRST, so "Vol. 2 · Ch. 14" is 14 rather than 2. Case-insensitive, and tolerant
    // of the separators providers actually use ("Ch.14", "Ch 14", "Chapter 14", "c14").
    static const QRegularExpression chRe(
        QStringLiteral("(?:^|[^a-z])c(?:h|hapter|hap)?\\s*\\.?\\s*(\\d+(?:\\.\\d+)?)"),
        QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatch m = chRe.match(title);
    if (m.hasMatch()) return int(m.captured(1).toDouble());
    // Otherwise the FIRST bare number in the title ("007" -> 7). Not the last: a title like "Chapter 5 of
    // 200" would otherwise report 200.
    static const QRegularExpression numRe(QStringLiteral("(\\d+(?:\\.\\d+)?)"));
    m = numRe.match(title);
    if (m.hasMatch()) return int(m.captured(1).toDouble());
    return fallback;
}

int tracker::episodeFromStreamId(const QString& streamId)
{
    const QStringList parts = streamId.split(QLatin1Char(':'));
    if (parts.size() < 3) return 0;             // a movie id ("tt123") names no episode
    bool ok = false;
    const int ep = parts.at(2).toInt(&ok);
    return (ok && ep > 0) ? ep : 0;
}

QString tracker::seriesFromStreamId(const QString& streamId)
{
    const QStringList parts = streamId.split(QLatin1Char(':'));
    if (parts.size() < 3) return QString();
    return parts.at(0);
}

QString tracker::itemKeyFor(const QString& imdbStreamId, const QString& title)
{
    const QString series = seriesFromStreamId(imdbStreamId);
    if (!series.isEmpty()) return series;               // "ttShow:2:7" -> "ttShow"
    if (!imdbStreamId.isEmpty()) return imdbStreamId;   // a bare id with no episode part
    const QString t = title.trimmed().toLower();
    // Prefixed so a title can never be mistaken for an id, and so the fallback family is visible in the ini
    // rather than looking like a stray key.
    return t.isEmpty() ? QString() : QStringLiteral("title:") + t;
}

// ---- state keys ----------------------------------------------------------------------------------------

QString tracker::profileSlot(const QString& profileId)
{
    return profileId.isEmpty() ? QStringLiteral("default") : profileId;
}

QString tracker::queueKey(const QString& profileId, Id id)
{
    return stateKeyPrefix() + profileSlot(profileId) + QLatin1Char('/') + idToken(id)
         + QStringLiteral("/queue");
}

QString tracker::lastErrorKey(const QString& profileId, Id id)
{
    return stateKeyPrefix() + profileSlot(profileId) + QLatin1Char('/') + idToken(id)
         + QStringLiteral("/lastError");
}

QString tracker::lastSentKey(const QString& profileId, Id id, const QString& itemKey)
{
    // Hashed for the reason every per-item ini key is: an item key is a url or a title and can hold '/',
    // '=' and '[', each of which means something to the ini format.
    const QString h = QString::fromLatin1(
        QCryptographicHash::hash(itemKey.toUtf8(), QCryptographicHash::Md5).toHex().left(10));
    return stateKeyPrefix() + profileSlot(profileId) + QLatin1Char('/') + idToken(id)
         + QStringLiteral("/sent/") + h;
}
