// A local AUDIOBOOK library (issue #139, increment 1) — the scan and the index, and nothing else. The root
// is Settings::audiobookFolder() (default <data>/audiobooks). We walk it, read each file's tags through
// src/media/AudioTags — the ONE tag reader in this tree, never a second pass — and group the result into
// Authors / Narrators / Series -> Books -> the files a book is made of. The browse surface that renders
// those is AudiobookCatalogs; this file decides the SHAPE it renders.
//
// It is MusicLibrary with different nouns, ON PURPOSE, down to the split between the pure functions
// (isAudioFile / scanFolder / buildIndex / loadIndexFile / saveIndexFile), which take an explicit root or an
// explicit file path and are probe-tested, and the cached convenience layer at the bottom (root /
// indexFilePath / installIndex / index / hasLibrary), which reads Settings and is main-thread only. The scan
// runs on a worker thread and NOTHING off that thread may touch the installed index or Settings — the same
// contract MusicLibrary.h states at length, for the same reason, and matching it was cheaper than inventing
// a variant of it.
//
// ---- WHY THIS IS NOT PART OF THE MUSIC LIBRARY ----------------------------------------------------------
//
// THE ROOT IS THE CLASSIFICATION, AND THE USER OWNS IT. An .mp3 under the audiobook root is an audiobook;
// the same file under the music root is music. Nothing about the FILE decides it, and nothing here sniffs
// one. #139 takes that position explicitly and it is the right one: every heuristic anybody proposes —
// "files over an hour are books", "files with chapters are books", "a COMPOSER tag means classical" — is
// confidently wrong about somebody's library, wrong SILENTLY, and offers them no way to say so. A folder
// somebody chose is a statement they made and can change in one press.
//
// It also means a MUSIC-ONLY INSTALL IS UNTOUCHED, which is a checkable claim rather than a hope: this
// library has its own root, its own persisted index file (<data>/audiobookindex.json), its own parse stamp,
// and MusicLibrary copies not one field of it. The only shared code is AudioTags, and the one thing this
// feature added there — the chapter read — is OPT-IN, so the music scan does not open a single extra byte.
//
// ---- WHAT A BOOK IS ------------------------------------------------------------------------------------
//
// A BOOK IS THE AUDIO FILES OF ONE FOLDER THAT SHARE ONE BOOK IDENTITY, where the identity is the ALBUM tag
// when the files carry one and the FOLDER ITSELF when they do not. That single sentence is the whole of the
// grouping rule, and every case #139 names falls out of it:
//
//   * A FOLDER OF NUMBERED MP3s IS ONE BOOK — one tile, one queue, one progress bar — whether they are
//     tagged with the book's title or carry no tags at all. This is the case the issue leads with, and the
//     reason the folder is in the key at all.
//   * A SINGLE .m4b IS ONE BOOK, because it is the only audio file in whatever folder it sits in, or because
//     its ALBUM tag differs from its neighbours'. Two .m4b files in one folder with different ALBUM tags are
//     two books; two that share an ALBUM tag are one two-part book, which is what a split m4b actually is.
//   * A SERIES DIRECTORY holding one directory per book is one book per directory, because each has its own
//     folder. Files loose in the series directory itself are a book of their own, which is the honest
//     reading of "somebody put loose files here".
//
// THE FOLDER IS ALWAYS IN THE KEY, and the author never is. Two different books can therefore only collide
// by sharing a folder AND a title, which is not a shape that occurs; while putting the AUTHOR in the key —
// the way MusicLibrary keys an album by (album artist, album title) — would SHATTER a folder whose files
// disagree about the artist tag, and audiobook rips disagree about that constantly (narrator in ARTIST on
// one file, author on the next). Shattering a book into fourteen one-part books is far worse than the
// theoretical merge it would prevent, so the trade is taken deliberately in the direction of holding
// together. The cost, stated plainly: two UNTAGGED single-file books dropped in the same folder read as one
// two-part book. Naming either of them, or putting one in its own folder, fixes it.
//
// THE ORDER OF A BOOK'S FILES is disc, then track number, then NATURAL FILENAME — and the last of those is
// load-bearing rather than a tiebreak, because a folder of parts named "1 - x.mp3" … "10 - x.mp3" very often
// carries no track tags at all. It goes through core/NaturalOrder, never a hand-built QCollator: a
// default-constructed one is INERT under the C locale (numeric mode is accepted, reads back true, and does
// nothing), so "10 - chapter" sorts before "2 - chapter" on any machine with LANG/LC_ALL unset — a kiosk, a
// set-top session, and every CI runner. See NaturalOrder.h; issue #205 is the one that found it.
//
// ---- THE THREE BROWSE DIMENSIONS -------------------------------------------------------------------------
//
// AUTHOR is where a book LIVES. Authors -> their books -> the book's files, which is the artist/album/track
// shape one level shorter. The author is the album artist when tagged and the track artist otherwise, the
// same fallback the music library applies, because an audiobook's ARTIST is the author far more often than
// it is anything else.
//
// NARRATOR AND SERIES ARE VIEWS OVER THE SAME BOOKS, exactly as Composers is a view over the same tracks
// (#196 part 2). A narrator bucket holds COPIES of Books that are already filed under their author, each
// still carrying its own key, so opening one from either side opens the same book and plays the same queue.
// Neither bucket owns storage, neither is persisted, and a library whose files carry no narrator or no
// series tag gets an EMPTY vector and no door on the browse — which is the compatibility rule every level
// of this feature follows.
//
// NARRATOR IS THE COMPOSER TAG, AND THAT IS THE ONE FACT WORTH WRITING DOWN. The m4b convention stores the
// narrator in COMPOSER, which is the very field #196 added for classical music. So ONE TAG NOW MEANS TWO
// THINGS, and what decides which is the ROOT the file was found under — nothing in the file, and no
// heuristic, because a 55-minute Mahler movement and a 55-minute chapter are indistinguishable by any
// property a reader can see. AudioTags reports the tag and refuses to name it (its header says so); THIS
// file is where it becomes a narrator, and only for files under this root. An explicit NARRATOR tag (MP4's
// `©nrt`, or a freeform NARRATOR) WINS over the composer when a file carries one, because a file that
// spells the narrator out is not guessing.
//
// A SERIES is the SERIES tag, else MOVEMENTNAME — the Apple Books spelling — with its index from
// SERIES-PART / MOVEMENTNUMBER. Deliberately NOT the ALBUM tag: the album is the book's own title here, and
// a dialect that puts the series in ALBUM and the book number in TRACK cannot coexist with "a folder of
// numbered files is one book ordered by TRACK" — the two readings of TRACK contradict each other, and the
// multi-file one is the case #139 leads with. #139 rules the deeper Audible dialects out by name.
//
// ---- CHAPTERS ---------------------------------------------------------------------------------------------
//
// Read AT SCAN TIME, and only their titles and their starts. mpv surfaces a file's chapters at play time and
// that is where chapter NAVIGATION during playback lives; the SHELF wants to say "38 chapters" without
// opening a twelve-hour file per tile, and re-deriving that per navigation would be one file open per row.
// So AudioTags::read is asked for them once, here, and the index carries them. A book whose files carry none
// says nothing rather than guessing.
//
// INCREMENT 2 RENDERS THAT LIST (chapterRows below) rather than reading anything new. The browse-side
// chapter surface is the one place the two book shapes meet: an .m4b's atoms and a folder's parts are the
// same rows, so BookFile now carries the atoms it already had the count of — an in-memory widening of the
// browse index, with no change to what the scan opens, what the index FILE holds, or the parse stamp. An
// index written by increment 1 loads with its chapters intact because FileEntry has persisted them all
// along; nothing has to be re-tagged for this feature to light up.
//
// ---- PROGRESS -----------------------------------------------------------------------------------------
//
// A BOOK'S PROGRESS IS DERIVED, NOT STORED, and it is derived from the marks the PLAYER already writes —
// see progressFor below for the rule and why "Finished" needs a mark of its own. The one thing to know at
// this level is that this file gained no store, no persisted field and no new stamp for it: the durations
// it divides by are the ones increment 1 already wrote for the "9h 14m" on the shelf, and the positions it
// sums are the ones openAudiobook already reads to decide where "Play book" resumes.
#pragma once
#include "../media/AudioTags.h"   // the ONE tag reader; FileEntry stores what it read

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>

