// TAKING A SUBSONIC SERVER'S MUSIC ON A PLANE (issue #193) — offline downloads, in Jellyfin's shape.
//
// This is the part of the feature that can be decided with no socket and no window: which rows download
// what, what a job is called and where it lands, the order an album is queued in, and which file on this
// disk a qualified track id plays from. It is deliberately the SAME shape as JellyfinDownload.h (#110), and
// the places it differs are only the places a music server differs from a video server:
//
//   * THE JOB HOLDS AN ID, NEVER A URL. A download job's `sourceRef` and `key` are the server-qualified
//     TRACK id (Subsonic::qualify). DownloadManager mints the signed download.view url from that ref at the
//     top of every start() through the url minter MainWindow installs — so the `t`/`s` (or `p`) pair exists
//     between the minter and one QNetworkRequest and nowhere else: not in queue.json, not in the Downloads
//     store, not in a log line. A restart RESUMES rather than failing, because the ref survives and the
//     password it re-mints from is still in the device-local server store. Exactly #110's decision; see
//     JellyfinDownload.h for the argument and DownloadManager::start for the one site it turns on.
//
//   * AN ALBUM OR A PLAYLIST IS ONE JOB PER TRACK, the way a Jellyfin season is one job per episode: the
//     existing queue, its progress rows, its Cancel and its resume all work per file, and a track that is
//     already on this device is skipped rather than fetched twice. albumBatch() orders by disc then track,
//     the same rule MusicLibrary::buildIndex and Subsonic::fillAlbumTracks apply, so a batch downloads in
//     the order the record plays.
//
//   * A DOWNLOADED TRACK IS STILL THAT TRACK. The Downloads entry is keyed by the qualified id, and
//     MusicSupply::playUrl — the ONE place a music track id becomes something mpv is handed — asks
//     localCopy() before it mints a stream url. So the album on the server's browse level, a queue, a
//     favourite and Recents all play the file on this disk once it is here, and keep their identity (the
//     queue maps the local path back to the qualified id exactly as it maps a stream url).
//
// The bitrate cap does not reach this file at all: a download is download.view, the original, whatever the
// streaming quality setting says (Subsonic::buildDownloadUrl has no bitrate parameter).
#pragma once
#include "DownloadManager.h"   // DownloadJob
#include "DownloadsStore.h"    // DownloadedItem
#include "MusicLibrary.h"

#include <QSet>
#include <QString>
#include <QVector>
#include <functional>

namespace SubsonicDownload
{
    // ---- Which rows download what --------------------------------------------------------------------
    // A Track target names one qualified TRACK id; an Album target names a qualified ALBUM or PLAYLIST key
    // (both are "an ordered list of tracks" and both are fetched by SubsonicClient::fetchAlbumTracks).
    // Anything else — a local file, a Jellyfin or EverythingBox-server track, an artist, a starred-tracks
    // container the server has never heard of — is None, so no Download verb is offered on it.
    enum class Kind { None, Track, Album };
    struct Target
    {
        Kind    kind = Kind::None;
        QString ref;
        bool ok() const { return kind != Kind::None && !ref.isEmpty(); }
    };
    // `isTrack` / `albumKey` / `trackPath` are browse::QueueTarget's three facts about a row (the one reading
    // of "is this a music row, and what does it name"), passed in so this file needs no browse code.
    Target targetFor(bool isTrack, bool isAlbum, const QString& albumKey, const QString& trackPath);

    // ---- The job -------------------------------------------------------------------------------------
    // What a downloaded track is shown as: "Title — Artist" (just the title when the artist is unknown).
    QString displayTitle(const MusicLibrary::IndexTrack& t);

    // The file name, sanitised for the filesystem and unique per server:
    //   "<artist> - <album> - <disc-><NN> <title> [<serverId8>-<trackId8>].<suffix>"
    // The disc prefix appears only on a multi-disc record. The id suffix is what makes the same song on two
    // servers two files; it is an ID, never a credential. An empty `suffix` gives ".mp3" (mpv opens by
    // content either way; the extension is for the user's file manager).
    QString fileNameFor(const MusicLibrary::IndexTrack& t, const QString& albumTitle, int discCount,
                        const QString& suffix);

    // The DownloadJob for one track: `sourceRef` and `key` are the qualified track id, `url` is EMPTY,
    // `kind` is "audio", `thumb` is the album cover. Null job (empty sourceRef) for a non-Subsonic path.
    DownloadJob jobFor(const MusicLibrary::IndexTrack& t, const QString& albumTitle, int discCount,
                       const QString& suffix, const QString& coverPath, const QString& downloadsDir);

    // Every Subsonic track of `tracks` that `alreadyHave` does not hold, in disc-then-track order.
    QVector<MusicLibrary::IndexTrack> albumBatch(const QVector<MusicLibrary::IndexTrack>& tracks,
                                                 const QSet<QString>& alreadyHave);

    // ---- The local copy ------------------------------------------------------------------------------
    // The file on this disk for a qualified Subsonic TRACK id, or "" — a Downloads entry keyed by that id
    // whose file still exists. `exists` is a parameter so the probe can drive a deleted file.
    QString localCopy(const QString& qualifiedTrackId, const QVector<DownloadedItem>& downloads,
                      const std::function<bool(const QString&)>& exists);

    // The qualified track ids this device already has or has queued — what albumBatch skips.
    QSet<QString> downloadedIds(const QVector<DownloadedItem>& downloads, const QVector<DownloadJob>& jobs,
                                const std::function<bool(const QString&)>& exists);
}
