#include "Audiobookshelf.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <cmath>

namespace {

// An id half is usable iff it is non-empty and carries neither of the scheme's two separators. Refused
// rather than escaped — Audiobookshelf.h says why.
bool usableIdPart(const QString& s)
{
    return !s.isEmpty() && !s.contains(QLatin1Char(':')) && !s.contains(Abs::episodeSep());
}

QJsonObject objOf(const QByteArray& body)
{
    return QJsonDocument::fromJson(body).object();
}

// Audiobookshelf answers list endpoints under several different keys depending on the endpoint and the
// server version ("results", "libraries", "authors", "episodes"). Read whichever is there rather than
// guessing per call site: a version that renamed one would otherwise present as an empty shelf.
QJsonArray arrayOf(const QJsonObject& o, const QStringList& keys)
{
    for (const QString& k : keys)
        if (o.value(k).isArray()) return o.value(k).toArray();
    return QJsonArray();
}

double num(const QJsonValue& v) { return v.isDouble() ? v.toDouble() : 0.0; }

} // namespace

// ==================================================================================================
// The id scheme
// ==================================================================================================
QString Abs::qualify(const QString& serverId, const QString& itemId)
{
    if (!usableIdPart(serverId) || !usableIdPart(itemId)) return QString();
    return QString::fromLatin1(kScheme) + serverId + QLatin1Char(':') + itemId;
}

QString Abs::qualifyEpisode(const QString& serverId, const QString& itemId, const QString& episodeId)
{
    const QString base = qualify(serverId, itemId);
    if (base.isEmpty() || !usableIdPart(episodeId)) return QString();
    return base + episodeSep() + episodeId;
}

Abs::Ref Abs::parse(const QString& s)
{
    Ref r;
    const QString prefix = QString::fromLatin1(kScheme);
    if (!s.startsWith(prefix)) return r;
    const QString rest = s.mid(prefix.size());
    const int colon = rest.indexOf(QLatin1Char(':'));
    if (colon <= 0) return r;                          // no server half, or an empty one
    const QString server = rest.left(colon);
    QString item = rest.mid(colon + 1);
    if (item.isEmpty()) return r;
    // A second ':' means this is not one of ours: an id half may not contain one (qualify refuses it), so
    // a string with three colons was minted by something else and must not be answered for.
    if (item.contains(QLatin1Char(':'))) return r;
    QString episode;
    const int hash = item.indexOf(episodeSep());
    if (hash >= 0)
    {
        episode = item.mid(hash + 1);
        item    = item.left(hash);
        if (item.isEmpty() || episode.isEmpty() || episode.contains(episodeSep())) return r;
    }
    r.ok = true; r.serverId = server; r.itemId = item; r.episodeId = episode;
    return r;
}

QString Abs::itemIdOf(const QString& qualified)
{
    const Ref r = parse(qualified);
    return r.ok ? qualify(r.serverId, r.itemId) : QString();
}

// ==================================================================================================
// The server address
// ==================================================================================================
Abs::UrlVerdict Abs::checkUrl(const QString& url, bool allowPlainHttp)
{
    const QString t = url.trimmed();
    if (t.isEmpty()) return UrlVerdict::Malformed;
    const QUrl u(t);
    if (!u.isValid() || u.host().isEmpty()) return UrlVerdict::Malformed;
    const QString scheme = u.scheme().toLower();
    if (scheme == QLatin1String("https")) return UrlVerdict::Ok;
    if (scheme == QLatin1String("http"))
        return allowPlainHttp ? UrlVerdict::Ok : UrlVerdict::InsecureRefused;
    return UrlVerdict::NotHttp;
}

QString Abs::normalizeRoot(const QString& url, bool allowPlainHttp)
{
    if (checkUrl(url, allowPlainHttp) != UrlVerdict::Ok) return QString();
    QString t = url.trimmed();
    while (t.endsWith(QLatin1Char('/'))) t.chop(1);
    return t;
}

// ==================================================================================================
// The endpoints
// ==================================================================================================
QString Abs::loginPath()     { return QStringLiteral("/login"); }
QString Abs::authorizePath() { return QStringLiteral("/api/authorize"); }
QString Abs::librariesPath() { return QStringLiteral("/api/libraries"); }

QString Abs::libraryItemsPath(const QString& libraryId, int limit)
{
    // `limit=0` is Audiobookshelf's "no paging" — the whole library in one reply, which is what a browse
    // level wants and what every other client asks for. A caller that wants a cap passes one.
    return QStringLiteral("/api/libraries/") + libraryId + QStringLiteral("/items?limit=")
           + QString::number(std::max(0, limit));
}