namespace AudiobookLibrary
{
    // One chapter of one file, flattened out of AudioTags::Chapter onto the entry that owns the file.
    // Flattened rather than stored as the reader's own type for the same reason MusicLibrary::CueTrack is:
    // this is the PERSISTED shape, and a persisted struct that tracks a reader's is a reader that can no
    // longer change.
    struct Chapter
    {
        QString title;      // may be empty; the display layer numbers an untitled chapter
        int     startSec = 0;
    };

    // ------------------------------------------------------------------------------------------------
    // The per-file unit: what one scan of one file produced, and what the persisted index stores.
    // ------------------------------------------------------------------------------------------------
    struct FileEntry
    {
        QString path;        // absolute
        qint64  mtime = 0;   // last-modified, seconds since epoch — half of the incremental-rescan key
        qint64  size  = 0;   // bytes — the other half; an edit that preserves mtime still changes this

        // AudioTags::Tags, flattened (minus the cover bytes — see MusicLibrary.h for why an index never
        // holds encoded images).
        QString title;        // this FILE's title ("Chapter 3"), not the book's
        QString artist;       // as tagged
        QString albumArtist;  // as tagged; empty when absent
        QString book;         // the ALBUM tag: the BOOK's title
        QString narrator;     // an explicit NARRATOR / `©nrt` tag, verbatim; usually empty
        QString composer;     // the COMPOSER tag, verbatim — the narrator by m4b convention (see the header)
        QString series;
        int seriesIndex = 0;
        int track = 0, disc = 0, year = 0, durationSec = 0;
        bool hasCover = false;
        bool untagged = false;   // AudioTags::Tags::isEmpty() as it read at scan time

