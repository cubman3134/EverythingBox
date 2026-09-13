// What pressing a row on Home's ★ FAVORITES shelf DOES (issue #364) — as a pure decision, because the function
// that acts on it, HomeView::openFavorite, lives in a class no probe can link.
//
// THE ORDER is the one openFavorite always had, with ONE arm added:
//
//   1. (HomeView, ahead of this) a merged PC game — rebuilt from the library as it is now, which nothing pure
//      can see.
//   2. A stored record carrying a PATH re-opens by it (a local game, a Live TV channel, a channel).
//   3. A Steam / Epic id re-opens the store's own info page.
//   4. A MUSIC TRACK — the new arm. See below.
//   5. Everything else is an add-on's item: its detail page, or a sentence when the add-on is gone.
//
// WHY 4 EXISTS. A track favourite — written by browse::trackFavoriteFor (#297), by the themed chooser's generic
// arm (HomeView::favoriteThemedLeaf) and by HomeView::adoptStarredFavourites for a server's own stars — holds
// the track's id and type "track", and nothing else: no path, no kind, no add-on. It skipped 2 and 3, landed on
// 5, looked for an add-on it never had, and told the user "That favourite's source addon isn't available."
//
// It keys on TYPE, not on a path the record could be taught to carry, because every track favourite already
// stored was written without one: this opens the stars people already have, with no migration. And it keys on
// type "track" WITH NO ADD-ON. A music track row belongs to no add-on (all three writers leave addonId empty);
// an add-on's own catalogue item that happens to be typed "track" names its add-on, and still opens there.
//
// WHICH PLAY PATH, per kind of track id. Both are doors that already exist; nothing new is resolved here.
//   * A Subsonic-qualified id: MainWindow::openRecent's qualified-TRACK arm, which mints a fresh stream url
//     from the id alone (MusicSupply::playUrl -> openAudioStream) — the door a playlist entry opens by. It
//     needs no cached index, so a star adopted last session opens before any Music level has been browsed
//     (the Starred level's own queue, openMusicAlbum over the starred record, needs that record fetched
//     first and would otherwise say the album is no longer in the library).
//   * A JELLYFIN music track (#368): the same door, with the same property — JellyfinMusicClient::streamUrl
//     mints from the id and the server's stored sign-in, with nothing fetched first, so it opens straight after
//     a restart. openRecent's Jellyfin arm would otherwise take the id for a VIDEO (Jellyfin ids carry no kind),
//     so its remote-track arm sits ahead of that one and claims a Jellyfin id only with kind "audio". A server
//     that has been removed says so; one that is switched off says THAT.
//   * An EVERYTHINGBOX-SERVER track (#368) does NOT mint from its id: its url may be signed, so the client
//     holds it for one session, filled when the track's ALBUM is fetched, and nothing persists it. And the id
//     cannot name that album — it carries the shelf and the track, and the server answers a track id with
//     nothing (its detail route expands artists and albums only). So the favourite carries it: every track
//     favourite records the album its row was on (FavoriteItem::albumKey), and this hands openRecent the album
//     as the path. Cold, openRecent fetches that album and then plays; warm, it plays at once. A star written
//     before #368 has no album; it opens when this session already holds its url, and otherwise SAYS what to do.
//   * An unqualified id is LOCAL by definition (MusicSupply's own rule, and structural: see Subsonic.h) and is
//     the file — or, for a cue track, mpv's clip url of it (MusicLibrary::IndexTrack::path either way).
//     - WHEN THE LOCAL MUSIC INDEX HOLDS IT (issue #369) it opens its ALBUM, starting at it: LocalAlbum,
//       which the caller hands to MainWindow::openMusicAlbum(albumKey, path) — the door a library track row
//       plays by. The album's tracks are already in disc-then-track order (MusicLibrary::Album::tracks), so
//       a disc-2 favourite queues both discs, in track order, whatever the files are called. The album is
//       found by MusicLibrary::Index::track — the index walked, not a second path -> album table beside it.
//     - WHEN IT DOES NOT (not scanned yet, outside the library roots, or dropped by a rescan) it re-opens by
//       openRecent with kind "audio" exactly as #364 made it: the route that same track's own Recents row
//       takes (startLocalAudioQueue files it under that kind). Never worse than before #369.
//     Either way the FILE is checked first: the track's own file, or for a cue track the one file its clip
//     url names (IndexTrack::sourcePath, else CueSheet::clipFile) — so a moved rip says so instead of failing
//     inside the player. A clip url whose file cannot be read out of it is opened as before, unchecked.
//
// NOTHING HERE WRITES THE STORE, and neither may the caller. FavoritesStore's love hook sends a server star
// when a "track" favourite is added, and pressing Play is not starring.
//
// NOTHING HERE TOUCHES THE WORLD either: the three questions it has to ask (is the file there, is the server
// set up, is the add-on loaded) arrive as FavoriteWorld, so probe_leafroute can answer them.
//
// Its own translation unit, not a section of LeafRoute.cpp, because it reads three suppliers' id parsers and
// LeafRoute is linked by probes (probe_musicqueue) that deliberately carry none of them.
#pragma once
#include "../addons/AddonModels.h"   // MediaItem
#include "../core/FavoritesStore.h"  // FavoriteItem — the struct; nothing here reads or writes the store

#include <QString>
#include <QVector>
#include <functional>

namespace MusicLibrary { struct Index; }   // the local music index the router looks a track up in (#369)

