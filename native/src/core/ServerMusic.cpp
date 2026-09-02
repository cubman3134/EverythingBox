#include "ServerMusic.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QUrl>

#include <algorithm>

namespace
{
    QString kindWord(ServerMusic::Kind k)
    {
        switch (k)
        {
        case ServerMusic::Kind::Artist: return QString::fromLatin1(ServerMusic::kArtistType);
        case ServerMusic::Kind::Album:  return QString::fromLatin1(ServerMusic::kAlbumType);
        case ServerMusic::Kind::Track:  return QString::fromLatin1(ServerMusic::kTrackType);
        }
        return QString();
    }

    bool kindFromWord(const QString& w, ServerMusic::Kind& out)
    {
        if (w == QLatin1String(ServerMusic::kArtistType)) { out = ServerMusic::Kind::Artist; return true; }
        if (w == QLatin1String(ServerMusic::kAlbumType))  { out = ServerMusic::Kind::Album;  return true; }
        if (w == QLatin1String(ServerMusic::kTrackType))  { out = ServerMusic::Kind::Track;  return true; }
        return false;
    }

    // The protocol's catalogue envelope, or false when this body is not one. The distinction between "no
    // music" and "not an answer" is the whole reason `ok` exists — see the header.
    bool itemsArray(const QByteArray& body, QJsonArray& out)
    {
        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(body, &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) return false;
        const QJsonValue items = doc.object().value(QStringLiteral("items"));
        if (!items.isArray()) return false;
        out = items.toArray();
        return true;
    }

    QJsonObject metaOf(const QJsonObject& item)
    {
        return item.value(QStringLiteral("meta")).toObject();
    }

    // A number out of `meta`, tolerating the string form. A JSON shelf assembled by hand — and every one of
    // them is, at least once — writes "year": "1979" sooner or later, and reading that as 0 would put an
    // album into the merge with its year gate silently disarmed.
    int metaInt(const QJsonObject& meta, const char* key)
    {
        const QJsonValue v = meta.value(QString::fromLatin1(key));
        if (v.isDouble()) return int(v.toDouble());
        if (v.isString()) { bool ok = false; const int n = v.toString().toInt(&ok); return ok ? n : 0; }
        return 0;
    }

    QString metaStr(const QJsonObject& meta, const char* key)
    {
        return meta.value(QString::fromLatin1(key)).toString().trimmed();
    }

    // Rows of the level's own type only, and a row with no id is not a row. A shelf may legally mix a
    // heading or a verb into a level; reading one as an album would put a record in the merge that names
    // nothing. A row that declares NO type at all is accepted: a minimal shelf that sends `id` and `title`
    // and nothing else is exactly the case the header promises still works.
    bool rowIsA(const QJsonObject& o, const char* want)
    {
        const QString t = o.value(QStringLiteral("type")).toString().trimmed();
        return t.isEmpty() || t.compare(QString::fromLatin1(want), Qt::CaseInsensitive) == 0;
    }
}

QString ServerMusic::qualify(const QString& sourceId, Kind kind, const QString& remoteId)
{
    if (sourceId.isEmpty() || remoteId.isEmpty()) return QString();
    const QChar us = idSep();
    return QStringLiteral("ebs") + us + sourceId + us + kindWord(kind) + us + remoteId;
}

ServerMusic::Ref ServerMusic::parse(const QString& qualified)
{
    Ref r;
    const QChar us = idSep();
    // Four fields, and the remote half is EVERYTHING after the third separator — Subsonic.h's rule, for its
    // reason: splitting on every separator and taking index 3 would truncate a remote id containing one.
    const int a = qualified.indexOf(us);
    if (a != 3 || !qualified.startsWith(QLatin1String("ebs"))) return r;
    const int b = qualified.indexOf(us, a + 1);
    if (b < 0) return r;
    const int c = qualified.indexOf(us, b + 1);
    if (c < 0) return r;

    const QString source = qualified.mid(a + 1, b - a - 1);
    if (source.isEmpty()) return r;

    Kind k;
    // The kind word is the second half of what keeps this family structurally distinct from a MusicLibrary
    // key: an album key's middle field is "t" or "d" and a work key's is "w" or "a", none of which is one of
    // these three words. So a library whose album artist is literally "ebs" still cannot mint a key that
    // parses as one of these.
    if (!kindFromWord(qualified.mid(b + 1, c - b - 1), k)) return r;

    const QString remote = qualified.mid(c + 1);
    if (remote.isEmpty()) return r;

    r.sourceId = source;
    r.kind     = k;
    r.remoteId = remote;
    r.ok       = true;
    return r;
}

