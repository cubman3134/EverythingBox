#include "TrackerRules.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>

using namespace tracker;

// ================= the one send policy (issue #326) ======================================================

// THE WHOLE OF WHAT A FAILED PUSH MEANS, for every tracker. This is increment 2's mal::backoffFor body,
// MOVED rather than rewritten: the doubling, the clamp on the exponent, the "Retry-After only wins upward"
// rule and the retry-by-default fallthrough are the same lines, with the four status FAMILIES lifted out
// into the policy the caller hands in. mal::backoffFor is now a forwarder onto this, so every increment-2
// assertion about MAL's numbers is an assertion about these ones.
tracker::SendVerdict tracker::classifySend(const SendPolicy& p, int httpStatus, qint64 retryAfterSec,
                                           int consecutiveFailures)
{
    SendVerdict v;
    const int n = qMax(1, consecutiveFailures);
    const qint64 baseMs = p.baseMs > 0 ? p.baseMs : 1;
    const qint64 maxMs  = qMax(baseMs, p.maxMs);
    // Doubling from the base, capped. The shift is CLAMPED before it is applied: a long outage would
    // otherwise run the exponent past 63 and produce a negative delay, which qBound would then read as
    // "wait the minimum" — a tight retry loop arrived at by arithmetic.
    const qint64 grown = baseMs << qMin(n - 1, 20);
    const qint64 base = qBound<qint64>(baseMs, grown, maxMs);
    const qint64 asked = qMax<qint64>(0, retryAfterSec) * 1000;

    if (httpStatus >= 200 && httpStatus < 300) return v;   // not a failure; nothing to decide

    // The three named families are DISJOINT by construction, so the order they are asked in cannot matter.
    if (p.permanent.contains(httpStatus))
    {
        // Never acceptable by waiting, so the row is dropped rather than left to wedge the head of the
        // queue for ever — every later chapter behind it would be lost too.
        v.permanent = true;
        return v;
    }
    if (p.reauth.contains(httpStatus))
    {
        // The TOKEN, not the request. Refresh and try again; the row stays queued. Deliberately NOT
        // lengthened by a Retry-After: the wait here is ours, and the header is about the rate limit.
        v.retry = true; v.reauth = true; v.delayMs = base;
        return v;
    }
    if (p.throttle.contains(httpStatus))
    {
        // Rate limited. The service's own Retry-After wins whenever it asks for LONGER than we would wait;
        // a header asking for less is not honoured downward, because the base is there to protect the
        // account rather than to be the smallest legal wait.
        v.retry = true; v.delayMs = qMax(base, asked);
        return v;
    }
    // EVERYTHING ELSE IS RETRYABLE, and that is the safe default rather than an oversight: 5xx and 0 ("no
    // reply at all") are a waiting problem, and a 4xx nobody has thought about must not cost somebody
    // their queue. A status a provider really will never accept is named in its policy, not guessed here.
    v.retry = true;
    v.delayMs = qMax(base, asked);
    return v;
}

// ================= the OAuth loopback callback (issue #326) ==============================================

tracker::LoopbackCallback tracker::parseLoopbackRequest(const QByteArray& httpRequest)
{
    LoopbackCallback out;
    // The REQUEST LINE only. Everything we want is in the target, and reading no further means a header a
    // browser invented can never reach the query parser.
    const QByteArray line = httpRequest.left(httpRequest.indexOf('\r'));
    const int sp1 = line.indexOf(' ');
    const int sp2 = line.indexOf(' ', sp1 + 1);
    if (sp1 < 0 || sp2 <= sp1) return out;   // not a request line; nothing to read out of it
    const QString target = QString::fromUtf8(line.mid(sp1 + 1, sp2 - sp1 - 1));
    const QUrlQuery q(QUrl::fromEncoded(("http://localhost" + target.toUtf8())).query());
    out.error = q.queryItemValue(QStringLiteral("error"));
    out.code  = q.queryItemValue(QStringLiteral("code"));
    out.state = q.queryItemValue(QStringLiteral("state"));
    return out;
}

