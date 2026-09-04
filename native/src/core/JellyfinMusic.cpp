#include "JellyfinMusic.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>

namespace
{
    // Jellyfin measures duration in 100-nanosecond ticks. The ONE conversion in this feature; see the
    // header. Rounds to nearest so a 3:29.6 track does not read 3:29 while every other source rounds up.
    int secondsFromTicks(qint64 ticks)
    {
        if (ticks <= 0) return 0;
        return int((ticks + 5000000) / 10000000);
    }

    QString providerId(const QJsonObject& item, const char* key)
    {
        return item.value(QStringLiteral("ProviderIds")).toObject()
                   .value(QString::fromLatin1(key)).toString().trimmed();
    }

    // The first ARTIST name on a row. Jellyfin gives `AlbumArtist` (a string) on an album and `Artists` (an
    // array) on a track; a row may carry either, and an album by several artists carries both. Preferring
    // the singular is what keeps a compilation's album artist from becoming whichever performer happened to
    // be first — the same rule MusicLibrary applies to its own album grouping.
    QString firstArtist(const QJsonObject& o)
    {
        const QString single = o.value(QStringLiteral("AlbumArtist")).toString().trimmed();
        if (!single.isEmpty()) return single;
        const QJsonArray arr = o.value(QStringLiteral("Artists")).toArray();
        for (const QJsonValue& v : arr)
        {
            const QString s = v.toString().trimmed();
            if (!s.isEmpty()) return s;
        }
        return QString();
    }

    // The `Items` array, or a null value when this body is not an item envelope at all. Jellyfin::readItems
    // makes the same distinction for the same reason, restated because it is the difference between "your
    // server has no music" and "something in front of your server answered with an HTML error page".
    bool itemsArray(const QByteArray& body, QJsonArray& out)
    {
        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(body, &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) return false;
        const QJsonValue items = doc.object().value(QStringLiteral("Items"));
        if (!items.isArray()) return false;
        out = items.toArray();
        return true;
    }

    // The container this copy is in, upper-cased, from MediaSources. Empty when the server did not say —
    // an ABSENCE, never a guess: showing "MP3" beside a copy nobody asked about the format of is exactly
    // the kind of confident wrong answer the picker exists to avoid.
    void mediaFacts(const QJsonObject& o, QString& format, int& bitrateKbps)
    {
        format.clear();
        bitrateKbps = 0;
        const QJsonArray srcs = o.value(QStringLiteral("MediaSources")).toArray();
        if (!srcs.isEmpty() && srcs.first().isObject())
        {
            const QJsonObject s = srcs.first().toObject();
            format = s.value(QStringLiteral("Container")).toString().trimmed().toUpper();
            const qint64 bps = qint64(s.value(QStringLiteral("Bitrate")).toDouble());
            if (bps > 0) bitrateKbps = int((bps + 500) / 1000);
        }
        if (format.isEmpty() || bitrateKbps == 0)
        {
            // Some deployments answer with MediaStreams and no MediaSources (a /Items query that did not ask
            // for the latter, a plugin that rewrites the row). The AUDIO stream is the one that describes
            // the file; a cover-art stream would otherwise supply a codec of "mjpeg".
            for (const QJsonValue& v : o.value(QStringLiteral("MediaStreams")).toArray())
            {
                if (!v.isObject()) continue;
                const QJsonObject st = v.toObject();
                if (st.value(QStringLiteral("Type")).toString() != QStringLiteral("Audio")) continue;
                if (format.isEmpty())
                    format = st.value(QStringLiteral("Codec")).toString().trimmed().toUpper();
                if (bitrateKbps == 0)
                {
                    const qint64 bps = qint64(st.value(QStringLiteral("BitRate")).toDouble());
                    if (bps > 0) bitrateKbps = int((bps + 500) / 1000);
                }
                break;
            }
        }
    }
}

