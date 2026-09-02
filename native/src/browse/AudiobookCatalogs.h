// The BROWSE half of the local audiobook library (issue #139): the levels a person walks — Authors (and,
// beside them, Narrators and Series) -> that bucket's Books -> that book's files — as pure builders over
// AudiobookLibrary::Index.
//
// This is MusicCatalogs with different nouns and one level fewer, deliberately so. Nothing here decides
// anything about grouping — that is settled in AudiobookLibrary.h, including the rule this file must not
// quietly re-implement: a book is the audio files of ONE FOLDER sharing ONE BOOK IDENTITY, already ordered
// disc-then-track-then-natural-filename. These builders RENDER that index; they never regroup it.
//
// KEPT IN ITS OWN TRANSLATION UNIT, for exactly the reason MusicCatalogs.cpp gives: only the app and this
// feature's probe want AudiobookLibrary/AudioTags/TagLib linked, and folding these builders into
// SyntheticCatalogs would hand probe_browse, probe_iptv and probe_locallib a TagLib dependency they have no
// use for. Data in, a MediaCatalog out, no Settings read, no UI, no scan, and — unlike MusicCatalogs — no
// filesystem either: the cover resolver is injected with NO default, so this whole unit is pure.
//
// THE ROUTING CONTRACT is the `type` + `mime` pair on each row, spelled out as constants below because the
// surface dispatches on them and on nothing else. A book key contains a folder path and arbitrary tag text
// joined by 0x1F, so it can contain ':' — every reader takes the key as "everything after the prefix"
// (audiobookKeyOf), never as a colon-separated field.
//
// WHY A FILE ROW CARRIES A URL AND STILL NEEDS INTERCEPTING — the same reason a music track does, and the
// same fix. A part's `url` is its file path, so the tile behaves like the playable thing it is. But the
// generic "this item has a url" route would open it alone and queue its CONTAINING FOLDER, which for a
// series directory holding loose files is the wrong set and for a book that resumes across a file boundary
// is the wrong ORDER. So the surface intercepts kAudiobookFilePrefix ahead of that route — through
// browse::localLeafRoute, the ONE table both layouts read — and plays the BOOK from that file.
#pragma once
#include "../addons/AddonModels.h"        // MediaCatalog / MediaItem
#include "../core/AudiobookLibrary.h"     // the Index these render

#include <QString>
#include <QStringList>
#include <functional>

namespace browse
{
    // How a row gets its picture. Injected, and DEFAULTING TO NOTHING rather than to a filesystem lookup —
    // which is where this differs from MusicCoverFn on purpose. MusicCatalogs' default reaches MusicArt (an
    // extracted-cover cache and a sibling-image lookup), and that is the one thing keeping that unit from
    // being pure; there is no reason to inherit it here. The app injects MusicArt::keyedCover over the book's
    // key and folder; a probe injects nothing and pins the rows without touching a disk.
    using AudiobookCoverFn = std::function<QString(const AudiobookLibrary::Book&)>;

    // How a book row learns HOW FAR IN somebody is (issue #139, increment 2). Injected for the same reason
    // the cover is, and for one more: the answer is AudiobookLibrary::progressFor over the player's resume
    // marks and the reader's completion mark, and both of those are stores this translation unit is not
    // allowed to open. Not supplied (a probe, or any level that shows no bar) means every row reports the
    // default Progress — known == false — and the surface renders exactly what it did before.
    using AudiobookProgressFn = std::function<AudiobookLibrary::Progress(const AudiobookLibrary::Book&)>;

    // What the Audiobooks category says when there is nothing in it. Deliberately its OWN type rather than a
    // shared one with MusicEmptyNote: the two structs are identical today, and importing MusicCatalogs.h to
    // share three fields would drag MusicLibrary (and TagLib behind it) into every consumer of this header —
    // exactly what keeping these builders in their own translation unit exists to prevent.
    struct AudiobookEmptyNote
    {
        QString text;      // the sentence a person reads
        QString detail;    // the folder it is about, in native separators, or empty
        bool isEmpty() const { return text.isEmpty(); }
    };

