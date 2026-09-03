// A local READING library — books and comics (issue #134), increment 1: the scan and the index, and nothing
// else. The root is Settings::readingFolder() (default <data>/books). We walk it, read each file through
// core/BookMeta — the ONE container reader, never a second pass — and group the result into Authors and
// Series -> Books. The browse surface that renders those is BookCatalogs; this file decides the SHAPE it
// renders.
//
// It is MusicLibrary with different nouns and AudiobookLibrary's shape almost exactly, ON PURPOSE, down to
// the split between the pure functions (isReadingFile / scanFolder / buildIndex / loadIndexFile /
// saveIndexFile), which take an explicit root or an explicit file path and are probe-tested, and the cached
// convenience layer at the bottom (root / indexFilePath / installIndex / index / hasLibrary), which reads
// Settings and is main-thread only. The scan runs on a worker thread and NOTHING off that thread may touch
// the installed index or Settings — the same contract MusicLibrary.h states at length, for the same reason.
// This is the THIRD application of that skeleton and the first one that did not have to change it.
//
// ---- THE ROOT IS THE CLASSIFICATION, AND THE USER OWNS IT ------------------------------------------------
//
// A .pdf under the reading root is a book. The SAME FILE anywhere else is a document this app has no opinion
// about. Nothing here sniffs a file to decide, and #134 and #139 both take that position for the same
// reason: every heuristic anybody proposes is confidently wrong about somebody's collection, wrong SILENTLY,
// and offers them no way to say so. A folder somebody chose is a statement they made and can change.
//
// It also means A READING-ONLY INSTALL AND A MUSIC-ONLY INSTALL ARE BOTH UNTOUCHED, which is a checkable
// claim rather than a hope: this library has its own root, its own persisted index (<data>/bookindex.json),
// its own parse stamp, and neither MusicLibrary nor AudiobookLibrary copies one field of it. The default
// root (<data>/books) is never created by anything, so an install that has not asked for this runs a scan
// that returns instantly and empty, writes no index file and gets no Books category.
//
// ---- ONE FILE IS ONE BOOK, AND THAT IS THE WHOLE GROUPING RULE -------------------------------------------
//
// This is where reading parts company with audio, and the difference is worth stating because the two look
// alike from a distance. An audiobook is a FOLDER of files that add up to one book, so AudiobookLibrary has
// to decide which files belong together. An .epub, a .pdf and a .cbz are each already the whole of the thing
// they are: a folder of fourteen CBZs is fourteen comics, not one comic in fourteen parts. So there is no
// folder key, no shatter risk and no merge risk here — the book's identity is its PATH, and two books can
// only collide by being the same file.
//
// What a folder of CBZs IS, is a SERIES, and that is a browse dimension rather than an identity. See below.
//
// ---- WHAT DECIDES A BOOK'S TITLE, AUTHOR AND SERIES -------------------------------------------------------
//
// EPUB gets all three from the package, through EpubMeta: `dc:title`, the first `dc:creator`, and either
// Calibre's `calibre:series` (what managed libraries actually carry) or EPUB 3's `belongs-to-collection`.
// That is a real metadata standard and it is simply read.
//
// PDF gets a title and an author from the document information dictionary and nothing else, because a PDF
// has no series field. A PDF whose Title says "Microsoft Word - draft3.doc" shows that, verbatim: it is what
// the file says, and inventing a rule to detect a bad one is the class of guess this feature refuses.
//
// A COMIC WITH A ComicInfo.xml IN IT SAYS ALL THREE OUTRIGHT (issue #152) — series, issue number, creators
// and more besides — and what it says WINS, because it is a statement the publisher's tagger made and the
// filename is a guess this app made. comic/ComicInfo reads it in the same archive pass that counts the
// pages, and core/BookMeta hands it up like any other container's metadata.
//
// A COMIC WITHOUT ONE HAS NO METADATA AT ALL, which is most of them, so their series comes from the FILENAME
// through comic/ComicName — a normaliser, in a repository with a documented history of normalisers that
// matched more than they meant. Read that header; the rule it lands on is that a BARE trailing number only
// becomes an issue number when another file in the SAME FOLDER agrees, which is why a comic's series is
// derived HERE, in buildIndex, and never stored on a scanned entry: the evidence for it is the folder's
// contents, and a cached verdict would go stale the moment a file was added.
//
// THE PRECEDENCE IS PER DIMENSION, not per file, and each dimension is settled the same way: what the
// document SAID beats what the filename SUGGESTS, and a field the document did not carry leaves that
// dimension exactly where it was.
//
//   * SERIES + NUMBER: the document's when it names either; ComicName's folder-corroborated reading of the
//     filename when it names neither. Never half of each — a series from one source and a number from the
//     other would put an issue in the right shelf at the wrong place in it.
//   * TITLE: the document's <Title> when it has one. Most issues have none (the series and the number ARE
//     the identification), and those keep showing exactly the name they showed before this existed.
//   * AUTHOR: the document's first <Writer>. A comic never had one before, so there is nothing to displace.
//
// AND THE CORROBORATION COUNT IS COMPUTED OVER THE WHOLE FOLDER EITHER WAY, so tagging one file changes
// nothing about how its untagged neighbours are grouped.
//
// AND WHEN THERE IS NOTHING AT ALL, THE FILENAME IS THE ANSWER. A book with no metadata still appears —
// under its own file name, in the unknown-author bucket, opening exactly as it always did. That is not a
// nicety: a scanner that silently omits its untagged files is how a library loses somebody's collection, and
// the untagged half is usually the older, stranger, more irreplaceable half.
//
// ---- THE TWO BROWSE DIMENSIONS ---------------------------------------------------------------------------
//
// AUTHOR is where a book LIVES: Authors -> their books, one level shorter than the music shape because a
// book has no tracks. The unknown-author bucket sorts LAST, so a pile of untagged PDFs is not the first
// thing the shelf shows, and it is never hidden.
//
// SERIES IS A VIEW OVER THE SAME BOOKS, exactly as an audiobook narrator is (#139) and a composer is (#196
// part 2). A series bucket holds COPIES of Books that are already filed under their author, each still
// carrying its own key, so opening one from either side opens the same file. It owns no storage, is not
// persisted, and a library whose files name no series gets an EMPTY vector and no door on the browse —
// which is the compatibility rule every level of this feature follows.
//
// SERIES ORDER IS THE INDEX, THEN THE NATURAL TITLE. The index is a DECIMAL (EpubMeta.h says why: Calibre
// numbers novellas 2.5 and truncating that collides them with book 2), 0 means unnumbered and sorts last,
// and the fallback ordering goes through core/NaturalOrder — never a hand-built QCollator, which is INERT
// under the C locale and would put "Volume 10" before "Volume 2" on every machine with no locale set
// (issue #205). That trap was found in this app's COMIC PAGE ORDER, which makes it doubly this feature's
// business.
//
// ---- WHAT IS DELIBERATELY NOT HERE -------------------------------------------------------------------------
//
//   * .cb7 / .cbt. Both are comics the reader opens, and both would cost a FULL ARCHIVE EXTRACTION to reach
//     page one — the exact per-file cost a library scan must not pay. .cbz is a random-access zip and costs
//     one member, and .cbr (issue #144) costs a walk of RAR's block-header chain, which decompresses nothing
//     at all; both are inside the rule, and the 7z and tar readers still are not.
//
//   (.mobi USED TO BE HERE, on the grounds that reading its title meant decompressing every text record of
//   the book. That was true of the reader as it stood; it stopped being true when the container walk moved
//   into ebook/MobiHeader, which answers title/author/cover out of the headers and the EXTH block and
//   inflates nothing. The refusal went with the cost that justified it — issue #144.)
//   * Reading PROGRESS on a tile. ConsumptionStats has the data; putting it on a row is a browse decision
//     with its own marks/completion vocabulary, and it is the follow-up this increment names.
//   * Online enrichment (the AIO catalog / #73 pattern) for bare PDFs. Local metadata always wins, so this
//     is blank-filling and strictly additive — a second increment, not a rule buried in this one.
#pragma once
#include "../comic/ComicInfo.h"   // #152: the Rating / Direction vocabularies an entry carries
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

