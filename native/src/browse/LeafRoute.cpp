#include "LeafRoute.h"
#include "MusicCatalogs.h"   // kMusicTrackPrefix + musicKeyOf — a keyed kind's contract lives with its feature

#include <QLatin1String>

namespace browse
{

const QVector<LocalLeafKind>& localLeafKinds()
{
    // THE list of local leaf kinds. Every entry is a kind that belongs to no addon, so HomeView::resolvePlay
    // cannot do anything with it — which is why both surfaces have to claim it here first.
    //
    // Order is match order, first hit wins. The kinds do not overlap today; if a future prefix could shadow a
    // whole-match kind, put the whole match above it.
    static const QVector<LocalLeafKind> kinds = {
        { kLocalVideoMime,   LocalLeafKind::Mime, false, LeafPlay::OpenFile   },
        { kPhotoMime,        LocalLeafKind::Mime, false, LeafPlay::OpenFile   },
        { kOpdsBookType,     LocalLeafKind::Type, false, LeafPlay::OpdsBook   },
        { kMusicTrackPrefix, LocalLeafKind::Mime, true,  LeafPlay::MusicAlbum },
    };
    return kinds;
}

LeafRoute localLeafRoute(const MediaItem& it)
{
    for (const LocalLeafKind& k : localLeafKinds())
    {
        const QString& field = (k.field == LocalLeafKind::Mime) ? it.mime : it.type;
        const QLatin1String id(k.id);
        if (k.prefix ? !field.startsWith(id) : field != id) continue;

        LeafRoute r;
        r.play = k.play;
        if (k.play == LeafPlay::MusicAlbum)
        {
            // The key is "everything after the prefix" — musicKeyOf, never a section(':'), because an album
            // key is arbitrary tag text and an album titled "Vol. 1: Live" would be truncated by one.
            r.key = musicKeyOf(it.mime, k.id);
            if (r.key.isEmpty()) return {};   // a track row naming no album: let the caller resolve it instead
        }
        else if (it.url.isEmpty())
        {
            // A file route with no file. Answering OpenFile here would claim the row and then open nothing,
            // which reads as Enter doing absolutely nothing; falling through leaves it with the resolve it
            // had before this file existed.
            return {};
        }
        return r;
    }
    return {};   // NotLocal: an addon's row, or a container — not ours to play
}

QueueTarget queueTargetFor(const MediaItem& it)
{
    // A TRACK, asked through the very same table Enter reads. localLeafRoute already refuses a track row
    // whose mime carries no album key, so the only extra requirement here is the FILE: Enter on such a row
    // would queue the whole record from the top (playMusicAlbumRequested with an empty start path), which is
    // a sensible fallback for "play", and nonsense for "add this one track".
    const LeafRoute lr = localLeafRoute(it);
    if (lr.play == LeafPlay::MusicAlbum)
    {
        if (it.url.isEmpty()) return {};
        QueueTarget t;
        t.what      = QueueAdd::Track;
        t.albumKey  = lr.key;
        t.trackPath = it.url;   // for a cue track this is the EDL clip url, which IS what the queue holds
        return t;
    }

    // A RECORD. Two rows name one: the album row in an artist's (or the library's) album list, and the
    // "Play album" action row at the top of that album's own track list — where somebody standing inside an
    // album is most likely to reach for the verb. Both are '_'-prefixed synthetic rows, which is why neither
    // can be reached through the themed inline chooser (themedEnterFor sends '_' rows down the ordinary
    // browse path, deliberately, so they can DRILL) and why the browse context menu is what carries them.
    struct AlbumRow { const char* type; const char* prefix; };
    static const AlbumRow kAlbumRows[] = {
        { kMusicAlbumType,     kMusicAlbumPrefix     },
        { kMusicPlayAlbumType, kMusicPlayAlbumPrefix },
    };
    for (const AlbumRow& a : kAlbumRows)
    {
        if (it.type != QLatin1String(a.type)) continue;
        QueueTarget t;
        t.albumKey = musicKeyOf(it.mime, a.prefix);
        if (t.albumKey.isEmpty()) return {};   // an album row naming no album: nothing to add
        t.what = QueueAdd::Album;
        return t;
    }
    return {};
}

ThemedEnter themedEnterFor(const QString& type, bool expandable)
{
    if (expandable) return ThemedEnter::Drill;                        // a container: series / console / volume
    if (type.startsWith(QLatin1Char('_'))) return ThemedEnter::Drill;  // synthetic: Playlists, a playlist, New…
    if (type == QLatin1String("info")) return ThemedEnter::Drill;      // guidance prose: inert, never a chooser
    return ThemedEnter::Chooser;                                       // a real leaf: Play / Favorite / …
}

} // namespace browse
