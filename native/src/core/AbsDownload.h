// TAKING AN AUDIOBOOKSHELF BOOK ON A PLANE (issue #197, offline listening) — the part that can be decided
// with no socket and no window.
//
// The SAME SHAPE as JellyfinDownload.h (#110) and SubsonicDownload.h (#193), and the places it differs are
// only the places a book differs from a film or a record:
//
//   * THE JOB HOLDS AN ID, NEVER A URL. Each file of the book is one ordinary DownloadManager job whose
//     `sourceRef` (and `key`) is a FILE REF — "absfile:<serverId>:<itemId>:<ino>", three ids and no
//     credential — and whose `url` is EMPTY. DownloadManager mints the link from the ref at the top of every
//     start() through the one url minter MainWindow installs (initJellyfinDownloads routes this family to
//     AbsClient::downloadUrlFor), so the `?token=` the download route needs exists between the minter and
//     one QNetworkRequest and nowhere else: not in queue.json, not in the Downloads store, not in the book's
//     manifest, not in a log line. A restart RESUMES, because the ref survives and the token it re-mints from
//     is still in the device-local server store.
//
//   * A BOOK IS ONE JOB PER FILE, the way a Jellyfin season is one job per episode and a Subsonic album is
//     one job per track: the existing queue, its progress rows, its Pause, Cancel and resume all work per
//     file. The jobs are INTERMEDIATES (`record == false`): a finished file is not a thing the user asked for
//     and gets no Downloaded row of its own. What the user asked for is the BOOK, and it is recorded ONCE,
//     when its last file lands (completedBook), as ONE ordinary Downloaded item keyed by the server-qualified
//     book id — the id the book already has everywhere else, so the Downloaded row re-opens through
//     openAbsItem exactly as a Recents row does.
//
//   * THE BOOK KEEPS WHAT THE SERVER KNEW ABOUT IT. A folder per book holds the audio files and a manifest
//     (book.json): the title, author, narrator, the chapters, and every file IN ORDER WITH ITS DURATION — so
//     the whole-book position bar (#197 increment 3) and the server's chapter list both work with the server
//     switched off. The manifest is written before the first byte is fetched, which is also what lets a
//     finished file find the book it belongs to. It holds ids, names and numbers, never a url.
//
//   * OPENING PREFERS THE LOCAL COPY through PreferLocal.h — the one rule a Subsonic track already plays
//     from disk by (#417). localManifest() is that rule behind the "is it a qualified Audiobookshelf id" gate.
//
// Positions while the book plays offline are AbsProgressQueue.h's business, not this file's.
#pragma once
#include "Audiobookshelf.h"
#include "DownloadManager.h"   // DownloadJob
#include "DownloadsStore.h"    // DownloadedItem

#include <QByteArray>
#include <QString>
#include <QVector>
#include <functional>

namespace AbsDownload
{
    // ---- The file ref: an id for one file of one book on one server ---------------------------------------
    inline const char* kFileScheme = "absfile:";
    // "absfile:<serverId>:<itemId>:<ino>", or empty when the book id is not a qualified BOOK id (an episode
    // is not downloaded by this) or the ino is unusable. Its own scheme, not "abs:", so a file ref can never
    // be mistaken for a book by the id router (Abs::parse refuses it outright).
    QString fileRef(const QString& qualifiedBookId, const QString& ino);
    struct FileRef { bool ok = false; QString qualifiedBookId; QString itemId; QString ino; };
    FileRef parseFileRef(const QString& ref);
    inline bool isFileRef(const QString& s) { return parseFileRef(s).ok; }

