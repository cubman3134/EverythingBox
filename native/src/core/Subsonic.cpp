#include "Subsonic.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>
#include <QXmlStreamReader>

#include <algorithm>

namespace {

const char* kKindArtist   = "artist";
const char* kKindAlbum    = "album";
const char* kKindTrack    = "track";
const char* kKindCover    = "cover";
const char* kKindPlaylist = "playlist";
const char* kKindVirtual  = "virtual";

QString kindWord(Subsonic::Kind k)
{
    switch (k)
    {
        case Subsonic::Kind::Artist:   return QString::fromLatin1(kKindArtist);
        case Subsonic::Kind::Album:    return QString::fromLatin1(kKindAlbum);
        case Subsonic::Kind::Track:    return QString::fromLatin1(kKindTrack);
        case Subsonic::Kind::Cover:    return QString::fromLatin1(kKindCover);
        case Subsonic::Kind::Playlist: return QString::fromLatin1(kKindPlaylist);
        case Subsonic::Kind::Virtual:  return QString::fromLatin1(kKindVirtual);
    }
    return QString();
}

bool kindFromWord(const QString& w, Subsonic::Kind& out)
{
    if (w == QLatin1String(kKindArtist))   { out = Subsonic::Kind::Artist;   return true; }
    if (w == QLatin1String(kKindAlbum))    { out = Subsonic::Kind::Album;    return true; }
    if (w == QLatin1String(kKindTrack))    { out = Subsonic::Kind::Track;    return true; }
    if (w == QLatin1String(kKindCover))    { out = Subsonic::Kind::Cover;    return true; }
    if (w == QLatin1String(kKindPlaylist)) { out = Subsonic::Kind::Playlist; return true; }
    if (w == QLatin1String(kKindVirtual))  { out = Subsonic::Kind::Virtual;  return true; }
    return false;
}

// A JSON scalar as the string an XML attribute would have carried. Subsonic's JSON renders the SAME values
// as its XML attributes, but typed — songCount is 12 in JSON and "12" in XML — so the node model has to
// flatten them to one spelling or every reader would need two branches. Numbers keep their integer form
// (12.0 would fail attrInt), booleans become the XML spelling.
QString scalarText(const QJsonValue& v)
{
    if (v.isString()) return v.toString();
    if (v.isBool())   return v.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    if (v.isDouble())
    {
        const double d = v.toDouble();
        const qint64 i = qint64(d);
        if (double(i) == d) return QString::number(i);
        return QString::number(d);
    }
    return QString();
}

void jsonInto(Subsonic::Node& parent, const QString& key, const QJsonValue& v)
{
    if (v.isObject())
    {
        Subsonic::Node n;
        n.name = key;
        const QJsonObject o = v.toObject();
        for (auto it = o.begin(); it != o.end(); ++it) jsonInto(n, it.key(), it.value());
        parent.kids.push_back(n);
        return;
    }
    if (v.isArray())
    {
        // An array member becomes N SIBLING children all named after the key — which is exactly the shape
        // the XML has ("song" repeated inside "album"), and the whole reason both encodings can be read by
        // one set of payload readers.
        const QJsonArray a = v.toArray();
        for (const QJsonValue& e : a)
        {
            if (e.isObject() || e.isArray()) jsonInto(parent, key, e);
            else
            {
                Subsonic::Node n; n.name = key;
                n.attrs.insert(QStringLiteral("value"), scalarText(e));
                parent.kids.push_back(n);
            }
        }
        return;
    }
    parent.attrs.insert(key, scalarText(v));
}

} // namespace

// ==================================================================================================
// Ids
// ==================================================================================================

QString Subsonic::qualify(const QString& serverId, Kind kind, const QString& remoteId)
{
    if (serverId.isEmpty() || remoteId.isEmpty()) return QString();
    const QChar us = idSep();
    return QStringLiteral("sub") + us + serverId + us + kindWord(kind) + us + remoteId;
}

Subsonic::Ref Subsonic::parse(const QString& qualified)
{
    Ref r;
    const QChar us = idSep();
    // Four fields, and the remote half is EVERYTHING after the third separator — see the header. Splitting
    // on every separator and taking index 3 would truncate a remote id that contains one.
    const int a = qualified.indexOf(us);
    // Exactly 3: the first field must be the literal "sub", so its separator can only be at index 3. Cheaper
    // than a substring compare and it rejects "substitute<US>..." without allocating.
    if (a != 3 || !qualified.startsWith(QLatin1String("sub"))) return r;
    const int b = qualified.indexOf(us, a + 1);
    if (b < 0) return r;
    const int c = qualified.indexOf(us, b + 1);
    if (c < 0) return r;

    const QString server = qualified.mid(a + 1, b - a - 1);
    // The uuid test is what makes a qualified id structurally un-confusable with a MusicLibrary key: an
    // album key's second field is "t" or "d", a work key's is "w" or "a", and none of those is a uuid. It
    // is also the cheapest possible rejection of a half-formed id.
    const QUuid u = QUuid::fromString(server);
    if (u.isNull()) return r;

    Kind k;
    if (!kindFromWord(qualified.mid(b + 1, c - b - 1), k)) return r;

    const QString remote = qualified.mid(c + 1);
    if (remote.isEmpty()) return r;

    r.serverId = server;
    r.kind     = k;
    r.remoteId = remote;
    r.ok       = true;
    return r;
}

