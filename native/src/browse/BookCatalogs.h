// The BROWSE half of the local reading library (issue #134): the levels a person walks — Authors (and,
// beside them, Series) -> that bucket's books — as pure builders over BookLibrary::Index.
//
// This is AudiobookCatalogs with different nouns and ONE LEVEL FEWER, and the missing level is the whole
// difference between the two features. An audiobook is a folder of files, so it needs a level that shows
// what is inside it before anything can be played. A book is ONE FILE: the row for it IS the leaf, its url
// IS what the reader opens, and a level in between would be a page whose only content was the thing you
// just pressed. So a book row is an ordinary local leaf (browse::kLocalBookMime, LeafPlay::OpenFile) and
// this file has no play-action row, no parts list and no key-carrying leaf mime.
//
// Nothing here decides anything about grouping — that is settled in BookLibrary.h, including the rule this
// file must not quietly re-implement: a comic's series comes from ComicName's folder-corroborated reading of
// the filename, already applied. These builders RENDER that index; they never regroup it.
//
// KEPT IN ITS OWN TRANSLATION UNIT for the reason MusicCatalogs.cpp and AudiobookCatalogs.cpp give: only the
// app and this feature's probe want BookLibrary linked, and folding these builders into SyntheticCatalogs
// would hand probe_browse, probe_iptv and probe_locallib a dependency they have no use for. Data in, a
// MediaCatalog out, no Settings read, no UI, no scan, and no filesystem: the cover resolver is injected with
// NO default, so this whole unit is pure.
//
// THE ROUTING CONTRACT is the `type` + `mime` pair on each row, spelled out as constants below because the
// surface dispatches on them and on nothing else. A series key is arbitrary text and a book key is a full
// path — "C:/Books/…" on Windows — so every reader takes the key as "everything after the prefix"
// (bookKeyOf), never as a colon-separated field.
#pragma once
#include "../addons/AddonModels.h"     // MediaCatalog / MediaItem
#include "../core/BookLibrary.h"       // the Index these render

#include <QString>
#include <functional>

namespace browse
{
    // How a row gets its picture. Injected, and DEFAULTING TO NOTHING rather than to a filesystem lookup —
    // the same choice AudiobookCoverFn makes and for the same reason: the app injects MusicArt::keyedCover
    // over the book's key and folder, and a probe injects nothing and pins the rows without touching a disk.
    using BookCoverFn = std::function<QString(const BookLibrary::Book&)>;

    // What the Books category says when there is nothing in it. Its own type rather than a shared one, for
    // the reason AudiobookEmptyNote gives: importing another feature's header to share three fields would
    // drag that feature's library into every consumer of this one.
    struct BookEmptyNote
    {
        QString text;      // the sentence a person reads
        QString detail;    // the folder it is about, in native separators, or empty
        bool isEmpty() const { return text.isEmpty(); }
    };

    // ---- The routing contract ----------------------------------------------------------------------------
    inline const char* kBookAuthorType     = "_bkauthor";
    inline const char* kBookSeriesListType = "_bkserieslist";   // the door: root -> the series list
    inline const char* kBookSeriesType     = "_bkseries";

    inline const char* kBookAuthorPrefix     = "bookauthor:";
    inline const char* kBookSeriesListPrefix = "bookserieslist:";
    inline const char* kBookSeriesPrefix     = "bookseries:";

    // The LEAF types. No leading '_', which is what gives them a media tile rather than a synthetic row and
    // — on the themed layouts — sends their Enter through the per-leaf action chooser (themedEnterFor splits
    // on exactly that character). Both are types this app already knows: core::mediaCategory files "book"
    // and "comic" under `reading`, so these rows land in the category they belong to with no new bucket.
    inline const char* kBookLeafType  = "book";
    inline const char* kComicLeafType = "comic";

    // "everything after `prefix`", or an empty string when `mime` does not start with it. The ONE reader, so
    // a key holding a ':' can never be truncated by a section() somewhere. INLINE so other translation units
    // can read a row's key without linking this one.
    inline QString bookKeyOf(const QString& mime, const char* prefix)
    {
        const QString p = QString::fromLatin1(prefix);
        return mime.startsWith(p) ? mime.mid(p.size()) : QString();
    }

    // ---- Level 1: the Books category root — every author -------------------------------------------------
    // The "Series" door first — it is a DIMENSION and the authors below are contents — then one expandable
    // row per author, in the index's order (display name, unknown bucket last).
    //
    // THE DOOR IS OFFERED ONLY WHEN THE DIMENSION HAS ANYTHING IN IT, which is the compatibility rule the
    // whole feature rests on: a shelf of standalone novels with no series metadata and no numbered comics
    // gets a plain list of authors and no idiom it did not ask for.
    //
    // `note` is what to say when the index has nothing: rather than an empty shelf the catalog then carries
    // ONE non-actionable "info" row saying it. It is a PARAMETER because only the caller can tell "no folder
    // chosen" from "still scanning" from "that folder has no books in it" — those need different sentences,
    // and each of them reads Settings or scan state, which is what this file has none of.
    MediaCatalog bookRootCatalog(const BookLibrary::Index& idx, const BookEmptyNote& note,
                                 const BookCoverFn& cover = {});

    // ---- Level 2: one bucket's books ---------------------------------------------------------------------
    // Two entrances, one shape, because an author's books and a series' books are the same rows read from
    // two sides. An unknown key yields an empty, titled catalog: a stale route must not be able to crash a
    // navigation, and the surface re-reads the index on Back.
    MediaCatalog bookAuthorCatalog(const BookLibrary::Index& idx, const QString& authorKey,
                                   const BookCoverFn& cover = {});
    MediaCatalog bookSeriesCatalog(const BookLibrary::Index& idx, const QString& seriesKey,
                                   const BookCoverFn& cover = {});

    // ---- The dimension list ------------------------------------------------------------------------------
    // Series, one row each, subtitled with how many books. An index with none yields an empty, titled
    // catalog; the door that leads here is not offered in that case, so it is reachable only by a stale
    // route, and a stale route must be empty rather than a crash.
    MediaCatalog bookSeriesListCatalog(const BookLibrary::Index& idx, const BookCoverFn& cover = {});
}
