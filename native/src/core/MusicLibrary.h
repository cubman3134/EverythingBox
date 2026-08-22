// A local MUSIC library (issue #74, increment 2) — the scan and the index, and nothing else. The root is
// Settings::musicFolder() (default <data>/musiclibrary). We walk it, read each file's tags through
// src/media/AudioTags — the ONE tag reader in this tree, never a second pass — and group the result into
// Artists -> Albums -> Tracks. The browse surface that renders those is increment 3; this file decides the
// SHAPE it renders, which is why the index types below are written to be consumed rather than merely produced.
//
// It mirrors LocalLibrary deliberately, down to the split: the pure functions (isAudioFile / scanFolder /
// buildIndex / loadIndexFile / saveIndexFile) take an explicit root or an explicit file path and are
// probe-tested; the cached convenience layer at the bottom (root / indexFilePath / installIndex / index)
// reads Settings and is main-thread only. That split is the whole reason the video library is testable
// without a window, and a music library that only worked inside MainWindow would have no way to prove the
// grouping rules below — which are the part of this that has to be right.
//
// WHY ALBUM ARTIST IS THE GROUPING KEY. An album is identified by (album artist, album title), taken from
// AudioTags::Tags::effectiveAlbumArtist() and never from the track `artist`. On a compilation every track
// carries a different artist and the same album artist, so grouping on `artist` shatters one album into one
// album per track — the failure #74 names by name, and the reason "Various Artists" has to come out as ONE
// artist holding ONE album. Two different artists with a self-titled album are two albums because the artist
// is half the key; one album split over two discs is one album because the disc number is NOT part of the
// key, only part of the track order.
//
// WHERE UNTAGGED FILES GO. Nowhere silent. A file whose tags read empty keeps exactly the same grouping rule
// as everything else — only the VALUES fall back:
//   * the track title falls back to the filename (AudioTags.h already anticipates this: it excludes duration
//     from isEmpty() precisely so an untagged file is left to the filename fallback rather than shown blank);
//   * the album falls back to the CONTAINING FOLDER, because a folder of files is what a person means by an
//     album, and the alternative — one library-wide "Unknown Album" — is a flat list of hundreds of files
//     with nothing to tell one rip from another. Two untagged folders are therefore two albums, which is
//     navigable; one untagged bucket is not;
//   * the album artist falls back to empty, which is its own artist bucket, sorted LAST so a pile of untagged
//     files is not the first thing the browse shows.
// The core stores the empty string rather than a fabricated name; displayArtist()/displayAlbum() apply the
// user-visible "Unknown …" wording, the same division of labour as LocalLibrary::displayTitle.
//
// WHY THE COVER BYTES ARE NOT IN THE INDEX. AudioTags::read() returns the embedded picture, and a TrackEntry
// keeps only `hasCover`. A library of twenty thousand tracks would otherwise hold twenty thousand encoded
// JPEGs in RAM and base64 them into the persisted index — for artwork the browse needs one of per album.
// Increment 3 re-reads the picture from the one track it wants to show, through the same AudioTags::read().
#pragma once
#include "../media/AudioTags.h"   // the ONE tag reader; TrackEntry stores its GainValue verbatim

#include <QHash>
#include <QString>
#include <QVector>

namespace MusicLibrary
{
    // ------------------------------------------------------------------------------------------------
    // The per-file unit: what one scan of one file produced, and what the persisted index stores.
    // ------------------------------------------------------------------------------------------------
    struct TrackEntry
    {
        QString path;        // absolute
        qint64  mtime = 0;   // last-modified, seconds since epoch — half of the incremental-rescan key
        qint64  size  = 0;   // bytes — the other half; an edit that preserves mtime still changes this

        // AudioTags::Tags, flattened (minus the cover bytes; see the header comment above).
        QString title, artist, albumArtist, album, genre;
        int track = 0, trackTotal = 0, disc = 0, discTotal = 0, year = 0, durationSec = 0;
        bool    hasCover = false;

        // ReplayGain rides along because increment 1 already read it out of the same tag block in the same
        // pass (AudioTags.h says why). Dropping it here would mean a second scan of the whole library later
        // for four numbers we are holding right now.
        AudioTags::GainValue trackGain, albumGain, trackPeak, albumPeak;

        // AudioTags::Tags::isEmpty() as it read at scan time. Stored rather than recomputed: it is the
        // reader's own verdict, and re-deriving it here would be a second copy of a rule that lives there.
        bool untagged = false;

        // Same fallback as AudioTags::Tags::effectiveAlbumArtist(), over the flattened fields — so a caller
        // cannot get the album grouping key by reaching for `artist` by accident.
        QString effectiveAlbumArtist() const { return albumArtist.isEmpty() ? artist : albumArtist; }
    };

    // ------------------------------------------------------------------------------------------------
    // The index: what increment 3 browses. Artists -> Albums -> Tracks, each level already sorted.
    // ------------------------------------------------------------------------------------------------
    struct IndexTrack
    {
        QString path;                 // absolute; what playback is handed
        QString title;                // tag title, else the filename base — NEVER empty
        QString artist;               // the TRACK artist as tagged; may differ from the album's on a
                                      // compilation, which is exactly what a track list wants to show
        int     disc = 0;             // 0 == untagged (ordered as disc 1; see the sort rule in the .cpp)
        int     track = 0;            // 0 == untagged (ordered after the numbered tracks)
        int     durationSec = 0;
        bool    hasCover = false;     // this file carries embedded art — re-read it when you need the bytes
    };