    // ---- The plan: which files, in what order, called what -------------------------------------------------
    struct PlannedFile
    {
        QString ino;
        QString title;          // the server's file name — the part's display row
        QString localName;      // "NN - <name>.<ext>", unique within the book's folder
        double  duration = 0.0;
        double  startOffset = 0.0;   // where this file begins in the WHOLE book
    };
    struct Plan
    {
        bool    ok = false;
        QString qualifiedId;    // abs:<serverId>:<itemId>
        QString title;
        QString author;
        QString narrator;
        double  duration = 0.0;
        QString folderName;     // "<title> [abs-<server8>-<itemId>]" — the id suffix keeps two servers apart
        QVector<PlannedFile> files;
        QVector<Abs::Chapter> chapters;
    };
    // EVERY audio file of the item, in the server's order, with its length. Read from the expanded item's
    // `media.audioFiles` (the file list the download route is keyed by); a server whose reply carries no
    // file list falls back to the tracks, whose contentUrl ends in the same inode. Not ok for an episode id,
    // a podcast, or an item with nothing to fetch.
    Plan planFor(const QString& qualifiedId, const Abs::ItemDetail& detail);

    QString folderFor(const Plan& plan, const QString& downloadsDir);
    inline QString manifestName() { return QStringLiteral("book.json"); }

    // One DownloadJob per file, in plan order: `sourceRef` and `key` are the file ref, `url` is EMPTY, `kind`
    // is "audio", `record` is false (see the header), `dest` is inside folderFor(). `thumb` is shown on the
    // Downloads panel's rows.
    QVector<DownloadJob> jobsFor(const Plan& plan, const QString& downloadsDir, const QString& thumb);

    // ---- The manifest ------------------------------------------------------------------------------------
    // `coverFile` is the cover's file name inside the folder, or empty.
    QByteArray manifestJson(const Plan& plan, const QString& coverFile);
    struct Manifest
    {
        bool    ok = false;
        Plan    plan;
        QString folder;      // absolute, the manifest's own directory
        QString coverFile;   // absolute path of the cover in the folder, or empty
    };
    Manifest readManifest(const QString& manifestPath);
    // Every file the manifest names is on disk (and none is still a .part).
    bool isComplete(const Manifest& m, const std::function<bool(const QString&)>& exists = {});

    // The ONE ordinary Downloaded item the book is recorded as: path = the manifest, key = the qualified book
    // id, kind "audio" (the Audiobooks catalogue's Downloaded folder), thumb = the local cover.
    DownloadedItem recordFor(const Manifest& m, const QString& manifestPath);

    // A job for one of the book's files has finished. If it completes the book, `out` is the book's record and
    // this answers true; the caller adds it to DownloadsStore. False for any other job, and for a book with a
    // file still to come.
    bool completedBook(const DownloadJob& finished, DownloadedItem* out,
                       const std::function<bool(const QString&)>& exists = {});

    // ---- Playing it --------------------------------------------------------------------------------------
    // The manifest of the downloaded copy of this qualified BOOK id, or "" — PreferLocal::localCopy behind the
    // Audiobookshelf gate, so an id that is not one of ours is never looked up.
    QString localManifest(const QString& qualifiedId, const QVector<DownloadedItem>& downloads,
                          const std::function<bool(const QString&)>& exists = {});
    // The book as a play session with no server in it: one track per file, its contentUrl the ABSOLUTE LOCAL
    // PATH, its start and length the server's, and the server's chapters. What AbsClient::adoptLocalSession
    // holds so that minting a part, the whole-book bar and "a position in part k is a position in the book"
    // all work exactly as for a streamed book.
    Abs::Session localSession(const Manifest& m);

    // ---- Removing it -------------------------------------------------------------------------------------
    // The book's FOLDER (its files, manifest and cover), its Downloads entry and its offline-progress entry.
    // Nothing on the server is touched. Refuses a folder that is not strictly inside `downloadsDir` before it
    // asks the filesystem anything — the rule JellyfinDownload::removeDownloadedItem sets for the same reason.
    enum class Removal { Removed, AlreadyGone, RefusedOutsideDownloads, DeleteFailed };
    Removal removeBook(const QString& qualifiedId, const QString& manifestPath, const QString& downloadsDir);
}
