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
}
