// The BROWSE half of the Audiobookshelf client (issue #197): the levels a person walks — servers ->
// libraries -> (series / authors / books | podcasts -> episodes) -> a book's parts — as pure builders
// over what Audiobookshelf.h read off the wire.
//
// This is AudiobookCatalogs with a server behind it instead of a folder, and MusicCatalogs' server levels
// with books instead of records. Nothing here decides anything the protocol layer decided (ids, ordering,
// what a track is); these builders RENDER those answers. Data in, a MediaCatalog out — no Settings, no
// filesystem, no network, no UI — so probe_absclient drives every level headlessly.
//
// THE ROUTING CONTRACT is the `type` + `mime` pair on each row, spelled out below as constants because the
// surface dispatches on them and on nothing else.
//
//   type            mime                                              the level it opens
//   ------------    ----------------------------------------------    ------------------------------
//   _absservers     absservers:                                        the saved-server list (one door)
//   _absaddserver   absaddserver:                                      the add prompt (one door)
//   _absserver      absserver:<serverId>                               that server's libraries
//   _abslibrary     abslibrary:abs:<sid>:<libId>                       a library's doors, or its podcasts
//   _absserieslist  absserieslist:abs:<sid>:<libId>                    every series in it
//   _absseries      absseries:abs:<sid>:<libId><US><seriesName>        one series' books
//   _absauthors     absauthors:abs:<sid>:<libId>                       every author in it
//   _absauthor      absauthor:abs:<sid>:<libId><US><authorName>        one author's books
//   _absbooks       absbooks:abs:<sid>:<libId>                         every book in it
//   _absbook        absbook:abs:<sid>:<itemId>                         one book: play it, or its parts
//   _absplaybook    absplaybook:abs:<sid>:<itemId>                     PLAY (an action row, not a level)
//   _abspart        abspart:abs:<sid>:<itemId><US><index>              PLAY, starting at that part
//   _abspodcast     abspodcast:abs:<sid>:<itemId>                      one podcast's episodes
//   _absepisode     absepisode:abs:<sid>:<itemId>#<episodeId>          PLAY that episode
//
// EVERY TYPE STARTS WITH '_', which is not decoration: browse::themedEnterFor splits on exactly that
// character, so a leading underscore is what makes a row DRILL on the themed layouts rather than open the
// per-leaf Play/Favourite/Add-to-playlist chooser. These rows are all either levels or verbs of their own,
// and none of them is a file the chooser's Play could hand to a player — a part is a name that has to be
// minted (RemoteAudiobook.h), and the chooser's Download and Favourite have nothing to act on.
//
// A key may contain ':' (every qualified id does) and 0x1F (the composite keys above), so every reader
// takes the key as "everything after the prefix" — absKeyOf — and never as a colon-separated field. That
// is AudiobookCatalogs' rule and the trap it names; it applies here for the same reason.
#pragma once
#include "../addons/AddonModels.h"     // MediaCatalog / MediaItem
#include "../core/Audiobookshelf.h"    // the payloads these render

#include <QString>
#include <QStringList>
#include <functional>

namespace browse
{
    // How a row gets its picture: the qualified id in, a LOCAL FILE PATH out (empty when the cover has not
    // landed yet). Injected and defaulting to nothing, exactly as AudiobookCoverFn is, so this unit stays
    // pure and a probe can pin the rows without touching a disk.
    //
    // A LOCAL PATH AND NEVER THE SERVER'S URL, which is the whole reason this is a function rather than a
    // field: the cover endpoint takes the token in its query, and a MediaItem's thumbnailUrl is copied into
    // caches, Recents rows and item records. SubsonicClient.h states it; AbsClient fetches the bytes into
    // MetaCache and this hands back where they landed.
    using AbsCoverFn = std::function<QString(const QString& qualifiedId)>;