QString ServerMusic::catalogPath(const QString& catalogId)
{
    const QString id = catalogId.isEmpty() ? QStringLiteral("default") : catalogId;
    return QStringLiteral("/catalog/")
           + QString::fromLatin1(QUrl::toPercentEncoding(id)) + QStringLiteral(".json");
}

QString ServerMusic::detailPath(const QString& type, const QString& remoteId)
{
    const QString t = type.isEmpty() ? QStringLiteral("item") : type;
    return QStringLiteral("/detail/")
           + QString::fromLatin1(QUrl::toPercentEncoding(t)) + QStringLiteral("/")
           + QString::fromLatin1(QUrl::toPercentEncoding(remoteId)) + QStringLiteral(".json");
}

QVector<ServerMusic::RemoteArtist> ServerMusic::readArtists(const QByteArray& body, bool* ok)
{
    QVector<RemoteArtist> out;
    if (ok) *ok = false;
    QJsonArray items;
    if (!itemsArray(body, items)) return out;
    for (const QJsonValue& v : items)
    {
        if (!v.isObject()) continue;
        const QJsonObject o = v.toObject();
        if (!rowIsA(o, kArtistType)) continue;
        const QJsonObject m = metaOf(o);
        RemoteArtist a;
        a.id                  = o.value(QStringLiteral("id")).toString();
        a.name                = o.value(QStringLiteral("title")).toString();
        a.musicBrainzArtistId = metaStr(m, "musicBrainzArtistId");
        a.albumCount          = metaInt(m, "albumCount");
        if (a.id.isEmpty()) continue;
        out.push_back(a);
    }
    if (ok) *ok = true;
    return out;
}

QVector<ServerMusic::RemoteAlbum> ServerMusic::readAlbums(const QByteArray& body, bool* ok)
{
    QVector<RemoteAlbum> out;
    if (ok) *ok = false;
    QJsonArray items;
    if (!itemsArray(body, items)) return out;
    for (const QJsonValue& v : items)
    {
        if (!v.isObject()) continue;
        const QJsonObject o = v.toObject();
        if (!rowIsA(o, kAlbumType)) continue;
        const QJsonObject m = metaOf(o);
        RemoteAlbum b;
        b.id                        = o.value(QStringLiteral("id")).toString();
        b.name                      = o.value(QStringLiteral("title")).toString();
        b.artist                    = metaStr(m, "albumArtist");
        b.musicBrainzAlbumId        = metaStr(m, "musicBrainzAlbumId");
        b.musicBrainzReleaseGroupId = metaStr(m, "musicBrainzReleaseGroupId");
        b.musicBrainzArtistId       = metaStr(m, "musicBrainzArtistId");
        b.format                    = metaStr(m, "format").toUpper();
        b.coverUrl                  = o.value(QStringLiteral("thumbnailUrl")).toString();
        b.trackCount                = metaInt(m, "trackCount");
        b.year                      = metaInt(m, "year");
        b.durationSec               = metaInt(m, "durationSec");
        b.bitrateKbps               = metaInt(m, "bitrateKbps");
        if (b.id.isEmpty()) continue;
        out.push_back(b);
    }
    if (ok) *ok = true;
    return out;
}

QVector<ServerMusic::RemoteSong> ServerMusic::readSongs(const QByteArray& body, bool* ok)
{
    QVector<RemoteSong> out;
    if (ok) *ok = false;
    QJsonArray items;
    if (!itemsArray(body, items)) return out;
    for (const QJsonValue& v : items)
    {
        if (!v.isObject()) continue;
        const QJsonObject o = v.toObject();
        if (!rowIsA(o, kTrackType)) continue;
        const QJsonObject m = metaOf(o);
        RemoteSong s;
        s.id          = o.value(QStringLiteral("id")).toString();
        s.title       = o.value(QStringLiteral("title")).toString();
        s.url         = o.value(QStringLiteral("url")).toString();
        s.artist      = metaStr(m, "artist");
        s.format      = metaStr(m, "format").toUpper();
        s.track       = metaInt(m, "track");
        s.disc        = metaInt(m, "disc");
        s.durationSec = metaInt(m, "durationSec");
        s.bitrateKbps = metaInt(m, "bitrateKbps");
        if (s.id.isEmpty()) continue;
        out.push_back(s);
    }
    if (ok) *ok = true;
    return out;
}

MusicLibrary::Index ServerMusic::indexOfArtists(const QString& sourceId,
                                                const QVector<RemoteArtist>& artists)
{
    MusicLibrary::Index idx;
    for (const RemoteArtist& a : artists)
    {
        const QString key = qualify(sourceId, Kind::Artist, a.id);
        if (key.isEmpty()) continue;
        MusicLibrary::Artist out;
        out.key        = key;
        out.name       = a.name;
        out.albumCount = a.albumCount;
        out.trackCount = 0;             // deliberate — see the header
        out.mbid       = a.musicBrainzArtistId;
        idx.artists.push_back(out);
        idx.albumCount += a.albumCount;
    }
    return idx;
}

