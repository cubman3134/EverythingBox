#include "SubsonicDownload.h"
#include "PreferLocal.h"   // the one prefer-local rule (#417), shared with Audiobookshelf downloads (#197)
#include "Subsonic.h"

#include <QFileInfo>

#include <algorithm>

namespace {

// Everything a filesystem (or a user reading a folder) would rather not see. Its own copy for the reason
// JellyfinDownload.cpp gives for its: this file is QtCore-only and linked into a probe with no UI.
QString sanitize(const QString& in)
{
    QString out;
    out.reserve(in.size());
    for (const QChar c : in)
    {
        const ushort u = c.unicode();
        if (u < 0x20) continue;
        if (c == QLatin1Char('/') || c == QLatin1Char('\\') || c == QLatin1Char(':') || c == QLatin1Char('*')
            || c == QLatin1Char('?') || c == QLatin1Char('"') || c == QLatin1Char('<') || c == QLatin1Char('>')
            || c == QLatin1Char('|'))
        { out += QLatin1Char('_'); continue; }
        out += c;
    }
    out = out.simplified();
    // A trailing dot or space is legal to WRITE on Windows and then unopenable.
    while (out.endsWith(QLatin1Char('.')) || out.endsWith(QLatin1Char(' '))) out.chop(1);
    return out.left(80);
}

QString two(int n) { return QStringLiteral("%1").arg(n, 2, 10, QLatin1Char('0')); }

bool isSubsonicTrack(const QString& path)
{
    const Subsonic::Ref r = Subsonic::parse(path);
    return r.ok && r.kind == Subsonic::Kind::Track;
}

} // namespace

SubsonicDownload::Target SubsonicDownload::targetFor(bool isTrack, bool isAlbum, const QString& albumKey,
                                                     const QString& trackPath)
{
    Target t;
    if (isTrack)
    {
        if (isSubsonicTrack(trackPath)) { t.kind = Kind::Track; t.ref = trackPath; }
        return t;
    }
    if (isAlbum)
    {
        // An ALBUM or a PLAYLIST: both are fetched by SubsonicClient::fetchAlbumTracks and both are an ordered
        // list of real server tracks. The starred container (Kind::Virtual) is neither — the server never
        // minted it and SubsonicClient refuses to fetch it — so it offers no batch.
        const Subsonic::Ref r = Subsonic::parse(albumKey);
        if (r.ok && (r.kind == Subsonic::Kind::Album || r.kind == Subsonic::Kind::Playlist))
        { t.kind = Kind::Album; t.ref = albumKey; }
    }
    return t;
}

QString SubsonicDownload::displayTitle(const MusicLibrary::IndexTrack& t)
{
    const QString title = t.title.trimmed();
    const QString artist = t.artist.trimmed();
    return artist.isEmpty() ? title : title + QString::fromUtf8(" \xE2\x80\x94 ") + artist;
}

QString SubsonicDownload::fileNameFor(const MusicLibrary::IndexTrack& t, const QString& albumTitle,
                                      int discCount, const QString& suffix)
{
    const Subsonic::Ref ref = Subsonic::parse(t.path);
    // The id suffix: what makes the same song on two servers two files. An id, never a credential.
    const QString idTag = ref.ok
        ? QStringLiteral(" [") + ref.serverId.left(8) + QLatin1Char('-') + sanitize(ref.remoteId).left(8)
              + QLatin1Char(']')
        : QString();

    QStringList parts;
    const QString artist = sanitize(t.artist);
    const QString album  = sanitize(albumTitle);
    if (!artist.isEmpty()) parts << artist;
    if (!album.isEmpty())  parts << album;
    QString number;
    if (t.track > 0)
        number = (discCount > 1 && t.disc > 0 ? QString::number(t.disc) + QLatin1Char('-') : QString())
                 + two(t.track) + QLatin1Char(' ');
    QString title = sanitize(t.title);
    if (title.isEmpty()) title = QStringLiteral("Track");
    parts << number + title;
    const QString stem = parts.join(QStringLiteral(" - "));

    // THE SUFFIX IS THE SERVER'S STRING AND BECOMES A PATH COMPONENT, so it is accepted or refused, never
    // edited — the rule JellyfinDownload::fileNameFor states for a container, for the same "../../etc" reason.
    const QString raw = suffix.toLower();
    bool plain = !raw.isEmpty() && raw.size() <= 5;
    for (const QChar c : raw) if (!c.isLetterOrNumber() || c.unicode() > 0x7F) { plain = false; break; }
    return stem + idTag + QLatin1Char('.') + (plain ? raw : QStringLiteral("mp3"));
}