    // ---- The routing contract ----------------------------------------------------------------------------
    // The three DOORS are keyless, like "Shuffle all music": each is one door, not one per anything.
    inline const char* kAudiobookAuthorType    = "_abauthor";
    inline const char* kAudiobookNarratorsType = "_abnarrators";   // the door: root -> the narrator list
    inline const char* kAudiobookNarratorType  = "_abnarrator";
    inline const char* kAudiobookSeriesListType = "_abserieslist"; // the door: root -> the series list
    inline const char* kAudiobookSeriesType    = "_abseries";
    inline const char* kAudiobookBookType      = "_abbook";
    inline const char* kAudiobookPlayType      = "_abplaybook";
    // The CHAPTERS door on a book (issue #139, increment 2). A '_' type like every other synthetic row here,
    // which is what sends it down the ordinary browse path on every one of the four layouts rather than to
    // the themed per-leaf action chooser.
    inline const char* kAudiobookChaptersType  = "_abchapters";
    // A part of a book IS a real, playable leaf, so its type has no leading '_': that is what gives it a
    // media tile rather than a synthetic row, and — on the themed layouts — what sends its Enter through the
    // per-leaf action chooser (browse::themedEnterFor splits on exactly that character). "audiobook" is
    // already a media type this app knows: core::mediaCategory files it under audio.
    inline const char* kAudiobookFileType      = "audiobook";

    inline const char* kAudiobookAuthorPrefix     = "audiobookauthor:";
    inline const char* kAudiobookNarratorsPrefix  = "audiobooknarrators:";
    inline const char* kAudiobookNarratorPrefix   = "audiobooknarrator:";
    inline const char* kAudiobookSeriesListPrefix = "audiobookserieslist:";
    inline const char* kAudiobookSeriesPrefix     = "audiobookseries:";
    inline const char* kAudiobookBookPrefix       = "audiobookbook:";
    inline const char* kAudiobookPlayPrefix       = "audiobookplay:";
    inline const char* kAudiobookChaptersPrefix   = "audiobookchapters:";
    // A file row's mime carries the BOOK it belongs to, which is what the router needs to queue the right
    // set in the right order. Declared in LeafRoute.h's kinds block too? No — a KEYED kind's contract lives
    // with its feature, exactly as kMusicTrackPrefix lives in MusicCatalogs.h. LeafRoute.cpp names this
    // constant from here.
    inline const char* kAudiobookFilePrefix       = "audiobookfile:";

    // "everything after `prefix`", or an empty string when `mime` does not start with it. The ONE reader, so
    // a key holding a ':' (a Windows folder path — "C:/Books/…" — which every book key on that platform
    // does) can never be truncated by a section() somewhere.
    //
    // INLINE, in the header, so that LeafRoute.cpp can read a file row's book key without linking this TU.
    inline QString audiobookKeyOf(const QString& mime, const char* prefix)
    {
        const QString p = QString::fromLatin1(prefix);
        return mime.startsWith(p) ? mime.mid(p.size()) : QString();
    }

    // ---- Level 1: the Audiobooks category root — every author --------------------------------------------
    // The "Narrators" and "Series" doors first — they are DIMENSIONS and the authors below are contents —
    // then one expandable row per author, in the index's order (display name, unknown bucket last).
    //
    // EACH DOOR IS OFFERED ONLY WHEN ITS DIMENSION HAS ANYTHING IN IT, which is the compatibility rule the
    // whole feature rests on: a collection whose files carry no narrator and no series tag gets a plain list
    // of authors and no idioms it did not ask for.
    //
    // `note` is what to say when the index has nothing: rather than an empty shelf the catalog then carries
    // ONE non-actionable "info" row saying it. It is a PARAMETER because only the caller can tell "no folder
    // chosen" from "still scanning" from "that folder has no books in it" — those need different sentences,
    // and each of them reads Settings or scan state, which is what this file has none of.
    MediaCatalog audiobookRootCatalog(const AudiobookLibrary::Index& idx, const AudiobookEmptyNote& note,
                                      const AudiobookCoverFn& cover = {});

