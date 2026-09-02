#include "Jellyfin.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <QUrlQuery>

namespace {

// A server id is 32 hex digits. Dashes are tolerated and ignored so that the compact form Jellyfin's own
// /System/Info/Public reports and the dashed form a .NET GUID round-trip can produce are ONE identity — they
// are the same 128 bits, and treating them as two would give one server two id spaces.
bool hexOnly(const QString& s)
{
    for (const QChar c : s)
        if (!((c >= QLatin1Char('0') && c <= QLatin1Char('9'))
           || (c >= QLatin1Char('a') && c <= QLatin1Char('f'))
           || (c >= QLatin1Char('A') && c <= QLatin1Char('F'))))
            return false;
    return true;
}

} // namespace

bool Jellyfin::isServerId(const QString& s)
{
    if (s.isEmpty()) return false;
    QString bare = s;
    bare.remove(QLatin1Char('-'));
    return bare.size() == 32 && hexOnly(bare);
}

QString Jellyfin::qualify(const QString& serverId, const QString& itemId)
{
    // An unqualifiable reference is ABSENT, never half-formed. See Jellyfin.h's section 1: a caller that
    // received "jf::4f2" would file a row under a key nothing can ever resolve, and no later pass could tell
    // it apart from a row that was meant to be there.
    if (itemId.isEmpty() || !isServerId(serverId)) return QString();
    return idPrefix() + idSep() + serverId + idSep() + itemId;
}

Jellyfin::Ref Jellyfin::parse(const QString& qualified)
{
    Ref r;
    // Exactly two separators are consumed, and the item half is EVERYTHING after the second one. Not a
    // section() split: an item id containing a colon is legal (it is opaque server-chosen text), and a split
    // would silently truncate it into a different item's id.
    const QString pfx = idPrefix() + idSep();
    if (!qualified.startsWith(pfx)) return r;
    const int second = qualified.indexOf(idSep(), pfx.size());
    if (second < 0) return r;                                  // two fields: the legacy shape, not this one
    const QString server = qualified.mid(pfx.size(), second - pfx.size());
    const QString item   = qualified.mid(second + 1);
    if (item.isEmpty()) return r;
    // The second field must be a WELL-FORMED SERVER ID. Field count alone would be enough for every id
    // either shape actually mints, but this is the line that keeps a legacy id whose item half contains a
    // colon from parsing as a qualified id belonging to a server that does not exist.
    if (!isServerId(server)) return r;
    r.serverId = server;
    r.itemId   = item;
    r.ok       = true;
    return r;
}

QString Jellyfin::legacyItemId(const QString& s)
{
    // EXACTLY two fields. Everything else in the world answers an empty string here, and that is the arm
    // that matters most: it is what stops the migration touching a file path, an addon item id, a Subsonic
    // key or an already-qualified id that happens to live in the same store.
    //
    // THE LINE BELOW IS A TRIPWIRE NO MUTATION CAN KILL, AND IT IS SAID OUT LOUD RATHER THAN LEFT TO BE
    // REDISCOVERED (the rule MusicRemap.cpp states about its own read-back). It is subsumed today by the
    // separator test at the end: a qualified id is "jf:" + <server id> + ":" + <item>, so what follows the
    // prefix ALWAYS contains a separator and is always refused there anyway. It stays because it says the
    // intent at the top of the function instead of leaving it as a consequence of the last line, and because
    // it is the line that keeps this reader correct if the id grammar ever gains a form whose second field
    // carries no separator. jellyfin-mutants.json therefore mutates the separator test — which carries BOTH
    // this refusal and the migration's idempotence — and does not mutate this one.
    if (parse(s).ok) return QString();
    const QString pfx = idPrefix() + idSep();
    if (!s.startsWith(pfx)) return QString();
    const QString rest = s.mid(pfx.size());
    if (rest.isEmpty()) return QString();
    // THREE OR MORE FIELDS IS NOT A LEGACY REFERENCE — and this one line carries two separate properties:
    //   * a string that merely starts with "jf:" is not claimed and re-keyed (rule 1);
    //   * AND the migration is IDEMPOTENT, because its own output is exactly such a string. Without this,
    //     the second run re-qualifies jf:<srv>:<item> into jf:<srv>:jf:<srv>:<item> and every record it
    //     moved the first time moves again, onto a key nothing reads.
    if (rest.contains(idSep())) return QString();
    return rest;
}

