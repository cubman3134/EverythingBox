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
//   (Reading PROGRESS on a tile and online blank-filling both USED to be here, named as the follow-up. They
//   are #134 increment 2 and they are below — progressFor and the enrichment pair — as pure functions over
//   an already-built index, still reading no store and still opening no file.)
//
// ---- WHERE A PERSON IS IN A BOOK (increment 2) -------------------------------------------------------------
//
// progressFor is a PURE FUNCTION of one Book plus a ReadState the caller gathered, and the whole of the
// progress vocabulary is decided there so a tile's badge and the Continue-reading shelf cannot disagree —
// they are two readings of one answer. AudiobookLibrary::progressFor is the same shape for the same reason
// (#139), down to `known == false` meaning SAY NOTHING rather than "zero".
//
// THREE RULES SETTLE IT, and each of them is a refusal:
//
//   * A MARK THE PERSON SET BY HAND IS NEVER OVERWRITTEN. The marks menu exists so somebody can say the
//     automatic answer is wrong about their book, and an automatic mark that could clobber it would make
//     that menu a lie. So NOTHING here ever writes ItemMarks: the automatic state is DERIVED at the moment
//     it is displayed, and the only thing in that store is what a person put there. "Which way was this
//     set" is then structural rather than remembered — anything in ItemMarks is by hand, by construction —
//     and `fromUser` reports it. (A person who clears their mark back to None gets the automatic answer
//     again, which is what "clear" means.)
//   * HIGH-WATER, so paging back a chapter does not un-finish a book. The furthest page ever reached is what
//     ConsumptionStats::addPagesRead already stores, per title, for exactly this reason.
//   * pageCount == 0 SHOWS NO PERCENTAGE RATHER THAN A WRONG ONE. A container that did not say how long it
//     is can still be known to be in progress — that is the page count of the READING, not of the book — but
//     it cannot be placed on a bar without inventing the denominator, and 0/0 rendered as "100%" is the
//     specific wrongness this refuses.
//
// ---- ONLINE BLANK-FILLING (increment 2), AND WHY IT IS TWO PURE FUNCTIONS ----------------------------------
//
// A book with NO author or NO cover may — only with the setting on, default OFF — be asked about online, the
// #73 way (resolve -> getMeta -> MetaCache). The two decisions that make that safe are pure and live here:
// `enrichmentTargets` says WHICH books may be asked at all, and `acceptedFill` says WHAT of an answer may be
// used. Everything else (which addon, the queue, the timeout) is app wiring above this file.
//
// SPLITTING IT THIS WAY IS THE POINT. "Nothing is requested with the setting off" and "nothing is requested
// for a book that already has the field" become one assertion over a whole fixture library — the target list
// is EMPTY — rather than a comment above a network call that no probe can see. And "local metadata always
// wins" becomes an assertion that acceptedFill DROPPED the fields the book already had, rather than a
// promise about merge order somewhere downstream.
//
// AND THE ANSWER NEVER RE-GROUPS THE LIBRARY. A filled author is display, not identity: the Authors buckets
// come from the scan and from nothing else, so an enrichment cannot move a book, cannot feed itself on the
// next sweep, and cannot survive as a fact about a file that the file never said. AudiobookMeta.h reaches
// the same rule from the other direction (#198), and the persisted index holds container fields only.
#pragma once
#include "ItemMarks.h"            // Completion — the states a reading state is already spelled in
#include "../comic/ComicInfo.h"   // #152: the Rating / Direction vocabularies an entry carries
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>

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

    // ------------------------------------------------------------------------------------------------
    // WHERE A PERSON IS IN A BOOK (issue #134 increment 2). Pure; see the header for the three rules.
    // ------------------------------------------------------------------------------------------------

    // WHAT THE APP KNOWS about one book's reading, gathered by the caller. This file reads no store, for the
    // reason every other pure function here does not: a probe hands it a struct and asserts on the answer.
    struct ReadState
    {
        // THE FURTHEST PAGE EVER REACHED, 1-based; 0 == never opened. It is ConsumptionStats::Totals::
        // pagesRead, which IS a high-water page index (its header says so at length) — not a count of
        // sessions and not a position that can go backwards.
        int    furthestPage = 0;
        // When that page was last moved, epoch seconds; 0 == never. ORDERING ONLY — it decides where a book
        // sits on the Continue-reading shelf and nothing about which state it is in.
        qint64 lastRead = 0;
        // WHAT THE PERSON SAID, and only that: ItemMarks holds nothing this feature wrote. None == they
        // never said, so the automatic answer stands.
        ItemMarks::Completion userMark = ItemMarks::Completion::None;
    };

    struct Progress
    {
        ItemMarks::Completion completion = ItemMarks::Completion::None;
        bool   fromUser = false;   // the state above is the one somebody set by hand
        bool   started  = false;   // read past the first page (or said to be in progress)
        bool   finished = false;
        // FALSE MEANS SHOW NO PERCENTAGE — not "0%". A container that did not say how long it is has no
        // denominator, and AudiobookLibrary::Progress::known refuses the same way for the same reason.
        bool   known    = false;
        double fraction = 0.0;     // 0..1, meaningful only when `known`; exactly 1.0 when finished
        int    page     = 0;       // the furthest page reached, echoed back so a caller need not re-clamp
        qint64 lastRead = 0;
    };

    Progress progressFor(const Book& b, const ReadState& st);

    // IS THIS BOOK ON "CONTINUE READING"? Exactly the books whose state is InProgress — which is what makes
    // the shelf and the badge one answer rather than two. It excludes, for free and without a second rule:
    // a finished book, a book never opened, a book abandoned at page one (no state at all), and the two
    // states somebody chose ON PURPOSE that are not "I am part-way through this" — Abandoned and Planned.
    bool continueReading(const Progress& p);

    // The Continue-reading shelf, in the order it shows: most recently read FIRST, then by natural title so
    // a hand-marked book nobody has opened has a stable place instead of an arbitrary one. Over `authors`
    // only — the series vector holds COPIES of books already filed there, and sweeping both would list a
    // book twice.
    //
    // It takes the PROGRESS supplier rather than the state one, so the shelf is built out of the very
    // answers the tiles are showing. A second lookup here could be correct on Tuesday and disagree with a
    // badge on Wednesday; there is nothing to keep in step because there is only one answer.
    QVector<Book> continueReadingBooks(const Index& idx, const std::function<Progress(const Book&)>& progressOf);

    // ------------------------------------------------------------------------------------------------
    // ONLINE BLANK-FILLING (issue #134 increment 2). Pure; see the header.
    // ------------------------------------------------------------------------------------------------

    // "Does this book have a picture already", asked of the caller because the answer is a filesystem
    // question (a cover inside the container, or a cover.* beside it) and this file touches no disk.
    using HasCoverFn = std::function<bool(const Book&)>;

    // WHAT AN ANSWER MAY CARRY. Every field but `title` is optional; an empty one is "the provider said
    // nothing".
    //
    // `title` IS WHAT THE PROVIDER THINKS IT ANSWERED ABOUT, and it is not a field to be filled — it is the
    // EVIDENCE. A book catalogue's search never says "I do not have that": ask Open Library about "Alpha
    // Chronicle" and it returns its best guess with the same confidence it returns Dune, so an answer taken
    // on trust puts a stranger's name under somebody's untagged scan. This repository has been bitten by
    // exactly that (a romhack search answering "Advance Wars" with "Guild Wars"), and the lesson written
    // down there is that the safety is a MATCH GATE. acceptedFill is where it is applied.
    // `year` and `pageCount` are EVIDENCE TOO (issue #294) and are never filled in anywhere: nothing on a
    // shelf reads them off an answer, they exist so that a second and a third field can agree or disagree
    // with the file. 0 means the provider did not say, which scores nothing rather than counting against.
    struct Fill
    {
        QString title;
        QString author;
        QString coverUrl;
        QString description;
        int     year = 0;
        int     pageCount = 0;
        bool isEmpty() const { return author.isEmpty() && coverUrl.isEmpty() && description.isEmpty(); }
    };

    // "Is this answer about THIS book": the two titles, normalised (case, punctuation and spacing dropped),
    // equal or one a whole-word prefix of the other — which is what lets "Dune" match "Dune (Dune
    // Chronicles, #1)" while refusing "Dune" for "Duneland Folk". An answer that names NO title is refused
    // outright: a reply that will not say what it is about is not evidence about anything.
    bool titleCorroborates(const QString& bookTitle, const QString& answerTitle);

    // ---- HOW SURE ARE WE THAT THIS ANSWER IS THIS BOOK (issue #294) ------------------------------------
    //
    // 0..100, and the SAME SHAPE #198 gave audiobook metadata rather than a second scheme: several fields
    // scored, one threshold, and nothing at all applied below it. The reason it had to stop being a title
    // comparison is that a title comparison cannot separate two real books that share a name — "Foundation",
    // "Bluebird", "The Gift" each name several unrelated books and the catalogue's first answer wins.
    //
    // THE TERMS, and why each is worth what it is:
    //   * titleCorroborates SURVIVES AS A PRECONDITION and scores 0 when it fails, so every invention #134's
    //     live run caught is refused by exactly the gate that caught it. It is necessary and never
    //     sufficient.
    //   * An EXACT title (folded) is worth more than one being a whole-word prefix of the other: a prefix is
    //     as likely to be a different book in the same series as the same book with a subtitle.
    //   * A DISTINCTIVE title — three or more words — is corroboration in itself. Open Library answered
    //     "Alpha Chronicle" with a book called something else entirely; it does not invent an exact
    //     five-word match. A one- or two-word title earns nothing here and needs a second field.
    //   * The AUTHOR either agrees or it does not, and a disagreement is disqualifying on its own: that is
    //     the "two Foundations" case, and it is the only term strong enough to settle it.
    //   * The YEAR the same way, with a year's slack for the edition/printing drift between what an EPUB
    //     stamps and what a catalogue calls first publication.
    //   * The PAGE COUNT is a NUDGE AND NEVER A PENALTY, because this app's count is chapters for an EPUB
    //     and page images for a comic — agreement is evidence, disagreement is usually two different units.
    //     Only counted above kComparablePages, below which our number is certainly not a publisher's pages.
    //
    // A book whose file says nothing but a short name, matched to a catalogue's first guess, now scores
    // BELOW the threshold and nothing is applied. That is deliberate: half-applying a half-match is the
    // failure #294 exists to stop, and a wrong author under somebody's scan is worse than a blank.
    int fillConfidence(const Book& b, const Fill& f);

    // Below this nothing is filled — acceptedFill returns an empty Fill and the blanks stay blank, which is
    // exactly what a failed lookup already looks like. Named and inline so a probe asserts against the same
    // number the code uses.
    inline constexpr int kFillAcceptThreshold = 60;
    // Below this many pages, our count is a chapter list or a handful of page images rather than anything a
    // publisher would call a page count, and the two sides are not comparable.
    inline constexpr int kComparablePages = 40;

    // WHICH BOOKS MAY BE ASKED ABOUT AT ALL. Empty when `enabled` is false — that is the "zero requests with
    // the setting off" guarantee, as a value a probe can assert over a whole library rather than as a
    // comment above a network call. A book is a target only when it is missing an author or missing a
    // cover; one that has both is never asked, however many times the sweep runs.
    QVector<Book> enrichmentTargets(const Index& idx, const HasCoverFn& hasCover, bool enabled);

    // WHAT OF `f` MAY ACTUALLY BE USED for `b`. Two gates, in this order:
    //   1. THE MATCH GATE — fillConfidence(b, f) must reach kFillAcceptThreshold, or the answer is dropped
    //      ENTIRELY, not field by field. Half of a wrong answer is still a wrong answer. (Issue #294 turned
    //      this from a title comparison into a score over several fields; a non-corroborating title still
    //      scores 0, so nothing that was refused before is admitted now.)
    //   2. Every field the book already has is DROPPED. This is "local metadata always wins" as a function
    //      rather than as a merge order somewhere downstream.
    // An all-empty result means the answer added nothing and nothing should be stored; a failed lookup is
    // simply an empty `f`, which returns an empty Fill and leaves the blank exactly as it was.
    Fill acceptedFill(const Book& b, bool hasCover, const Fill& f);

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