QString Abs::librarySeriesPath(const QString& libraryId)
{
    return QStringLiteral("/api/libraries/") + libraryId + QStringLiteral("/series");
}

QString Abs::libraryAuthorsPath(const QString& libraryId)
{
    return QStringLiteral("/api/libraries/") + libraryId + QStringLiteral("/authors");
}

QString Abs::itemPath(const QString& itemId)
{
    return QStringLiteral("/api/items/") + itemId + QStringLiteral("?expanded=1");
}

QString Abs::playPath(const QString& itemId, const QString& episodeId)
{
    QString p = QStringLiteral("/api/items/") + itemId + QStringLiteral("/play");
    if (!episodeId.isEmpty()) p += QLatin1Char('/') + episodeId;
    return p;
}

QString Abs::progressPath(const QString& itemId, const QString& episodeId)
{
    QString p = QStringLiteral("/api/me/progress/") + itemId;
    if (!episodeId.isEmpty()) p += QLatin1Char('/') + episodeId;
    return p;
}

QString Abs::coverPath(const QString& itemId)
{
    return QStringLiteral("/api/items/") + itemId + QStringLiteral("/cover");
}

QJsonObject Abs::loginBody(const QString& username, const QString& password)
{
    QJsonObject o;
    o.insert(QStringLiteral("username"), username);
    o.insert(QStringLiteral("password"), password);
    return o;
}

QJsonObject Abs::progressBody(double currentTime, double duration)
{
    QJsonObject o;
    const double t = std::max(0.0, currentTime);
    o.insert(QStringLiteral("currentTime"), t);
    if (duration > 0.0)
    {
        o.insert(QStringLiteral("duration"), duration);
        // Clamped to [0,1]: a position past a stale duration would otherwise report a fraction above one,
        // which Audiobookshelf stores verbatim and every client then renders as a bar past its own end.
        const double frac = std::min(1.0, t / duration);
        o.insert(QStringLiteral("progress"), frac);
        // NOT a "finished" flag of our own invention. The server owns that state; what we say is where the
        // listener is, and only a position genuinely at the end claims the end.
        o.insert(QStringLiteral("isFinished"), frac >= 0.99);
    }
    return o;
}

QString Abs::streamUrl(const QString& root, const QString& contentUrl, const QString& token)
{
    if (root.isEmpty() || contentUrl.isEmpty()) return QString();
    // The server gives a path; a server that gave an absolute url would be handing us another origin, and
    // this feature never follows one (see AbsClient's SameOriginRedirectPolicy).
    if (contentUrl.contains(QStringLiteral("://"))) return QString();
    QUrl u(root + (contentUrl.startsWith(QLatin1Char('/')) ? contentUrl
                                                           : QLatin1Char('/') + contentUrl));
    if (!token.isEmpty())
    {
        QUrlQuery q(u.query());
        q.addQueryItem(QStringLiteral("token"), token);
        u.setQuery(q);
    }
    return u.toString();
}

QString Abs::coverUrl(const QString& root, const QString& itemId, const QString& token)
{
    if (root.isEmpty() || itemId.isEmpty()) return QString();
    QUrl u(root + coverPath(itemId));
    QUrlQuery q;
    if (!token.isEmpty()) q.addQueryItem(QStringLiteral("token"), token);
    u.setQuery(q);
    return u.toString();
}

// ==================================================================================================
// The payloads
// ==================================================================================================
Abs::Login Abs::readLogin(const QByteArray& body)
{
    Login out;
    const QJsonObject o = objOf(body);
    const QJsonObject user = o.value(QStringLiteral("user")).toObject();
    // Older servers put the token on the user; newer ones also offer it at the root. Read both rather than
    // pick, because a client that reads only one presents "wrong password" for a version difference.
    out.token = user.value(QStringLiteral("token")).toString();
    if (out.token.isEmpty()) out.token = o.value(QStringLiteral("token")).toString();
    out.username = user.value(QStringLiteral("username")).toString();
    out.ok = !out.token.isEmpty();
    return out;
}

QString Abs::serverIdOf(const QByteArray& body)
{
    const QJsonObject o = objOf(body);
    QString id = o.value(QStringLiteral("serverSettings")).toObject()
                  .value(QStringLiteral("id")).toString();
    if (id.isEmpty()) id = o.value(QStringLiteral("serverId")).toString();
    // "server-settings" is the constant EVERY Audiobookshelf install answers with — see the header. An id
    // shared by every server in the world qualifies nothing, so it is treated as no id at all.
    if (id == QLatin1String("server-settings")) return QString();
    return usableIdPart(id) ? id : QString();
}