    // ---- The routing contract ---------------------------------------------------------------------------
    inline const char* kAbsServersType    = "_absservers";
    inline const char* kAbsAddServerType  = "_absaddserver";
    inline const char* kAbsServerType     = "_absserver";
    inline const char* kAbsLibraryType    = "_abslibrary";
    inline const char* kAbsSeriesListType = "_absserieslist";
    inline const char* kAbsSeriesType     = "_absseries";
    inline const char* kAbsAuthorsType    = "_absauthors";
    inline const char* kAbsAuthorType     = "_absauthor";
    inline const char* kAbsBooksType      = "_absbooks";
    inline const char* kAbsBookType       = "_absbook";
    inline const char* kAbsPlayBookType   = "_absplaybook";
    inline const char* kAbsPartType       = "_abspart";
    inline const char* kAbsPodcastType    = "_abspodcast";
    inline const char* kAbsEpisodeType    = "_absepisode";

    inline const char* kAbsServersPrefix    = "absservers:";
    inline const char* kAbsAddServerPrefix  = "absaddserver:";
    inline const char* kAbsServerPrefix     = "absserver:";
    inline const char* kAbsLibraryPrefix    = "abslibrary:";
    inline const char* kAbsSeriesListPrefix = "absserieslist:";
    inline const char* kAbsSeriesPrefix     = "absseries:";
    inline const char* kAbsAuthorsPrefix    = "absauthors:";
    inline const char* kAbsAuthorPrefix     = "absauthor:";
    inline const char* kAbsBooksPrefix      = "absbooks:";
    inline const char* kAbsBookPrefix       = "absbook:";
    inline const char* kAbsPlayBookPrefix   = "absplaybook:";
    inline const char* kAbsPartPrefix       = "abspart:";
    inline const char* kAbsPodcastPrefix    = "abspodcast:";
    inline const char* kAbsEpisodePrefix    = "absepisode:";

    // The join for a COMPOSITE key (a library plus a series, a book plus a part number). 0x1F, the same
    // character AudiobookLibrary's book keys and Subsonic's qualified ids use, because it cannot occur in
    // an id, a name or a file path — so splitting on it cannot cut a key in half.
    inline QChar absKeySep() { return QChar(0x1F); }
    inline QString absJoinKey(const QString& a, const QString& b) { return a + absKeySep() + b; }
    inline QString absKeyHead(const QString& k)
    {
        const int i = k.indexOf(absKeySep());
        return i < 0 ? k : k.left(i);
    }
    inline QString absKeyTail(const QString& k)
    {
        const int i = k.indexOf(absKeySep());
        return i < 0 ? QString() : k.mid(i + 1);
    }

    // "everything after `prefix`", or empty when `mime` does not start with it. INLINE so a router can read
    // a row's key without linking this translation unit.
    inline QString absKeyOf(const QString& mime, const char* prefix)
    {
        const QString p = QString::fromLatin1(prefix);
        return mime.startsWith(p) ? mime.mid(p.size()) : QString();
    }

    // Is this row one of ours at all? The ONE test the surfaces use to send a row down the Audiobookshelf
    // dispatch, rather than fourteen `type ==` comparisons written out twice (which is the drift
    // LeafRoute.h exists to have removed).
    inline bool isAbsType(const QString& type) { return type.startsWith(QLatin1String("_abs")); }

    // ---- The door onto the whole feature, shown on the Audiobooks root ------------------------------------
    // One row, keyless, like "Music Servers" and "Book Servers". `count` is how many servers are saved.
    MediaItem absServersRow(int count);

    // ---- Level 1: the saved servers ----------------------------------------------------------------------
    // One row per server plus, ALWAYS LAST AND ALWAYS PRESENT, the add row — with none saved that row is the
    // whole level, which is what makes the first server addable at all (the Live TV / Music Servers rule).
    // A disabled server is still listed, said to be off, and still openable: hiding it would leave "off"
    // with no way back on.
    MediaCatalog absServersCatalog(const QStringList& ids, const QStringList& names,
                                   const QStringList& urls, const QVector<bool>& enabled);

