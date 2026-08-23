// MULTI-ALBUM music queues (the "put an hour of music on" half of the local music library) — the pure
// builders that turn MusicLibrary's Index into the ordered file list PlaybackSession already knows how to
// play, and the one shuffle that spans the whole set.
//
// WHY THIS FILE EXISTS AT ALL. Until it did, every queue this app could build was single-album:
// openMusicAlbum queues one album, openAudioPath queues one folder, and the only producer that could span
// albums was the multi-select file dialog — which the themed/XMB surface does not expose. That made two
// shipped features nearly unreachable: crossfade (#141) correctly suppresses inside a record, so on a tidy
// library it never fired; and ReplayGain's track mode exists for shuffled listening that could not happen.
// The verbs on top of this ("Play all", "Shuffle all", "Shuffle all music") are what makes them reachable.
//
// IT FEEDS THE EXISTING QUEUE. Nothing here plays anything, owns a cursor, or remembers a position — it
// produces a QVector<Entry> and stops. MainWindow::openMusicQueue hands that to the SAME
// PlaybackSession::setQueue an album play uses, which is what keeps shuffle, channel mode, playlists,
// resume, gapless and crossfade working across a multi-album queue without any of them learning about it.
// A parallel queue would be a second copy of all of that.
//
// THE ORDER IS THE INDEX'S ORDER, RESTATED NOWHERE. forArtist walks Artist::albums (already sorted year then
// title) and each Album::tracks (already sorted disc, then track, then natural filename); forLibrary walks
// Index::artists (already sorted by display name, unknown bucket last) and then does exactly what forArtist
// does. Every sort rule in this feature is settled in MusicLibrary::buildIndex; re-sorting here would be a
// second definition of album order, and the two would drift.
//
// THE SHUFFLE IS OVER THE WHOLE SET. One Fisher-Yates pass across the finished queue, not a shuffle of each
// album stitched back together — the difference is invisible in a completeness check and audible
// immediately, because a per-album shuffle keeps every record contiguous and therefore never crosses one.
// It is also exactly what crossfade needs: a boundary between two records is one it may take, and a
// boundary inside a record is one it must suppress, so a queue whose records stay contiguous produces the
// same "no crossfade ever" as the single-album queues this file exists to replace. probe_musicqueue pins
// that as "the shuffled queue has more album RUNS than there are albums", which a per-album shuffle fails
// and a whole-set shuffle passes.
//
// SEEDED, on purpose. The seed is a parameter rather than a global read, so a probe can pin a permutation
// rather than merely a property, and so the RNG is not a hidden input. The app passes randomSeed().
//
// WHY THIS RUNS ON THE GUI THREAD, deliberately, when the SCAN does not. The scan is off-thread because it
// opens and parses every file on disk; this walk opens nothing. It is a copy of two refcounted QStrings per
// track out of memory the caller already holds, and the shuffle is one swap per track. Handing it to a
// worker would mean handing the worker the Index — and the Index is exactly the thing that cannot be
// referenced off the main thread (installIndex replaces it in place, so a reference dangles), so it would
// have to be COPIED first, which costs strictly more than the walk it was meant to avoid. probe_musicqueue
// measures the real thing on a 20,000-track index and fails if it is not comfortably inside one frame.
#pragma once
#include "MusicLibrary.h"

#include <QString>
#include <QVector>

namespace MusicQueue
{
    // One queue entry: what PlaybackSession needs (the path), what the playlist/now-playing surfaces show
    // (title + artist), and which record it came from.
    //
    // `albumKey` rides along because the caller needs it twice and re-deriving it would mean a lookup per
    // track: the now-playing page re-reads the ALBUM ART at each boundary (a cross-album queue that keeps
    // the first record's sleeve on screen for an hour is showing the wrong record for most of it), and it is
    // what makes "did the shuffle actually break the records apart" assertable at all.
    struct Entry
    {
        QString path;       // absolute; handed to PlaybackSession verbatim
        QString title;      // the track title as the index holds it (never empty — see MusicLibrary)
        QString artist;     // the TRACK artist; on a compilation this differs from the album's
        QString albumKey;   // MusicLibrary::Album::key — which record this track is from
    };

    // Every track by one artist, in album (year, then title) then disc then track order. An unknown key —
    // the library was rescanned out from under the row that named it — is an EMPTY queue, never a crash and
    // never a fallback to something else: a stale route must not silently play a different artist.
    QVector<Entry> forArtist(const MusicLibrary::Index& idx, const QString& artistKey);

    // Every track in the library, in the index's artist -> album -> disc -> track order. Mostly useful as
    // the input to shuffle(): it is what "Shuffle all music" shuffles.
    QVector<Entry> forLibrary(const MusicLibrary::Index& idx);

    // Fisher-Yates over the WHOLE queue, in place. Deterministic in `seed`. A queue of 0 or 1 is a no-op.
    void shuffle(QVector<Entry>& q, quint32 seed);

    // A fresh seed from the process RNG. Separated from shuffle() so the pure function has no hidden input.
    quint32 randomSeed();
}