QVector<Abs::Library> Abs::readLibraries(const QByteArray& body)
{
    QVector<Library> out;
    const QJsonArray arr = arrayOf(objOf(body), { QStringLiteral("libraries"), QStringLiteral("results") });
    for (const QJsonValue& v : arr)
    {
        const QJsonObject o = v.toObject();
        Library l;
        l.id        = o.value(QStringLiteral("id")).toString();
        l.name      = o.value(QStringLiteral("name")).toString();
        l.mediaType = o.value(QStringLiteral("mediaType")).toString();
        if (l.id.isEmpty()) continue;   // a row nothing can be fetched for is worse than one row fewer
        out.push_back(l);
    }
    return out;
}

// One library-item object -> our row. Shared by the listing reader and the single-item reader, because a
// listing row and an expanded row are the same object with more in it, and two readers would drift.
static Abs::Item readItemObject(const QJsonObject& o)
{
    Abs::Item it;
    it.id = o.value(QStringLiteral("id")).toString();
    const QJsonObject media = o.value(QStringLiteral("media")).toObject();
    const QJsonObject meta  = media.value(QStringLiteral("metadata")).toObject();
    it.title    = meta.value(QStringLiteral("title")).toString();
    it.author   = meta.value(QStringLiteral("authorName")).toString();
    if (it.author.isEmpty()) it.author = meta.value(QStringLiteral("author")).toString();
    it.narrator = meta.value(QStringLiteral("narratorName")).toString();
    it.series   = meta.value(QStringLiteral("seriesName")).toString();
    it.seriesSequence = meta.value(QStringLiteral("sequence")).toString();
    it.duration   = media.value(QStringLiteral("duration")).toDouble();
    it.trackCount = media.value(QStringLiteral("numTracks")).toInt(
                        media.value(QStringLiteral("numAudioFiles")).toInt(0));
    it.episodeCount = media.value(QStringLiteral("numEpisodes")).toInt(0);
    const QString mt = o.value(QStringLiteral("mediaType")).toString();
    it.isPodcast = (mt == QLatin1String("podcast"));
    it.hasCover  = !media.value(QStringLiteral("coverPath")).toString().isEmpty()
                   || !o.value(QStringLiteral("coverPath")).toString().isEmpty();
    return it;
}

QVector<Abs::Item> Abs::readLibraryItems(const QByteArray& body)
{
    QVector<Item> out;
    const QJsonArray arr = arrayOf(objOf(body), { QStringLiteral("results"), QStringLiteral("items"),
                                                  QStringLiteral("libraryItems") });
    for (const QJsonValue& v : arr)
    {
        const Item it = readItemObject(v.toObject());
        if (it.id.isEmpty()) continue;
        out.push_back(it);
    }
    return out;
}

QVector<Abs::SeriesRow> Abs::readSeries(const QByteArray& body)
{
    QVector<SeriesRow> out;
    const QJsonArray arr = arrayOf(objOf(body), { QStringLiteral("results"), QStringLiteral("series") });
    for (const QJsonValue& v : arr)
    {
        const QJsonObject o = v.toObject();
        SeriesRow s;
        s.id   = o.value(QStringLiteral("id")).toString();
        s.name = o.value(QStringLiteral("name")).toString();
        // The count is the books array's length where the server sends one, and its own field otherwise.
        s.bookCount = o.value(QStringLiteral("books")).isArray()
                          ? o.value(QStringLiteral("books")).toArray().size()
                          : o.value(QStringLiteral("numBooks")).toInt(0);
        if (s.id.isEmpty()) continue;
        out.push_back(s);
    }
    return out;
}

QVector<Abs::AuthorRow> Abs::readAuthors(const QByteArray& body)
{
    QVector<AuthorRow> out;
    const QJsonArray arr = arrayOf(objOf(body), { QStringLiteral("authors"), QStringLiteral("results") });
    for (const QJsonValue& v : arr)
    {
        const QJsonObject o = v.toObject();
        AuthorRow a;
        a.id   = o.value(QStringLiteral("id")).toString();
        a.name = o.value(QStringLiteral("name")).toString();
        a.bookCount = o.value(QStringLiteral("numBooks")).toInt(
                          o.value(QStringLiteral("books")).toArray().size());
        if (a.id.isEmpty()) continue;
        out.push_back(a);
    }
    return out;
}

