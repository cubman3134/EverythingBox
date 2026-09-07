// TAKING A JELLYFIN SERVER'S CONTENT ON A PLANE (issue #110, increment 1) — the ORIGINAL-quality path.
//
// This file is the part of offline downloads that can be decided without a socket and without a window: how
// a download url is spelled, what a download job is called, which episodes a batch verb expands to, and what
// the per-profile storage cap would evict if it were allowed to (it is not — it only ever suggests).
//
// ==========================================================================================================
// 1. THE URL, AND WHY THE CREDENTIAL IS NOT IN THIS STRUCT
// ==========================================================================================================
// A Jellyfin download is an ORDINARY DownloadManager job: the server's own authenticated /Items/<id>/Download
// endpoint, under the user's own account, streamed to a .part file and resumed by Range like every other job.
// There is no second downloader here and nothing that works around the server rather than with it.
//
// That endpoint authenticates by `api_key`, which is the user's ACCESS TOKEN — a bearer credential for a
// whole account, usable from anywhere until it is revoked. And a DownloadJob is PERSISTED: queue.json is an
// ordinary file in the app folder, not a credential store. RecentStore.h and Jellyfin::recordedPath already
// state at length what happened the last time a signed url reached a store it outlived (#200), and
// DownloadJob::headerGated states the same thing about proxy headers.
//
// So the decision, of the two the issue allows, is THE JOB HOLDS AN ID AND THE URL IS MINTED AT REQUEST TIME:
//
//     * the job carries `sourceRef` = the server-qualified id (jf:<serverId>:<itemId>) and NO url at all —
//       not a scrubbed one, not an empty-string-where-a-url-was: the credential is never in the object, so
//       there is no save() path, no log site and no crash dump that could carry it;
//     * DownloadManager mints the url from the ref at the top of every start(), through the url-minter hook,
//       which reads the token out of the device-local JellyfinServerStore for that one request;
//     * and a restart therefore RESUMES CORRECTLY rather than failing the way a header-gated job does — the
//       ref survives, and the token it re-mints from is still where it always was.
//
// The scrub-on-the-way-in alternative would have left the live url in memory in a struct whose whole job is
// to be written down, and would have had to stay right in three places (save, log, the retry re-arm).
// Minting is right in one.
//
// downloadUrl() is a free function taking the token as a parameter for exactly the same reason: it is called
// once, into a QNetworkRequest, and nothing keeps the result. probe_jfdownload byte-scans a fixture token
// against everything this feature writes.
//
// ==========================================================================================================
// 2. THE BATCH VERBS
// ==========================================================================================================
// "One verb, not a per-episode grind." From a series or a season the user asks for a SEASON, or for the NEXT
// N UNWATCHED, and gets a batch into the existing queue. Both expansions are pure functions over the episode
// list the client already fetched, and both SKIP what this device already has — an episode already downloaded
// is not re-queued, because the alternative is a batch that looks like it did nothing (every job de-dups by
// destination) or, worse, one that re-fetches six gigabytes over a hotel connection.
//
// nextUnwatched() reads `played` from the server's own UserData, and orders by (season, episode) rather than
// by the order the server happened to return — a "next 3" that depends on a server's sort order is a verb
// that means something different on two servers.
//
// ==========================================================================================================
// 3. THE CAP SUGGESTS AND NEVER DELETES
// ==========================================================================================================
// A per-profile storage cap that deleted silently would eventually delete the one film somebody downloaded
// for the flight they are on. So evictionSuggestion() returns a VERDICT — how much is used, what the cap is,
// and which items an LRU pass WOULD free, oldest-touched first, stopping as soon as the total is back under
// the cap. THE CAP REMOVES NOTHING: the caller shows the list and the user decides. (The one deletion this
// file is allowed to make at all is the ACCEPTED half of "remove after watched" further down — likewise
// never taken without the user answering a card, and refusing any path outside the downloads folder.)
//
// LRU is by lastPlayedMs, falling back to downloadedMs for something never played: "least recently useful",
// not "smallest" and not "oldest download". Ties break on the qualified id so two runs suggest the same
// victims — a suggestion that reshuffles between two openings of the same screen reads as a bug.
#pragma once
#include "Jellyfin.h"

#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

namespace JellyfinDownload
{
    // ---- The url ------------------------------------------------------------------------------------
    // "/Items/<itemId>/Download" — the server's own authenticated original-file endpoint. Separate from
    // downloadUrl() so the path can be asserted without a token anywhere near the assertion.
    QString downloadPath(const QString& itemId);