namespace BookLibrary
{
    // WHICH READER OPENS IT, and nothing more. It is stored per entry because the browse wants to say
    // "comic" rather than "book" on a row and because the two take their titles from different places; it
    // is NOT an identity, NOT a grouping key, and NOT a second classification of the root. The extension
    // decides it, always.
    enum class Kind { Book, Comic };

    // ------------------------------------------------------------------------------------------------
    // The per-file unit: what one scan of one file produced, and what the persisted index stores.
    // ------------------------------------------------------------------------------------------------
    struct FileEntry
    {
        QString path;        // absolute
        qint64  mtime = 0;   // last-modified, seconds since epoch — half of the incremental-rescan key
        qint64  size  = 0;   // bytes — the other half; an edit that preserves mtime still changes this

        Kind    kind = Kind::Book;

        // BookMeta::Info, flattened. Every one of these is exactly what the CONTAINER said and never a
        // filename fallback — the fallback is applied in buildIndex, where it can be marked as such.
        QString title;
        QString author;
        QString series;       // EPUB only; a comic's series is derived per FOLDER in buildIndex (see header)
        double  seriesIndex = 0.0;
        QString language;
        int     year = 0;
        int     pageCount = 0;
        bool    hasCover = false;
        bool    untagged = false;   // BookMeta::Info::isEmpty() as it read at scan time