QVector<JellyfinMusic::RemoteArtist> JellyfinMusic::readArtists(const QByteArray& body, bool* ok)
{
    QVector<RemoteArtist> out;
    if (ok) *ok = false;
    QJsonArray items;
    if (!itemsArray(body, items)) return out;
    for (const QJsonValue& v : items)
    {
        if (!v.isObject()) continue;
        const QJsonObject o = v.toObject();
        RemoteArtist a;
        a.id                  = o.value(QStringLiteral("Id")).toString();
        a.name                = o.value(QStringLiteral("Name")).toString();
        a.musicBrainzArtistId = providerId(o, "MusicBrainzArtist");
        a.albumCount          = o.value(QStringLiteral("ChildCount")).toInt();
        if (a.id.isEmpty()) continue;   // a row with no id can never be qualified; it is not a row
        out.push_back(a);
    }
    if (ok) *ok = true;
    return out;
}

QVector<JellyfinMusic::RemoteAlbum> JellyfinMusic::readAlbums(const QByteArray& body, bool* ok)
{
    QVector<RemoteAlbum> out;
    if (ok) *ok = false;
    QJsonArray items;
    if (!itemsArray(body, items)) return out;
    for (const QJsonValue& v : items)
    {
        if (!v.isObject()) continue;
        const QJsonObject o = v.toObject();
        RemoteAlbum b;
        b.id     = o.value(QStringLiteral("Id")).toString();
        b.name   = o.value(QStringLiteral("Name")).toString();
        b.artist = firstArtist(o);
        const QJsonArray aa = o.value(QStringLiteral("AlbumArtists")).toArray();
        if (!aa.isEmpty() && aa.first().isObject())
        {
            const QJsonObject a0 = aa.first().toObject();
            b.artistId = a0.value(QStringLiteral("Id")).toString();
            if (b.artist.isEmpty()) b.artist = a0.value(QStringLiteral("Name")).toString().trimmed();
        }
        b.musicBrainzAlbumId        = providerId(o, "MusicBrainzAlbum");
        b.musicBrainzReleaseGroupId = providerId(o, "MusicBrainzReleaseGroup");
        b.musicBrainzArtistId       = providerId(o, "MusicBrainzAlbumArtist");
        if (b.musicBrainzArtistId.isEmpty()) b.musicBrainzArtistId = providerId(o, "MusicBrainzArtist");
        b.songCount   = o.value(QStringLiteral("ChildCount")).toInt();
        b.year        = o.value(QStringLiteral("ProductionYear")).toInt();
        b.durationSec = secondsFromTicks(qint64(o.value(QStringLiteral("RunTimeTicks")).toDouble()));
        if (b.id.isEmpty()) continue;
        out.push_back(b);
    }
    if (ok) *ok = true;
    return out;
}

QVector<JellyfinMusic::RemoteSong> JellyfinMusic::readSongs(const QByteArray& body, bool* ok)
{
    QVector<RemoteSong> out;
    if (ok) *ok = false;
    QJsonArray items;
    if (!itemsArray(body, items)) return out;
    for (const QJsonValue& v : items)
    {
        if (!v.isObject()) continue;
        const QJsonObject o = v.toObject();
        RemoteSong s;
        s.id      = o.value(QStringLiteral("Id")).toString();
        s.title   = o.value(QStringLiteral("Name")).toString();
        s.album   = o.value(QStringLiteral("Album")).toString();
        s.albumId = o.value(QStringLiteral("AlbumId")).toString();
        // The TRACK artist, which may differ from the album's on a compilation — so `Artists` is read first
        // here and `AlbumArtist` only as the fallback, the reverse of the album reader's preference.
        const QJsonArray arr = o.value(QStringLiteral("Artists")).toArray();
        for (const QJsonValue& av : arr)
        {
            const QString a = av.toString().trimmed();
            if (!a.isEmpty()) { s.artist = a; break; }
        }
        if (s.artist.isEmpty()) s.artist = o.value(QStringLiteral("AlbumArtist")).toString().trimmed();
        s.track       = o.value(QStringLiteral("IndexNumber")).toInt();
        s.disc        = o.value(QStringLiteral("ParentIndexNumber")).toInt();
        s.year        = o.value(QStringLiteral("ProductionYear")).toInt();
        s.durationSec = secondsFromTicks(qint64(o.value(QStringLiteral("RunTimeTicks")).toDouble()));
        mediaFacts(o, s.format, s.bitrateKbps);
        if (s.id.isEmpty()) continue;
        out.push_back(s);
    }
    if (ok) *ok = true;
    return out;
}