QString Subsonic::streamPath() { return QStringLiteral("/rest/stream.view"); }

// The url reader (#203). Deliberately conservative in three ways, because its answer becomes the identity a
// user's saved playlist row is filed under:
//
//   * the ROOT must match a configured server exactly (see the header). A url from a server this install has
//     never heard of is not guessed at;
//   * the PATH must be the stream endpoint. Any other endpoint (getCoverArt, download.view) names something
//     that is not a track, and calling it one would mint an id that resolves to nothing;
//   * `id` must be present and non-empty. qualify() refuses a half-formed id anyway, so the failure is an
//     empty string either way — but refusing here says why.
QString Subsonic::trackIdFromStreamUrl(const QString& url, const QVector<QPair<QString, QString>>& serverRoots)
{
    if (url.isEmpty() || serverRoots.isEmpty()) return QString();
    const QUrl u(url);
    if (!u.isValid() || u.scheme().isEmpty()) return QString();

    // The part of the url BEFORE the query — cut at the first '?' or '#', the same rule StoredUrl::location
    // uses and for the same reason (a fragment may precede a malformed query). Working on the raw string
    // rather than on QUrl::path() is what lets the root be compared byte for byte against the string
    // normalizeRoot handed the builder: QUrl re-encodes what it round-trips, and a root that came back
    // spelled differently would silently match no server at all.
    int end = url.size();
    for (int i = 0; i < url.size(); ++i)
        if (url.at(i) == QLatin1Char('?') || url.at(i) == QLatin1Char('#')) { end = i; break; }
    QString before = url.left(end);
    while (before.endsWith(QLatin1Char('/'))) before.chop(1);
    const QString tail = streamPath();
    if (!before.endsWith(tail)) return QString();
    const QString root = before.left(before.size() - tail.size());
    if (root.isEmpty()) return QString();

    const QString id = QUrlQuery(u).queryItemValue(QStringLiteral("id"), QUrl::FullyDecoded);
    if (id.isEmpty()) return QString();

    for (const QPair<QString, QString>& s : serverRoots)
        if (!s.second.isEmpty() && s.second == root) return qualify(s.first, Kind::Track, id);
    return QString();
}

// ==================================================================================================
// Auth
// ==================================================================================================

QString Subsonic::tokenFor(const QString& password, const QString& salt)
{
    // An empty password must not produce a well-formed token; see the declaration.
    if (password.isEmpty() || salt.isEmpty()) return QString();
    const QByteArray in = password.toUtf8() + salt.toUtf8();
    return QString::fromLatin1(QCryptographicHash::hash(in, QCryptographicHash::Md5).toHex());
}

QString Subsonic::saltFrom(quint64 seed)
{
    // 16 hex characters out of the caller's 64 bits. Deterministic in `seed` on purpose: the randomness is
    // the caller's business, and a probe that could not pin a salt could not pin the token either.
    return QString::fromLatin1(QByteArray::number(qulonglong(seed), 16).rightJustified(16, '0'));
}

QString Subsonic::stableSalt(const QString& subject)
{
    // MD5 of the subject, first 16 hex characters. Derived from (server, track) and NEVER from the password:
    // a salt is public, and one derived from the secret would leak a function of it into every url.
    const QByteArray h = QCryptographicHash::hash(subject.toUtf8(), QCryptographicHash::Md5).toHex();
    return QString::fromLatin1(h.left(16));
}

QList<QPair<QString, QString>> Subsonic::authParams(const QString& user, const QString& password,
                                                    const QString& salt, bool legacy, const QString& client)
{
    QList<QPair<QString, QString>> out;
    out.push_back({ QStringLiteral("u"), user });
    if (legacy)
    {
        // The old plaintext parameter, hex-encoded behind "enc:" — the form every server that predates the
        // token scheme understands. A per-server opt-in; nothing falls back to it on a refusal.
        out.push_back({ QStringLiteral("p"),
                        QStringLiteral("enc:") + QString::fromLatin1(password.toUtf8().toHex()) });
    }
    else
    {
        out.push_back({ QStringLiteral("t"), tokenFor(password, salt) });
        out.push_back({ QStringLiteral("s"), salt });
    }
    out.push_back({ QStringLiteral("v"), QStringLiteral("1.16.1") });
    out.push_back({ QStringLiteral("c"), client });
    // Ask for JSON. The reader tolerates XML anyway — see the header — because this is a request, not a
    // guarantee, and the servers that ignore it are exactly the ones a client is most likely to meet.
    out.push_back({ QStringLiteral("f"), QStringLiteral("json") });
    return out;
}

// ==================================================================================================
// Transport safety
// ==================================================================================================

