#include "MusicQueue.h"

#include <QRandomGenerator>
#include <algorithm>

namespace MusicQueue
{
namespace {

// One album's tracks appended to `out`, in the order the index already put them in (disc, then track, then
// natural filename). The ONE place a track becomes an Entry, so the artist/album fields cannot disagree
// between the artist queue and the library queue.
void appendAlbum(const MusicLibrary::Album& b, QVector<Entry>& out)
{
    for (const MusicLibrary::IndexTrack& t : b.tracks)
    {
        Entry e;
        e.path     = t.path;
        e.title    = t.title;
        // A track with no artist tag of its own belongs to the album artist — the same fallback the browse
        // rows make when they decide whether to print a per-track artist at all.
        e.artist   = t.artist.isEmpty() ? b.albumArtist : t.artist;
        e.albumKey = b.key;
        out.push_back(e);
    }
}

int trackTotal(const MusicLibrary::Artist& a)
{
    int n = 0;
    for (const MusicLibrary::Album& b : a.albums) n += int(b.tracks.size());
    return n;
}

} // namespace

QVector<Entry> forArtist(const MusicLibrary::Index& idx, const QString& artistKey)
{
    QVector<Entry> out;
    const MusicLibrary::Artist* a = idx.artist(artistKey);
    if (!a) return out;                 // stale route: empty, never "some other artist"
    out.reserve(trackTotal(*a));
    for (const MusicLibrary::Album& b : a->albums) appendAlbum(b, out);
    return out;
}

QVector<Entry> forAlbum(const MusicLibrary::Index& idx, const QString& albumKey)
{
    QVector<Entry> out;
    const MusicLibrary::Album* b = idx.album(albumKey);
    if (!b) return out;                 // stale route: empty, never "some other record"
    out.reserve(b->tracks.size());
    appendAlbum(*b, out);
    return out;
}

QVector<Entry> forLibrary(const MusicLibrary::Index& idx)
{
    QVector<Entry> out;
    out.reserve(idx.trackCount);        // the index already counted; one allocation for the whole library
    for (const MusicLibrary::Artist& a : idx.artists)
        for (const MusicLibrary::Album& b : a.albums) appendAlbum(b, out);
    return out;
}

// shuffle() is defined in the header (#465): probe_musicsources shuffles a fallback artist queue with the
// very same function, and it cannot link this file (Index::artist/album live in MusicLibrary.cpp).

quint32 randomSeed() { return QRandomGenerator::global()->generate(); }

} // namespace MusicQueue
