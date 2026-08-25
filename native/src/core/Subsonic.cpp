#include "Subsonic.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QUrl>
#include <QUuid>
#include <QXmlStreamReader>

#include <algorithm>

namespace {

const char* kKindArtist = "artist";
const char* kKindAlbum  = "album";
const char* kKindTrack  = "track";
const char* kKindCover  = "cover";

QString kindWord(Subsonic::Kind k)
{
    switch (k)
    {
        case Subsonic::Kind::Artist: return QString::fromLatin1(kKindArtist);
        case Subsonic::Kind::Album:  return QString::fromLatin1(kKindAlbum);
        case Subsonic::Kind::Track:  return QString::fromLatin1(kKindTrack);
        case Subsonic::Kind::Cover:  return QString::fromLatin1(kKindCover);
    }
    return QString();
}

bool kindFromWord(const QString& w, Subsonic::Kind& out)
{
    if (w == QLatin1String(kKindArtist)) { out = Subsonic::Kind::Artist; return true; }
    if (w == QLatin1String(kKindAlbum))  { out = Subsonic::Kind::Album;  return true; }
    if (w == QLatin1String(kKindTrack))  { out = Subsonic::Kind::Track;  return true; }
    if (w == QLatin1String(kKindCover))  { out = Subsonic::Kind::Cover;  return true; }
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
        if (b.id.isEmpty()) continue;
        out.push_back(b);
    }
    return out;
}

QVector<Subsonic::RemoteSong> Subsonic::readSongs(const Node& root)
{
    QVector<RemoteSong> out;
    // "song" is the ID3 spelling (getAlbum, search3); "child" is the folder spelling (getMusicDirectory).
    // Reading both costs one extra walk and makes this work against a server whose ID3 endpoints are off.
    QVector<const Node*> nodes = root.findAll(QStringLiteral("song"));
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