Subsonic::UrlVerdict Subsonic::checkUrl(const QString& url, bool allowPlainHttp)
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

QString Subsonic::normalizeRoot(const QString& url, bool allowPlainHttp)
{
    if (checkUrl(url, allowPlainHttp) != UrlVerdict::Ok) return QString();
    QString t = url.trimmed();
    while (t.endsWith(QLatin1Char('/'))) t.chop(1);
    return t;
}

// ==================================================================================================
// The response
// ==================================================================================================

int Subsonic::Node::attrInt(const QString& k, int def) const
{
    bool ok = false;
    const int v = attrs.value(k).toInt(&ok);
    return ok ? v : def;
}

const Subsonic::Node* Subsonic::Node::find(const QString& n) const
{
    for (const Node& k : kids) if (k.name == n) return &k;
    for (const Node& k : kids) if (const Node* d = k.find(n)) return d;
    return nullptr;
}

QVector<const Subsonic::Node*> Subsonic::Node::findAll(const QString& n) const
{
    QVector<const Node*> out;
    for (const Node& k : kids)
    {
        if (k.name == n) out.push_back(&k);
        else             out += k.findAll(n);   // a match does not recurse into itself: a song holds no songs
    }
    return out;
}

Subsonic::Node Subsonic::parseXml(const QByteArray& body, bool* ok)
{
    if (ok) *ok = false;
    Node root;
    QXmlStreamReader r(body);
    QVector<Node*> stack;
    while (!r.atEnd())
    {
        const auto t = r.readNext();
        if (t == QXmlStreamReader::StartElement)
        {
            Node n;
            n.name = r.name().toString();
            for (const QXmlStreamAttribute& a : r.attributes())
                n.attrs.insert(a.name().toString(), a.value().toString());
            if (stack.isEmpty())
            {
                root = n;
                stack.push_back(&root);
            }
            else
            {
                stack.last()->kids.push_back(n);
                stack.push_back(&stack.last()->kids.last());
            }
        }
        else if (t == QXmlStreamReader::EndElement)
        {
            if (!stack.isEmpty()) stack.pop_back();
        }
    }
    if (r.hasError() || root.name.isEmpty()) return Node{};
    if (ok) *ok = true;
    return root;
}