    // <root>/Items/<itemId>/Download?api_key=<token>. `root` is a NORMALIZED root (Jellyfin::normalizeRoot).
    // Empty when the root or the item id is empty — a url that would address the wrong thing is not
    // returned in a half-built state for a caller to notice.
    //
    // The token rides the query for the reason Jellyfin::streamUrl already gives: this url is handed to a
    // network stack that takes a url and not a header set, and a header set is dropped by a restart anyway
    // (DownloadJob::headerGated). It is minted per request and never stored.
    QString downloadUrl(const QString& root, const QString& itemId, const QString& token);

    // ---- Job identity -------------------------------------------------------------------------------
    // The DownloadJob key for a qualified id. Server-qualified by construction, because the qualified id is:
    // the same item id on two servers is two different downloads and must not de-dup into one.
    QString jobKey(const QString& qualifiedId);
    bool    isJobKey(const QString& key);
    // The qualified id back out of a job key, or "" if that is not one of ours.
    QString refFromJobKey(const QString& key);

    // The file name a downloaded item is saved under, sanitised for the filesystem and unique per server:
    //   * a film     -> "<title> [<serverId8>-<itemId8>].<ext>"
    //   * an episode -> "<series> S01E02 <title> [<serverId8>-<itemId8>].<ext>"
    // The id suffix is what makes two servers' identically-named episodes two files rather than one; it is
    // an ID, not a credential. `container` is the server's own container string ("mkv", "mp4", ...); empty
    // gives ".mkv", which mpv opens by content anyway.
    QString fileNameFor(const Jellyfin::UnionItem& item, const QString& container);

    // ---- The batch verbs ----------------------------------------------------------------------------
    // Every episode of `episodes` that `alreadyHave` does not hold, in (season, episode) order.
    // `alreadyHave` is a set of QUALIFIED IDS.
    QVector<Jellyfin::UnionItem> seasonBatch(const QVector<Jellyfin::UnionItem>& episodes,
                                             const QSet<QString>& alreadyHave);

    // The next `n` episodes the user has not watched and this device does not have, in (season, episode)
    // order. n <= 0 gives an empty batch rather than "all of them".
    QVector<Jellyfin::UnionItem> nextUnwatched(const QVector<Jellyfin::UnionItem>& episodes, int n,
                                               const QSet<QString>& alreadyHave);

    // ---- The cap ------------------------------------------------------------------------------------
    struct StoredItem
    {
        QString qualifiedId;
        QString path;
        qint64  bytes        = 0;
        qint64  lastPlayedMs = 0;   // 0: never played
        qint64  downloadedMs = 0;
        bool    watched      = false;
    };

    struct CapVerdict
    {
        bool        over      = false;
        qint64      usedBytes = 0;
        qint64      capBytes  = 0;
        qint64      freedBytes = 0;      // what the victims below would free
        QStringList victims;             // qualified ids, oldest-touched first. NEVER acted on here.
    };

    // capBytes <= 0 means "no cap": never over, never a victim. See the header for the LRU order.
    CapVerdict evictionSuggestion(const QVector<StoredItem>& items, qint64 capBytes);

    // The "remove after watched" candidates: everything flagged watched. Also a suggestion — the toggle
    // decides whether the caller acts on it, and the caller is the only thing that touches a file.
    QStringList watchedCandidates(const QVector<StoredItem>& items);

    // ---- "Remove after watched": the offer, and the ONE deletion this feature may make --------------
    //
    // THE TRIGGER IS "STOPPED PAST THE WATCHED FRACTION", NOT "MPV REPORTED END-OF-FILE", and that is a
    // decision, not an accident. An EOF-only rule reads the credits as the item: somebody who watches to the
    // last scene and presses Back has finished the episode by every meaning the word has, and is exactly the
    // person who turned this setting on. mpv's EOF seam would never fire for them, and a housekeeping rule
    // that silently does nothing for the commonest way of finishing something is the same failure as a
    // switch that does nothing at all. So the rule is the one the rest of the app already speaks
    // (MainWindow::stopScrobble computes the same percentage for Trakt) — measured at the ONE stop site
    // every leave-the-media route runs through, so no route can miss it.
    //
    // 0.90 AND NOT TRAKT'S 0.80, because the two answers cost different things. Trakt being early marks a
    // row on a website; this being early offers to delete a file somebody is still watching. Ten per cent of
    // a film is six minutes of slack, and the offer is refusable — but the cheap direction is the careful
    // one, and a threshold this side of the credits is still comfortably reached by anyone who saw the end.
    //
    // A duration of zero is NOT "finished". An item whose length was never reported (a stream mpv could not
    // measure, a file closed before the duration arrived) has no fraction to be past, and guessing produces
    // exactly the mid-item deletion offer this must never make.
    constexpr double kWatchedFraction = 0.90;

