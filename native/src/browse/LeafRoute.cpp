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

ThemedEnter themedEnterFor(const QString& type, bool expandable)
{
    if (expandable) return ThemedEnter::Drill;                        // a container: series / console / volume
    if (type.startsWith(QLatin1Char('_'))) return ThemedEnter::Drill;  // synthetic: Playlists, a playlist, New…
    if (type == QLatin1String("info")) return ThemedEnter::Drill;      // guidance prose: inert, never a chooser
    return ThemedEnter::Chooser;                                       // a real leaf: Play / Favorite / …
}

} // namespace browse
