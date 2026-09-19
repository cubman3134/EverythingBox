// SUBSONIC OFFLINE DOWNLOADS (issue #193) — the MainWindow half.
//
// Everything that decides something lives in core/SubsonicDownload.h and is pinned by probe_subsonic: which
// rows download, what a job is called, the order an album is queued in, and which file a track id plays from.
// This file only gathers the facts a job needs out of the per-session SubsonicClient cache (fetching an album's
// tracks first when the level was never opened) and hands ordinary DownloadJobs to the ordinary DownloadManager
// — the shape MainWindowJellyfinDownload.cpp has for a Jellyfin season.
//
// NOTHING HERE HOLDS A URL. A job carries the qualified track id as its sourceRef; DownloadManager asks
// initJellyfinDownloads' minter (which routes a Subsonic ref to SubsonicClient::downloadUrlFor) for a freshly
// signed download.view url at the top of every start(). The log lines below name the SERVER ID and the FILE
// NAME, both of which are ids and titles, never a request.
#include "MainWindow.h"

#include "FeedbackPolicy.h"          // kFeedbackShort / kFeedbackLong
#include "../core/AppPaths.h"
#include "../core/DownloadManager.h"
#include "../core/DownloadsStore.h"
#include "../core/Subsonic.h"
#include "../core/SubsonicClient.h"
#include "../core/SubsonicDownload.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QStatusBar>

namespace {

void ssdLog(const QString& msg)
{
    QFile f(AppPaths::dataDir() + QStringLiteral("/stream_debug.log"));
    if (f.open(QIODevice::Append | QIODevice::Text))
        f.write((QDateTime::currentDateTime().toString(Qt::ISODate) + QStringLiteral("  ") + msg
                 + QChar(QLatin1Char('\n'))).toUtf8());
}

QString downloadsDir() { return AppPaths::dataDir() + QStringLiteral("/downloads"); }

// The record a track id is on, in whichever of the client's two indexes holds it (the browse index for an album,
// the sections index for a playlist or the starred tracks). Null when this session has not read it.
const MusicLibrary::Album* recordHolding(const QString& trackId, const MusicLibrary::IndexTrack** trackOut)
{
    const QString serverId = Subsonic::serverOf(trackId);
    const SubsonicClient& cl = SubsonicClient::instance();
    for (const MusicLibrary::Index* idx : { &cl.index(serverId), &cl.sectionIndex(serverId) })
        for (const MusicLibrary::Artist& a : idx->artists)
            for (const MusicLibrary::Album& b : a.albums)
                for (const MusicLibrary::IndexTrack& t : b.tracks)
                    if (t.path == trackId) { if (trackOut) *trackOut = &t; return &b; }
    return nullptr;
}

} // namespace

void MainWindow::downloadSubsonic(const QString& ref, const QString& title, const QString& thumb)
{
    if (!dm_) return;
    const Subsonic::Ref r = Subsonic::parse(ref);
    if (!r.ok) return;
    const auto exists = [](const QString& p) { return QFileInfo::exists(p); };

    // Queue a set of tracks from one record: skip what is here, one job per track, in playing order.
    const auto queueFrom = [this, exists](const MusicLibrary::Album& album, const QVector<MusicLibrary::IndexTrack>& pick,
                                          const QString& fallbackThumb) -> int {
        const QSet<QString> have = SubsonicDownload::downloadedIds(DownloadsStore::list(), dm_->jobs(), exists);
        const QVector<MusicLibrary::IndexTrack> batch = SubsonicDownload::albumBatch(pick, have);
        QString cover = SubsonicClient::instance().albumCoverPath(album.key);
        if (cover.isEmpty()) cover = fallbackThumb;
        const QString albumTitle = MusicLibrary::displayAlbum(album);
        for (const MusicLibrary::IndexTrack& t : batch)
        {
            const DownloadJob j = SubsonicDownload::jobFor(t, albumTitle, album.discCount,
                                                           SubsonicClient::instance().trackSuffix(t.path),
                                                           cover, downloadsDir());
            if (j.sourceRef.isEmpty()) continue;
            dm_->enqueue(j);
            ssdLog(QStringLiteral("ssdownload: queued %1 -> %2")
                       .arg(Subsonic::serverOf(t.path), QFileInfo(j.dest).fileName()));
        }
        return int(batch.size());
    };

    if (r.kind == Subsonic::Kind::Track)
    {
        const QString local = SubsonicDownload::localCopy(ref, DownloadsStore::list(), exists);
        if (!local.isEmpty()) { notify(tr("“%1” is already downloaded.").arg(title), kFeedbackShort); return; }
        const MusicLibrary::IndexTrack* t = nullptr;
        const MusicLibrary::Album* album = recordHolding(ref, &t);
        if (!album || !t)
        {
            // Every row that offers this verb was drawn from the cache this looks in, so this is a server that
            // was removed or a cache replaced under the row. Say so rather than queueing a nameless file.
            notify(tr("Couldn't download “%1” — open its album again and retry.").arg(title), kFeedbackLong);
            return;
        }
        if (queueFrom(*album, { *t }, thumb) == 0)
        { notify(tr("“%1” is already downloading.").arg(title), kFeedbackShort); return; }
        notify(tr("“%1” added to Downloads. See Settings ▸ Downloads for progress.").arg(title), kFeedbackLong);
        return;
    }

    if (r.kind != Subsonic::Kind::Album && r.kind != Subsonic::Kind::Playlist) return;

    // An album / playlist: its tracks, fetched first when this session never opened it. The same fetch the album
    // level makes, coalesced with it by the client; the answer lands in the index this reads.
    const auto queueRecord = [this, ref, title, thumb, queueFrom] {
        const MusicLibrary::Album* album = MusicSupply::indexFor(ref).album(ref);
        if (!album || album->tracks.isEmpty())
        { notify(tr("“%1” has no tracks to download.").arg(title), kFeedbackLong); return; }
        const int n = queueFrom(*album, album->tracks, thumb);
        if (n == 0)
        {
            notify(tr("Everything on “%1” is already on this device or downloading.").arg(title), kFeedbackLong);
            return;
        }
        notify(tr("Queued %n track(s) from “%1”. See Settings ▸ Downloads for progress.", "", n).arg(title),
               kFeedbackLong);
    };
    if (SubsonicClient::instance().albumTracksLoaded(ref)) { queueRecord(); return; }
    statusBar()->showMessage(tr("Looking up “%1”…").arg(title), kFeedbackShort);
    QPointer<MainWindow> self(this);
    SubsonicClient::instance().fetchAlbumTracks(ref, [self, title, queueRecord](const SubsonicClient::Result& res) {
        if (!self) return;
        if (!res.ok)
        {
            // The client's own sentence — the server's words or a transport sentence, never a url.
            self->notify(res.message.isEmpty() ? tr("Couldn't read “%1” from its music server.").arg(title)
                                               : res.message, kFeedbackLong);
            return;
        }
        queueRecord();
    });
}