QString JellyfinMusic::artistsPath(const QString& userId)
{
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("userId"), userId);
    q.addQueryItem(QStringLiteral("Recursive"), QStringLiteral("true"));
    q.addQueryItem(QStringLiteral("SortBy"), QStringLiteral("SortName"));
    // WITHOUT THIS THE MERGE LOSES ITS GROUND TRUTH. See the header.
    q.addQueryItem(QStringLiteral("Fields"), QStringLiteral("ProviderIds"));
    q.addQueryItem(QStringLiteral("EnableTotalRecordCount"), QStringLiteral("false"));
    return QStringLiteral("/Artists?") + q.query(QUrl::FullyEncoded);
}

QString JellyfinMusic::albumsPath(const QString& userId, const QString& artistId)
{
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("Recursive"), QStringLiteral("true"));
    q.addQueryItem(QStringLiteral("IncludeItemTypes"), QStringLiteral("MusicAlbum"));
    // BY ALBUM ARTIST, not by artist. `ArtistIds` would return every record this person merely PLAYS on,
    // so a session musician's page would fill up with other people's albums and — worse for #194 — those
    // albums would then be merged onto the local library under the wrong artist.
    q.addQueryItem(QStringLiteral("AlbumArtistIds"), artistId);
    q.addQueryItem(QStringLiteral("SortBy"), QStringLiteral("ProductionYear,SortName"));
    q.addQueryItem(QStringLiteral("Fields"), QStringLiteral("ProviderIds"));
    q.addQueryItem(QStringLiteral("EnableTotalRecordCount"), QStringLiteral("false"));
    return Jellyfin::itemsPath(userId) + QStringLiteral("?") + q.query(QUrl::FullyEncoded);
}

QString JellyfinMusic::songsPath(const QString& userId, const QString& albumId)
{
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("ParentId"), albumId);
    q.addQueryItem(QStringLiteral("IncludeItemTypes"), QStringLiteral("Audio"));
    q.addQueryItem(QStringLiteral("SortBy"), QStringLiteral("ParentIndexNumber,IndexNumber,SortName"));
    // MediaSources is where the container and the bitrate live: the picker's format line, and the only
    // free quality signal this protocol offers.
    q.addQueryItem(QStringLiteral("Fields"), QStringLiteral("ProviderIds,MediaSources"));
    q.addQueryItem(QStringLiteral("EnableTotalRecordCount"), QStringLiteral("false"));
    return Jellyfin::itemsPath(userId) + QStringLiteral("?") + q.query(QUrl::FullyEncoded);
}

QString JellyfinMusic::audioStreamUrl(const QString& root, const QString& itemId, const QString& token)
{
    if (root.isEmpty() || itemId.isEmpty()) return QString();
    QUrl u(root + QStringLiteral("/Audio/") + itemId + QStringLiteral("/stream"));
    QUrlQuery q;
    // The bytes as they are on the server's disk. This app decodes everything libmpv decodes, so asking a
    // server to re-encode a file it already has would cost quality and CPU for nothing.
    q.addQueryItem(QStringLiteral("static"), QStringLiteral("true"));
    // THE TOKEN IS IN THIS QUERY, for the reason Jellyfin.h's section 3 gives: mpv cannot be handed a
    // header. That is exactly why this string is minted at the moment the player is handed it and is never
    // stored, never logged and never written into a queue.
    if (!token.isEmpty()) q.addQueryItem(QStringLiteral("api_key"), token);
    u.setQuery(q);
    return u.toString(QUrl::FullyEncoded);
}