qint64 tracker::retryAfterSeconds(const QByteArray& headerValue)
{
    bool ok = false;
    const qint64 secs = QString::fromLatin1(headerValue).trimmed().toLongLong(&ok);
    return (ok && secs > 0) ? secs : 0;
}

QByteArray tracker::loopbackResponse(bool signedIn)
{
    const QByteArray page = signedIn
        ? QByteArray("Signed in. You can close this tab and go back to the app.")
        : QByteArray("Sign-in was not completed. You can close this tab.");
    return "HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\n"
           "Cache-Control: no-store\r\nReferrer-Policy: no-referrer\r\nConnection: close\r\n"
           "Content-Length: " + QByteArray::number(page.size()) + "\r\n\r\n" + page;
}

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

// ================= the MyAnimeList wire ==================================================================
//
// Written from MyAnimeList's published API v2 reference. NOTHING IN THIS WORK CONTACTED MYANIMELIST: no
// account was created, no API client was registered, and every byte the probe and the live drive saw came
// from a fixture server stood up locally.

// One form field, percent-encoded by hand rather than through QUrlQuery. QUrlQuery leaves '+' alone, and a
// '+' inside an authorization code or a refresh token decodes on the far side as a SPACE — which presents as
// "invalid_grant" on a credential that is perfectly good.
static QByteArray formField(const char* key, const QString& value)
{
    return QByteArray(key) + '=' + QUrl::toPercentEncoding(value);
}

static QByteArray joinForm(const QVector<QByteArray>& fields)
{
    QByteArray out;
    for (const QByteArray& f : fields)
    {
        if (!out.isEmpty()) out += '&';
        out += f;
    }
    return out;
}

// RFC 7636's unreserved set, which is exactly what MAL accepts in a code verifier.
static const char kVerifierAlphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";

bool mal::isValidCodeVerifier(const QString& v)
{
    if (v.size() < kVerifierMinChars || v.size() > kVerifierMaxChars) return false;
    for (const QChar c : v)
    {
        const char16_t u = c.unicode();
        const bool unreserved = (u >= u'A' && u <= u'Z') || (u >= u'a' && u <= u'z')
                             || (u >= u'0' && u <= u'9')
                             || u == u'-' || u == u'.' || u == u'_' || u == u'~';
        if (!unreserved) return false;
    }
    return true;
}

QString mal::makeCodeVerifier()
{
    // 64 characters: comfortably inside 43..128, and 64 draws from a 66-symbol alphabet is ~387 bits, well
    // past what this has to resist. The SYSTEM generator, not the default one — this is the only secret in
    // the sign-in and a seeded PRNG would make it predictable.
    const int alphabetSize = int(sizeof(kVerifierAlphabet)) - 1;
    QString out;
    out.reserve(64);
    for (int i = 0; i < 64; ++i)
        out.append(QLatin1Char(kVerifierAlphabet[QRandomGenerator::system()->bounded(alphabetSize)]));
    return out;
}

