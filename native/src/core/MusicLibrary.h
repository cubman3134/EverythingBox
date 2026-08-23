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
// MULTI-VALUE ARTISTS, AND WHY THEY DO NOT TOUCH THE GROUPING (issue #196, part 1). A track credited to
// several people belongs to EACH of them, and the index says so in two separate places on purpose:
//   * THE ALBUM still hangs off ONE artist — the album artist when tagged, otherwise the FIRST credited
//     artist. Filing an album under every performer on it would produce one copy of the record per credit,
//     each holding only that person's tracks, which is #74's shattered compilation arriving from the other
//     direction. Album artist is therefore never split; see AudioTags.h.
//   * THE TRACK additionally appears in Artist::credits for every OTHER artist it credits. That bucket holds
//     tracks, not albums, and is what makes "browse B and find the track B is on" true without moving the
//     record. An artist who is only ever a co-credit gets a bucket with no albums in it and their tracks in
//     credits, which is exactly what they are.
// A CREDIT IS ONLY EVER MINTED FROM A TRACK WITH MORE THAN ONE ARTIST. That single condition is what keeps
// this increment to the problem it is for: a compilation track whose one performer differs from "Various
// Artists" is not a multi-value tag, it is the general "appears on" dimension, and adding it here would give
// every library — including the ones that use no multi-value tags at all — a browse full of new artists
// nobody asked for. #196 says the model must carry several values; it does not say every credit is a shelf.
//
// COMPOSERS ARE A VIEW OVER THE SAME TRACKS, NOT A SECOND LIBRARY (issue #196, part 2). Classical listeners
// are ill-served by an artist/album shape because the interesting axis is the composer, so the index grows a
// THIRD top-level list — Composers -> that composer's Works -> tracks — built from the very same TrackEntry
// vector in the very same pass. Read the negative half of that sentence carefully, because it is what the
// issue asks for and what it rules out:
//   * NOTHING ABOUT HOW AN ALBUM OR A TRACK IS STORED CHANGES. Album keys, artist keys, the grouping rule,
//     the sort order and the persisted entry shape are exactly what they were. A composer bucket holds
//     COPIES of IndexTracks that are already on their album, each still carrying that album's key, so
//     pressing one plays the record it is on — the same route a credit row takes.
//   * A "WORK" IS A LABEL, NOT A HIERARCHY. The issue rules out a real work/movement hierarchy explicitly
//     ("a much larger project that can follow if anyone asks"), so ComposerWork below is only "these tracks
//     of this composer, grouped by the WORK tag when the files carry one and by their ALBUM when they do
//     not". It owns no storage, it is not persisted, and deleting it would cost the browse a level and cost
//     the model nothing.
//   * A LIBRARY WITH NO COMPOSER TAGS GETS AN EMPTY `composers` VECTOR, and every surface asks whether it is
//     empty before offering anything. That is the whole compatibility story: most people have no COMPOSER
//     tag anywhere, and for them this increment is bytes in a struct nobody reads.
//
// GENRES ARE MULTI-VALUED TOO, and ride on the track (TrackEntry::genres, IndexTrack::genres) because there
// is no genre BUCKET to hang them off — nothing in this app browses by genre yet. When something does, the
// values are already there and already split; what is deliberately absent is a Genres level nobody asked for.
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
#include <QStringList>
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

        // The multi-value halves of `artist` and `genre` (issue #196), exactly as AudioTags parsed them —
        // one entry for a single-valued file, so a reader never has to check which shape it got. NOT derived
        // here from the display strings: the split depends on a user setting and on what the container held
        // structurally, and re-deriving it at read time would answer a question only the SCAN was in a
        // position to answer.
        QStringList artists, genres;

        // The classical fields (#196, part 2), flattened the same way: a display string and the list it was
        // parsed into, plus the two single-valued ones. Empty for every file that carries no such tag, which
        // is the whole library for most people — and empty here means absent from the persisted entry too,
        // so an ordinary library's index file does not grow by a byte.
        QString composer, conductor, performer, work, movement;
        QStringList composers, conductors, performers;

        // ReplayGain rides along because increment 1 already read it out of the same tag block in the same
        // pass (AudioTags.h says why). Dropping it here would mean a second scan of the whole library later
        // for four numbers we are holding right now.
        AudioTags::GainValue trackGain, albumGain, trackPeak, albumPeak;

        // AudioTags::Tags::isEmpty() as it read at scan time. Stored rather than recomputed: it is the
        // reader's own verdict, and re-deriving it here would be a second copy of a rule that lives there.
        bool untagged = false;

        // Same fallback as AudioTags::Tags::effectiveAlbumArtist(), over the flattened fields — so a caller
        // cannot get the album grouping key by reaching for `artist` by accident. It takes the FIRST credited
        // artist for the same reason that one does: "A; B" with no album artist is an album by A that B also
        // played on, not an album by a band called "A; B".
        QString effectiveAlbumArtist() const
        {
            if (!albumArtist.isEmpty()) return albumArtist;
            return artists.isEmpty() ? artist : artists.first();
        }
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
        QString albumKey;             // the album this track is ON. Redundant inside Album::tracks, load-
                                      // bearing in Artist::credits: a credit row is rendered away from its
                                      // album and still has to route to it (browse queues the ALBUM behind a
                                      // track, never the folder — see MusicCatalogs.h).
        QStringList genres;           // every genre this track carries (#196). No genre bucket exists to
                                      // hang them off yet; see the header note.
        // The classical credits (#196, part 2), carried on the ROW rather than looked up again: a composer's
        // work list and a filter over "Bach conducted by Gardiner" both read them from a track that has been
        // lifted out of its album, where there is nothing left to look them up on.
        QStringList composers, conductors, performers;
        QString work;                 // the piece this track belongs to, as tagged; empty when untagged
        QString movement;             // this track's movement of that piece; empty when untagged
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
        int     trackCount = 0;       // tracks on `albums` ONLY — the discography, and what the artist-level
                                      // Play all / Shuffle all rows queue. Credits are deliberately not in
                                      // it: they belong to somebody else's record.
        QVector<Album> albums;        // sorted: year, then title

        // Tracks that CREDIT this artist but sit on an album filed under another one (issue #196). In the
        // scan's natural path order, so an album's co-credited tracks stay together and two runs agree.
        // Empty for every artist in a library that uses no multi-value artist tags, which is most of them.
        QVector<IndexTrack> credits;
    };

    // ------------------------------------------------------------------------------------------------
    // The CLASSICAL VIEW (issue #196, part 2): Composers -> Works -> tracks, over the very same tracks.
    // Both structs are derived, neither is persisted, and both are absent from a library with no COMPOSER
    // tag in it. See the header note for what this deliberately is NOT.
    // ------------------------------------------------------------------------------------------------
    // ONE RECORDING of a piece, not one piece. The key is (composer, album, work title), so two performances
    // of the Goldberg Variations are two rows told apart by their performers — which is how a classical
    // listener picks between them, and the only reading under which a row can have one album's artwork, one
    // album's queue and one coherent movement order.
    struct ComposerWork
    {
        QString key;                  // stable route id; contains the composer key, so it is unique library-wide
        QString title;                // the WORK tag, else the album's title — what a person calls this piece
        QString albumKey;             // the album these tracks sit on: where the artwork and the queue come from
        QStringList performers;       // distinct performers/conductors heard on it, in track order — the one
                                      // fact that tells two recordings of the same piece apart
        int  durationSec = 0;
        bool fromWork = false;        // titled by a WORK tag rather than borrowed from the album
        QVector<IndexTrack> tracks;   // this composer's tracks only, in disc-then-track order
    };

    struct Composer
    {
        QString key;                  // stable grouping key (case-folded composer name)
        QString name;                 // display spelling, first seen; never empty (an empty tag mints nothing)
        int     trackCount = 0;
        QVector<ComposerWork> works;  // sorted by title
    };

    struct Index
    {
        QVector<Artist> artists;      // sorted by display name; the unknown-artist bucket LAST
        // The classical view (#196). EMPTY unless some file in the library carries a COMPOSER tag, which is
        // the gate every surface checks before offering the dimension at all.
        QVector<Composer> composers;  // sorted by display name
        int trackCount = 0;
        int albumCount = 0;

        // Deliberately still only about `artists`: "the library is empty" must not become false because a
        // composer bucket exists, since every composer bucket is built from tracks that are already on an
        // album under some artist. The two can never disagree, and asking the same question twice is how
        // they would start to.
        bool isEmpty() const { return artists.isEmpty(); }

        // Lookups by the keys above — how a browse route ("show me this album") gets back to the data.
        // Linear, because they run on a navigation, not per frame, and a QHash of pointers into these
        // nested vectors would dangle the moment the Index is copied (which installIndex does).
        const Artist* artist(const QString& artistKey) const;
        const Album*  album(const QString& albumKey) const;
        const Composer*     composer(const QString& composerKey) const;
        const ComposerWork* work(const QString& workKey) const;
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
    //
    // `separators` is handed straight to AudioTags::read for the multi-value split (#196). It is a PARAMETER
    // rather than a Settings read for the same reason `root` is: this runs on a worker thread, and the value
    // is read once on the main thread by the caller (Settings::musicTagSeparatorList()). Passing none means
    // structured multi-values only, which is what every probe that does not care wants.
    QVector<TrackEntry> scanFolder(const QString& root,
                                   const QHash<QString, TrackEntry>& known = {},
                                   ScanStats* stats = nullptr,
                                   const QStringList& separators = {});

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
    //
    // THE PARSE STAMP, and there is exactly ONE of them (#196). A cached entry is never re-opened while its
    // mtime and size hold, so anything that changes what a READ of an unchanged file would produce has to
    // invalidate the cache by hand or it sits there doing nothing. Two such things now exist — the user's
    // separator list, and the set of tags the reader knows about — and they share this one string on
    // purpose: a library that rescans TWICE for one settings change is a worse outcome than either change
    // alone, so a future field goes in here too rather than growing a second condition beside it.
    //
    // `rulesUsed` reports the stamp the file was written with; the caller compares it against
    // parseStamp(the list it is about to scan with) and drops the cache when they differ. Any index written
    // before this stamp existed reports "", which differs from every stamp and therefore re-tags once.
    QString             parseStamp(const QStringList& separators);
    QVector<TrackEntry> loadIndexFile(const QString& filePath, QString* rulesUsed = nullptr);
    bool                saveIndexFile(const QString& filePath, const QVector<TrackEntry>& entries,
                                      const QStringList& separators = {});

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

    // Has a scan finished since the app started? Increment 3's browse needs to tell "we have not looked yet"
    // from "we looked and there is nothing there" — the two want opposite sentences on screen, and an empty
    // index alone cannot distinguish them (the scan is asynchronous, so the Music category is reachable while
    // it is still running). Set by installIndex and never cleared: a later rescan replaces the index in place.
    bool indexReady();

    // Should the home surface offer a Music category at all? True when the configured root EXISTS on disk —
    // the user has pointed us at something real — or when a scan already found tracks. A fresh install whose
    // default <data>/musiclibrary was never created gets no Music tab, exactly as it gets no Photos tab; the
    // moment a folder is chosen the tab appears, and if that folder turns out to hold nothing the category
    // says so rather than showing an empty shelf. Reads Settings: main thread only.
    bool hasLibrary();
}
