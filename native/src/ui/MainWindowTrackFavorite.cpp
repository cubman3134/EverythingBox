// Starring a music TRACK from the classic layout's menus (issue #297) — MainWindow's members for it, in their
// own translation unit for the reason MainWindowPlayOn.cpp gives: MainWindow.cpp is the busiest merge surface
// in the repository, and this costs it two call sites instead of a block of definitions.
//
// WHAT WAS MISSING. On the classic layout the two menus a browse row has — Start (openBrowseContextMenu) and
// the right-click / long-press (showBrowseQueueMenu) — offered a music track the queue verbs and nothing else.
// A track could be starred only from the THEMED chooser, and since #193 increment 6 that also made the themed
// chooser the only door to a music server's star.
//
// WHY THERE IS NO SERVER CODE HERE. The server star is not sent by a button. FavoritesStore::setLoveHook
// (installed in MainWindow's constructor) fires on every add() and remove() of a favourite whose type is
// "track", and routes it to the scrobbler's love/star path. So a press here only has to reach the store —
// through toggle(), which is add()/remove(), and NEVER through addFromSource(), which exists for stars that
// came FROM a server and deliberately does not fire the hook. probe_leafroute §7 watches the hook fire for
// this path and stay quiet for that one.
#include "MainWindow.h"

#include "FeedbackPolicy.h"
#include "../browse/FavoriteRoute.h"     // #368: remoteTrackOpenFor / openRemoteTrack
#include "../core/FavoritesStore.h"
#include "../core/ServerMusicClient.h"   // #368: a cold shelf track's album fetch
#include "../core/SubsonicClient.h"      // MusicSupply::playUrl — the one place a credential enters a queue

#include <QPointer>
#include <QTimer>

// The label said once for both classic menus, so the two cannot come to disagree about what the row does.
// Decided by browse::trackFavoriteVerb: Favorite on a track that is not one, Remove on a track that is.
QString MainWindow::trackFavoriteVerbLabel(browse::TrackFavVerb verb) const
{
    return verb == browse::TrackFavVerb::Remove ? tr("Remove from Favorites") : tr("Favorite");
}

// The press. `fav` was resolved from the row BEFORE the menu opened (both callers say why: the grid can move
// under a NavMenu's nested loop, and re-reading the cursor here would star whatever it moved to).
void MainWindow::pressTrackFavorite(const FavoriteItem& fav)
{
    if (fav.itemId.isEmpty()) return;
    const bool nowFavorite = FavoritesStore::toggle(fav);
    notify(nowFavorite ? tr("Added “%1” to Favorites.").arg(fav.title) : tr("Removed from Favorites."),
           kFeedbackShort);
}

// ---- #365: the rest of a track row's verbs, in the same two menus -----------------------------------------
// Which rows get them is browse::trackMenuVerbsFor (LeafRoute.h, with the evidence #365 drove for Download);
// the presses are HomeView's own entries — queueAddToPlaylist (the P key's picker, on a copy, a turn later) and
// downloadBrowseItem (the themed Download's crawl). Only the words live here, said once for both menus.
QString MainWindow::trackPlaylistVerbLabel() const { return tr("Add to playlist…"); }
QString MainWindow::trackDownloadVerbLabel() const { return tr("Download"); }

// ---- #368: a starred Jellyfin or EverythingBox-server track, opened from ★ Favorites ------------------------
// openRecent's remote-track arm. WHAT it opens is browse::remoteTrackOpenFor and the ORDER it does things in is
// browse::openRemoteTrack — both pure, both walked by probe_leafroute §12. Only the doors are here:
//   * mint  — MusicSupply::playUrl, the one place a credential enters a queue. The url goes straight to the
//             player and is kept nowhere (openAudioStream's Recents row is location()-scrubbed by the store).
//   * fetch — ServerMusicClient::fetchAlbumTracks, for a shelf track this session holds no url for. Its reply
//             is handled a turn LATER: `done` runs the whole play path, and running that inside the reply's own
//             finished delivery is the #211 shape (a nested loop frees the reply under Qt's frames).
//   * say   — notify, which the user sees. openRecent's other lines go to a status bar the app keeps hidden.
// Nothing here touches FavoritesStore: opening is not starring, and the love hook must not fire.
bool MainWindow::openRemoteMusicTrack(const QString& path, const QString& kind, const QString& resumeKey,
                                      const QString& title, const QString& thumb)
{
    const browse::RemoteTrackOpen o = browse::remoteTrackOpenFor(path, kind, resumeKey);
    if (o.trackId.isEmpty()) return false;
    QPointer<MainWindow> self(this);
    browse::RemoteTrackDoors doors;
    doors.mint = [](const QString& trackId) { return MusicSupply::playUrl(trackId); };
    doors.fetchAlbum = [self](const QString& albumKey, std::function<void(bool, const QString&)> done) {
        ServerMusicClient::instance().fetchAlbumTracks(albumKey, [self, done](const ServerMusicClient::Result& r) {
            if (!self) return;
            QTimer::singleShot(0, self.data(), [self, done, r] { if (self) done(r.ok, r.message); });
        });
    };
    doors.play = [self, title, thumb](const QString& url, const QString& trackId) {
        if (self) self->openAudioStream(url, trackId, title, thumb);   // re-keyed to the stable id
    };
    doors.say = [self](const QString& sentence) { if (self) self->notify(sentence, kFeedbackLong); };
    browse::openRemoteTrack(o, title, doors);
    return true;
}