QString mal::authorizeUrl(const QString& authBase, const QString& clientId, const QString& redirectUri,
                          const QString& codeVerifier, const QString& state)
{
    QUrl u(authBase + QStringLiteral("/authorize"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("response_type"), QStringLiteral("code"));
    q.addQueryItem(QStringLiteral("client_id"), clientId);
    // PLAIN is the only method MAL accepts, so the challenge IS the verifier. It is in the browser URL by
    // construction; that is what PKCE-plain is, and it is why the verifier is single-use, per-attempt and
    // never written to disk.
    q.addQueryItem(QStringLiteral("code_challenge"), codeVerifier);
    q.addQueryItem(QStringLiteral("code_challenge_method"), QStringLiteral("plain"));
    if (!redirectUri.isEmpty()) q.addQueryItem(QStringLiteral("redirect_uri"), redirectUri);
    // CSRF. Compared on the way back; a callback that does not carry it back is refused, because anything
    // able to reach the loopback listener could otherwise feed us an authorization code of its choosing.
    if (!state.isEmpty()) q.addQueryItem(QStringLiteral("state"), state);
    u.setQuery(q);
    return u.toString();
}

QByteArray mal::tokenExchangeBody(const QString& clientId, const QString& clientSecret,
                                  const QString& redirectUri, const QString& code,
                                  const QString& codeVerifier)
{
    QVector<QByteArray> f;
    f << formField("client_id", clientId);
    // MAL issues both confidential clients (with a secret) and public ones (without). An empty secret is
    // OMITTED rather than sent empty: a present-but-blank client_secret is a different request to MAL than
    // an absent one, and it is the one it refuses.
    if (!clientSecret.isEmpty()) f << formField("client_secret", clientSecret);
    f << formField("grant_type", QStringLiteral("authorization_code"));
    f << formField("code", code);
    f << formField("code_verifier", codeVerifier);
    if (!redirectUri.isEmpty()) f << formField("redirect_uri", redirectUri);
    return joinForm(f);
}

QByteArray mal::tokenRefreshBody(const QString& clientId, const QString& clientSecret,
                                 const QString& refreshToken)
{
    QVector<QByteArray> f;
    f << formField("client_id", clientId);
    if (!clientSecret.isEmpty()) f << formField("client_secret", clientSecret);
    f << formField("grant_type", QStringLiteral("refresh_token"));
    f << formField("refresh_token", refreshToken);
    return joinForm(f);
}

mal::TokenReply mal::parseTokenReply(const QByteArray& json)
{
    TokenReply r;
    const QJsonDocument d = QJsonDocument::fromJson(json);
    if (!d.isObject()) return r;
    const QJsonObject o = d.object();
    const QString access = o.value(QStringLiteral("access_token")).toString();
    // THE ONE GATE. {"error":"invalid_request","message":"…"} parses as an object and would otherwise be
    // stored over the live tokens, unlinking the account permanently on a transient failure.
    if (access.isEmpty()) return r;
    r.ok = true;
    r.accessToken = access;
    r.refreshToken = o.value(QStringLiteral("refresh_token")).toString();
    const QJsonValue exp = o.value(QStringLiteral("expires_in"));
    r.expiresInSec = exp.isString() ? exp.toString().toLongLong() : static_cast<qint64>(exp.toDouble());
    return r;
}

bool mal::searchable(const QString& title)
{
    return title.trimmed().size() >= kMinQueryChars;
}

// The path segment and the field list for one kind, spelled once each so a search, a read and a write
// cannot disagree about what an anime is.
static QString malSegment(Kind kind)
{
    return kind == Kind::Manga ? QStringLiteral("manga") : QStringLiteral("anime");
}

static QString malCountField(Kind kind)
{
    return kind == Kind::Manga ? QStringLiteral("num_chapters") : QStringLiteral("num_episodes");
}

// Pre-encoded, because these URLs are read back with QUrl::FullyEncoded: a raw space in a title would
// otherwise reach the request as a raw space and truncate the query at the first word.
static QString pctEnc(const QString& s)
{
    return QString::fromLatin1(QUrl::toPercentEncoding(s));
}

QString mal::searchUrl(const QString& apiBase, const QString& title, Kind kind, int limit)
{
    if (!searchable(title)) return QString();   // shorter than MAL will answer — see kMinQueryChars
    QUrl u(apiBase + QLatin1Char('/') + malSegment(kind));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("q"), pctEnc(title.trimmed()));
    q.addQueryItem(QStringLiteral("limit"), QString::number(qBound(1, limit, 100)));
    // MAL returns ONLY the fields asked for. A missing count here is a missing COMPLETED rule later, so the
    // count for this kind is always requested.
    q.addQueryItem(QStringLiteral("fields"),
                   pctEnc(QStringLiteral("alternative_titles,start_date,main_picture,")
                          + malCountField(kind)));
    u.setQuery(q);
    return u.toString(QUrl::FullyEncoded);
}