        QVector<Chapter> chapters;   // empty for a file with no chapter list, which is most mp3 parts

        // The AUTHOR of this file: the album artist when tagged, else the track artist. Kept here so no
        // call site can get the grouping key by reaching for the wrong one.
        QString effectiveAuthor() const { return albumArtist.isEmpty() ? artist : albumArtist; }

        // The NARRATOR of this file. The explicit tag wins; the composer is the m4b convention and is what
        // most books actually carry. THIS is the line that makes COMPOSER mean two things — see the header.
        QString effectiveNarrator() const { return narrator.isEmpty() ? composer : narrator; }
    };

    // ------------------------------------------------------------------------------------------------
    // The index: what the browse walks.
    // ------------------------------------------------------------------------------------------------
    // One FILE of a book, browse-facing. A single-file m4b book has exactly one of these, so a caller never
    // has to ask which shape it got.
    struct BookFile
    {
        QString path;          // WHAT PLAYBACK IS HANDED — the absolute file path
        QString title;         // tag title, else the filename base — NEVER empty
        int     disc = 0;
        int     track = 0;
        int     durationSec = 0;
        int     chapterCount = 0;
        bool    hasCover = false;
        // The chapter atoms themselves, carried up from the FileEntry the scan produced (issue #139
        // increment 2). The COUNT alone was enough while the shelf only wanted to say "38 chapters"; a
        // chapter LIST has to name and place them, and the only other way to get that is to open a
        // twelve-hour file per navigation — the exact cost the scan-time read exists to avoid. Nothing new
        // is read or persisted: FileEntry::chapters has held this since increment 1 and the index file
        // already round-trips it.
        QVector<Chapter> chapters;
    };

    struct Book
    {
        QString key;                // stable grouping key; the browse's route id for this book
        QString title;              // display; empty only when the folder fallback had no name either
        QString author;             // display spelling, first seen; empty == unknown
        QString narrator;           // display spelling, first seen; empty == none tagged
        QString series;             // display spelling, first seen; empty == not in a series
        int     seriesIndex = 0;    // this book's place in that series; 0 == untagged
        int     year = 0;           // earliest non-zero year among its files
        int     durationSec = 0;    // sum over the files — the number "9h 14m" comes from
        int     chapterCount = 0;   // sum over the files' chapter lists; 0 == none of them carry one
        QString folder;             // where a cover.*/folder.* sibling would live
        QString coverSourcePath;    // the first file carrying embedded art, for the cover extractor; else ""
        bool    titleFromFolder = false;   // untagged: the book is named after its directory
        QVector<BookFile> files;    // sorted: disc, then track, then NATURAL filename (see the header)

        bool isMultiFile() const { return files.size() > 1; }
    };

    // The three top-level buckets. Identical in shape on purpose: the browse renders all three with one
    // builder, and a fourth dimension would be one more of these rather than a new idiom.
    struct Author
    {
        QString key;                // stable grouping key (case-folded author name)
        QString name;               // display spelling, first seen; empty == unknown
        int     durationSec = 0;    // over its books
        QVector<Book> books;        // sorted: series name, then place in that series, then title
                                    // (for a bucket with no series tags in it that IS plain title order)
    };
    using Narrator = Author;        // same fields, same order, different meaning — see the header
    using Series   = Author;        // ...and a series' books are ordered by their index

    struct Index
    {
        QVector<Author>   authors;    // sorted by display name; the unknown-author bucket LAST
        QVector<Narrator> narrators;  // EMPTY unless some file carries a narrator/composer tag
        QVector<Series>   series;     // EMPTY unless some file carries a series tag
        int bookCount = 0;
        int fileCount = 0;