    struct Album
    {
        QString key;                  // stable grouping key; increment 3's route id for this album
        QString albumArtist;          // display spelling, first seen; empty == unknown
        QString title;                // display; empty only when the fallback had no folder name either
        QString folder;               // folder of the first track — where a cover.*/folder.* sibling lives
        int     year = 0;             // earliest non-zero year among its tracks; 0 == unknown
        int     discCount = 1;
        int     durationSec = 0;      // sum over the tracks
        bool    titleFromFolder = false;  // the album is untagged and named after its directory
        QVector<IndexTrack> tracks;   // sorted: disc, then track number, then natural filename
    };

    struct Artist
    {
        QString key;                  // stable grouping key (case-folded album artist)
        QString name;                 // display spelling, first seen; empty == unknown
        int     trackCount = 0;
        QVector<Album> albums;        // sorted: year, then title
    };

    struct Index
    {
        QVector<Artist> artists;      // sorted by display name; the unknown-artist bucket LAST
        int trackCount = 0;
        int albumCount = 0;

        bool isEmpty() const { return artists.isEmpty(); }

        // Lookups by the keys above — how a browse route ("show me this album") gets back to the data.
        // Linear, because they run on a navigation, not per frame, and a QHash of pointers into these
        // nested vectors would dangle the moment the Index is copied (which installIndex does).
        const Artist* artist(const QString& artistKey) const;
        const Album*  album(const QString& albumKey) const;
    };

    // ------------------------------------------------------------------------------------------------
    // Pure (probe-tested), root/path explicit.
    // ------------------------------------------------------------------------------------------------

    // The "is this even a music file" filter, delegating to AudioTags::isSupportedFile so the extension set
    // is decided in ONE place. Named here anyway because the scan's behaviour is what a reader comes looking
    // for, and a silent delegation is easy to miss.
    bool isAudioFile(const QString& path);

    // What one scanFolder() call did, for the caller that wants to say so (and for the probe that proves the
    // incremental path is real rather than merely fast).
    struct ScanStats
    {
        int files    = 0;   // audio files found under the root
        int retagged = 0;   // files actually opened and re-read by AudioTags
        int reused   = 0;   // files whose mtime AND size matched a known entry, so were not opened at all
        int dropped  = 0;   // known entries whose file is no longer on disk
    };

    // Recursive scan of a root -> one TrackEntry per audio file. `known` is a previous scan's entries keyed
    // by path (byPath() builds it): any file whose mtime and size both still match is carried over verbatim
    // and NEVER re-opened, which is what makes a rescan of a large library cheap. Anything in `known` that is
    // no longer on disk is simply absent from the result — the scan is authoritative about what exists.
    // Empty/missing root => empty result (feature-dormant, and instant).
    QVector<TrackEntry> scanFolder(const QString& root,
                                   const QHash<QString, TrackEntry>& known = {},
                                   ScanStats* stats = nullptr);

    // Entries keyed by absolute path — the `known` argument above, and the shape the persisted file loads into.
    QHash<QString, TrackEntry> byPath(const QVector<TrackEntry>& entries);

    // The grouping. See the header comment for the rules; this is where they are applied.
    Index buildIndex(const QVector<TrackEntry>& entries);

    // The grouping keys, exposed because the probe asserts on them and because increment 3 needs to be able
    // to ask "which album does this file belong to" without re-deriving the rule.
    QString artistKeyFor(const TrackEntry& e);
    QString albumKeyFor(const TrackEntry& e);

    // User-visible wording for the two "unknown" buckets. Kept out of the data so the core never fabricates
    // a name that could then be grouped on (LocalLibrary::displayTitle is the same division).
    QString displayArtist(const Artist& a);
    QString displayAlbum(const Album& a);

    // Persistence — plain JSON, the localresolve.json pattern, explicitly NOT a database (#74 says so). The
    // file is the previous scan's TrackEntry list; default-valued fields are omitted so a library of tracks
    // that are mostly untagged does not pay for keys that say nothing. A missing or corrupt file loads as
    // empty, which costs a full re-tag and nothing else.
    QVector<TrackEntry> loadIndexFile(const QString& filePath);
    bool                saveIndexFile(const QString& filePath, const QVector<TrackEntry>& entries);

    // ------------------------------------------------------------------------------------------------
    // Cached process-wide index (main-thread only): the async scan installs it, browse reads it.
    // ------------------------------------------------------------------------------------------------

    // Settings::musicFolder(). NOTE this is NOT BackgroundMusic::musicDir() (<data>/music), which holds the
    // UI's ambient loops — see Settings::musicFolder()'s comment for why the defaults must not collide.
    QString root();

    // Where the persisted scan lives: <data>/musicindex.json. Read on the main thread and passed into the
    // worker, exactly as root() is — nothing off-thread should be reaching into AppPaths/Settings itself.
    QString indexFilePath();

    void         installIndex(Index idx);
    const Index& index();
}