// ---- Transport safety ---------------------------------------------------------------------------------

Jellyfin::UrlVerdict Jellyfin::checkUrl(const QString& url, bool allowPlainHttp)
{
    const QUrl u(url.trimmed(), QUrl::StrictMode);
    if (!u.isValid() || u.host().isEmpty()) return UrlVerdict::Malformed;
    const QString scheme = u.scheme().toLower();
    if (scheme == QLatin1String("https")) return UrlVerdict::Ok;
    if (scheme != QLatin1String("http"))  return UrlVerdict::NotHttp;
    return allowPlainHttp ? UrlVerdict::Ok : UrlVerdict::InsecureRefused;
}

QString Jellyfin::normalizeRoot(const QString& url, bool allowPlainHttp)
{
    if (checkUrl(url, allowPlainHttp) != UrlVerdict::Ok) return QString();
    QString s = url.trimmed();
    while (s.endsWith(QLatin1Char('/'))) s.chop(1);
    return s;
}

// ---- Auth ---------------------------------------------------------------------------------------------

QString Jellyfin::authHeader(const QString& client, const QString& device, const QString& deviceId,
                             const QString& version, const QString& token)
{
    // Jellyfin's own spelling. The quoted values are escaped only for the quote character itself, which is
    // the only one the grammar cannot carry; every field here is app-controlled except the token, which is
    // hex from the server.
    auto quoted = [](const QString& v) {
        QString e = v; e.replace(QLatin1Char('"'), QLatin1String("\\\""));
        return QLatin1Char('"') + e + QLatin1Char('"');
    };
    QString h = QStringLiteral("MediaBrowser Client=") + quoted(client)
              + QStringLiteral(", Device=") + quoted(device)
              + QStringLiteral(", DeviceId=") + quoted(deviceId)
              + QStringLiteral(", Version=") + quoted(version);
    if (!token.isEmpty()) h += QStringLiteral(", Token=") + quoted(token);
    return h;
}

