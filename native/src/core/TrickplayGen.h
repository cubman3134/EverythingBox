// TrickplayGen — the background job that turns a file the user owns into seek-preview sprite sheets, and
// the small read side the player uses to find one (issue #85).
//
// THE CAPTURE PATH is a second, headless libmpv: `vo=null`, no audio, no subtitles, paused, with
// `vf=scale=<tile>` in front of it, seeking exactly and taking `screenshot-to-file` at each stop. It was
// picked by measurement against the alternative the issue names — a second MPV_RENDER_API_TYPE_SW context,
// the path theme2/MpvPreview already proves works on this build — over the same file and the same seek
// ladder (40 frames, warm-up excluded, software decode, this machine):
//
//     screenshot-to-file, mpv scaling to 320x180 in the filter chain   52 ms/frame   9 KB per tile
//     screenshot-to-file at full resolution, we scale afterwards       34 ms/frame   96 KB per tile
//     screenshot-raw (same scale, no temp file), read back as a node   62-73 ms/frame
//     MPV_RENDER_API_TYPE_SW into a 320x180 CPU buffer                 260-400 ms/frame
//
// The render API lost on both counts that matter. It is five to eight times slower here — it is built to
// present a frame at its display time, not to grab one — and it was not reliably synchronised to the seek:
// the same 40-frame ladder over the same file produced two different pixel checksums across runs, i.e. some
// renders handed back the PREVIOUS frame. A thumbnailer that occasionally shows the wrong second is the one
// failure this feature must not have.
//
// Scaling in mpv's own filter chain costs 18 ms/frame over grabbing full-resolution frames, and buys back a
// tenfold drop in the size of the intermediate — 9 KB against 96 KB — and, more to the point, removes a
// full-resolution image decode and allocation per frame on the host side. The issue's own sequencing note
// says the devices that need this courtesy most are the low-powered ones; that is the trade taken.
//
// THE MANNERS (the issue asks for the same ones as a fingerprint job, and there is no such job in the tree
// yet, so these are stated here rather than inherited):
//
//   * off the GUI thread entirely — one worker thread, one mpv handle, created and destroyed on it;
//   * one file at a time. There is no pool. A queue of one pending item, and a newer request replaces it;
//   * NEVER while playback is running. The host arms and disarms it; a file requested while it is playing
//     is generated when the player is done with it;
//   * cancellable between every frame, and resumable: finished grids are kept and the walk picks up at the
//     first grid MISSING (not at the count — a grid lost to a failed rename must not leave a permanent hole);
//   * nothing is written until a grid is COMPLETE. Each grid and the sidecar are written to a ".part" beside
//     themselves and renamed into place, so an interrupted run leaves whole grids or nothing;
//   * it writes only inside its own cache directory, and opens the source file read-only;
//   * eviction runs here too, on this thread, never on the GUI thread.
//
// ISSUE #302 WIDENS THE TRIGGER, NOT THE MANNERS. There is now a second way an item reaches this queue:
// `requestIdle()`, from a walk of the local library run on genuine idle (TrickplayIdleWalk). Every rule above
// still holds for it, and it carries two extra ones of its own, because it is work the user did not ask for
// at that moment:
//
//   * a user-originated request ALWAYS displaces a pending idle one, never the other way round;
//   * an idle item never evicts. It checks the size bound BEFORE it starts and again between grids, and when
//     the bound is reached it STOPS with the whole grids it has. Eviction exists to make room for what the
//     user IS watching; deleting one film's strip to cache another they have never opened is the cache
//     working against its owner.
#pragma once
#include "Trickplay.h"
#include "TrickplayIdle.h"

#include <QObject>
#include <QString>

class QThread;
class TrickplayWorker;

class TrickplayGen : public QObject
{
    Q_OBJECT
public:
    explicit TrickplayGen(QObject* parent = nullptr);
    ~TrickplayGen() override;

    // Ask for previews for a local file. Silently does nothing — never a message, never a spinner — when
    // previews are off, when the path is not something we may preview (Trickplay::classify), when the file
    // is gone, or when its sheets are already complete. "Silently" is the feature's own rule: a preview that
    // is not there has to be invisible.
    void request(const QString& path);

    // The same, from the idle library walk (#302). Refused outright when idle generation is off, and it will
    // not displace a request the user's own playback made — that one is about a file they just watched, this
    // one is about a file nobody has opened. The item generated under it does NOT sweep the cache.
    void requestIdle(const QString& path);

    // Stop an idle walk in its tracks, leaving a user-originated one alone. This is what the walk calls the
    // moment its conditions stop saying Go.
    void cancelIdle();

    bool busy() const { return busy_; }

    // What the preview cache held when the worker last measured it. Advisory on this thread — the
    // authoritative check is the worker's own, taken with a live number immediately before it opens a
    // decoder — and zero until the first idle item has been considered. It is the only cache-size fact the
    // GUI side has, so "there is room" and "we are at the bound" are both read off it.
    qint64 knownCacheBytes() const;

    // Playback started / stopped. While busy the queue does not drain and a walk in progress is cancelled:
    // the decoder, the disk and the CPU belong to what the user is watching.
    void setPlaybackBusy(bool busy);

    // Drop the current walk and the pending request. Finished grids stay on disk.
    void cancelAll();

    // ---- the read side (GUI thread; one small file) ----
    static QString cacheRoot();
    static QString itemDirFor(const QString& path);   // "" when the path may not be previewed / is gone
    // Load an item's sidecar. False (and *out left alone) when there is none, it does not parse, or it
    // describes a different file than the one on disk now — which is the mtime/size invalidation, applied at
    // the moment of use rather than by a sweep.
    static bool loadIndex(const QString& path, Trickplay::Index* out, QString* dirOut);

    // What this file's cache directory already holds — the idle walk's skip rule (#302). Complete means every
    // grid the film earns is on disk, and the walk passes over it without opening anything. Deliberately NOT
    // derived from loadIndex(), whose returned layout is clamped to the COMPLETE grids for the player's
    // benefit: comparing that clamped count against itself would call every partial item complete.
    static TrickplayIdle::ItemState itemState(const QString& path);

    // "I showed this item's previews just now" — the input to LRU eviction. Queued onto the worker, because
    // it is a disk write and this is called from the GUI thread on every open.
    void noteUsed(const QString& dir);

signals:
    // An item gained grids. The player re-reads its sidecar; a strip can therefore appear mid-session
    // without anything being torn down.
    void itemProgressed(const QString& path);

    // An IDLE-originated item is finished with (done, cancelled or refused — this signal does not claim
    // success). The library walk's cue to consider the next file. #302.
    void idleItemFinished(const QString& path);

private:
    void pump();

    QThread*         thread_ = nullptr;
    TrickplayWorker* worker_ = nullptr;
    QString          pending_;
    bool             pendingIdle_ = false;  // #302: the queued item came from the library walk
    bool             runningIdle_ = false;  // …and so did the one the worker has now
    bool             busy_    = false;   // a walk is running on the worker
    bool             playing_ = false;   // the player has a file open
};