    // ---- Level 2: one server's libraries -----------------------------------------------------------------
    MediaCatalog absLibrariesCatalog(const QString& serverId, const QString& serverName,
                                     const QVector<Abs::Library>& libs);

    // ---- Level 3: one library --------------------------------------------------------------------------
    // A BOOK library gets three doors — Series, Authors, All Books — offered the way AudiobookCatalogs
    // offers its dimensions: each only when it has anything in it, so a library whose books carry no series
    // is a plain list of books and no idioms it did not ask for.
    //
    // A PODCAST library has no such dimensions and is its podcasts, one row each, straight away.
    MediaCatalog absLibraryCatalog(const QString& qualifiedLibraryId, const QString& libraryName,
                                   bool isPodcast, int seriesCount, int authorCount, int bookCount,
                                   const QVector<Abs::Item>& podcasts, const AbsCoverFn& cover = {});

    // ---- The two dimension lists -------------------------------------------------------------------------
    //
    // THE BUCKET KEY IS THE NAME, not the server's series/author id — the one place in this feature that is
    // true, and the .cpp says why at length: a library LISTING joins to these buckets by `seriesName` /
    // `authorName` and does not carry the ids these endpoints are keyed by. Nothing is persisted under it.
    MediaCatalog absSeriesListCatalog(const QString& qualifiedLibraryId, const QString& libraryName,
                                      const QVector<Abs::SeriesRow>& series);
    MediaCatalog absAuthorsCatalog(const QString& qualifiedLibraryId, const QString& libraryName,
                                   const QVector<Abs::AuthorRow>& authors);

    // ---- A list of books ---------------------------------------------------------------------------------
    // Three entrances, one shape: a series' books, an author's books and a whole library's books are the
    // same rows read from three sides. `serverId` is what the rows are qualified WITH, and is required —
    // an unqualified row could not be re-opened after a restart (Audiobookshelf.h says why at length).
    MediaCatalog absBooksCatalog(const QString& title, const QString& serverId,
                                 const QVector<Abs::Item>& items, const AbsCoverFn& cover = {});

    // ---- Level 4: one book -------------------------------------------------------------------------------
    // Leads with a "Play book" ACTION row, then the book's parts in the server's order. The action row is
    // there for the reason audiobookBookCatalog gives: a book row one level up has to do exactly one thing
    // when activated, and for a multi-file book that thing is "show me what is in it" — without a row here,
    // "play the book" would be reachable only by pressing part one, which is not a thing anyone would guess.
    //
    // A SINGLE-FILE BOOK STILL GETS BOTH, for the same reason, and the parts still say how long each is.
    // The chapter COUNT is on the play row rather than a chapter list of its own: chapters are what the
    // PLAYER navigates (that is the whole point of reading the server's list), and a second, browsable
    // chapter list would be a second place to press play with a different meaning.
    MediaCatalog absBookCatalog(const QString& qualifiedItemId, const Abs::Item& item,
                                const QVector<Abs::Track>& tracks, int chapterCount,
                                const AbsCoverFn& cover = {});

    // ---- A podcast's episodes ----------------------------------------------------------------------------
    // Newest first, which is what a podcast is read in and what every podcast client does. The server sends
    // them in its own order; this is the one place that order is decided, so both layouts get the same one.
    MediaCatalog absEpisodesCatalog(const QString& qualifiedItemId, const Abs::Item& item,
                                    const QVector<Abs::Episode>& episodes, const AbsCoverFn& cover = {});

    // ---- What a level says when it is empty or still loading ---------------------------------------------
    // ONE non-actionable "info" row rather than a blank shelf, the same shape AudiobookEmptyNote drives.
    // It is a PARAMETER of the caller because only the caller can tell "still fetching" from "that library
    // is empty" from "that server refused the sign-in" — and those need different sentences.
    MediaCatalog absNoteCatalog(const QString& title, const QString& text, const QString& detail = QString());
}
