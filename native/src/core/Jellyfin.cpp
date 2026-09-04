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
        const QJsonObject ud = o.value(QStringLiteral("UserData")).toObject();
        it.played       = ud.value(QStringLiteral("Played")).toBool();
        // #83. Read here rather than in a second pass over the same body: a reader that skipped them would
        // make every caller re-parse the row to find out which episode it is.
        it.indexNumber       = o.value(QStringLiteral("IndexNumber")).toInt();
        it.parentIndexNumber = o.value(QStringLiteral("ParentIndexNumber")).toInt();
        it.positionTicks     = qint64(ud.value(QStringLiteral("PlaybackPositionTicks")).toDouble());
        it.seriesId          = o.value(QStringLiteral("SeriesId")).toString();
        it.seasonId          = o.value(QStringLiteral("SeasonId")).toString();
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
            u.indexNumber       = it.indexNumber;
            u.parentIndexNumber = it.parentIndexNumber;
            u.runTimeTicks      = it.runTimeTicks;
            u.positionTicks     = it.positionTicks;
            // The parents are qualified by the SAME minter, so they are either a resolvable reference or
            // absent - never a bare server-scoped id sitting in a field something will later route on.
            u.seriesRef  = qualify(r.serverId, it.seriesId);
            u.seasonRef  = qualify(r.serverId, it.seasonId);
            out.push_back(u);
        }
    }
    return out;
}