// The track list and the chapter list, out of whichever object carries them — a play session carries them
// at its root, an expanded library item under `media`. ONE reader each, shared by both callers, because
// two readers of one shape is how a field read correctly in one place stops being read in the other.
static QVector<Abs::Track> readTrackArray(const QJsonArray& tracks)
{
    QVector<Abs::Track> out;
    for (const QJsonValue& v : tracks)
    {
        const QJsonObject t = v.toObject();
        Abs::Track tr;
        tr.index       = t.value(QStringLiteral("index")).toInt(out.size() + 1);
        tr.contentUrl  = t.value(QStringLiteral("contentUrl")).toString();
        tr.startOffset = t.value(QStringLiteral("startOffset")).toDouble();
        tr.duration    = t.value(QStringLiteral("duration")).toDouble();
        tr.mimeType    = t.value(QStringLiteral("mimeType")).toString();
        tr.title       = t.value(QStringLiteral("title")).toString();
        if (tr.title.isEmpty())
            tr.title = t.value(QStringLiteral("metadata")).toObject()
                        .value(QStringLiteral("filename")).toString();
        if (tr.contentUrl.isEmpty()) continue;   // nothing can be minted from it: not a part
        out.push_back(tr);
    }
    // THE SERVER'S ORDER IS THE BOOK'S ORDER, and it is `index` that says so — not the array order and
    // certainly not the file name. RemoteAudiobook::playableParts sorts a TORRENT release by name because
    // a name is all a release gives; a play session gives an explicit ordinal and a start offset, and
    // re-deriving the order from names here would be strictly worse information.
    std::stable_sort(out.begin(), out.end(),
                     [](const Abs::Track& a, const Abs::Track& b) { return a.index < b.index; });
    return out;
}

static QVector<Abs::Chapter> readChapterArray(const QJsonArray& chapters)
{
    QVector<Abs::Chapter> out;
    for (const QJsonValue& v : chapters)
    {
        const QJsonObject c = v.toObject();
        Abs::Chapter ch;
        ch.start = c.value(QStringLiteral("start")).toDouble();
        ch.end   = c.value(QStringLiteral("end")).toDouble();
        ch.title = c.value(QStringLiteral("title")).toString();
        if (ch.end <= ch.start) continue;      // a zero-length chapter is not a region anything can be in
        out.push_back(ch);
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const Abs::Chapter& a, const Abs::Chapter& b) { return a.start < b.start; });
    return out;
}

Abs::ItemDetail Abs::readItem(const QByteArray& body)
{
    ItemDetail d;
    const QJsonObject o = objOf(body);
    if (o.isEmpty()) return d;
    d.item = readItemObject(o);
    if (d.item.id.isEmpty()) return d;
    d.ok = true;
    const QJsonObject media = o.value(QStringLiteral("media")).toObject();
    d.tracks   = readTrackArray(arrayOf(media, { QStringLiteral("tracks"),
                                                 QStringLiteral("audioTracks") }));
    d.chapters = readChapterArray(arrayOf(media, { QStringLiteral("chapters") }));
    if (d.item.trackCount <= 0) d.item.trackCount = d.tracks.size();
    const QJsonArray eps = arrayOf(o.value(QStringLiteral("media")).toObject(),
                                   { QStringLiteral("episodes") });
    for (const QJsonValue& v : eps)
    {
        const QJsonObject e = v.toObject();
        Episode ep;
        ep.id        = e.value(QStringLiteral("id")).toString();
        ep.title     = e.value(QStringLiteral("title")).toString();
        ep.subtitle  = e.value(QStringLiteral("subtitle")).toString();
        ep.published = e.value(QStringLiteral("pubDate")).toString();
        ep.duration  = num(e.value(QStringLiteral("duration")));
        if (ep.duration <= 0.0)
            ep.duration = num(e.value(QStringLiteral("audioFile")).toObject()
                               .value(QStringLiteral("duration")));
        if (ep.id.isEmpty()) continue;
        d.episodes.push_back(ep);
    }
    if (!d.episodes.isEmpty()) { d.item.isPodcast = true; d.item.episodeCount = d.episodes.size(); }
    return d;
}

