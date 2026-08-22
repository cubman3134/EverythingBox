#include "MusicCatalogs.h"
#include "../core/MusicArt.h"

#include <QObject>
#include <QString>

namespace browse
{
namespace {

// "m:ss" / "h:mm:ss". Nothing at all for a duration of 0, which is what a container that cannot give one
// cheaply reports (AudioTags::Tags::durationSec says so) — printing "0:00" beside a real track reads as a
// broken file rather than as a missing number.
QString fmtDuration(int secs)
{
    if (secs <= 0) return QString();
    const int h = secs / 3600, m = (secs % 3600) / 60, s = secs % 60;
    return h > 0 ? QStringLiteral("%1:%2:%3").arg(h).arg(m, 2, 10, QLatin1Char('0')).arg(s, 2, 10, QLatin1Char('0'))
                 : QStringLiteral("%1:%2").arg(m).arg(s, 2, 10, QLatin1Char('0'));
}

QString joinDot(const QStringList& parts)
{
    QStringList kept;
    for (const QString& p : parts) if (!p.isEmpty()) kept << p;
    return kept.join(QStringLiteral(" · "));
}

// The cover for one album, through the injected resolver or the default (extracted cache, then sibling file).
QString coverFor(const MusicLibrary::Album& b, const MusicCoverFn& fn)
{
    if (fn) return fn(b);
    static const QString dir = MusicArt::cacheDir();   // one AppPaths read per process, not one per tile
    return MusicArt::albumCover(b, dir);
}

MediaItem albumRow(const MusicLibrary::Album& b, const MusicCoverFn& cover)
{
    MediaItem it;
    it.id           = QString::fromLatin1(kMusicAlbumPrefix) + b.key;
    it.type         = QString::fromLatin1(kMusicAlbumType);
    it.mime         = QString::fromLatin1(kMusicAlbumPrefix) + b.key;   // -> musicAlbumCatalog
    it.expandable   = true;
    it.title        = MusicLibrary::displayAlbum(b);
    it.thumbnailUrl = coverFor(b, cover);
    it.subtitle     = joinDot({ b.year > 0 ? QString::number(b.year) : QString(),
                                QObject::tr("%n track(s)", "", int(b.tracks.size())),
                                b.discCount > 1 ? QObject::tr("%n disc(s)", "", b.discCount) : QString() });
    return it;
}

} // namespace

QString musicKeyOf(const QString& mime, const char* prefix)
{
    const QString p = QString::fromLatin1(prefix);
    return mime.startsWith(p) ? mime.mid(p.size()) : QString();
}

MediaCatalog musicArtistsCatalog(const MusicLibrary::Index& idx, const MusicEmptyNote& note,
                                 const MusicCoverFn& cover)
{
    MediaCatalog cat; cat.title = QObject::tr("Music");
    cat.hasMore = false;

    if (idx.artists.isEmpty())
    {
        // An empty shelf with no explanation is the failure this parameter exists to prevent: the user has
        // just pointed the app at a folder and wants to know what happened to it.
        if (!note.isEmpty())
        {
            MediaItem info;
            info.type     = QStringLiteral("info");   // the surface's own non-actionable guidance row
            info.id       = QStringLiteral("_musicempty");
            info.title    = note.text;
            info.subtitle = note.detail;
            cat.items.push_back(info);
        }
        return cat;
    }

    for (const MusicLibrary::Artist& a : idx.artists)
    {
        MediaItem it;
        it.id         = QString::fromLatin1(kMusicArtistPrefix) + a.key;
        it.type       = QString::fromLatin1(kMusicArtistType);
        it.mime       = QString::fromLatin1(kMusicArtistPrefix) + a.key;   // -> musicArtistCatalog
        it.expandable = true;
        it.title      = MusicLibrary::displayArtist(a);
        it.subtitle   = joinDot({ QObject::tr("%n album(s)", "", int(a.albums.size())),
                                  QObject::tr("%n track(s)", "", a.trackCount) });
        // An artist has no artwork of their own here (that is a MusicBrainz job, which #74 defers): show the
        // first album's cover so the shelf is pictures rather than a grid of placeholders.
        if (!a.albums.isEmpty()) it.thumbnailUrl = coverFor(a.albums.first(), cover);
        cat.items.push_back(it);
    }
    return cat;
}

MediaCatalog musicArtistCatalog(const MusicLibrary::Index& idx, const QString& artistKey,
                                const MusicCoverFn& cover)
{
    const MusicLibrary::Artist* a = idx.artist(artistKey);
    MediaCatalog cat;
    cat.title   = a ? MusicLibrary::displayArtist(*a) : QObject::tr("Music");
    cat.hasMore = false;
    if (!a) return cat;   // a stale route (the library was rescanned under us) is empty, never a crash
    for (const MusicLibrary::Album& b : a->albums) cat.items.push_back(albumRow(b, cover));
    return cat;
}

MediaCatalog musicAlbumCatalog(const MusicLibrary::Index& idx, const QString& albumKey,
                               const MusicCoverFn& cover)
{
    const MusicLibrary::Album* b = idx.album(albumKey);
    MediaCatalog cat;
    cat.title   = b ? MusicLibrary::displayAlbum(*b) : QObject::tr("Music");
    cat.hasMore = false;
    if (!b) return cat;

    const QString art = coverFor(*b, cover);

    // The action row, first: "play the whole thing" is what an album page is mostly for.
    {
        MediaItem it;
        it.id           = QString::fromLatin1(kMusicPlayAlbumPrefix) + b->key;
        it.type         = QString::fromLatin1(kMusicPlayAlbumType);
        it.mime         = QString::fromLatin1(kMusicPlayAlbumPrefix) + b->key;
        it.title        = QObject::tr("Play album");
        it.subtitle     = joinDot({ QObject::tr("%n track(s)", "", int(b->tracks.size())),
                                    fmtDuration(b->durationSec) });
        it.thumbnailUrl = art;
        cat.items.push_back(it);                 // no url: the surface routes it by mime, not as a file
    }

    for (const MusicLibrary::IndexTrack& t : b->tracks)
    {
        MediaItem it;
        it.url          = t.path;                                            // the real file
        it.id           = t.path;
        it.type         = QString::fromLatin1(kMusicTrackType);
        it.mime         = QString::fromLatin1(kMusicTrackPrefix) + b->key;   // WHICH album to queue behind it
        it.thumbnailUrl = art;                                               // the album's cover, on every row
        // The number, so a track list reads as one. On a multi-disc set the bare track number repeats across
        // discs, so it is qualified — "2-3." is the third track of the second disc.
        QString num;
        if (t.track > 0)
            num = (b->discCount > 1 && t.disc > 0) ? QStringLiteral("%1-%2. ").arg(t.disc).arg(t.track)
                                                   : QStringLiteral("%1. ").arg(t.track);
        it.title    = num + t.title;
        // The track artist ONLY when it differs from the album's: on a compilation that is every row and is
        // the information the list is for; on an ordinary album it would be the same name eleven times.
        const bool differs = !t.artist.isEmpty()
                          && t.artist.trimmed().toCaseFolded() != b->albumArtist.trimmed().toCaseFolded();
        it.subtitle = joinDot({ differs ? t.artist : QString(), fmtDuration(t.durationSec) });
        cat.items.push_back(it);
    }
    return cat;
}

} // namespace browse
