// What Enter on a browse row DOES — as pure decisions, so the app's two surfaces cannot answer it
// differently.
//
// WHY THIS FILE EXISTS. A leaf's Enter reaches playback down two different paths depending on the layout:
//
//   * the CLASSIC grid calls HomeView::activateItem;
//   * the THEMED (Triple/XMB) column opens an inline Play / Favorite / Add-to-playlist chooser, and its
//     Play calls HomeView::playThemedLeaf.
//
// Both then have to answer the same question — "is this row a file this machine already has, which no addon
// can resolve?" — and until this file existed both answered it from a list of mimes and types written out by
// hand, in two places, which had already drifted apart three ways. A kind present in one list and missing
// from the other falls through to HomeView::resolvePlay, which has no local branch, and the user is told
// "Nothing to play for X" while the very same row plays on the other layout. That is issue #74's themed
// break (a music track), and, found while fixing it, the same break for a PHOTO (#102) and an OPDS BOOK
// (#146) — two kinds that were live bugs on the surface this app is actually used through.
//
// So the list is now a TABLE, localLeafKinds(), and localLeafRoute() is implemented BY it rather than
// beside it. Adding a local kind is one row, and both surfaces get it in the same edit because neither has
// a list of its own any more. The `=== themed local-leaf routing parity ===` gate in
// tools/run-headless-probes.sh holds that shape: it fails if either call site stops consulting the table,
// if a LeafPlay is handled on one surface and not the other, or if a kind is declared here and left out of
// the table. probe_leafroute pins the decisions themselves.
//
// NOTHING HERE TOUCHES THE WORLD. No UI, no Settings, no filesystem, no scan — the same rule
// MusicCatalogs.h and SyntheticCatalogs.h are written to. A caller that must look at the disk (the
// prefer-local lookup in playThemedLeaf, say) does that around this, not inside it.
#pragma once
#include "../addons/AddonModels.h"   // MediaItem
#include "../core/FavoritesStore.h"  // FavoriteItem — the struct only; nothing here reads or writes the store

#include <QString>
#include <QVector>

namespace browse
{
    // ---- The local-leaf kinds --------------------------------------------------------------------------
    // ONE home for each spelling: the catalog builder that stamps a row and the router that reads it back
    // name the same constant, so a rename cannot silently unroute a whole category. Clause 3 of the parity
    // gate requires every constant in this block to appear in the table in LeafRoute.cpp — declaring a kind
    // and forgetting to route it is a build failure, not a dead category discovered by a user.
    //
    // A KEYED kind (one whose mime carries a route id after a prefix) belongs with its feature, not here:
    // kMusicTrackPrefix lives in MusicCatalogs.h with the builder that stamps it and the musicKeyOf that
    // reads it, kAudiobookFilePrefix lives in AudiobookCatalogs.h and kJellyfinItemPrefix in
    // JellyfinCatalogs.h, all for the same reason. This block is for the kinds whose whole contract is the
    // spelling.
    //
    // --- LOCAL LEAF KINDS (the parity gate reads this block) ---
    inline const char* kLocalVideoMime = "local:video";   // a scanned local-library video (#8/#73)
    inline const char* kPhotoMime      = "photo";         // a photo tile (#102)
    inline const char* kOpdsBookType   = "opdsbook";      // a downloadable OPDS acquisition (#146)
    // A scanned local BOOK or COMIC (#134). ONE kind for both, because the route is identical: the row's
    // url is the file, and MainWindow's existing dispatch already opens .epub in the ebook reader, .pdf in
    // the PDF reader and .cbz in the comic reader. A key-carrying kind (the shape a music track and an
    // audiobook part need) would be wrong here — one file is one book, so there is no containing thing to
    // name and nothing for a key to point at.
    inline const char* kLocalBookMime  = "local:book";
    // --- END LOCAL LEAF KINDS ---