    // ---- Level 2: one bucket's books ---------------------------------------------------------------------
    // Three entrances, one shape, because a narrator's books and an author's books are the same rows read
    // from two sides. An unknown key yields an empty, titled catalog: a stale route must not be able to
    // crash a navigation, and the surface re-reads the index on Back.
    //
    // `progress` is what puts the CONTINUE-LISTENING BAR on a book tile (#139 increment 2): each row carries
    // its own fraction in MediaItem::progress, because a book's position lives under its PARTS' keys and so
    // is not something the surface can look up from the row's own id the way it does for a film. Omitted, no
    // row carries one and the tiles are byte-for-byte what they were.
    MediaCatalog audiobookAuthorCatalog(const AudiobookLibrary::Index& idx, const QString& authorKey,
                                        const AudiobookCoverFn& cover = {},
                                        const AudiobookProgressFn& progress = {});
    MediaCatalog audiobookNarratorCatalog(const AudiobookLibrary::Index& idx, const QString& narratorKey,
                                          const AudiobookCoverFn& cover = {},
                                          const AudiobookProgressFn& progress = {});
    MediaCatalog audiobookSeriesCatalog(const AudiobookLibrary::Index& idx, const QString& seriesKey,
                                        const AudiobookCoverFn& cover = {},
                                        const AudiobookProgressFn& progress = {});

    // ---- The two dimension lists -------------------------------------------------------------------------
    // Narrators / Series, one row each, subtitled with how many books and how long. An index with none
    // yields an empty, titled catalog; the door that leads here is not offered in that case, so it is
    // reachable only by a stale route, and a stale route must be empty rather than a crash.
    MediaCatalog audiobookNarratorsCatalog(const AudiobookLibrary::Index& idx,
                                           const AudiobookCoverFn& cover = {});
    MediaCatalog audiobookSeriesListCatalog(const AudiobookLibrary::Index& idx,
                                            const AudiobookCoverFn& cover = {});

    // ---- Level 3: one book -------------------------------------------------------------------------------
    // Leads with a "Play book" ACTION row (kAudiobookPlayType), then the book's parts in the index's order.
    //
    // The action row exists because a book row one level up has to do exactly one thing when activated, and
    // for a MULTI-FILE book that thing is "show me what is in it"; without a row here, "play the book" would
    // be reachable only by pressing part one, which is not a thing anyone would guess. It is a row rather
    // than a button for the reason pcLauncherFilterRow gives: this app has four layouts and only one of them
    // has chrome a button could live in, while a row is D-pad reachable in all four by construction.
    //
    // A SINGLE-FILE BOOK STILL GETS BOTH — the verb and the one part — rather than a special case. The row
    // is what a person presses; the part is what tells them the book is one file, and pressing either plays
    // the same thing. A special case here would be a second answer to "what does this level look like",
    // and the first thing it would disagree about is where the resume position came from.
    //
    // ...then a CHAPTERS door (#139 increment 2), and only when there is more than one row behind it: a
    // chapterless single-file book has exactly one chapter — itself — and a list of one is a door that leads
    // where you already are. It is a ROW rather than a button for the reason the Play row above is one and
    // pcLauncherFilterRow is one: this app has four layouts and only one of them has chrome a button could
    // live in, while a row is D-pad reachable in all four BY CONSTRUCTION. That is what makes "both layouts"
    // a property of the shape here rather than a thing two code paths have to keep agreeing about.
    //
    // `progress` also decides what the Play row SAYS: "14h 20m left" or "Finished" leads its subtitle when
    // the book has been started, and nothing at all is added when it has not (or when some part's length is
    // unknown, which is the case that must never be guessed at).
    MediaCatalog audiobookBookCatalog(const AudiobookLibrary::Index& idx, const QString& bookKey,
                                      const AudiobookCoverFn& cover = {},
                                      const AudiobookProgressFn& progress = {});

    // ---- The chapter list itself -------------------------------------------------------------------------
    // The rows AudiobookLibrary::chapterRows produced, as the strings a NavMenu shows: the chapter/part name,
    // its length when the index knows one, and a marker on the one the listener is in. Pure, and here rather
    // than in the surface so the wording is pinned by the same probe the rows are — a list whose "you are
    // here" marker is on the wrong row is a bug no compiler finds.
    QStringList audiobookChapterMenuRows(const QVector<AudiobookLibrary::ChapterRow>& rows);
}