QByteArray Jellyfin::authenticateBody(const QString& username, const QString& password)
{
    QJsonObject o;
    o.insert(QStringLiteral("Username"), username);
    o.insert(QStringLiteral("Pw"), password);
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

Jellyfin::PublicInfo Jellyfin::readPublicInfo(const QByteArray& body)
{
    PublicInfo info;
    const QJsonObject o = QJsonDocument::fromJson(body).object();
    info.serverId   = o.value(QStringLiteral("Id")).toString();
    info.serverName = o.value(QStringLiteral("ServerName")).toString();
    info.version    = o.value(QStringLiteral("Version")).toString();
    // `ok` is about the IDENTITY and nothing else. A reply with a name and a version but no usable `Id`
    // cannot qualify a single row, so adding that server would write ids nothing can resolve.
    info.ok = isServerId(info.serverId);
    if (!info.ok) info.serverId.clear();
    return info;
}

Jellyfin::AuthResult Jellyfin::readAuthResult(const QByteArray& body)
{
    AuthResult r;
    const QJsonObject o = QJsonDocument::fromJson(body).object();
    r.token = o.value(QStringLiteral("AccessToken")).toString();
    const QJsonObject user = o.value(QStringLiteral("User")).toObject();
    r.userId   = user.value(QStringLiteral("Id")).toString();
    r.userName = user.value(QStringLiteral("Name")).toString();
    // BOTH halves or neither. A token with no user id cannot address /Users/<id>/Items, so it would present
    // itself as a successful sign-in that can never list anything.
    r.ok = !r.token.isEmpty() && !r.userId.isEmpty();
    return r;
}

// ---- Items --------------------------------------------------------------------------------------------

QString Jellyfin::publicInfoPath()  { return QStringLiteral("/System/Info/Public"); }
QString Jellyfin::authenticatePath(){ return QStringLiteral("/Users/AuthenticateByName"); }

QString Jellyfin::itemsPath(const QString& userId)
{
    return QStringLiteral("/Users/") + userId + QStringLiteral("/Items");
}

QVector<Jellyfin::RemoteItem> Jellyfin::readItems(const QByteArray& body, bool* ok)
{
    QVector<RemoteItem> out;
    if (ok) *ok = false;
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(body, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return out;
    const QJsonObject root = doc.object();
    // "A SERVER WITH NO ITEMS" AND "A BODY THAT IS NOT AN ITEM ENVELOPE" ARE DIFFERENT ANSWERS, and the
    // union treats them differently — the first is an empty contribution, the second is a failure the user
    // is told about. So the `Items` member must be PRESENT and an array; its being empty is fine.
    const QJsonValue items = root.value(QStringLiteral("Items"));
    if (!items.isArray()) return out;
    for (const QJsonValue& v : items.toArray())
    {
        if (!v.isObject()) continue;
        const QJsonObject o = v.toObject();
        RemoteItem it;
        it.id           = o.value(QStringLiteral("Id")).toString();
        it.name         = o.value(QStringLiteral("Name")).toString();
        it.type         = o.value(QStringLiteral("Type")).toString();
        it.seriesName   = o.value(QStringLiteral("SeriesName")).toString();
        it.year         = o.value(QStringLiteral("ProductionYear")).toInt();
        it.runTimeTicks = qint64(o.value(QStringLiteral("RunTimeTicks")).toDouble());
        it.played       = o.value(QStringLiteral("UserData")).toObject()
                           .value(QStringLiteral("Played")).toBool();
        if (it.id.isEmpty()) continue;      // a row with no id can never be qualified; it is not a row
        out.push_back(it);
    }
    if (ok) *ok = true;
    return out;
}

QString Jellyfin::streamUrl(const QString& root, const QString& itemId, const QString& token)
{
    if (root.isEmpty() || itemId.isEmpty()) return QString();
    QUrl u(root + QStringLiteral("/Videos/") + itemId + QStringLiteral("/stream"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("static"), QStringLiteral("true"));
    // THE TOKEN IS IN THIS QUERY. mpv cannot be handed a header, so it must be — and that is precisely why
    // this string is minted at the moment the player is handed it and is never stored, never logged and
    // never written into a queue, a playlist or a recents row. Jellyfin.h's section 3 has the rule.
    if (!token.isEmpty()) q.addQueryItem(QStringLiteral("api_key"), token);
    u.setQuery(q);
    return u.toString(QUrl::FullyEncoded);
}

// ---- The union ----------------------------------------------------------------------------------------

QVector<Jellyfin::UnionItem> Jellyfin::unionOf(const QVector<ServerReply>& replies)
{
    QVector<UnionItem> out;
    for (const ServerReply& r : replies)
    {
        // FAILURE ISOLATION AS THE ABSENCE OF A SPECIAL CASE. A server that timed out, failed or is switched
        // off contributes nothing here; there is no placeholder row, no partial shelf and no error that
        // stops the other servers' rows being emitted.
        if (r.outcome != Outcome::Ok) continue;
        for (const RemoteItem& it : r.items)
        {
            const QString id = qualify(r.serverId, it.id);
            // An unqualifiable row is DROPPED rather than emitted bare. There is no safe half-measure: a
            // bare id is exactly the corruption this whole file exists to prevent.
            if (id.isEmpty()) continue;
            UnionItem u;
            u.id         = id;
            u.title      = it.name;
            u.type       = it.type;
            u.seriesName = it.seriesName;
            u.year       = it.year;
            u.serverId   = r.serverId;
            u.serverName = r.serverName;
            u.played     = it.played;
            out.push_back(u);
        }
    }
    return out;
}

QString Jellyfin::unavailableNote(const ServerReply& reply)
{
    // The display name and a verdict. No url, no header, no token — Jellyfin.h's section 3, and the reason
    // this is a function rather than a printf at each call site.
    const QString name = reply.serverName.isEmpty() ? QStringLiteral("(unnamed server)") : reply.serverName;
    switch (reply.outcome)
    {
    case Outcome::TimedOut: return QStringLiteral("jellyfin: \"") + name
                                 + QStringLiteral("\" did not answer in time; its items are not in this view");
    case Outcome::Failed:   return QStringLiteral("jellyfin: \"") + name
                                 + QStringLiteral("\" could not be read; its items are not in this view");
    case Outcome::Disabled: return QStringLiteral("jellyfin: \"") + name
                                 + QStringLiteral("\" is switched off; its items are hidden");
    case Outcome::Ok:       break;
    }
    return QString();
}