Subsonic::Node Subsonic::parseJson(const QByteArray& body, bool* ok)
{
    if (ok) *ok = false;
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(body, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return Node{};
    const QJsonObject o = doc.object();
    // Subsonic's JSON wraps everything in one member named after the XML root element, so the tree below
    // has the SAME shape as the XML one and the payload readers cannot tell them apart.
    const QString rootKey = QStringLiteral("subsonic-response");
    if (!o.contains(rootKey)) return Node{};
    Node holder;
    jsonInto(holder, rootKey, o.value(rootKey));
    if (holder.kids.isEmpty()) return Node{};
    if (ok) *ok = true;
    return holder.kids.first();
}

Subsonic::Node Subsonic::parseBody(const QByteArray& body, bool* ok)
{
    if (ok) *ok = false;
    QByteArray t = body.trimmed();
    if (t.isEmpty()) return Node{};
    // Sniff on the first meaningful byte. '{' is JSON, '<' is XML (or an HTML error page from a proxy in
    // front of the server, which parses to something that is not a subsonic-response and is refused by
    // envelopeOf — the right answer either way).
    if (t.startsWith('{')) return parseJson(body, ok);
    if (t.startsWith('<')) return parseXml(body, ok);
    // Neither. Try both anyway rather than guessing wrong on a body with a BOM or leading junk.
    bool got = false;
    Node n = parseJson(body, &got);
    if (got) { if (ok) *ok = true; return n; }
    return parseXml(body, ok);
}

Subsonic::Envelope Subsonic::envelopeOf(const Node& root)
{
    Envelope e;
    if (root.name != QLatin1String("subsonic-response")) return e;   // Unparsable: not our envelope at all
    e.version = root.attr(QStringLiteral("version"));
    const QString st = root.attr(QStringLiteral("status"));
    if (st == QLatin1String("ok")) { e.status = Status::Ok; return e; }
    // ANY non-ok status is a failure, including a missing one: a subsonic-response that does not say it
    // succeeded did not succeed, and treating "no status attribute" as success is the 200-means-fine bug
    // arriving through the back door.
    e.status = Status::Failed;
    if (const Node* err = root.find(QStringLiteral("error")))
    {
        e.code    = err->attrInt(QStringLiteral("code"));
        e.message = err->attr(QStringLiteral("message"));
    }
    return e;
}

bool Subsonic::isAuthCode(int code)
{
    return code == 40 || code == 41 || code == 42 || code == 43 || code == 44;
}

// ==================================================================================================
// The payloads
// ==================================================================================================

QVector<Subsonic::RemoteArtist> Subsonic::readArtists(const Node& root)
{
    QVector<RemoteArtist> out;
    for (const Node* n : root.findAll(QStringLiteral("artist")))
    {
        RemoteArtist a;
        a.id         = n->attr(QStringLiteral("id"));
        a.name       = n->attr(QStringLiteral("name"));
        a.coverArt   = n->attr(QStringLiteral("coverArt"));
        a.albumCount = n->attrInt(QStringLiteral("albumCount"));
        // The artist's MusicBrainz id (#194). Navidrome and other OpenSubsonic servers emit it; the original
        // spec does not require it, so an absent attribute is the ordinary case and is simply empty.
        a.musicBrainzId = n->attr(QStringLiteral("musicBrainzId"));
        if (a.id.isEmpty()) continue;   // an artist with no id cannot be opened; a row that cannot be
                                        // pressed is worse than an absent one
        out.push_back(a);
    }
    return out;
}

QVector<Subsonic::RemoteAlbum> Subsonic::readAlbums(const Node& root)
{
    QVector<RemoteAlbum> out;
    for (const Node* n : root.findAll(QStringLiteral("album")))
    {
        RemoteAlbum b;
        b.id          = n->attr(QStringLiteral("id"));
        b.name        = n->attr(QStringLiteral("name"));
        if (b.name.isEmpty()) b.name = n->attr(QStringLiteral("album"));
        b.artist      = n->attr(QStringLiteral("artist"));
        b.artistId    = n->attr(QStringLiteral("artistId"));
        b.coverArt    = n->attr(QStringLiteral("coverArt"));
        b.songCount   = n->attrInt(QStringLiteral("songCount"));
        b.year        = n->attrInt(QStringLiteral("year"));
        b.durationSec = n->attrInt(QStringLiteral("duration"));
        // An album's musicBrainzId is the RELEASE id, not the release group's — which is why MusicId keeps
        // the two apart and only ever compares like with like (see MusicId.h).
        b.musicBrainzId = n->attr(QStringLiteral("musicBrainzId"));
        if (b.id.isEmpty()) continue;
        out.push_back(b);
    }
    return out;
}

QVector<Subsonic::RemoteSong> Subsonic::readSongs(const Node& root)
{
    QVector<RemoteSong> out;
    // "song" is the ID3 spelling (getAlbum, search3, getStarred2); "entry" is what a PLAYLIST's tracks are
    // called (getPlaylist, #193 increment 6); "child" is the folder spelling (getMusicDirectory). Three
    // spellings of the same element, and reading all three costs two extra walks and makes this work against
    // a playlist, an album and a server whose ID3 endpoints are off - with ONE reader rather than three that
    // would drift. The order is most-specific-first; a reply carries only one of them.
    QVector<const Node*> nodes = root.findAll(QStringLiteral("song"));
    if (nodes.isEmpty()) nodes = root.findAll(QStringLiteral("entry"));
    if (nodes.isEmpty()) nodes = root.findAll(QStringLiteral("child"));
    for (const Node* n : nodes)
    {
        RemoteSong s;
        s.id          = n->attr(QStringLiteral("id"));
        s.title       = n->attr(QStringLiteral("title"));
        s.artist      = n->attr(QStringLiteral("artist"));
        s.album       = n->attr(QStringLiteral("album"));
        s.albumId     = n->attr(QStringLiteral("albumId"));
        s.coverArt    = n->attr(QStringLiteral("coverArt"));
        s.contentType = n->attr(QStringLiteral("contentType"));
        s.suffix      = n->attr(QStringLiteral("suffix"));
        s.track       = n->attrInt(QStringLiteral("track"));
        s.disc        = n->attrInt(QStringLiteral("discNumber"));
        s.year        = n->attrInt(QStringLiteral("year"));
        s.durationSec = n->attrInt(QStringLiteral("duration"));
        if (s.id.isEmpty()) continue;
        // A "child" row can be a FOLDER (getMusicDirectory lists both). A directory has no duration and no
        // suffix and must never reach a queue as if it were audio.
        if (n->attr(QStringLiteral("isDir")) == QLatin1String("true")) continue;
        if (s.title.isEmpty()) s.title = n->attr(QStringLiteral("name"));
        out.push_back(s);
    }
    return out;
}

// ==================================================================================================
// Onto the existing catalog shapes
// ==================================================================================================

MusicLibrary::Index Subsonic::indexOfArtists(const QString& serverId, const QVector<RemoteArtist>& artists)
{
    MusicLibrary::Index idx;
    for (const RemoteArtist& a : artists)
    {
        const QString key = qualify(serverId, Kind::Artist, a.id);
        if (key.isEmpty()) continue;
        MusicLibrary::Artist out;
        out.key        = key;
        out.name       = a.name;
        out.albumCount = a.albumCount;   // what the server told us; the albums themselves arrive on drill
        out.trackCount = 0;              // deliberate — see the header note in Subsonic.h
        out.mbid       = a.musicBrainzId;  // ground truth for the cross-source merge (#194), when served
        idx.artists.push_back(out);
        idx.albumCount += a.albumCount;
    }
    // Index::trackCount stays 0 on purpose: it gates "Shuffle all music", which cannot work over tracks
    // that have not been fetched. See the header.
    return idx;
}

void Subsonic::fillArtistAlbums(MusicLibrary::Index& idx, const QString& serverId, const QString& artistKey,
                                const QVector<RemoteAlbum>& albums)
{
    MusicLibrary::Artist* target = nullptr;
    for (MusicLibrary::Artist& a : idx.artists) if (a.key == artistKey) { target = &a; break; }
    if (!target) return;

    target->albums.clear();
    for (const RemoteAlbum& b : albums)
    {
        const QString key = qualify(serverId, Kind::Album, b.id);
        if (key.isEmpty()) continue;
        MusicLibrary::Album out;
        out.key         = key;
        out.albumArtist = b.artist.isEmpty() ? target->name : b.artist;
        out.title       = b.name;
        out.year        = b.year;
        out.durationSec = b.durationSec;
        out.trackCount  = b.songCount;   // the server's own count; `tracks` fills in on drill
        out.discCount   = 1;             // not known until the tracks are; a wrong count would print
        out.mbidRelease = b.musicBrainzId;   // the RELEASE id (#194); the release GROUP is not in this API
        out.artistMbid  = target->mbid;
        target->albums.push_back(out);
    }
    target->albumCount = int(target->albums.size());
}

void Subsonic::adoptAlbum(MusicLibrary::Index& idx, const QString& serverId, const RemoteAlbum& album,
                          const QVector<RemoteSong>& songs)
{
    const QString albumKey = qualify(serverId, Kind::Album, album.id);
    if (albumKey.isEmpty()) return;
    for (const MusicLibrary::Artist& a : idx.artists)
        for (const MusicLibrary::Album& b : a.albums)
            if (b.key == albumKey) { fillAlbumTracks(idx, serverId, albumKey, songs); return; }

    // The artist this record hangs off. The server's own artistId when it gave one; otherwise a key derived
    // from the ALBUM, which is unique per record and therefore cannot merge two unrelated artists together.
    const QString artistRemote = album.artistId.isEmpty() ? (QStringLiteral("album:") + album.id)
                                                          : album.artistId;
    const QString artistKey = qualify(serverId, Kind::Artist, artistRemote);
    MusicLibrary::Artist* target = nullptr;
    for (MusicLibrary::Artist& a : idx.artists) if (a.key == artistKey) { target = &a; break; }
    if (!target)
    {
        MusicLibrary::Artist a;
        a.key  = artistKey;
        a.name = album.artist;
        idx.artists.push_back(a);
        target = &idx.artists.last();
    }
    MusicLibrary::Album b;
    b.key         = albumKey;
    b.albumArtist = album.artist.isEmpty() ? target->name : album.artist;
    b.title       = album.name;
    b.year        = album.year;
    b.durationSec = album.durationSec;
    b.trackCount  = album.songCount;
    b.discCount   = 1;
    b.mbidRelease = album.musicBrainzId;   // (#194) — see fillArtistAlbums
    b.artistMbid  = target->mbid;
    target->albums.push_back(b);
    target->albumCount = int(target->albums.size());
    fillAlbumTracks(idx, serverId, albumKey, songs);
}

void Subsonic::fillAlbumTracks(MusicLibrary::Index& idx, const QString& serverId, const QString& albumKey,
                               const QVector<RemoteSong>& songs)
{
    MusicLibrary::Album* target = nullptr;
    for (MusicLibrary::Artist& a : idx.artists)
        for (MusicLibrary::Album& b : a.albums)
            if (b.key == albumKey) { target = &b; break; }
    if (!target) return;

    QVector<MusicLibrary::IndexTrack> tracks;
    int maxDisc = 1;
    for (const RemoteSong& s : songs)
    {
        const QString path = qualify(serverId, Kind::Track, s.id);
        if (path.isEmpty()) continue;
        MusicLibrary::IndexTrack t;
        // THE QUALIFIED ID, NOT A STREAM URL. A stream url carries the user's token and salt, and this
        // struct is copied into queues, into the now-playing state and (for a local library) onto disk.
        // MusicSupply::playUrl turns it into a signed url at the ONE moment mpv is handed it.
        t.path        = path;
        t.sourcePath  = path;
        t.title       = s.title.isEmpty() ? path : s.title;
        t.artist      = s.artist;
        t.albumKey    = albumKey;
        t.disc        = s.disc;
        t.track       = s.track;
        t.durationSec = s.durationSec;
        t.hasCover    = false;   // there is no local file to re-read art out of; the cover is fetched
        if (s.disc > maxDisc) maxDisc = s.disc;
        tracks.push_back(t);
    }
    // Disc, then track, then the server's own order — the same ordering rule MusicLibrary::buildIndex
    // applies, so an album from a server and an album from disk read the same way round.
    std::stable_sort(tracks.begin(), tracks.end(),
                     [](const MusicLibrary::IndexTrack& a, const MusicLibrary::IndexTrack& b) {
                         const int ad = a.disc > 0 ? a.disc : 1, bd = b.disc > 0 ? b.disc : 1;
                         if (ad != bd) return ad < bd;
                         // An untagged track number sorts AFTER the numbered ones, as it does locally.
                         const int at = a.track > 0 ? a.track : 1 << 30;
                         const int bt = b.track > 0 ? b.track : 1 << 30;
                         return at < bt;
                     });

    target->tracks    = tracks;
    target->discCount = maxDisc;
    // Now that the tracks are here, the count is the tracks themselves — so a server that under-reported
    // songCount cannot leave a subtitle disagreeing with the list under it.
    target->trackCount = int(tracks.size());
    int secs = 0;
    for (const MusicLibrary::IndexTrack& t : tracks) secs += t.durationSec;
    if (secs > 0) target->durationSec = secs;
}

// ==================================================================================================
// WHAT THE SERVER ALREADY KNOWS (issue #193, increment 6)
// ==================================================================================================

QVector<Subsonic::RemotePlaylist> Subsonic::readPlaylists(const Node& root)
{
    QVector<RemotePlaylist> out;
    for (const Node* n : root.findAll(QStringLiteral("playlist")))
    {
        RemotePlaylist p;
        p.id          = n->attr(QStringLiteral("id"));
        p.name        = n->attr(QStringLiteral("name"));
        p.comment     = n->attr(QStringLiteral("comment"));
        p.owner       = n->attr(QStringLiteral("owner"));
        p.coverArt    = n->attr(QStringLiteral("coverArt"));
        p.songCount   = n->attrInt(QStringLiteral("songCount"));
        p.durationSec = n->attrInt(QStringLiteral("duration"));
        if (p.id.isEmpty()) continue;   // a row that cannot be opened is worse than an absent row
        out.push_back(p);
    }
    return out;
}

namespace {

// One RemoteSong as the browse row it becomes, carrying the key of whatever record it is queued behind.
// Shared by the playlist level and the starred level so the two cannot describe a track differently; the
// ORDER is the caller's business, which is the whole reason this does not sort (see adoptPlaylist).
MusicLibrary::IndexTrack songRow(const QString& serverId, const Subsonic::RemoteSong& s,
                                 const QString& albumKey)
{
    MusicLibrary::IndexTrack t;
    // THE QUALIFIED ID, NOT A STREAM URL - the rule fillAlbumTracks states at length. A stream url carries
    // the token and the salt, and this struct is copied into queues and (for a saved playlist) onto disk.
    const QString path = Subsonic::qualify(serverId, Subsonic::Kind::Track, s.id);
    t.path        = path;
    t.sourcePath  = path;
    t.title       = s.title.isEmpty() ? path : s.title;
    t.artist      = s.artist;
    t.albumKey    = albumKey;
    t.disc        = s.disc;
    t.track       = s.track;
    t.durationSec = s.durationSec;
    t.hasCover    = false;
    return t;
}

// Find an artist bucket by key, creating it if it is not there. Returns a pointer INTO idx.artists, which is
// only valid until the vector next grows - every caller below uses it and drops it before pushing again.
MusicLibrary::Artist* bucketFor(MusicLibrary::Index& idx, const QString& key, const QString& name)
{
    for (MusicLibrary::Artist& a : idx.artists) if (a.key == key) return &a;
    MusicLibrary::Artist a;
    a.key  = key;
    a.name = name;
    idx.artists.push_back(a);
    return &idx.artists.last();
}

} // namespace

QVector<MusicLibrary::Album> Subsonic::playlistRows(const QString& serverId,
                                                    const QVector<RemotePlaylist>& playlists)
{
    QVector<MusicLibrary::Album> out;
    for (const RemotePlaylist& p : playlists)
    {
        const QString key = qualify(serverId, Kind::Playlist, p.id);
        if (key.isEmpty()) continue;
        MusicLibrary::Album b;
        b.key         = key;
        b.title       = p.name;
        // WHO MADE IT, in the album-artist slot, because that is where the album row's own builder reads the
        // second line from and a playlist's owner is the one fact that distinguishes two identically named
        // ones on a shared server. Empty for a personal server, which reads exactly as it did.
        b.albumArtist = p.owner;
        b.trackCount  = p.songCount;   // the server's own count; `tracks` fills in when it is opened
        b.durationSec = p.durationSec;
        b.discCount   = 1;
        out.push_back(b);
    }
    return out;
}

QVector<MusicLibrary::Album> Subsonic::albumRows(const QString& serverId,
                                                 const QVector<RemoteAlbum>& albums)
{
    QVector<MusicLibrary::Album> out;
    for (const RemoteAlbum& a : albums)
    {
        const QString key = qualify(serverId, Kind::Album, a.id);
        if (key.isEmpty()) continue;
        MusicLibrary::Album b;
        b.key         = key;
        b.albumArtist = a.artist;
        b.title       = a.name;
        b.year        = a.year;
        b.durationSec = a.durationSec;
        b.trackCount  = a.songCount;
        b.discCount   = 1;
        b.mbidRelease = a.musicBrainzId;
        out.push_back(b);
    }
    return out;
}

QVector<MusicLibrary::Artist> Subsonic::artistRows(const QString& serverId,
                                                   const QVector<RemoteArtist>& artists)
{
    QVector<MusicLibrary::Artist> out;
    for (const RemoteArtist& a : artists)
    {
        const QString key = qualify(serverId, Kind::Artist, a.id);
        if (key.isEmpty()) continue;
        MusicLibrary::Artist r;
        r.key        = key;
        r.name       = a.name;
        r.albumCount = a.albumCount;
        r.trackCount = 0;              // deliberate - see the note in Subsonic.h
        r.mbid       = a.musicBrainzId;
        out.push_back(r);
    }
    return out;
}

QString Subsonic::starredTracksKey(const QString& serverId)
{
    return qualify(serverId, Kind::Virtual, QStringLiteral("starred"));
}

Subsonic::Starred Subsonic::readStarred(const QString& serverId, const Node& root)
{
    Starred s;
    s.artists = artistRows(serverId, readArtists(root));
    s.albums  = albumRows(serverId, readAlbums(root));
    const QString key = starredTracksKey(serverId);
    for (const RemoteSong& song : readSongs(root))
    {
        const MusicLibrary::IndexTrack t = songRow(serverId, song, key);
        if (t.path.isEmpty()) continue;
        s.tracks.push_back(t);
    }
    return s;
}

void Subsonic::adoptStarred(MusicLibrary::Index& idx, const QString& serverId, const Starred& s)
{
    // The starred ARTISTS and ALBUMS need nothing here: both have real ids and open through the ordinary
    // artist and album routes, which adoptArtist and adoptAlbum already handle from a cold cache. Only the
    // loose tracks need a record invented for them.
    if (s.tracks.isEmpty()) return;

    // The one invented record. IDEMPOTENT by replacement rather than by append: this is re-run whenever a
    // getArtists has replaced the cache wholesale, and an append would give the level two copies of every
    // starred track the second time it was opened.
    const QString albumKey  = starredTracksKey(serverId);
    const QString bucketKey = qualify(serverId, Kind::Virtual, QStringLiteral("starredbucket"));
    MusicLibrary::Artist* bucket = bucketFor(idx, bucketKey, QString());
    MusicLibrary::Album* target = nullptr;
    for (MusicLibrary::Album& b : bucket->albums) if (b.key == albumKey) { target = &b; break; }
    if (!target)
    {
        MusicLibrary::Album b;
        b.key = albumKey;
        bucket->albums.push_back(b);
        target = &bucket->albums.last();
    }
    target->title      = QStringLiteral("Starred");
    target->tracks     = s.tracks;
    target->trackCount = int(s.tracks.size());
    target->discCount  = 1;
    int secs = 0;
    for (const MusicLibrary::IndexTrack& t : s.tracks) secs += t.durationSec;
    target->durationSec = secs;
    bucket->albumCount  = int(bucket->albums.size());
}

void Subsonic::adoptPlaylist(MusicLibrary::Index& idx, const QString& serverId,
                             const RemotePlaylist& playlist, const QVector<RemoteSong>& songs)
{
    const QString albumKey = qualify(serverId, Kind::Playlist, playlist.id);
    if (albumKey.isEmpty()) return;

    // The record, wherever it already is - the playlists level put it under the bucket below, but a Recents
    // row may be opening it in a session where that level was never visited.
    MusicLibrary::Album* target = nullptr;
    for (MusicLibrary::Artist& a : idx.artists)
        for (MusicLibrary::Album& b : a.albums)
            if (b.key == albumKey) { target = &b; break; }
    if (!target)
    {
        const QString bucketKey = qualify(serverId, Kind::Virtual, QStringLiteral("playlists"));
        MusicLibrary::Artist* bucket = bucketFor(idx, bucketKey, QString());
        MusicLibrary::Album b;
        b.key         = albumKey;
        b.title       = playlist.name;
        b.albumArtist = playlist.owner;
        bucket->albums.push_back(b);
        bucket->albumCount = int(bucket->albums.size());
        target = &bucket->albums.last();
    }
    if (!playlist.name.isEmpty()) target->title = playlist.name;

    // THE PLAYLIST'S OWN ORDER, AND THIS IS THE ONE PLACE IT MATTERS. fillAlbumTracks sorts by disc and then
    // track number, which is right for a record and destroys a playlist: the sequence somebody arranged is
    // the entire content of the thing, and re-sorting it by the track numbers the songs happen to carry on
    // their own albums would shuffle it into an order nobody chose and no server would agree with. So the
    // rows go in exactly as the server listed them.
    QVector<MusicLibrary::IndexTrack> tracks;
    for (const RemoteSong& s : songs)
    {
        const MusicLibrary::IndexTrack t = songRow(serverId, s, albumKey);
        if (t.path.isEmpty()) continue;
        tracks.push_back(t);
    }
    target->tracks     = tracks;
    target->trackCount = int(tracks.size());
    target->discCount  = 1;
    int secs = 0;
    for (const MusicLibrary::IndexTrack& t : tracks) secs += t.durationSec;
    if (secs > 0) target->durationSec = secs;
}

QVector<Subsonic::StarredFavorite> Subsonic::starredAdditions(const QString& serverId,
                                                              const QVector<RemoteSong>& songs,
                                                              const QSet<QString>& alreadyFavourite)
{
    QVector<StarredFavorite> out;
    QSet<QString> seen;
    for (const RemoteSong& s : songs)
    {
        // The favourite is filed under the SAME id a track row carries (MusicCatalogs' trackRow sets
        // MediaItem::id to IndexTrack::path), or the star would be recorded against something the heart on
        // the row cannot find - a favourite that exists and does not show.
        const QString id = qualify(serverId, Kind::Track, s.id);
        if (id.isEmpty() || alreadyFavourite.contains(id) || seen.contains(id)) continue;
        seen.insert(id);
        StarredFavorite f;
        f.itemId   = id;
        f.title    = s.title.isEmpty() ? id : s.title;
        f.subtitle = s.artist;
        out.push_back(f);
    }
    return out;
}

QList<QPair<QString, QString>> Subsonic::scrobbleParams(const QVector<QString>& remoteIds,
                                                        const QVector<qint64>& atUnixSeconds,
                                                        bool submission)
{
    QList<QPair<QString, QString>> out;
    for (int i = 0; i < remoteIds.size(); ++i)
    {
        if (remoteIds.at(i).isEmpty()) continue;
        out.append({ QStringLiteral("id"), remoteIds.at(i) });
        const qint64 at = i < atUnixSeconds.size() ? atUnixSeconds.at(i) : 0;
        // MILLISECONDS, which is the spec's unit. A non-positive time is OMITTED rather than sent as 0:
        // "no time" means "now" to every server, while 0 means January 1970, where the play is filed
        // half a century in the past and nothing anywhere complains about it.
        if (at > 0) out.append({ QStringLiteral("time"), QString::number(at * 1000LL) });
    }
    if (out.isEmpty()) return out;   // nothing to say: the caller must not make the request at all
    out.append({ QStringLiteral("submission"), submission ? QStringLiteral("true")
                                                          : QStringLiteral("false") });
    return out;
}

QList<QPair<QString, QString>> Subsonic::starParams(Kind kind, const QString& remoteId)
{
    if (remoteId.isEmpty()) return {};
    switch (kind)
    {
        // A SONG is `id`; an ID3 ALBUM and ARTIST have parameters of their own. The three namespaces are
        // independent, so sending an album's id as `id` stars whichever SONG happens to carry that id on
        // that server - a star that lands on something the user never pressed.
        case Kind::Track:  return { { QStringLiteral("id"),       remoteId } };
        case Kind::Album:  return { { QStringLiteral("albumId"),  remoteId } };
        case Kind::Artist: return { { QStringLiteral("artistId"), remoteId } };
        // A cover is not a thing that can be starred, and a Virtual container is one this app invented and
        // the server has never heard of - putting either in a request is the bug Kind::Virtual exists to
        // make impossible. Empty, so the caller does not make the request.
        case Kind::Cover:
        case Kind::Playlist:
        case Kind::Virtual: break;
    }
    return {};
}

Subsonic::Fate Subsonic::fateOf(const Envelope& env)
{
    switch (env.status)
    {
        case Status::Ok: return Fate::Ok;
        case Status::Unparsable:
            // Not a subsonic-response at all: a proxy's HTML error page, a captive portal, a truncated body.
            // RETRYABLE, because every one of those is a transport problem that goes away on its own, and
            // dropping the listens over it would lose them to a router reboot.
            return Fate::Retryable;
        case Status::Failed: break;
    }
    if (isAuthCode(env.code)) return Fate::Auth;
    // 70 "the requested data was not found". The track has been deleted or the library rescanned out from
    // under the queue; no amount of retrying brings it back, and keeping it would jam every listen behind it.
    if (env.code == 70) return Fate::Rejected;
    // 10 "required parameter is missing" and 0 "a generic error" are OURS to have got wrong, not the
    // network's - retrying them for ever would be a loop. Everything else (server error, upgrade required,
    // a trial expiring) is worth another attempt.
    if (env.code == 10) return Fate::Rejected;
    return Fate::Retryable;
}

void Subsonic::adoptArtist(MusicLibrary::Index& idx, const QString& serverId, const RemoteArtist& artist,
                           const QVector<RemoteAlbum>& albums)
{
    const QString artistKey = qualify(serverId, Kind::Artist, artist.id);
    if (artistKey.isEmpty()) return;
    MusicLibrary::Artist* target = bucketFor(idx, artistKey, artist.name);
    // The name and the mbid come from the server's own answer and overwrite whatever a colder route put
    // there: this reply is the authoritative one about this artist.
    if (!artist.name.isEmpty())          target->name = artist.name;
    if (!artist.musicBrainzId.isEmpty()) target->mbid = artist.musicBrainzId;
    fillArtistAlbums(idx, serverId, artistKey, albums);
}