Abs::Session Abs::readPlaySession(const QByteArray& body)
{
    Session s;
    const QJsonObject o = objOf(body);
    if (o.isEmpty()) return s;
    s.id          = o.value(QStringLiteral("id")).toString();
    s.duration    = num(o.value(QStringLiteral("duration")));
    s.currentTime = num(o.value(QStringLiteral("currentTime")));
    // The item's own title, wherever this server puts it: on the embedded `libraryItem` (what a real
    // Audiobookshelf sends) or on the session itself (what its own sessions list is keyed by).
    s.title = o.value(QStringLiteral("libraryItem")).toObject()
               .value(QStringLiteral("media")).toObject()
               .value(QStringLiteral("metadata")).toObject()
               .value(QStringLiteral("title")).toString();
    if (s.title.isEmpty()) s.title = o.value(QStringLiteral("displayTitle")).toString();

    s.tracks   = readTrackArray(arrayOf(o, { QStringLiteral("audioTracks"), QStringLiteral("tracks") }));
    s.chapters = readChapterArray(arrayOf(o, { QStringLiteral("chapters") }));
    s.ok = !s.tracks.isEmpty();
    return s;
}

Abs::Progress Abs::readProgress(const QByteArray& body)
{
    Progress p;
    const QJsonObject o = objOf(body);
    // A user who has never opened this item gets an EMPTY object (or a 404 the caller turns into one).
    // "Never opened" is not "at zero": the difference decides whether the server's answer wins over a
    // local mark, so it is carried in `found` rather than collapsed into a position of 0.
    if (o.isEmpty() || !o.contains(QStringLiteral("currentTime"))) return p;
    p.found       = true;
    p.currentTime = num(o.value(QStringLiteral("currentTime")));
    p.duration    = num(o.value(QStringLiteral("duration")));
    p.finished    = o.value(QStringLiteral("isFinished")).toBool();
    return p;
}

// ==================================================================================================
// The book's timeline
// ==================================================================================================
double Abs::absoluteTime(const QVector<Track>& tracks, int trackIndex, double within)
{
    if (trackIndex < 0 || trackIndex >= tracks.size()) return std::max(0.0, within);
    return tracks.at(trackIndex).startOffset + std::max(0.0, within);
}

int Abs::trackAtTime(const QVector<Track>& tracks, double absolute)
{
    if (tracks.isEmpty()) return -1;
    if (absolute <= 0.0) return 0;
    // The LAST track that starts at or before the position. Walked rather than short-circuited on the
    // first later track, so a session whose offsets are not monotonic still resolves to the greatest
    // qualifying start rather than to whatever happened to be first (SleepTimer.h makes the same choice
    // over a chapter list, for the same reason).
    int best = 0;
    for (int i = 0; i < tracks.size(); ++i)
        if (tracks.at(i).startOffset <= absolute) best = i;
    return best;
}

double Abs::offsetWithinTrack(const QVector<Track>& tracks, double absolute)
{
    const int i = trackAtTime(tracks, absolute);
    if (i < 0) return 0.0;
    const Track& t = tracks.at(i);
    double within = absolute - t.startOffset;
    if (within < 0.0) within = 0.0;
    // Clamped INSIDE the track. A position past this track's end means the book was re-scanned shorter (or
    // the server's own total disagrees with its tracks); resuming one second before the end of the last
    // track is recoverable, and seeking past the end of the file mpv holds is not.
    if (t.duration > 0.0 && within > t.duration) within = std::max(0.0, t.duration - 1.0);
    return within;
}

QVector<Abs::Chapter> Abs::chaptersForTrack(const QVector<Chapter>& chapters, double trackStart,
                                            double trackDuration)
{
    QVector<Chapter> out;
    if (trackDuration <= 0.0) return out;
    const double trackEnd = trackStart + trackDuration;
    for (const Chapter& c : chapters)
    {
        if (c.end <= trackStart) continue;     // wholly before this file
        if (c.start >= trackEnd) continue;     // wholly after it
        Chapter r;
        r.title = c.title;
        r.start = std::max(0.0, c.start - trackStart);                 // a straddling chapter clamps to 0
        r.end   = std::min(trackDuration, c.end - trackStart);
        if (r.end <= r.start) continue;
        out.push_back(r);
    }
    return out;
}

// ==================================================================================================
// When to tell the server
// ==================================================================================================
bool Abs::shouldReport(bool everSent, double lastSentPos, qint64 lastSentMs, double pos, qint64 nowMs)
{
    if (!everSent) return true;
    if (std::abs(pos - lastSentPos) >= kSeekJumpS) return true;
    return (nowMs - lastSentMs) >= kReportIntervalMs;
}