    // Was this playback a finish, of a downloaded file, that the user has not already said no to?
    //   * settingOn false            -> false. The switch off means no offer AT ALL, not a quieter one.
    //   * playedFromLocalFile false  -> false. Streaming an item does not put a copy of it on this disk.
    //   * alreadyDeclined            -> false. "No" is remembered; being asked twice about the same file is
    //                                  how a considerate prompt becomes the thing people switch off.
    //   * durationSeconds <= 0       -> false (see above), as is a negative position.
    // Pure so probe_jfdownload can pin the mid-item case, which is the one that must never be true.
    bool shouldOfferRemoval(bool settingOn, bool playedFromLocalFile, bool alreadyDeclined,
                            double positionSeconds, double durationSeconds);

    // IS `path` A FILE THIS FEATURE IS ALLOWED TO DELETE? True only for something strictly inside
    // `downloadsDir` — the folder this feature writes into and nothing else. A local-library film, a ROM, a
    // path assembled out of a store row that has since been edited by hand, and the downloads directory
    // ITSELF all answer false. Both sides are cleaned, made native-separator-blind and (where they exist)
    // resolved through canonicalFilePath, so a junction or a "..\.." spelling cannot walk out of the folder
    // and back in. Case-insensitive on Windows only, because that is where two spellings are one file.
    bool isInsideDownloads(const QString& path, const QString& downloadsDir);

    enum class RemovalOutcome
    {
        Removed,                  // the file was there and is now gone
        NotFound,                 // there was nothing to delete — also "gone", for the store's purposes
        RefusedOutsideDownloads,  // the path is not inside the downloads folder: NOTHING was touched
        DeleteFailed              // the OS refused (open elsewhere, read-only, not a file)
    };

    // The only line in this feature that removes anything from a disk. The containment check is made FIRST,
    // before the file is even asked whether it exists, so a path outside the folder is refused whatever is
    // or is not at the end of it.
    RemovalOutcome removeDownloadedFile(const QString& path, const QString& downloadsDir);

    // MAY THE DOWNLOADS ENTRY GO NOW? Only when the file is actually gone. A refusal or a failed delete
    // leaves the row exactly where it was, pointing at a file that is still there — a Downloads folder that
    // forgets an item it did not manage to remove is a folder that lies about what is on the disk.
    bool entryMayLeaveDownloads(RemovalOutcome outcome);

    // THE TWO STEPS IN THEIR ONE ORDER: delete the file, and drop the Downloads row for `qualifiedId` only
    // if that actually happened. Here rather than at the call site so the ordering is a single fact that a
    // probe can drive against the REAL DownloadsStore — a caller that got it the other way round would
    // forget an item that is still occupying the disk, and nothing above this line would notice.
    // The caller still owns everything a person sees, and everything that is not this store.
    RemovalOutcome removeDownloadedItem(const QString& qualifiedId, const QString& path,
                                        const QString& downloadsDir);

    // ---- Settings (per profile, device-local) -------------------------------------------------------
    // Both keys live under "downloads/", which CloudSync::isDeviceLocalKey already carves out of the synced
    // bundle ("downloads*" — this device's local download catalog). See CloudSync.cpp: the carve-out exists
    // because a downloaded FILE is a fact about one machine's disk, and neither a cap measured against that
    // disk nor a housekeeping rule for those files means anything on another one. probe_jfdownload asserts
    // the classification of every key added here rather than trusting the prefix to keep matching.
    QString capKey();                 // downloads/<profile>/capGb
    QString removeWatchedKey();       // downloads/<profile>/removeAfterWatched

    int  capGb();                     // 0 = no cap (the default)
    void setCapGb(int gb);
    bool removeAfterWatched();        // false by default: nothing deletes a user's file by omission
    void setRemoveAfterWatched(bool on);

    constexpr qint64 kBytesPerGb = 1024LL * 1024LL * 1024LL;

#ifdef EB_JELLYFIN_TEST_SEAM
    // The same ini redirect JellyfinServerStore has, and for the same reason: these setters PERSIST, so a
    // probe run against the app's real ini would move a user's settings. Absent without the macro, so a
    // production call is a compile error rather than a silent process-wide redirect.
    void setIniPathForTesting(const QString& path);
#endif
}