DownloadJob SubsonicDownload::jobFor(const MusicLibrary::IndexTrack& t, const QString& albumTitle, int discCount,
                                     const QString& suffix, const QString& coverPath, const QString& downloadsDir)
{
    DownloadJob j;
    if (!isSubsonicTrack(t.path)) return j;
    j.title     = displayTitle(t);
    j.sourceRef = t.path;          // THE DURABLE HALF: the id the minter signs a url from at every start()
    j.key       = t.path;          // ...and the identity the Downloads entry and MusicSupply::playUrl key on
    // j.url stays EMPTY. That is the whole credential decision — see the header.
    j.dest      = downloadsDir + QLatin1Char('/') + fileNameFor(t, albumTitle, discCount, suffix);
    j.kind      = QStringLiteral("audio");
    j.thumb     = coverPath;
    return j;
}

QVector<MusicLibrary::IndexTrack> SubsonicDownload::albumBatch(const QVector<MusicLibrary::IndexTrack>& tracks,
                                                               const QSet<QString>& alreadyHave)
{
    QVector<MusicLibrary::IndexTrack> out;
    for (const MusicLibrary::IndexTrack& t : tracks)
    {
        if (!isSubsonicTrack(t.path)) continue;       // nothing a url could ever be minted for
        if (alreadyHave.contains(t.path)) continue;   // on this device already, or queued: not fetched twice
        out.push_back(t);
    }
    // Disc, then track — untagged numbers after the numbered ones — the order the record plays in, and the
    // rule Subsonic::fillAlbumTracks and MusicLibrary::buildIndex already apply. Stable, so a playlist whose
    // tracks carry no useful numbers keeps the server's own order.
    std::stable_sort(out.begin(), out.end(), [](const MusicLibrary::IndexTrack& a, const MusicLibrary::IndexTrack& b) {
        const int ad = a.disc > 0 ? a.disc : 1, bd = b.disc > 0 ? b.disc : 1;
        if (ad != bd) return ad < bd;
        const int at = a.track > 0 ? a.track : 1 << 30;
        const int bt = b.track > 0 ? b.track : 1 << 30;
        return at < bt;
    });
    return out;
}

QString SubsonicDownload::localCopy(const QString& qualifiedTrackId, const QVector<DownloadedItem>& downloads,
                                    const std::function<bool(const QString&)>& exists)
{
    // The rule itself is PreferLocal's (#197 moved it there so an Audiobookshelf book asks the same one); what
    // is Subsonic's is the gate: only a qualified TRACK id is ever looked up.
    return PreferLocal::localCopy(isSubsonicTrack(qualifiedTrackId) ? qualifiedTrackId : QString(), downloads, exists);
}

QString SubsonicDownload::preferLocal(const QString& qualifiedTrackId, const QVector<DownloadedItem>& downloads,
                                      const std::function<bool(const QString&)>& exists,
                                      const std::function<QString()>& otherwise)
{
    return PreferLocal::prefer(isSubsonicTrack(qualifiedTrackId) ? qualifiedTrackId : QString(), downloads, exists,
                               otherwise);
}

QString SubsonicDownload::openEntry(const QString& entry, const QString& identity,
                                    const QVector<DownloadedItem>& downloads,
                                    const std::function<bool(const QString&)>& exists,
                                    const std::function<QString(const QString&)>& mintStream)
{
    if (entry.isEmpty() || !isSubsonicTrack(identity)) return entry;
    return preferLocal(identity, downloads, exists, [&]() -> QString {
        // A stream url stays what it was: no re-mint, no changed query, for a track with no download.
        const bool stream = entry.startsWith(QLatin1String("http://"), Qt::CaseInsensitive)
                         || entry.startsWith(QLatin1String("https://"), Qt::CaseInsensitive);
        if (stream || (exists ? exists(entry) : QFileInfo::exists(entry))) return entry;
        // The queue was built from a download that is no longer on disk: back to the server.
        const QString url = mintStream ? mintStream(identity) : QString();
        return url.isEmpty() ? entry : url;
    });
}

QSet<QString> SubsonicDownload::downloadedIds(const QVector<DownloadedItem>& downloads,
                                              const QVector<DownloadJob>& jobs,
                                              const std::function<bool(const QString&)>& exists)
{
    QSet<QString> out;
    for (const DownloadedItem& d : downloads)
        if (isSubsonicTrack(d.key) && !d.path.isEmpty() && (exists ? exists(d.path) : QFileInfo::exists(d.path)))
            out.insert(d.key);
    for (const DownloadJob& j : jobs)
        if (isSubsonicTrack(j.sourceRef)) out.insert(j.sourceRef);
    return out;
}