void ServerMusic::fillArtistAlbums(MusicLibrary::Index& idx, const QString& sourceId,
                                   const QString& artistKey, const QVector<RemoteAlbum>& albums)
{
    MusicLibrary::Artist* target = nullptr;
    for (MusicLibrary::Artist& a : idx.artists) if (a.key == artistKey) { target = &a; break; }
    if (!target) return;

    target->albums.clear();
    for (const RemoteAlbum& b : albums)
    {
        const QString key = qualify(sourceId, Kind::Album, b.id);
        if (key.isEmpty()) continue;
        MusicLibrary::Album out;
        out.key              = key;
        out.albumArtist      = b.artist.isEmpty() ? target->name : b.artist;
        out.title            = b.name;
        out.year             = b.year;
        out.durationSec      = b.durationSec;
        out.trackCount       = b.trackCount;
        out.discCount        = 1;
        out.mbidRelease      = b.musicBrainzAlbumId;
        out.mbidReleaseGroup = b.musicBrainzReleaseGroupId;
        out.artistMbid       = b.musicBrainzArtistId.isEmpty() ? target->mbid : b.musicBrainzArtistId;
        out.format           = b.format;
        out.bitrateKbps      = b.bitrateKbps;
        target->albums.push_back(out);
    }
    target->albumCount = int(target->albums.size());
}

void ServerMusic::fillAlbumTracks(MusicLibrary::Index& idx, const QString& sourceId,
                                  const QString& albumKey, const QVector<RemoteSong>& songs)
{
    MusicLibrary::Album* target = nullptr;
    for (MusicLibrary::Artist& a : idx.artists)
    {
        for (MusicLibrary::Album& b : a.albums) if (b.key == albumKey) { target = &b; break; }
        if (target) break;
    }
    if (!target) return;

    QVector<MusicLibrary::IndexTrack> tracks;
    int maxDisc = 1;
    for (const RemoteSong& s : songs)
    {
        const QString path = qualify(sourceId, Kind::Track, s.id);
        if (path.isEmpty()) continue;
        MusicLibrary::IndexTrack t;
        // THE QUALIFIED ID, NOT THE URL THE ROW CAME WITH. See the header's last section.
        t.path        = path;
        t.sourcePath  = path;
        t.title       = s.title.isEmpty() ? path : s.title;
        t.artist      = s.artist;
        t.albumKey    = albumKey;
        t.disc        = s.disc;
        t.track       = s.track;
        t.durationSec = s.durationSec;
        t.hasCover    = false;
        t.format      = s.format;
        t.bitrateKbps = s.bitrateKbps;
        if (s.disc > maxDisc) maxDisc = s.disc;
        tracks.push_back(t);
    }
    std::stable_sort(tracks.begin(), tracks.end(),
                     [](const MusicLibrary::IndexTrack& a, const MusicLibrary::IndexTrack& b) {
                         const int ad = a.disc > 0 ? a.disc : 1, bd = b.disc > 0 ? b.disc : 1;
                         if (ad != bd) return ad < bd;
                         const int at = a.track > 0 ? a.track : 1 << 30;
                         const int bt = b.track > 0 ? b.track : 1 << 30;
                         return at < bt;
                     });

    target->tracks     = tracks;
    target->discCount  = maxDisc;
    target->trackCount = int(tracks.size());
    // The album level's own claim wins when the shelf made one; otherwise the tracks decide it, and only
    // when they AGREE — the rule JellyfinMusic states at length, restated in one line because it is the
    // same rule and a supplier that quietly differed would put two different badges on one picker.
    if (target->format.isEmpty() || target->bitrateKbps == 0)
    {
        QString fmt;
        int     bits = 0;
        bool    first = true, fmtAgrees = true, bitsAgree = true;
        for (const MusicLibrary::IndexTrack& t : tracks)
        {
            if (first) { fmt = t.format; bits = t.bitrateKbps; first = false; continue; }
            if (t.format != fmt) fmtAgrees = false;
            if (t.bitrateKbps != bits) bitsAgree = false;
        }
        if (target->format.isEmpty() && fmtAgrees)     target->format      = fmt;
        if (target->bitrateKbps == 0   && bitsAgree)   target->bitrateKbps = bits;
    }
    int secs = 0;
    for (const MusicLibrary::IndexTrack& t : tracks) secs += t.durationSec;
    if (secs > 0) target->durationSec = secs;
}