        // Deliberately only about `authors`: every book is filed under exactly one author bucket (an
        // untagged one is the empty-named bucket), so "the library is empty" is one question with one place
        // to ask it. The narrator and series vectors hold copies of books that are already in there.
        bool isEmpty() const { return authors.isEmpty(); }

        // Lookups by the keys above. Linear, because they run on a navigation rather than per frame, and a
        // QHash of pointers into these nested vectors would dangle the moment the Index is copied (which
        // installIndex does).
        const Author*   author(const QString& authorKey) const;
        const Narrator* narrator(const QString& narratorKey) const;
        const Series*   seriesFor(const QString& seriesKey) const;
        // The book, from its canonical home under its author — never from a narrator or series copy, so
        // there is exactly one answer however the user navigated to it.
        const Book*     book(const QString& bookKey) const;
    };

    // ------------------------------------------------------------------------------------------------
    // PROGRESS AND CHAPTERS (issue #139, increment 2) — pure, a book in, an answer out.
    // ------------------------------------------------------------------------------------------------

    // How far into ONE part the listener is, in seconds; <= 0 for a part carrying no position. INJECTED,
    // for the two reasons everything else in this half of the file is: nothing here may read Settings (the
    // header's contract), and a computation over a real store is a computation no probe can pin.
    using PartPositionFn = std::function<double(const QString& partPath)>;

    // Where somebody is in a book, as a number.
    //
    // COMPUTED, NEVER STORED, and that is the whole design. The player writes ONE resume mark per FILE and
    // drops it when a file plays to its end — right for a track, and all a book has, because a book is not
    // a file. openAudiobook already reads exactly those marks to decide where "Play book" resumes: the LAST
    // part still carrying a position is where the listener is, and every part before it has been heard.
    // This turns that same reading into a fraction over the durations the scan already stored, so the tile,
    // the chapter list and the play verb cannot disagree about where somebody is. No new store, no new mark.
    struct Progress
    {
        // FALSE MEANS SAY NOTHING — not "zero". A book with any part whose length the index does not know
        // cannot be placed on a bar without inventing the missing piece, and a bar that is wrong about a
        // twelve-hour book is worse than no bar at all. An index written before a file was ever tagged with
        // a length reads this way until the next scan re-opens that file; nothing has to be invalidated.
        bool   known    = false;
        bool   started  = false;   // some part carries a position, or the book is marked finished
        bool   finished = false;
        double fraction = 0.0;     // 0..1; exactly 1.0 when finished
        int    listenedSec  = 0;
        int    remainingSec = 0;   // what "14h 20m left" is made of
        // The part the listener is IN and how far into it — i.e. what openAudiobook would start, restated
        // nowhere. -1 / 0 when no part carries a position.
        int    partIndex  = -1;
        double partPosSec = 0.0;
    };

    // `completed` is the book's own completion MARK (ItemMarks), passed in for the same reason the position
    // lookup is: this file reads no store. It is also the ONLY evidence a finished book leaves. The player
    // DROPS a part's position at that part's end, so "no part carries a mark" is genuinely ambiguous between
    // "never opened" and "heard to the last second" — and a shelf must not congratulate somebody on a book
    // they have never pressed. Unmarked-and-unstarted therefore says nothing, which is the honest answer to
    // both readings; the mark is what makes "Finished" a statement rather than a guess.
    Progress progressFor(const Book& b, const PartPositionFn& posOf, bool completed = false);

    // ONE ROW OF A BOOK'S CHAPTER LIST, for either book shape — an .m4b's chapter atoms or a folder's parts.
    // They are the same row because they answer the same question ("take me to that piece of the book"), and
    // one row type is what lets the surface, the current-row rule and the play verb each be written once.
    struct ChapterRow
    {
        QString title;            // never empty: the atom's title, else "Chapter N", else the part's title
        QString path;             // WHICH FILE — handed to openAudiobook as its start part
        int     startSec = 0;     // seconds INTO that file; always 0 on a part row
        int     durationSec = 0;  // this row's own length; 0 == unknown, and the surface then says nothing
        int     fileIndex = 0;    // index into Book::files
        bool    current = false;  // where the listener is, by progressFor's rule and no second copy of it
    };

    // A book's chapter list, from the INDEX and never by opening a file. Chapters win wherever the files
    // carry them (an .m4b's atoms, read once at scan time); a book whose files carry none gets ONE ROW PER
    // PART in the index's order — disc, then track, then NATURAL filename, decided in buildIndex and not
    // restated here. A chapterless single-file book yields ONE row, which is a list worth nothing: the
    // surface offers no door when there is only one row to pick.
    QVector<ChapterRow> chapterRows(const Book& b, const PartPositionFn& posOf);