QVector<Match> mal::parseSearch(const QByteArray& json, Kind asked)
{
    QVector<Match> out;
    const QJsonDocument d = QJsonDocument::fromJson(json);
    if (!d.isObject()) return out;
    // An error envelope has no `data` array, so the chain below simply comes back empty and no special case
    // can be forgotten.
    const QJsonArray data = d.object().value(QStringLiteral("data")).toArray();
    for (const QJsonValue& rv : data)
    {
        const QJsonObject node = rv.toObject().value(QStringLiteral("node")).toObject();
        const int id = node.value(QStringLiteral("id")).toInt();
        if (id <= 0) continue;                 // a row with no id names nothing linkable
        Match x;
        x.mediaId = QString::number(id);
        // MAL's `title` is the official (usually romaji) one; the English name, when it has one, is under
        // alternative_titles.en. Preferred the same way round AniList's are, so the picker reads the same
        // on both trackers.
        const QString main = node.value(QStringLiteral("title")).toString();
        const QString en = node.value(QStringLiteral("alternative_titles")).toObject()
                               .value(QStringLiteral("en")).toString();
        x.title = !en.isEmpty() ? en : main;
        x.altTitle = (!en.isEmpty() && !main.isEmpty() && en != main) ? main : QString();
        if (x.title.isEmpty()) continue;       // nor does one with no title
        // "2016-04-03", or sometimes just "2016". The leading four digits either way; a row with no
        // start_date reads 0, which is what Match::year means by "the tracker gave no year".
        x.year = node.value(QStringLiteral("start_date")).toString().left(4).toInt();
        const int eps = node.value(QStringLiteral("num_episodes")).toInt();
        const int chs = node.value(QStringLiteral("num_chapters")).toInt();
        // WHICH COUNT the row carries is the media type, exactly as it is on AniList. A row carrying
        // neither (an unaired series MAL has no count for) is filed under what the caller ASKED for rather
        // than guessed at, because the search endpoint itself is per-kind.
        x.kind = (chs > 0 && eps <= 0) ? Kind::Manga : (eps > 0 ? Kind::Anime : asked);
        x.totalUnits = (x.kind == Kind::Manga) ? chs : eps;
        const QJsonObject pic = node.value(QStringLiteral("main_picture")).toObject();
        x.coverUrl = pic.value(QStringLiteral("large")).toString();
        if (x.coverUrl.isEmpty()) x.coverUrl = pic.value(QStringLiteral("medium")).toString();
        out.push_back(x);
    }
    return out;
}

QString mal::nextPageUrl(const QByteArray& json, const QString& apiBase)
{
    const QJsonDocument d = QJsonDocument::fromJson(json);
    if (!d.isObject()) return QString();
    const QString next = d.object().value(QStringLiteral("paging")).toObject()
                          .value(QStringLiteral("next")).toString();
    if (next.isEmpty()) return QString();
    const QUrl n(next);
    const QUrl base(apiBase);
    if (!n.isValid() || n.isRelative() || !base.isValid()) return QString();
    // SAME ORIGIN ONLY. `paging.next` is an absolute URL out of a response body — attacker-controlled input
    // by definition — and the request that follows it carries the account's bearer token. Scheme, host AND
    // port, because "https://api.myanimelist.net.evil.test" and ":8080" are both different origins.
    if (n.scheme() != base.scheme() || n.host() != base.host() || n.port(-1) != base.port(-1))
        return QString();
    return n.toString(QUrl::FullyEncoded);
}

QString mal::entryUrl(const QString& apiBase, const QString& mediaId, Kind kind)
{
    if (mediaId.isEmpty()) return QString();
    QUrl u(apiBase + QLatin1Char('/') + malSegment(kind) + QLatin1Char('/') + pctEnc(mediaId));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("fields"),
                   pctEnc(malCountField(kind) + QStringLiteral(",my_list_status")));
    u.setQuery(q);
    return u.toString(QUrl::FullyEncoded);
}

