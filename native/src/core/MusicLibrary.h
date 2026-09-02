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
// A CUE ALBUM IS AN ALBUM WHOSE TRACKS SHARE A FILE (issue #196, part 3), and that is the whole of the
// model change. A single-file rip — one 70-minute FLAC plus `Album.cue` — was ONE track here, which is the
// complaint the issue and the original media survey both make. Nothing about how an album or a track is
// STORED changes to fix it; what changes is that one scanned FILE may now yield several browse TRACKS:
//   * ONE TrackEntry PER FILE, still. The entry is the unit of file identity — it is what `known` is keyed
//     by, what mtime/size compare against, and what the persisted index holds one of. A cue album's tracks
//     ride ON that entry as `cueTracks`, so the incremental rescan, byPath() and the index file keep
//     working exactly as they did, and a library with no cue sheets stores not one extra byte.
//   * buildIndex EXPANDS it. A TrackEntry carrying N cue tracks emits N IndexTracks — same album key, same
//     grouping rule, same sort — differing only in their number, title, artist and, crucially, their PATH.
//   * THE PATH OF A CUE TRACK IS A CLIP, NOT A FILE. IndexTrack::path is "what playback is handed", and for
//     a cue track that is CueSheet::mpvClipUrl — an mpv EDL url naming the one file and the one span inside
//     it. That is what lets a cue track be an ordinary entry in the ordinary PlaybackSession queue: the
//     strings differ per track (so a queue can hold all five and start at the third), the duration mpv
//     reports is the TRACK's, and end-of-file arrives at the track boundary rather than at the end of the
//     album. The app never splits the file and never keeps a boundary of its own. `sourcePath` is the real
//     file for the one caller that needs it (cover extraction), so nothing has to un-parse the url.
//   * THE SHEET FILLS IN WHAT THE FILE DID NOT SAY. A single-file rip is very often one enormous UNTAGGED
//     wav whose whole metadata is the sidecar, so a cue's TITLE / PERFORMER / REM GENRE / REM DATE stand in
//     for a missing album / album artist / genre / year. TAGS WIN wherever the file carries one, and a file
//     with no cue is untouched — so this can only ever add a name where the alternative was "Unknown".
//   * THE CACHE KEY GREW A THIRD FIELD, because editing only the .cue does not touch the audio file: an
//     entry is reused when path, mtime, size AND the identity of its sidecar all still match. See
//     scanFolder. The parse STAMP moved too, so an index written before this exists re-tags exactly once —
//     folded into the one stamp that already existed rather than a second condition beside it.
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
    // One track of a cue album, flattened out of CueSheet::Segment onto the entry that owns the file
    // (issue #196, part 3). Flattened rather than stored as the parser's own type for the same reason the
    // tags are: this is the PERSISTED shape, and a persisted struct that tracks a parser's is a parser that
    // can no longer change. Empty on every ordinary file.
    struct CueTrack
    {
        int     number = 0;    // TRACK nn from the sheet, as written
        QString title;         // the sheet's TITLE; empty is legal and falls back at display time
        QString artist;        // the sheet's track PERFORMER, else its disc PERFORMER
                               // The sheet's SONGWRITER is parsed (CueSheet::Segment carries it) and
                               // deliberately NOT stored: the Composers dimension is built from the FILE's
                               // COMPOSER tag, and a per-track composer that reached the row but not the
                               // bucket would be a dimension that disagreed with itself. A single-file
                               // classical rip still reaches Composers through its file tag, with its cue
                               // tracks as the movements — which is the case that matters.
        int     startMs = 0;   // where this track starts inside the one file
        int     endMs   = -1;  // where it ends; -1 == the end of the file, which only the LAST track is
    };

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

        // The MusicBrainz ids this file carries (#194), flattened from AudioTags the same way — empty on
        // every file that has never been through Picard, which is most of them, and therefore absent from the
        // persisted entry too. They are what lets an album on this disk be matched against the same album on
        // a music server with no string guessing at all; MusicId.h has the rule, including why the release
        // and the release-GROUP id are never compared against each other.
        QString mbReleaseGroupId, mbReleaseId, mbAlbumArtistId;

        // ReplayGain rides along because increment 1 already read it out of the same tag block in the same
        // pass (AudioTags.h says why). Dropping it here would mean a second scan of the whole library later
        // for four numbers we are holding right now.
        AudioTags::GainValue trackGain, albumGain, trackPeak, albumPeak;

        // AudioTags::Tags::isEmpty() as it read at scan time. Stored rather than recomputed: it is the
        // reader's own verdict, and re-deriving it here would be a second copy of a rule that lives there.
        bool untagged = false;

        // THE CUE ALBUM (issue #196, part 3). Empty — and absent from the persisted entry — for every file
        // that is just a file, which is the whole library for almost everybody. Non-empty means "this one
        // file is N tracks", and buildIndex expands it into N rows.
        QVector<CueTrack> cueTracks;

        // WHERE THOSE TRACKS CAME FROM, and half of the reuse decision. `cuePath` is the sidecar the scan
        // resolved (empty when the sheet was embedded in the audio file itself, or when there was none);
        // its mtime and size are checked alongside the audio file's, because editing a .cue does not touch
        // the file beside it and a scan that only watched the audio would show yesterday's track list
        // forever. All three are compared, and all three are absent from an ordinary entry.
        QString cuePath;
        qint64  cueMtime = 0;
        qint64  cueSize  = 0;

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
        QString path;                 // WHAT PLAYBACK IS HANDED. The absolute file path for an ordinary
                                      // track; for a cue album's track, the mpv EDL clip url naming the one
                                      // file and this track's span inside it (#196 part 3 — see the header,
                                      // and CueSheet::mpvClipUrl for the format). Unique per track either
                                      // way, which is what lets a queue hold all of an album's tracks and
                                      // start at one of them.
        QString sourcePath;           // THE REAL FILE ON DISK, always — identical to `path` for an ordinary
                                      // track, the shared audio file for a cue track. Exists so the one
                                      // caller that needs bytes rather than playback (MusicArt, re-reading
                                      // the embedded cover) never has to un-parse a clip url.
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
        // WHAT THIS COPY IS, where the supplier says so (issue #194, increment 3). Empty/0 mean the supplier
        // did NOT say, which is an absence and never a guess: the cross-source picker exists to tell a FLAC
        // on the NAS apart from a 128k copy on a phone, and a confidently wrong badge would defeat it.
        //
        // Left unset by the local scanner ON PURPOSE. A scanned track's format is already knowable, exactly,
        // from `sourcePath`'s extension at the moment it is displayed, and filling it in here would have put
        // a second copy of that fact into the persisted index — which would then be stale for every file the
        // user re-encodes in place. MusicMerge::qualityBits derives it from the path for a local copy and
        // reads these for a remote one, so there is still only ONE rule on screen.
        QString format;               // container/codec, upper case: "FLAC", "MP3", "OPUS"
        int     bitrateKbps = 0;      // 0 == unknown
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
        // HOW MANY TRACKS THIS RECORD HAS — which is not always the same question as "how many are in
        // `tracks`". For a scanned local album the two are identical and buildIndex sets this to
        // tracks.size(), so nothing about #74 changes; a REMOTE supplier (issue #193's Subsonic client)
        // knows a record's song count from the album listing and fetches the songs themselves only when the
        // album is opened, so between those two moments this is the only honest number there is. The browse
        // subtitles read THIS rather than tracks.size() for that reason. probe_musicbrowse pins the local
        // equality over the real fixtures, so a drift between them is a red probe rather than a wrong count
        // on somebody's shelf.
        int     trackCount = 0;
        bool    titleFromFolder = false;  // the album is untagged and named after its directory
        // THE GROUND TRUTH FOR CROSS-SOURCE IDENTITY (issue #194), when the supplier has one. For a scanned
        // album these are the first non-empty ids among its tracks; for a Subsonic album `mbidRelease` is the
        // server's `musicBrainzId` (Navidrome and other OpenSubsonic servers emit it). All three are empty for
        // a library that carries no MusicBrainz tags, which is the case MusicId's string matcher exists for.
        // The two album ids name different things and are never compared with each other — see MusicId.h.
        QString mbidReleaseGroup;     // MUSICBRAINZ_RELEASEGROUPID — "the album", across its reissues
        QString mbidRelease;          // MUSICBRAINZ_ALBUMID — one release of it
        QString artistMbid;           // the ALBUM ARTIST's id, carried here so an album can be matched
                                      // without walking back up to its artist bucket
        // WHAT THIS COPY IS, as one claim about the whole record (issue #194, increment 3) — see IndexTrack
        // for what empty/0 mean. A supplier sets these only when its tracks AGREE: a record holding one MP3
        // among the FLACs is not "a FLAC copy", and saying so in the picker would tell the user the opposite
        // of what they are choosing between.
        QString format;
        int     bitrateKbps = 0;
        QVector<IndexTrack> tracks;   // sorted: disc, then track number, then natural filename
    };

    struct Artist
    {
        QString key;                  // stable grouping key (case-folded album artist)
        QString name;                 // display spelling, first seen; empty == unknown
        // HOW MANY ALBUMS THIS ARTIST HAS, for the same reason Album::trackCount exists one level down:
        // buildIndex sets it to albums.size() so a local index is exactly what it was, and a remote
        // supplier that has fetched the ARTIST LIST but not yet any artist's albums has this and nothing
        // else to put in the row. Read by the artists-level subtitle.
        int     albumCount = 0;
        int     trackCount = 0;       // tracks on `albums` ONLY — the discography, and what the artist-level
                                      // Play all / Shuffle all rows queue. Credits are deliberately not in
                                      // it: they belong to somebody else's record.
        // The album artist's MusicBrainz id (#194), when any of their albums carries one. Ground truth for
        // "this is the same artist as the one on that server" — see MusicId::groundArtist. Empty otherwise.
        QString mbid;
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
    // CUE SIDECARS (#196 part 3) ride on the SAME walk and cost a library without them nothing: the walk
    // notes the .cue files it passes (it is already listing every file), and only a folder that actually
    // holds one ever parses anything. When a cue is in play its mtime and size join the reuse comparison,
    // so editing the sheet re-reads the album even though the audio file did not move.
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