namespace browse
{
    enum class FavoriteOpen
    {
        ReopenByPath,     // the stored record's path/kind -> openRecent (arm 2)
        NativeStore,      // steam: / epic: -> the store's info page (arm 3)
        LocalAlbum,       // a track the local music index holds -> openMusicAlbum(its album, the track) (#369)
        LocalTrack,       // a track on this machine the index does not hold -> openRecent(file, "audio")
        ServerTrack,      // a music server's track -> openRecent(…, "audio", qualified id) -> a fresh stream url
        TrackFileGone,    // a local track whose file has been moved or deleted
        TrackServerGone,  // a music server's track whose server is no longer set up
        TrackServerOff,   // a Jellyfin track whose server is set up but switched off (#368)
        TrackAlbumUnknown,// an EverythingBox-server track starred before #368, whose album nothing has fetched
        Addon,            // an add-on's item -> its detail page (arm 5)
        AddonMissing,     // ...whose add-on is not available here
    };

    struct FavoriteRoute
    {
        FavoriteOpen how = FavoriteOpen::AddonMissing;
        // What openRecent is handed, for ReopenByPath / LocalTrack / ServerTrack. Empty otherwise — except that
        // LocalAlbum carries the track's `path` too (openMusicAlbum's start row), with its title and cover.
        // For an EverythingBox-server track (#368) `path` is the ALBUM the track is on and `resumeKey` the
        // track: openRecent reads the key first for a music identity, and the path is where it plays from.
        QString path, kind, resumeKey, title, thumb;
        QString albumKey; // LocalAlbum / a server-shelf ServerTrack: the album the track is ON
        QString addonId;  // Addon / AddonMissing: the source add-on the favourite names
    };

    // What the router has to ask the world. An unset question answers false.
    struct FavoriteWorld
    {
        std::function<bool(const QString& file)>     fileExists;   // a local track's file is on disk
        std::function<bool(const QString& serverId)> serverKnown;  // a Subsonic server is configured
        std::function<bool(const QString& addonId)>  sourceKnown;  // an add-on is among the loaded sources
        // The local music library's index (#369) — the one MainWindow::openMusicAlbum plays a local album out
        // of (MusicSupply::indexFor answers MusicLibrary::index() for an unqualified key). Unset: no track is
        // in it, and a local track opens by openRecent as it did before #369.
        const MusicLibrary::Index* localMusic = nullptr;
        // #368. A Jellyfin server is in the store / is switched on (JellyfinMusicClient::streamUrl mints
        // nothing for one that is off).
        std::function<bool(const QString& serverId)> jellyfinKnown;
        std::function<bool(const QString& serverId)> jellyfinOn;
        // #368. An EverythingBox server's music shelf is among the connected ones / this session already holds
        // a url for a track of it (ServerMusicClient::has / hasStreamUrl).
        std::function<bool(const QString& sourceId)> shelfKnown;
        std::function<bool(const QString& trackId)>  shelfUrlReady;
    };

    // THE OTHER HALF: WHAT MainWindow::openRecent OPENS (#368), for the (path, kind, resumeKey) it was handed —
    // a Jellyfin music track or an EverythingBox-server track, by qualified id, plus the album to fetch for a
    // shelf track this session holds no url for. Both empty = neither; openRecent's other arms own it. A
    // Subsonic id keeps the arm #364 used, and a Jellyfin id with any kind but "audio" is a video, which
    // openJellyfinItem opens (Jellyfin ids carry no kind, so the caller's kind is the only thing that can say).
    struct RemoteTrackOpen { QString trackId, albumKey; };
    RemoteTrackOpen remoteTrackOpenFor(const QString& path, const QString& kind, const QString& resumeKey);

    // ...and the sequence it runs, with the doors handed in so a probe can walk it. A url is minted and played
    // at once when one can be (a Jellyfin track always, from its id and the server's stored sign-in; a shelf
    // track whose album this session fetched). Otherwise a shelf track's album is fetched first — its urls come
    // with it — and then played. Every failure is ONE sentence of our own: never built from a url or a request.
    struct RemoteTrackDoors
    {
        std::function<QString(const QString& trackId)> mint;        // MusicSupply::playUrl — THE url minter
        std::function<void(const QString& albumKey,
                           std::function<void(bool ok, const QString& sentence)> done)> fetchAlbum;
        std::function<void(const QString& url, const QString& trackId)> play;   // openAudioStream
        std::function<void(const QString& sentence)> say;                        // a visible notice
    };
    void openRemoteTrack(const RemoteTrackOpen& o, const QString& title, const RemoteTrackDoors& doors);

    // The ★ Favorites shelf row for one stored favourite: the fields the router reads back (id, type, and the
    // "fav:<addonId>" marker in mime) plus what the row shows. HomeView's buildFavorites calls this and then
    // swaps in its offline-first cached cover, so the probe routes the row the shelf really builds.
    MediaItem favoriteShelfRow(const FavoriteItem& f);

    // `favItem` is the shelf row as drawn; `stored` is FavoritesStore::list(), read by the caller.
    FavoriteRoute favoriteRouteFor(const MediaItem& favItem, const QVector<FavoriteItem>& stored,
                                   const FavoriteWorld& world);

    // The one sentence a track that cannot be opened says (TrackFileGone / TrackServerGone / TrackServerOff /
    // TrackAlbumUnknown), naming it. Empty for every other route: those open something, or keep their own sentence (AddonMissing's
    // lives at its call site, unchanged). Here rather than in HomeView so a probe can hold each failure to its
    // OWN sentence, and hold all three away from the add-on message — which is wrong for a track, and sends
    // somebody looking for an add-on the track never had.
    QString favoriteOpenSentence(FavoriteOpen how, const QString& title);
}