bool mal::parseEntry(const QByteArray& json, const QString& mediaId, Kind kind, Entry& out)
{
    const QJsonDocument d = QJsonDocument::fromJson(json);
    if (!d.isObject()) return false;
    const QJsonObject o = d.object();
    if (o.contains(QStringLiteral("error"))) return false;   // MAL's error envelope, at any status
    if (o.value(QStringLiteral("id")).toInt() <= 0) return false;   // not an entry reply at all
    out = Entry{};
    out.mediaId = mediaId;
    out.totalUnits = o.value(malCountField(kind)).toInt();
    const QJsonValue sv = o.value(QStringLiteral("my_list_status"));
    if (!sv.isObject()) return true;    // asked, answered: the account has no row for this media
    const QJsonObject s = sv.toObject();
    out.exists = true;
    // THE ASYMMETRY, read side. MAL REPORTS `num_episodes_watched`; it ACCEPTS `num_watched_episodes` (see
    // saveBody). Reading the write spelling here would report every account as being on episode 0, which
    // reconcile() would then answer by pushing our progress over a list that was already ahead.
    out.progress = (kind == Kind::Manga) ? s.value(QStringLiteral("num_chapters_read")).toInt()
                                         : s.value(QStringLiteral("num_episodes_watched")).toInt();
    out.status = statusFromToken(s.value(QStringLiteral("status")).toString());
    out.score = scoreFromMal(s.value(QStringLiteral("score")).toInt());
    return true;
}

QString mal::saveUrl(const QString& apiBase, const QString& mediaId, Kind kind)
{
    if (mediaId.isEmpty()) return QString();
    return apiBase + QLatin1Char('/') + malSegment(kind) + QLatin1Char('/') + pctEnc(mediaId)
         + QStringLiteral("/my_list_status");
}

QByteArray mal::saveBody(const Update& u, int totalUnits)
{
    const bool manga = u.kind == Kind::Manga;
    // COMPLETED needs BOTH the caller's claim and MAL's own count, exactly as it does on AniList: a
    // provider listing missing its final chapters would otherwise mark a running series finished, which is
    // the one push that cannot be undone by simply pushing again.
    const bool completed = u.completes && (totalUnits <= 0 || u.unit >= totalUnits);
    QVector<QByteArray> f;
    f << formField("status", statusToken(completed ? Status::Completed : Status::Current, u.kind));
    // THE ASYMMETRY, write side. `num_watched_episodes` is what MAL ACCEPTS; sending the read spelling is a
    // 200 that stores nothing, which looks from here like a perfect sync that never happened.
    // Never below 1: a 0 tells the account you have watched nothing, which is a regression dressed as an
    // update.
    f << formField(manga ? "num_chapters_read" : "num_watched_episodes", QString::number(qMax(1, u.unit)));
    // ABSENT unless the app really has a rating. MAL reads 0 as "no score" and WOULD clear one the user set
    // by hand — the same damage AniList's scoreRaw does, arrived at from the opposite convention.
    if (u.hasScore) f << formField("score", QString::number(scoreToMal(u.score)));
    return joinForm(f);
}

QString mal::statusToken(Status s, Kind kind)
{
    const bool manga = kind == Kind::Manga;
    switch (s)
    {
        case Status::Current:   return manga ? QStringLiteral("reading") : QStringLiteral("watching");
        case Status::Planning:  return manga ? QStringLiteral("plan_to_read")
                                             : QStringLiteral("plan_to_watch");
        case Status::Completed: return QStringLiteral("completed");
        case Status::Dropped:   return QStringLiteral("dropped");
        case Status::Paused:    return QStringLiteral("on_hold");
        // MAL has NO "repeating" status — a rewatch is a boolean (is_rewatching) beside an otherwise
        // ordinary `watching`. The seam never writes Repeating, and mapping it to `completed` would be a
        // status change nobody asked for, so it maps to the in-progress token, which is what a rewatch's
        // list status actually is on MAL.
        case Status::Repeating: return manga ? QStringLiteral("reading") : QStringLiteral("watching");
    }
    return manga ? QStringLiteral("reading") : QStringLiteral("watching");
}