    // ---- What playing a local leaf MEANS ---------------------------------------------------------------
    enum class LeafPlay
    {
        NotLocal,    // not a local leaf: the caller's addon / stream resolve owns this row
        OpenFile,    // hand the item over as it stands — its url IS the file the player/viewer opens
        MusicAlbum,  // queue the ALBUM named by `key`, starting at this track's file (#74)
        OpdsBook,    // fetch with the catalog's OWN auth first, then open (#146)
        // Queue the BOOK named by `key`, starting at this file (#139). A separate route from MusicAlbum
        // rather than a reuse of it, because the two name keys in two different namespaces and are looked up
        // in two different indexes — collapsing them would let a book key that happened to parse as an album
        // key play the wrong thing, silently, with nothing in the type to say so.
        AudiobookBook,
        // A PLAYABLE ITEM ON A JELLYFIN SERVER (#83). `key` is the server-qualified id (#160), and the row
        // carries NO url on purpose: the stream link carries the token in its query, so it is minted at the
        // moment the player is handed it and never written into a row, a queue or a recents entry
        // (JellyfinCatalogs.h says why at length). That is the whole reason this is a route of its own
        // rather than OpenFile — OpenFile hands `url` to the player, and there is deliberately nothing
        // there to hand.
        //
        // It is in the LOCAL table despite the bytes being on somebody's server, because what "local" means
        // here is exactly what LeafRoute.h's opening paragraph says it means: no addon can resolve this
        // row, so both surfaces have to claim it or one of them answers "Nothing to play".
        JellyfinItem,
        // A STARRED LIVE TV CHANNEL, shown on the ★ Favorites shelf (#244). `key` is the channel identity
        // (`livetv:<tvg-id>` / `livetv:name:<name>`), and the row carries NO url for exactly the reason a
        // Jellyfin row carries none: the stream link is minted at open from this device's own sources and is
        // never written into a row — an IPTV url puts the provider's credential in its PATH, which #200's
        // scrub cannot touch, which is the whole of #203.
        //
        // A route of its own rather than a reuse of JellyfinItem, though both end in the same openRecent
        // call: the two name keys in two different namespaces, resolved by two different subsystems, and one
        // enumerator covering both would let a mis-stamped mime send a channel at a media server.
        LiveTvChannel,
    };

    struct LeafRoute
    {
        LeafPlay play = LeafPlay::NotLocal;
        QString  key;   // MusicAlbum: the album key the surface hands to PlaybackSession.
                        // AudiobookBook: the book key, likewise. Empty otherwise.
        bool isLocal() const { return play != LeafPlay::NotLocal; }
    };

    // One row of the table. `field` names WHICH of a MediaItem's two routing fields carries the kind,
    // because the existing rows genuinely differ: local:video and photo are mimes, opdsbook is a type, and
    // collapsing them onto one field would have to rewrite three catalog builders to make the table tidy.
    struct LocalLeafKind
    {
        enum Field { Mime, Type };
        const char* id;      // the spelling, from the block above (or a keyed feature's own constant)
        Field       field;   // which field it names
        bool        prefix;  // true: a keyed kind, matched by prefix. false: matched whole.
        LeafPlay    play;
    };

    // THE table. Exposed so probe_leafroute can walk every kind rather than a hand-picked few — a probe
    // with its own list of kinds would be the very thing this file replaced.
    const QVector<LocalLeafKind>& localLeafKinds();

    // The routing answer for one row. NotLocal when no kind claims it, and ALSO when the claiming kind's
    // row is unusable: a file route with no url, or a music track whose mime carries no album key. Those
    // fall through to the resolve they would have taken anyway rather than being claimed and dropped,
    // which is the difference between "this surface can't play it" and a silent no-op.
    LeafRoute localLeafRoute(const MediaItem& it);

    // ---- What Enter on a THEMED row does ---------------------------------------------------------------
    // The themed column's fork, lifted out of MainWindow::showThemedXmb so it can be stated headlessly.
    // A container or a synthetic row acts through the ordinary browse path; a real leaf opens the inline
    // chooser. A GUIDANCE row ("info") — the sentence explaining why a level is empty, which browseItems
    // lets through alone so a themed column is never simply blank — drills, deliberately: activateItem
    // refuses type "info", so the ordinary path is an intentional no-op, whereas the chooser would offer
    // Play / Favorite / Download over a line of prose and its Play could only ever say "Nothing to play".
    enum class ThemedEnter { Drill, Chooser };
    ThemedEnter themedEnterFor(const QString& type, bool expandable);