        // ---- ComicInfo.xml (issue #152) --------------------------------------------------------------
        // A comic archive's embedded document, carried through the persisted index so the next launch does
        // not re-open every archive to learn it again. All defaulted for an entry that had none, which is
        // every entry of every library that existed before this build — and the parse stamp below is what
        // makes those get read once.
        QString     number;      // <Number> VERBATIM ("Annual 1"); seriesIndex holds its decimal, or 0
        int         volume = 0;
        QString     summary;
        int         month = 0;
        int         day = 0;
        QStringList creators;    // all credited roles, writers first; `author` is the first writer
        QString     publisher;
        QString     genre;
        QString     web;
        ComicInfo::Rating    rating    = ComicInfo::Rating::Unrated;
        ComicInfo::Direction direction = ComicInfo::Direction::Unspecified;
    };

    // ------------------------------------------------------------------------------------------------
    // The index: what the browse walks.
    // ------------------------------------------------------------------------------------------------
    struct Book
    {
        QString key;                // stable grouping key (the folded absolute path); the browse's route id
        QString path;               // WHAT THE READER IS HANDED — the absolute file path
        QString title;              // display; NEVER empty (filename fallback)
        QString author;             // display spelling, first seen; empty == unknown
        QString series;             // display spelling; empty == not in a series
        double  seriesIndex = 0.0;  // its place in that series; 0 == unnumbered
        Kind    kind = Kind::Book;
        int     year = 0;
        int     pageCount = 0;      // chapters / pages / page images; 0 == the container did not say
        QString folder;             // where a cover.*/folder.* sibling would live
        bool    titleFromFilename = false;   // untagged: the shelf is showing the file's own name
        bool    hasCover = false;   // the container has a cover the extractor could get at

        // ---- ComicInfo.xml (issue #152), rendered ----------------------------------------------------
        // `number` is the issue as the publisher WRITES it and is what a row shows; seriesIndex is its
        // sortable decimal and is 0 for the ones that have none ("Annual 1"), which sort last among their
        // series and are then separated from each other in natural order — see sortBooks.
        QString     number;
        int         volume = 0;
        QString     summary;
        QStringList creators;       // the full credit list; `author` is still the primary (the writer)
        QString     publisher;
        QString     genre;
        QString     language;
        QString     web;
        ComicInfo::Rating    rating    = ComicInfo::Rating::Unrated;
        ComicInfo::Direction direction = ComicInfo::Direction::Unspecified;
    };

    // The two top-level buckets. Identical in shape on purpose: the browse renders both with one builder,
    // and a third dimension (a language shelf, say) would be one more of these rather than a new idiom.
    struct Author
    {
        QString key;                // stable grouping key (case-folded author name)
        QString name;               // display spelling, first seen; empty == unknown
        QVector<Book> books;        // sorted: series name, then place in that series, then natural title
    };
    using Series = Author;          // same fields, different meaning — see the header

    struct Index
    {
        QVector<Author> authors;    // sorted by display name; the unknown-author bucket LAST
        QVector<Series> series;     // EMPTY unless something named a series
        int bookCount  = 0;         // .epub / .pdf / .fb2 / .mobi-family / .txt / .md
        int comicCount = 0;         // .cbz / .cbr

        // Deliberately only about `authors`: every book is filed under exactly one author bucket (an
        // untagged one is the empty-named bucket), so "the library is empty" is one question with one place
        // to ask it. The series vector holds copies of books that are already in there.
        bool isEmpty() const { return authors.isEmpty(); }

        // Lookups by the keys above. Linear, because they run on a navigation rather than per frame, and a
        // QHash of pointers into these nested vectors would dangle the moment the Index is copied (which
        // installIndex does).
        const Author* author(const QString& authorKey) const;
        const Series* seriesFor(const QString& seriesKey) const;
        // The book, from its canonical home under its author — never from a series copy, so there is
        // exactly one answer however the user navigated to it.
        const Book*   book(const QString& bookKey) const;
    };

    // ------------------------------------------------------------------------------------------------
    // Pure (probe-tested), root/path explicit.
    // ------------------------------------------------------------------------------------------------

    // "Is this a file this library scans": .epub / .pdf / .cbz / .cbr / .fb2 (and .fb2.zip / .fbz) /
    // .mobi / .azw / .azw3 / .txt / .md, and nothing else. The remaining exclusions are deliberate and the
    // header says why for each — .cb7 and .cbt cost a whole-archive extraction to reach page one, and a bare
    // .zip is not claimed at all because "a zip in a books folder is a comic" is a guess with no marker
    // behind it. .fb2.zip IS claimed, and by whole NAME rather than by suffix, because the name says FB2 in
    // so many words.
    bool isReadingFile(const QString& path);
    Kind kindFor(const QString& path);      // .cbz / .cbr => Comic, everything else this scans => Book