Status mal::statusFromToken(const QString& token)
{
    const QString t = token.trimmed().toLower();
    if (t == QLatin1String("completed")) return Status::Completed;
    if (t == QLatin1String("dropped"))   return Status::Dropped;
    if (t == QLatin1String("on_hold"))   return Status::Paused;
    if (t == QLatin1String("plan_to_watch") || t == QLatin1String("plan_to_read")) return Status::Planning;
    // "watching" / "reading" / anything MAL adds later. Current is the safest wrong answer, being the one
    // status a push overwrites with the same value.
    return Status::Current;
}

int mal::scoreToMal(int hundred)
{
    // ROUNDED, not truncated: 85 is a 9. Truncating would silently demote every half-point rating, and it
    // would do it in one direction only, so a value would not survive a push/pull round trip.
    return qBound(0, (qBound(0, hundred, 100) + 5) / 10, 10);
}

int mal::scoreFromMal(int ten)
{
    return qBound(0, ten, 10) * 10;
}

tracker::SendPolicy mal::sendPolicy()
{
    SendPolicy p;
    // A media id the account cannot write (400), an entry that no longer exists (404), or a field MAL's own
    // validation refuses (422). None of the three becomes acceptable by waiting.
    //
    // 403 is deliberately NOT here. MAL answers a suspended account and a temporarily-refused client with
    // the same status, and dropping every queued chapter for the second of those is the worse mistake.
    p.permanent = { 400, 404, 422 };
    p.reauth    = { 401 };
    p.throttle  = { 429 };
    p.baseMs    = kBackoffBaseMs;
    p.maxMs     = kBackoffMaxMs;
    return p;
}

// THE FORWARDER (#326). Every line that used to be here is now tracker::classifySend, asked with the policy
// above; what MAL answers to any (status, Retry-After, failure count) is unchanged to the millisecond.
mal::Backoff mal::backoffFor(int httpStatus, qint64 retryAfterSec, int consecutiveFailures)
{
    const SendVerdict v = classifySend(sendPolicy(), httpStatus, retryAfterSec, consecutiveFailures);
    Backoff b;
    b.retry     = v.retry;
    b.reauth    = v.reauth;
    b.permanent = v.permanent;
    b.delayMs   = v.delayMs;
    return b;
}

// ================= what an AniList push RESPONSE means (issue #326) ======================================

bool anilist::saveAccepted(int httpStatus, const QByteArray& body)
{
    // INCREMENT 1'S TEST, UNCHANGED. A GraphQL error arrives as HTTP 200 with an `errors` array and no
    // `data`, so transport success is not acceptance: anything that is not a SaveMediaListEntry payload
    // leaves the row queued.
    return httpStatus >= 200 && httpStatus < 300 && body.contains("SaveMediaListEntry");
}

int anilist::effectiveStatus(int httpStatus, const QByteArray& body)
{
    if (httpStatus < 200 || httpStatus >= 300) return httpStatus;
    // A 2xx that was not an acceptance is a GraphQL error payload, and AniList puts the status it WOULD
    // have answered with inside the error object. The FIRST one that carries a status wins — a mutation
    // refused for two reasons is still refused for the first of them.
    const QJsonArray errs = QJsonDocument::fromJson(body).object()
                                .value(QStringLiteral("errors")).toArray();
    for (const QJsonValue& e : errs)
    {
        const int s = e.toObject().value(QStringLiteral("status")).toInt();
        if (s > 0) return s;
    }
    // 0 — which classifySend treats as RETRYABLE, exactly as increment 1 treated every failure it could
    // not read. An error shape we do not recognise must never cost somebody their queue.
    return 0;
}

tracker::SendPolicy anilist::sendPolicy()
{
    SendPolicy p;
    // 400: a mutation AniList's schema refuses — a retry sends the identical bytes and gets the identical
    // answer. 404: the media, or the account's list entry for it, is gone. Neither can become a success.
    // 422 is absent because this GraphQL endpoint does not emit it (see the header), and 403 is absent for
    // the reason MAL's is.
    p.permanent = { 400, 404 };
    p.reauth    = { 401 };
    p.throttle  = { 429 };
    p.baseMs    = kBackoffBaseMs;
    p.maxMs     = kBackoffMaxMs;
    return p;
}