    // ------------------------------------------------------------------------------------------------
    // Pure (probe-tested), root/path explicit.
    // ------------------------------------------------------------------------------------------------

    // "Is this even an audio file", delegating to AudioTags::isSupportedFile so the extension set is decided
    // in ONE place — the same delegation MusicLibrary::isAudioFile makes, and the reason a .m4b under a
    // MUSIC root is still music: the extension says nothing about which library a file belongs to.
    bool isAudioFile(const QString& path);

    struct ScanStats
    {
        int files    = 0;   // audio files found under the root
        int retagged = 0;   // files actually opened and re-read
        int reused   = 0;   // files whose mtime AND size matched a known entry, so were not opened at all
        int dropped  = 0;   // known entries whose file is no longer on disk
    };

    // Recursive scan of a root -> one FileEntry per audio file. `known` is a previous scan's entries keyed
    // by path (byPath() builds it): any file whose mtime and size both still match is carried over verbatim
    // and NEVER re-opened, which is what makes a rescan of a large collection cheap. Anything in `known`
    // that is no longer on disk is simply absent from the result — the scan is authoritative about what
    // exists. Empty/missing root => empty result (feature-dormant, and instant).
    //
    // `separators` is handed straight to AudioTags::read for the multi-value split, exactly as the music
    // scan does. It is a PARAMETER rather than a Settings read because this runs on a worker thread.
    QVector<FileEntry> scanFolder(const QString& root,
                                  const QHash<QString, FileEntry>& known = {},
                                  ScanStats* stats = nullptr,
                                  const QStringList& separators = {});

    // Entries keyed by absolute path — the `known` argument above, and the shape the persisted file loads
    // into.
    QHash<QString, FileEntry> byPath(const QVector<FileEntry>& entries);

    // The grouping. See the header for the rules; this is where they are applied.
    Index buildIndex(const QVector<FileEntry>& entries);

    // The grouping keys, exposed because the probe asserts on them and because a surface needs to be able to
    // ask "which book does this file belong to" without re-deriving the rule.
    QString authorKeyFor(const FileEntry& e);
    QString bookKeyFor(const FileEntry& e);
    QString narratorKeyFor(const FileEntry& e);   // "" when the file names no narrator
    QString seriesKeyFor(const FileEntry& e);     // "" when the file names no series

    // User-visible wording for the empty buckets. Kept out of the data so the core never fabricates a name
    // that could then be grouped on (LocalLibrary::displayTitle is the same division).
    QString displayAuthor(const Author& a);
    QString displayBook(const Book& b);

    // Persistence — plain JSON, the musicindex.json pattern, in its OWN file so the music index is not
    // touched by any of this. Default-valued fields are omitted.
    //
    // THE PARSE STAMP works exactly as MusicLibrary's does and for the same reason: a cached entry is never
    // re-opened while its mtime and size hold, so anything that changes what a READ of an unchanged file
    // would produce has to invalidate the cache by hand or it sits there doing nothing. `rulesUsed` reports
    // the stamp the file was written with; the caller compares it against parseStamp(the list it is about to
    // scan with) and drops the cache when they differ.
    QString            parseStamp(const QStringList& separators);
    QVector<FileEntry> loadIndexFile(const QString& filePath, QString* rulesUsed = nullptr);
    bool               saveIndexFile(const QString& filePath, const QVector<FileEntry>& entries,
                                     const QStringList& separators = {});

    // ------------------------------------------------------------------------------------------------
    // Cached process-wide index (main-thread only): the async scan installs it, browse reads it.
    // ------------------------------------------------------------------------------------------------
    QString root();            // Settings::audiobookFolder()
    QString indexFilePath();   // <data>/audiobookindex.json — read on the main thread, passed into the worker

    void         installIndex(Index idx);
    const Index& index();

    // Has a scan finished since the app started? The browse needs to tell "we have not looked yet" from "we
    // looked and there is nothing there" — the two want opposite sentences on screen.
    bool indexReady();

    // Should the home surface offer an Audiobooks category at all? True when the configured root EXISTS on
    // disk, or when a scan already found books — the SAME rule MusicLibrary::hasLibrary() follows, which is
    // what makes "no Audiobooks category on a music-only install" true by construction: the default root
    // (<data>/audiobooks) is never created by anything. Reads Settings: main thread only.
    bool hasLibrary();
}