    // ---- What "add this row to the queue" MEANS (issue #193, increment 2) -------------------------------
    // #193 increment 1 gave PlaybackSession enqueue()/playNext() and gave the now-playing page a panel that
    // could call them. Nothing that could reach them from a row you are BROWSING existed, so the oldest verb
    // in music software — "I am listening to something, I found another track, put it at the end" — was not
    // reachable at all. This is the decision behind the row: which browse rows carry those verbs, and what
    // the verb is being asked to add.
    //
    // It is HERE, beside localLeafRoute, on purpose. A track's Enter and a track's "add to queue" have to
    // agree about what a track row IS, and the track case below is decided by CALLING localLeafRoute rather
    // than by a second reading of the mime — so a kind that plays cannot be a kind that will not queue, and
    // a rename of the track prefix cannot unroute one of the two.
    enum class QueueAdd
    {
        None,   // not a music row: no queue verbs on it
        Track,  // one file: the track this row names
        Album,  // one record: every track on it, in the index's order (MusicQueue::forAlbum)
    };

    struct QueueTarget
    {
        QueueAdd what = QueueAdd::None;
        QString  albumKey;    // the record; BOTH kinds carry one (a track is queued out of its album's order)
        QString  trackPath;   // Track only: the one file to add. Empty for Album.
        bool ok() const { return what != QueueAdd::None; }
    };

    // The answer for one row. None for everything that is not local music — a film, a game, a photo, a book,
    // an artist container, a composer's work. `None` is also the answer for a music row that names nothing
    // addable (a track with no file, an album row with no key), for the same reason localLeafRoute refuses
    // those: offering a verb that can only no-op is worse than not offering it.
    QueueTarget queueTargetFor(const MediaItem& it);

    // ---- Starring a music TRACK from a MENU (issue #297) ------------------------------------------------
    // The classic layout's two menus on a browse row — Start (MainWindow::openBrowseContextMenu) and the
    // right-click (showBrowseQueueMenu) — carried the queue verbs and nothing else, so a track could be starred
    // only from the THEMED chooser's Favorite row. That row was also the only door #193 increment 6's server
    // star had, because the star is not sent by any button: FavoritesStore's love hook sends it when a
    // favourite of type "track" is ADDED. So the classic verb needs no server code, only a favourite.
    //
    // WHICH ROWS: exactly the ones queueTargetFor calls a Track. The same reading, so a row that queues as a
    // track is a row that stars as one. An album row is not offered — the themed layout cannot star one
    // either (it is a '_' row and drills), and this change adds the verb the themed row has, not a new one.
    //
    // THE RECORD is the shape the themed chooser's generic arm writes (HomeView::favoriteThemedLeaf): itemId
    // is the row's id, which for a track is its path — the id adoptStarredFavourites files a server star
    // under, and the one the love hook recovers the track's tags by. Two surfaces writing two shapes of one
    // favourite would be two records for one track, and a heart that cannot find the other.
    FavoriteItem trackFavoriteFor(const MediaItem& it);   // itemId empty = not a track row: offer nothing

    // What the menu row SAYS, and does: Favorite on a track that is not one, Remove on a track that is. A verb
    // saying the opposite of what it does is worse than no verb, so the label is decided here, not guessed.
    enum class TrackFavVerb { None, Add, Remove };
    TrackFavVerb trackFavoriteVerb(const FavoriteItem& fav, bool alreadyFavorite);