// ================= how sure we are of a match ============================================================

// Case, punctuation and whitespace folded away, so "My Hero Academia!" and "my  hero-academia" are one
// string. Deliberately keeps letters and digits of EVERY script: folding a Japanese title to nothing would
// score every one of them 0 and make them all look like noise.
static QString normTitle(const QString& s)
{
    QString out;
    out.reserve(s.size());
    for (const QChar c : s)
    {
        if (c.isLetterOrNumber()) out.append(c.toLower());
        else if (!out.isEmpty() && !out.endsWith(QLatin1Char(' '))) out.append(QLatin1Char(' '));
    }
    while (out.endsWith(QLatin1Char(' '))) out.chop(1);
    return out;
}

static int confidenceAgainst(const QString& qn, const QString& tn)
{
    if (qn.isEmpty() || tn.isEmpty()) return 0;
    if (qn == tn) return 100;
    // One wholly inside the other ("Berserk" against "Berserk 1997"): strong, but NOT certain — a season or
    // a year is exactly the difference that makes two list entries two list entries.
    if (tn.contains(qn) || qn.contains(tn)) return 70;
    const QStringList a = qn.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    QStringList pool = tn.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (a.isEmpty() || pool.isEmpty()) return 0;
    int shared = 0;
    for (const QString& w : a)
    {
        const int i = pool.indexOf(w);
        if (i >= 0) { pool.removeAt(i); ++shared; }
    }
    if (shared == 0) return 0;   // not one word in common: this row is noise
    // Scaled by the LONGER side, so a one-word query matching one word of a six-word title does not score
    // as though it had answered the whole question.
    const int longer = qMax(a.size(), tn.split(QLatin1Char(' '), Qt::SkipEmptyParts).size());
    return int(60.0 * shared / longer);
}

int tracker::titleConfidence(const QString& query, const Match& m)
{
    const QString qn = normTitle(query);
    return qMax(confidenceAgainst(qn, normTitle(m.title)), confidenceAgainst(qn, normTitle(m.altTitle)));
}

QVector<Match> tracker::rankMatches(const QString& query, const QVector<Match>& ms)
{
    if (ms.isEmpty()) return ms;
    QVector<QPair<int, int>> scored;   // (confidence, the provider's own position)
    scored.reserve(ms.size());
    bool any = false;
    for (int i = 0; i < ms.size(); ++i)
    {
        const int c = titleConfidence(query, ms[i]);
        scored.push_back(qMakePair(c, i));
        if (c > 0) any = true;
    }
    // EVERY row noise: hand them back UNCHANGED rather than emptied. A title in a script the query is not
    // written in shares no word with it, and answering "nothing found" there would make exactly those
    // series permanently unlinkable — the opposite of what the conservatism rule is for.
    if (!any) return ms;
    std::stable_sort(scored.begin(), scored.end(),
                     [](const QPair<int, int>& a, const QPair<int, int>& b) { return a.first > b.first; });
    QVector<Match> out;
    out.reserve(scored.size());
    for (const QPair<int, int>& p : scored)
        if (p.first > 0) out.push_back(ms[p.second]);
    return out;
}

int tracker::confidentMatchIndex(const QString& query, const QVector<Match>& ms)
{
    int best = -1, bestScore = 0, runnerUp = 0;
    for (int i = 0; i < ms.size(); ++i)
    {
        const int c = titleConfidence(query, ms[i]);
        if (c > bestScore) { runnerUp = bestScore; bestScore = c; best = i; }
        else if (c > runnerUp) { runnerUp = c; }
    }
    // EXACT, AND ALONE. Anything less is a guess, and a guess writes somebody's progress onto the wrong
    // series in a list they curate by hand. Two rows that both match exactly are an ambiguous field, not a
    // certainty, so they fall through to the user as well.
    if (bestScore < 100 || runnerUp >= 70) return -1;
    return best;
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
