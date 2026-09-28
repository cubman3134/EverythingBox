// The offline fallback's MainWindow half (issue #194) — a SEPARATE translation unit that defines the one
// MainWindow member the feature needs, off the busiest file in the repository (the #143 / #186 rule).
//
// HomeView decides which copy of a merged album a Play press opens (MusicFallback::plan). When the preferred
// copy's server is not answering and another copy plays instead, HomeView also hands over the names that
// copy's tracks are to be filed under: the MERGED record's own tracks. MainWindow::openMusicAlbum turns those
// into queue-url keys and installs them here.
//
// WHAT THIS DOES NOT DO is as important as what it does. adoptMusicQueueIdentities also runs the #204
// stream-key migration over the table it is given, which MOVES records filed under a stream url onto the
// track's durable name. A fallback alias is not that — it is a name for the length of one queue — so it goes
// to the session and the host only, and nothing the user banked is moved by a fallback play. The resume
// position, the listening seconds, the speed and the sync offset of each track are read and written under the
// merged record's name from the first second, exactly as they would be had the server been up.
#include "MainWindow.h"

#include "../media/PlaybackSession.h"

void MainWindow::adoptMusicPlayAliases(const QHash<QString, QString>& urlToIdentity)
{
    if (urlToIdentity.isEmpty()) return;
    for (auto it = urlToIdentity.cbegin(); it != urlToIdentity.cend(); ++it)
        musicQueueIndexPaths_.insert(it.key(), it.value());
    if (session_) session_->setTrackIdentities(musicQueueIndexPaths_);
}