QVector<Jellyfin::LibraryRef> Jellyfin::unionOfLibraries(const QVector<LibraryReply>& replies)
{
    QVector<LibraryRef> out;
    for (const LibraryReply& r : replies)
    {
        // Failure isolation as the absence of a special case, exactly as unionOf has it.
        if (r.outcome != Outcome::Ok) continue;
        for (const Library& l : r.libraries)
        {
            const QString ref = qualify(r.serverId, l.id);
            if (ref.isEmpty()) continue;    // unqualifiable: dropped, never emitted bare
            LibraryRef out1;
            out1.ref            = ref;
            out1.name           = l.name;
            out1.collectionType = l.collectionType;
            out1.serverId       = r.serverId;
            out1.serverName     = r.serverName;
            out.push_back(out1);
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

// =========================================================================================================
// SECTION 5 (issue #83): BROWSE, PLAY AND PROGRESS. Still pure - paths, bodies and readers, no socket.
// =========================================================================================================

// ---- The user's libraries -------------------------------------------------------------------------------

QString Jellyfin::viewsPath(const QString& userId)
{
    return QStringLiteral("/Users/") + userId + QStringLiteral("/Views");
}

QVector<Jellyfin::Library> Jellyfin::readViews(const QByteArray& body, bool* ok)
{
    QVector<Library> out;
    if (ok) *ok = false;
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(body, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return out;
    // The same line readItems draws, and for the same reason: "this user has no libraries" and "that was
    // not a Jellyfin answer" are different facts and the browse surface says different things for them.
    const QJsonValue items = doc.object().value(QStringLiteral("Items"));
    if (!items.isArray()) return out;
    for (const QJsonValue& v : items.toArray())
    {
        if (!v.isObject()) continue;
        const QJsonObject o = v.toObject();
        Library l;
        l.id             = o.value(QStringLiteral("Id")).toString();
        l.name           = o.value(QStringLiteral("Name")).toString();
        l.collectionType = o.value(QStringLiteral("CollectionType")).toString().toLower();
        if (l.id.isEmpty()) continue;      // a view with no id can never be qualified; it is not a view
        out.push_back(l);
    }
    if (ok) *ok = true;
    return out;
}

QString Jellyfin::categoryForCollection(const QString& collectionType)
{
    const QString t = collectionType.toLower();
    if (t == QLatin1String("movies") || t == QLatin1String("tvshows") || t == QLatin1String("homevideos")
        || t == QLatin1String("musicvideos"))
        return QStringLiteral("video");
    // Named, and deliberately not browsed by this increment - see the header. #194 owns the music surface.
    if (t == QLatin1String("music")) return QStringLiteral("audio");
    if (t == QLatin1String("books")) return QStringLiteral("reading");
    if (t == QLatin1String("photos")) return QStringLiteral("photos");
    // EVERYTHING ELSE IS EMPTY, NOT "video". Jellyfin's own view list carries collections, playlists and
    // live tv, and an unrecognised type routed to the catch-all would put a folder in front of the user
    // whose every row is something this increment cannot open.
    return QString();
}

bool Jellyfin::isVideoCollection(const QString& collectionType)
{
    return categoryForCollection(collectionType) == QLatin1String("video");
}

// ---- Items, seasons and episodes ------------------------------------------------------------------------

QString Jellyfin::libraryItemsQuery(const QString& libraryId)
{
    // Movie and Series only, RECURSIVE: a library is a folder tree on the server and the rows a person
    // expects to see are the titles, not the top-level directories the admin happened to make. A Series is
    // a CONTAINER here - its seasons and episodes are fetched when it is opened, never up front, because
    // a library of two hundred shows would otherwise be several thousand rows nobody asked for.
    return QStringLiteral("ParentId=") + libraryId
         + QStringLiteral("&Recursive=true&IncludeItemTypes=Movie,Series&SortBy=SortName"
                          "&SortOrder=Ascending&Fields=ProductionYear,RunTimeTicks"
                          "&EnableTotalRecordCount=false");
}

QString Jellyfin::seasonsPath(const QString& seriesId)
{
    return QStringLiteral("/Shows/") + seriesId + QStringLiteral("/Seasons");
}

QString Jellyfin::episodesPath(const QString& seriesId)
{
    return QStringLiteral("/Shows/") + seriesId + QStringLiteral("/Episodes");
}

QString Jellyfin::seasonsQuery(const QString& userId)
{
    return QStringLiteral("userId=") + userId + QStringLiteral("&Fields=ProductionYear");
}

QString Jellyfin::episodesQuery(const QString& userId, const QString& seasonId)
{
    QString q = QStringLiteral("userId=") + userId
              + QStringLiteral("&Fields=ProductionYear,RunTimeTicks&EnableTotalRecordCount=false");
    // NO seasonId MEANS EVERY EPISODE OF THE SHOW, which is what the endpoint does with the parameter
    // absent - and is exactly right for a show with no season structure. Passing an empty one would ask
    // for the episodes of a season called "", and the server answers that with nothing.
    if (!seasonId.isEmpty()) q += QStringLiteral("&seasonId=") + seasonId;
    return q;
}

QString Jellyfin::resumeItemsPath(const QString& userId)
{
    return QStringLiteral("/Users/") + userId + QStringLiteral("/Items/Resume");
}

QString Jellyfin::resumeItemsQuery()
{
    // VIDEO ONLY, and capped. This feeds one section of the home list, so an unbounded answer from a server
    // with a large household would push everything else off the screen; and audio resume points belong to
    // the music surface (#194), not to Continue Watching.
    return QStringLiteral("Recursive=true&MediaTypes=Video&Limit=24"
                          "&Fields=ProductionYear,RunTimeTicks&EnableTotalRecordCount=false");
}

// ---- What the server says about ONE item ----------------------------------------------------------------

QString Jellyfin::itemPath(const QString& userId, const QString& itemId)
{
    return QStringLiteral("/Users/") + userId + QStringLiteral("/Items/") + itemId;
}

Jellyfin::UserState Jellyfin::readUserState(const QByteArray& body)
{
    UserState s;
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(body, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return s;
    const QJsonValue ud = doc.object().value(QStringLiteral("UserData"));
    // `ok` IS THE WHOLE POINT OF THIS READER. "The server said zero" and "the server said nothing" produce
    // the same number and must not produce the same decision: resumeSeconds() defers to the first and
    // keeps the local mark for the second.
    if (!ud.isObject()) return s;
    const QJsonObject o = ud.toObject();
    s.positionTicks = qint64(o.value(QStringLiteral("PlaybackPositionTicks")).toDouble());
    s.played        = o.value(QStringLiteral("Played")).toBool();
    s.ok            = true;
    return s;
}

double Jellyfin::resumeSeconds(const UserState& server, double localSeconds)
{
    // THE SERVER WINS WHENEVER IT ANSWERED, INCLUDING WITH ZERO. See the header: a film finished on another
    // device reports zero, and a local mark that beat it would restart every re-watch near the end.
    if (server.ok) return secondsFromTicks(server.positionTicks);
    return localSeconds > 0.0 ? localSeconds : 0.0;
}

// ---- PlaybackInfo: the server decides -------------------------------------------------------------------

QString Jellyfin::playbackInfoPath(const QString& itemId)
{
    return QStringLiteral("/Items/") + itemId + QStringLiteral("/PlaybackInfo");
}

QByteArray Jellyfin::playbackInfoBody(const QString& userId, qint64 startTicks,
                                      int audioStreamIndex, int subtitleStreamIndex)
{
    QJsonObject o;
    o.insert(QStringLiteral("UserId"), userId);
    o.insert(QStringLiteral("StartTimeTicks"), double(startTicks < 0 ? 0 : startTicks));
    // WE ASK FOR ALL THREE AND LET THE SERVER CHOOSE. Turning any of them off here would be this client
    // deciding what its own server may do with its own file - the exact thing the header says we do not do.
    o.insert(QStringLiteral("EnableDirectPlay"), true);
    o.insert(QStringLiteral("EnableDirectStream"), true);
    o.insert(QStringLiteral("EnableTranscoding"), true);
    o.insert(QStringLiteral("AllowVideoStreamCopy"), true);
    o.insert(QStringLiteral("AllowAudioStreamCopy"), true);
    o.insert(QStringLiteral("AutoOpenLiveStream"), true);
    // A SELECTED TRACK IS PASSED THROUGH; AN UNSELECTED ONE IS ABSENT. -1 means "the user has not chosen",
    // and sending -1 would pin the choice to "no track at all" - which for subtitles is a user-visible
    // regression (the server's own default subtitle stops appearing) and for audio is a broken file.
    if (audioStreamIndex >= 0)    o.insert(QStringLiteral("AudioStreamIndex"), audioStreamIndex);
    if (subtitleStreamIndex >= 0) o.insert(QStringLiteral("SubtitleStreamIndex"), subtitleStreamIndex);
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

Jellyfin::PlaybackChoice Jellyfin::readPlaybackInfo(const QByteArray& body)
{
    PlaybackChoice c;
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(body, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return c;
    const QJsonObject root = doc.object();
    const QJsonValue sources = root.value(QStringLiteral("MediaSources"));
    if (!sources.isArray()) return c;
    const QJsonArray arr = sources.toArray();
    if (arr.isEmpty()) return c;              // an envelope with no source: nothing to play, and it says so
    if (!arr.first().isObject()) return c;
    const QJsonObject src = arr.first().toObject();   // THE FIRST SOURCE DECIDES - see the header

    c.ok            = true;
    c.mediaSourceId = src.value(QStringLiteral("Id")).toString();
    c.container     = src.value(QStringLiteral("Container")).toString();
    c.transcodingUrl = src.value(QStringLiteral("TranscodingUrl")).toString();
    // PlaySessionId is on the ENVELOPE, not on the source: it names this playback, not this file.
    c.playSessionId = root.value(QStringLiteral("PlaySessionId")).toString();

    const bool direct = src.value(QStringLiteral("SupportsDirectPlay")).toBool()
                     || src.value(QStringLiteral("SupportsDirectStream")).toBool();
    if (direct)                        c.mode = PlaybackChoice::Mode::DirectPlay;
    else if (!c.transcodingUrl.isEmpty()) c.mode = PlaybackChoice::Mode::Transcode;
    else                               c.mode = PlaybackChoice::Mode::Unavailable;
    return c;
}

QString Jellyfin::playbackUrl(const QString& root, const QString& itemId, const QString& token,
                              const PlaybackChoice& choice)
{
    if (root.isEmpty() || itemId.isEmpty() || !choice.ok) return QString();

    if (choice.mode == PlaybackChoice::Mode::Transcode)
    {
        if (choice.transcodingUrl.isEmpty()) return QString();
        QString u = root + choice.transcodingUrl;
        // THE SERVER'S OWN URL, USED AS THE SERVER BUILT IT. The token is appended only when it is not
        // already there: Jellyfin reads the FIRST api_key, so a second one would leave us playing a url
        // that does not match the session the server thinks it opened.
        if (!token.isEmpty() && !u.contains(QLatin1String("api_key=")))
            u += (u.contains(QLatin1Char('?')) ? QLatin1Char('&') : QLatin1Char('?'))
               + QStringLiteral("api_key=") + QString::fromLatin1(QUrl::toPercentEncoding(token));
        return u;
    }
    if (choice.mode != PlaybackChoice::Mode::DirectPlay) return QString();   // Unavailable: nothing honest

    QUrl u(root + QStringLiteral("/Videos/") + itemId + QStringLiteral("/stream"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("static"), QStringLiteral("true"));
    // The SOURCE, named. A film held in two versions has two media sources under one item id, and a stream
    // url with no mediaSourceId lets the server pick - which is how the user asks for the 4K remux and
    // gets the 720p one.
    if (!choice.mediaSourceId.isEmpty())
        q.addQueryItem(QStringLiteral("mediaSourceId"), choice.mediaSourceId);
    // The same id every progress report carries, so the server's own session list shows ONE playback
    // rather than a stream and a set of unattached reports.
    if (!choice.playSessionId.isEmpty())
        q.addQueryItem(QStringLiteral("playSessionId"), choice.playSessionId);
    // THE TOKEN IS IN THIS QUERY, for the reason streamUrl above states: mpv cannot be handed a header.
    // That is precisely why this string is minted at the moment the player is handed it and is never
    // stored, never logged and never written into a queue, a playlist or a recents row (recordedPath).
    if (!token.isEmpty()) q.addQueryItem(QStringLiteral("api_key"), token);
    u.setQuery(q);
    return u.toString(QUrl::FullyEncoded);
}

// ---- Progress: the sessions API -------------------------------------------------------------------------

QString Jellyfin::progressPath(ProgressEvent ev)
{
    switch (ev)
    {
    case ProgressEvent::Start: return QStringLiteral("/Sessions/Playing");
    case ProgressEvent::Stop:  return QStringLiteral("/Sessions/Playing/Stopped");
    case ProgressEvent::Progress:
    case ProgressEvent::Pause:
    case ProgressEvent::Unpause:
        break;
    }
    return QStringLiteral("/Sessions/Playing/Progress");
}

QByteArray Jellyfin::progressBody(const QString& itemId, const QString& playSessionId,
                                  const QString& mediaSourceId, double positionSeconds, ProgressEvent ev)
{
    QJsonObject o;
    o.insert(QStringLiteral("ItemId"), itemId);
    // BOTH IDS RIDE EVERY REPORT. Without the play-session id the server files the report against no
    // playback and the "now playing" row on another device never updates; without the media-source id a
    // multi-version item records progress against whichever version the server guesses.
    if (!playSessionId.isEmpty()) o.insert(QStringLiteral("PlaySessionId"), playSessionId);
    if (!mediaSourceId.isEmpty()) o.insert(QStringLiteral("MediaSourceId"), mediaSourceId);
    o.insert(QStringLiteral("PositionTicks"), double(ticksFromSeconds(positionSeconds)));
    o.insert(QStringLiteral("IsPaused"), ev == ProgressEvent::Pause);
    o.insert(QStringLiteral("IsMuted"), false);
    o.insert(QStringLiteral("CanSeek"), true);
    // The event name Jellyfin's own web client sends, and the reason a pause is a PROGRESS report rather
    // than an endpoint of its own.
    if (ev == ProgressEvent::Pause)        o.insert(QStringLiteral("EventName"), QStringLiteral("pause"));
    else if (ev == ProgressEvent::Unpause) o.insert(QStringLiteral("EventName"), QStringLiteral("unpause"));
    else if (ev == ProgressEvent::Progress) o.insert(QStringLiteral("EventName"),
                                                     QStringLiteral("timeupdate"));
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

bool Jellyfin::shouldReportProgress(double lastReportedS, double nowS)
{
    // NOTHING REPORTED YET reports immediately: the first tick after a start is what tells the server the
    // playback is real, and waiting ten seconds for it makes a short open look like it never happened.
    if (lastReportedS < 0.0) return true;
    const double d = nowS - lastReportedS;
    return (d < 0.0 ? -d : d) >= kProgressIntervalS;
}

// ---- Media segments (Jellyfin 10.10+) --------------------------------------------------------------------

QString Jellyfin::mediaSegmentsPath(const QString& itemId)
{
    return QStringLiteral("/MediaSegments/") + itemId;
}

QString Jellyfin::mediaSegmentsQuery()
{
    // ASKED FOR BY NAME rather than left to the default, so what this client consumes is visible in one
    // string. Commercial and Preview are read even though the existing stack only ACTS on Intro and
    // Credits: MediaSegments::SegmentType already carries them, they are stored and not acted on, and a
    // reader that dropped them would have to be changed again the day the chip learns a third type.
    return QStringLiteral("includeSegmentTypes=Intro&includeSegmentTypes=Outro"
                          "&includeSegmentTypes=Recap&includeSegmentTypes=Commercial"
                          "&includeSegmentTypes=Preview");
}

QVector<Jellyfin::RemoteSegment> Jellyfin::readMediaSegments(const QByteArray& body)
{
    QVector<RemoteSegment> out;
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(body, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return out;
    const QJsonValue items = doc.object().value(QStringLiteral("Items"));
    if (!items.isArray()) return out;
    for (const QJsonValue& v : items.toArray())
    {
        if (!v.isObject()) continue;
        const QJsonObject o = v.toObject();
        RemoteSegment s;
        s.start = secondsFromTicks(qint64(o.value(QStringLiteral("StartTicks")).toDouble()));
        s.end   = secondsFromTicks(qint64(o.value(QStringLiteral("EndTicks")).toDouble()));
        s.type  = o.value(QStringLiteral("Type")).toString();
        // A RANGE THAT GOES NOWHERE IS NOT A RANGE. An inverted or zero-length row would arm a skip that
        // jumps to where you already are, which reads as the chip being broken rather than as bad data.
        if (s.end <= s.start) continue;
        if (s.type.isEmpty()) continue;
        out.push_back(s);
    }
    return out;
}

// ---- What a Jellyfin row may be WRITTEN DOWN --------------------------------------------------------------

QString Jellyfin::recordedPath(const QString& qualifiedId, const QString& playUrl)
{
    // THE ONE DECISION THAT KEEPS THE TOKEN OUT OF EVERY STORE. A qualified id is stable, carries no
    // credential and is re-openable (openRecent mints a fresh link from it), so it is what a row records.
    // Anything else is returned byte for byte, so no other route changes at all.
    return isQualified(qualifiedId) ? qualifiedId : playUrl;
}