    // ---- The rest of a TRACK row's verbs on the classic menus: Add to playlist, Download (issue #365) ------
    // #297 put Favorite in the classic layout's two menus (Start, and the right-click / long-press) and named two
    // more verbs the themed chooser has that those menus lacked. This is the decision behind both, out of HomeView
    // so a probe can state it; HomeView::trackMenuForRow supplies the one fact it cannot see (the add-on).
    //
    // WHICH ROWS ARE TRACK ROWS. Exactly two families:
    //   * a LIBRARY track — local, or a Subsonic / Jellyfin / EverythingBox-server track merged into the same
    //     artists — which is what queueTargetFor calls a Track, the reading #297's Favorite already makes;
    //   * an ADD-ON's track: a leaf typed track / song / music on a level (or a row) that names an add-on.
    //
    // ADD TO PLAYLIST on both: the picker the P key has always opened on any such row.
    //
    // DOWNLOAD ONLY WHERE A DOWNLOAD HAPPENS. #365 drove the themed Download verb on each kind first:
    //   a local library track   "Nothing here could be downloaded." — and it is already on this machine
    //   a Subsonic track        the same sentence; no request ever reaches the server
    //   a SCRIPT add-on's track (the AIO catalog's MusicBrainz rows: metadata, no url) — the same sentence
    //   a REMOTE add-on's track whose /stream answers — downloaded, byte for byte
    // because HomeView's download crawl has an arm for a remote add-on's leaf (its /stream) and none for a row
    // with no add-on or for a script add-on's track. So Download is offered for exactly the last kind: a verb
    // that can only say "nothing here" is not copied onto a second surface. The themed chooser offering it on
    // every leaf is #372.
    enum class TrackAddon { None, Script, Remote };   // the add-on the row's download crawl would walk
    struct TrackMenuVerbs
    {
        bool playlist = false;   // "Add to playlist…"
        bool download = false;   // "Download"
        bool any() const { return playlist || download; }
    };
    TrackMenuVerbs trackMenuVerbsFor(const MediaItem& it, TrackAddon addon);

    // ---- Is Download OFFERED on this row? (issue #372) ----------------------------------------------------
    // The themed layout had two surfaces answering this, differently. The detail view's action row asked
    // HomeView::classicActionGates; the XMB inline chooser asked nothing and offered Download on every leaf —
    // including the three track kinds #365 drove, where the press can only say "Nothing here could be
    // downloaded." Both now read ONE answer, HomeView::downloadOfferedFor, which is downloadOffered() below fed
    // the facts only HomeView can see (the add-ons, the disk).
    //
    // THE CRAWL'S OWN ARM TABLE. What a Download press does to a leaf is HomeView::dlResolveLeaf, and it
    // DISPATCHES ON downloadLeafArmFor — so an offer cannot name an arm the press does not have:
    //   None          a store-launcher game (Steam / Epic / GOG / Battle.net) or a page-based chapter, neither
    //                 of which can be pulled as one file; or a leaf no arm claims
    //   LocalBridge   a comic issue / book / audiobook / game under a SCRIPT add-on: the file provider's title
    //                 search
    //   RemoteStream  any leaf under a REMOTE add-on: its own /stream (a game falls back to the title search)
    //   MetaBridge    a movie / episode / series / tv from anywhere else: its /meta names the IMDB id, and the
    //                 stream add-ons resolve that
    // `addon` is the add-on the crawl walks: the level's, else the row's own sourceAddonId (downloadBrowseItem).
    //
    // WHY NOT JUST classicActionGates. It was built for the classic info page's remote and bridged leaves, and
    // it names fewer rows than the crawl downloads: a remote add-on's track (which #365 downloaded byte for
    // byte), a remote add-on's game, an AIO Catalog film (the MetaBridge arm), a row that names its own add-on
    // on a level with none. Pointing the chooser at it alone would have taken a working Download away from
    // each of those. So its answer is KEPT (everything it offered is still offered) and the crawl's arms are
    // added to it — which is why the detail row only ever gains.
    enum class DownloadLeafArm { None, LocalBridge, RemoteStream, MetaBridge };
    DownloadLeafArm downloadLeafArmFor(const MediaItem& it, TrackAddon addon);

    // A page-based chapter ("manga_chapter", …): read page by page, never pulled as one file. The one
    // definition; HomeView's own isReadableChapter answers through it.
    bool isReadableChapterType(const QString& type);

    struct DownloadOfferFacts
    {
        TrackAddon addon = TrackAddon::None; // the add-on the crawl would walk
        bool alreadyLocal = false;    // a local game file, or a Recent / Downloaded row: it is already saved
        bool classicGate = false;     // HomeView::classicActionGates(item).download — the detail row's old answer
        bool fileProvider = false;    // an enabled file provider for the LocalBridge search to ask
        bool streamProvider = false;  // a stream provider for the row's kind, which the MetaBridge resolve needs
    };
    bool downloadOffered(const MediaItem& it, const DownloadOfferFacts& facts);
}