    struct ScanStats
    {
        int files    = 0;   // reading files found under the root
        int reread   = 0;   // files actually opened and re-read
        int reused   = 0;   // files whose mtime AND size matched a known entry, so were not opened at all
        int dropped  = 0;   // known entries whose file is no longer on disk
    };

    // Recursive scan of a root -> one FileEntry per reading file. `known` is a previous scan's entries keyed
    // by path (byPath() builds it): any file whose mtime and size both still match is carried over verbatim
    // and NEVER re-opened, which is what makes a rescan of a large collection cheap. Anything in `known`
    // that is no longer on disk is simply absent from the result — the scan is authoritative about what
    // exists. Empty/missing root => empty result (feature-dormant, and instant).
    QVector<FileEntry> scanFolder(const QString& root,
                                  const QHash<QString, FileEntry>& known = {},
                                  ScanStats* stats = nullptr);

    // Entries keyed by absolute path — the `known` argument above, and the shape the persisted file loads
    // into.
    QHash<QString, FileEntry> byPath(const QVector<FileEntry>& entries);

    // The grouping. See the header for the rules; this is where they are applied — INCLUDING the comic
    // filename grouping, which is folder-scoped and therefore cannot live on a cached entry.
    Index buildIndex(const QVector<FileEntry>& entries);

    // WHAT A RESTRICTED (kids) PROFILE SEES. A pure filter over a built index: every book whose ComicInfo
    // AgeRating landed on Mature or Adults is dropped, author buckets left empty by that are dropped with
    // it, and the series view is rebuilt from what is left so a series does not go on advertising books that
    // are no longer in it. `restricted == false` returns the index unchanged, field for field.
    //
    // IT IS A FILTER RATHER THAN A SCAN RULE because a profile switch must not need a rescan: the index on
    // disk is the whole library, and which of it a person sees is decided at the moment they look. The
    // surface calls this on every navigation, which costs one copy of a structure it was already copying.
    //
    // An UNRATED book is shown — ComicInfo.h says at length why hiding the untagged would empty a kids
    // shelf on the first launch of this build, and why "unrated is never mistaken for Everyone" is the part
    // that actually matters.
    Index filterForProfile(const Index& idx, bool restricted);

    // The grouping keys, exposed because the probe asserts on them and because a surface needs to be able to
    // ask "which bucket does this file belong to" without re-deriving the rule.
    QString authorKeyFor(const QString& author);
    QString bookKeyFor(const QString& path);
    QString seriesKeyFor(const QString& series);

    // User-visible wording for the empty bucket. Kept out of the data so the core never fabricates a name
    // that could then be grouped on (LocalLibrary::displayTitle is the same division). There is no
    // displayBook: a Book's title is never empty by construction, which is #134's "an untagged book must
    // still appear" made structural rather than remembered.
    QString displayAuthor(const Author& a);

    // Persistence — plain JSON, the musicindex.json pattern, in its OWN file so neither of the other two
    // libraries is touched by any of this. Default-valued fields are omitted.
    //
    // THE PARSE STAMP works exactly as MusicLibrary's does and for the same reason: a cached entry is never
    // re-opened while its mtime and size hold, so anything that changes what a READ of an unchanged file
    // would produce has to invalidate the cache by hand or it sits there doing nothing. `rulesUsed` reports
    // the stamp the file was written with; the caller compares it against parseStamp() and drops the cache
    // when they differ.
    //
    // It takes NO ARGUMENT, unlike the two audio libraries': their stamp carries the user's tag-separator
    // setting because that setting changes what a read produces. Nothing a user can configure changes what
    // an OPF or a PDF information dictionary says, so the stamp is the rule VERSION alone.
    QString            parseStamp();
    QVector<FileEntry> loadIndexFile(const QString& filePath, QString* rulesUsed = nullptr);
    bool               saveIndexFile(const QString& filePath, const QVector<FileEntry>& entries);

    // ------------------------------------------------------------------------------------------------
    // Cached process-wide index (main-thread only): the async scan installs it, browse reads it.
    // ------------------------------------------------------------------------------------------------
    QString root();            // Settings::readingFolder()
    QString indexFilePath();   // <data>/bookindex.json — read on the main thread, passed into the worker

    void         installIndex(Index idx);
    const Index& index();

    // Has a scan finished since the app started? The browse needs to tell "we have not looked yet" from "we
    // looked and there is nothing there" — the two want opposite sentences on screen.
    bool indexReady();

    // Should the home surface offer a Books category at all? True when the configured root EXISTS on disk,
    // or when a scan already found something — the SAME rule MusicLibrary::hasLibrary() follows, which is
    // what makes "no Books category on an install that never asked for one" true by construction: the
    // default root (<data>/books) is never created by anything. Reads Settings: main thread only.
    bool hasLibrary();
}