MusicLibrary::Index JellyfinMusic::indexOfArtists(const QString& serverId,
                                                 const QVector<RemoteArtist>& artists)
{
    MusicLibrary::Index idx;
    for (const RemoteArtist& a : artists)
    {
        const QString key = Jellyfin::qualify(serverId, a.id);
        if (key.isEmpty()) continue;   // qualify() refuses a malformed server id; an unqualifiable row is dropped
        MusicLibrary::Artist out;
        out.key        = key;
        out.name       = a.name;
        out.albumCount = a.albumCount;
        out.trackCount = 0;            // deliberate — see the header
        out.mbid       = a.musicBrainzArtistId;
        idx.artists.push_back(out);
        idx.albumCount += a.albumCount;
    }
    return idx;
}

void JellyfinMusic::fillArtistAlbums(MusicLibrary::Index& idx, const QString& serverId,
                                     const QString& artistKey, const QVector<RemoteAlbum>& albums)
{
    MusicLibrary::Artist* target = nullptr;
    for (MusicLibrary::Artist& a : idx.artists) if (a.key == artistKey) { target = &a; break; }
    if (!target) return;

    target->albums.clear();
    for (const RemoteAlbum& b : albums)
    {
        const QString key = Jellyfin::qualify(serverId, b.id);
        if (key.isEmpty()) continue;
        MusicLibrary::Album out;
        out.key              = key;
        out.albumArtist      = b.artist.isEmpty() ? target->name : b.artist;
        out.title            = b.name;
        out.year             = b.year;
        out.durationSec      = b.durationSec;
        out.trackCount       = b.songCount;   // the server's own count; `tracks` fills in on drill
        out.discCount        = 1;             // not known until the tracks are; a wrong count would print
        out.mbidRelease      = b.musicBrainzAlbumId;
        out.mbidReleaseGroup = b.musicBrainzReleaseGroupId;
        out.artistMbid       = b.musicBrainzArtistId.isEmpty() ? target->mbid : b.musicBrainzArtistId;
        target->albums.push_back(out);
    }
    target->albumCount = int(target->albums.size());
}

void JellyfinMusic::fillAlbumTracks(MusicLibrary::Index& idx, const QString& serverId,
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
    QString format;
    int     bitrate = 0;
    bool    first = true, formatAgrees = true, bitrateAgrees = true;
    for (const RemoteSong& s : songs)
    {
        const QString path = Jellyfin::qualify(serverId, s.id);
        if (path.isEmpty()) continue;
        MusicLibrary::IndexTrack t;
        // THE QUALIFIED ID, NOT A STREAM URL — see the header, and SubsonicClient.h for the same rule
        // arrived at from the other protocol.
        t.path        = path;
        t.sourcePath  = path;
        t.title       = s.title.isEmpty() ? path : s.title;
        t.artist      = s.artist;
        t.albumKey    = albumKey;
        t.disc        = s.disc;
        t.track       = s.track;
        t.durationSec = s.durationSec;
        t.hasCover    = false;   // no local file to re-read art out of; the cover is fetched
        t.format      = s.format;
        t.bitrateKbps = s.bitrateKbps;
        if (s.disc > maxDisc) maxDisc = s.disc;
        if (first) { format = s.format; bitrate = s.bitrateKbps; first = false; }
        else
        {
            if (s.format != format) formatAgrees = false;
            if (s.bitrateKbps != bitrate) bitrateAgrees = false;
        }
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
    // ONE FORMAT ONLY WHEN THE TRACKS AGREE. "This copy is FLAC" is a claim about the whole record; a folder
    // holding one MP3 among the FLACs cannot honestly make it, and a picker that said so would be telling
    // the user the opposite of what they are choosing between.
    target->format      = formatAgrees ? format : QString();
    target->bitrateKbps = bitrateAgrees ? bitrate : 0;
    int secs = 0;
    for (const MusicLibrary::IndexTrack& t : tracks) secs += t.durationSec;
    if (secs > 0) target->durationSec = secs;
}
