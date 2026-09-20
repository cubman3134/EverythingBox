// LAN file drop (issue #115): upload ROMs and media to this device from any browser on the network.
//
// This file is the WHOLE of the feature's thinking, as data in / data out, and it pulls in QtCore only (QDir,
// QFile and QStorageInfo are QtCore; there is no socket here and no window). probe_filedrop drives every
// decision against temp folders. The live halves are thin on purpose: RemoteServer routes /drop/* here and
// applies the answer, MainWindowFileDrop.cpp builds the destination roots and runs the scan afterwards.
//
// SECURITY IS THE FEATURE. What a caller can and cannot make this unit do:
//
//   * FIXED DESTINATIONS. A client names a destination by an OPAQUE id and a BARE file name, nothing else. The
//     id maps to a directory the app configured (a ROM system folder, the video / music / photo root); a path
//     from the client is never used as a path. An unknown id is refused.
//   * NAMES are LibraryBundle::safePathSegment -- the one rule #292 lands gamelist entries under, reused, not
//     copied: no separator, no "..", no leading dot, no drive or stream colon, no control character, no
//     trailing dot or space, no Windows device name.
//   * UPLOAD ONLY. Nothing here reads a destination back, lists it, deletes a user file, renames one, or
//     overwrites one. A name that already exists is refused ("already there") at start AND again at the
//     final rename, where the rename itself refuses to replace (MoveFileEx without REPLACE_EXISTING /
//     link()+unlink()), so a file or link that appears between the check and the rename is never clobbered.
//     QFile::rename is deliberately NOT used: on failure it falls back to copy-with-truncate, which would
//     write THROUGH a dangling symlink sitting at the final name.
//   * NEVER HALF A FILE. Bytes go to a hidden ".eb-drop-<uploadId>.part" file IN the destination directory
//     (so the final rename is on one filesystem and atomic), are fsync'd, then renamed. A scan never sees a
//     partial ROM under its real name.
//   * CAPS. 64 GiB per file; a start is refused when the declared size (less what is already received) would
//     leave under 1 GiB free after every other in-progress upload also completes; one PUT carries at most
//     8 MiB; at most kMaxUploads uploads are in progress at once.
//   * THE OFFSET RULE. A chunk is appended only when its offset equals the bytes already received, and only
//     when it fits in the declared size. An overlap, a gap, a chunk past the end, or a second chunk while one
//     is in flight is refused before a body byte is accepted.
//
// Everything a route needs to answer is an Answer; RemoteServer writes answerJson() and the http status, and
// makes no decision of its own.
#pragma once
#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>
#include <functional>
#include <memory>

class QFile;

namespace FileDrop
{
    constexpr qint64 kMaxFileBytes      = 64LL * 1024 * 1024 * 1024;   // one file
    constexpr qint64 kFreeSpaceReserve  = 1LL * 1024 * 1024 * 1024;    // left free after an upload completes
    constexpr qint64 kChunkBytes        = 8LL * 1024 * 1024;           // what the page sends per PUT
    constexpr qint64 kMaxChunkBytes     = kChunkBytes;                 // the most one PUT may carry
    constexpr qint64 kStaleAfterSecs    = 24LL * 60 * 60;              // a part file untouched this long is swept
    constexpr int    kMaxUploads        = 64;                          // in progress at once
    constexpr const char* kPartPrefix   = ".eb-drop-";
    constexpr const char* kPartSuffix   = ".part";
    constexpr const char* kPageResource = ":/filedrop/drop.html";

    // A folder the app offers, as the APP describes it (built from SystemCatalog / Settings by the caller, so
    // this unit never learns either). kind is "rom", "video", "music" or "photo"; key is the system id for a
    // ROM folder and the kind for a library root.
    struct Root
    {
        QString kind;
        QString key;
        QString label;
        QString dir;
    };

    // What a client may name. `id` is opaque (a hash; it names nothing on disk) and stable for as long as the
    // same folder is configured for the same key. `dir` NEVER leaves this device: destinationsJson omits it.
    struct Destination
    {
        QString id;
        QString kind;
        QString key;
        QString label;
        QString dir;    // absolute, cleaned
    };

    QString destinationId(const QString& kind, const QString& key, const QString& dir);

    // Every root whose directory exists, as a Destination, in the order given; a root with an empty or missing
    // directory, or a duplicate id, is left out.
    QList<Destination> buildDestinations(const QList<Root>& roots);

    // {"ok":true,"destinations":[{"id","label","kind"}, ...]} -- ids, labels and kinds only, never a path.
    QByteArray destinationsJson(const QList<Destination>& dests);

    // The destination with that id, or nullptr. An id is matched exactly; nothing else about the string counts.
    const Destination* findDestination(const QList<Destination>& dests, const QString& id);

    // Whether a client-supplied file name may land: LibraryBundle::safePathSegment (shared with #292), which
    // also rules out every ".eb-drop-*" name since those start with a dot.
    bool safeName(const QString& name);

    // The upload id for a (destination, name, size) triple: a hash, so an identical triple re-issued after a
    // dropped connection, a page reload or an app restart finds its own part file again.
    QString uploadIdFor(const QString& destId, const QString& name, qint64 size);
    bool    validUploadId(const QString& id);            // 32 lower-case hex digits, nothing else
    QString partFileName(const QString& uploadId);       // ".eb-drop-<id>.part"
    bool    isPartFileName(const QString& fileName);     // exactly that shape

    // The sweep decision: only a part-file name, and only when it was last modified kStaleAfterSecs or more
    // before `now`. Anything else -- a user's file, a fresh part -- is never a candidate.
    bool shouldSweep(const QString& fileName, const QDateTime& lastModified, const QDateTime& now);

    // Durable-then-atomic landing primitives, exposed for the probe.
    bool syncFile(QFile& openFile);                      // fsync / FlushFileBuffers
    enum class RenameResult { Renamed, Exists, Failed };
    RenameResult renameNoReplace(const QString& from, const QString& to);

    // One route's answer. `http` is the status to send; `code` is a stable machine word the page keys its
    // message on ("ok", "exists", "nospace", "toolarge", "badname", "nodest", "offset", "busy", "unknown",
    // "incomplete", "badrequest", "writefailed", "toomany"); `reason` is a sentence for a human.
    struct Answer
    {
        int     http = 200;
        QString code = QStringLiteral("ok");
        QString reason;
        QString uploadId;
        QString name;
        qint64  size = -1;
        qint64  received = -1;
        bool    landed = false;
        QString landedPath;                              // absolute; NOT in answerJson (a path never leaves)
        QString destinationId;                           // for the landed callback
        bool ok() const { return code == QLatin1String("ok"); }
    };
    QByteArray answerJson(const Answer& a);

    // The in-progress uploads. Owned by the app (it outlives a listener restart); every call is made on the
    // thread that owns the RemoteServer. File I/O happens here, against the directory the destination maps to.
    class Uploads
    {
    public:
        // `freeBytes(dir)` answers the bytes available on dir's volume; unset, QStorageInfo is asked.
        explicit Uploads(std::function<qint64(const QString& dir)> freeBytes = {});
        ~Uploads();

        // POST /drop/start. Validates the destination, the name and the size; refuses a name that exists;
        // checks the free space; creates (or finds again) the part file. An identical in-progress triple gets
        // its existing id and the bytes already received.
        Answer start(const QList<Destination>& dests, const QString& destId, const QString& name, qint64 size);

        // GET /drop/status: how much of that upload is on disk.
        Answer status(const QString& id) const;

        // PUT /drop/chunk, in three steps so no body byte is accepted before the decision. begin refuses an
        // unknown id, a chunk while another is in flight, an offset that is not exactly the bytes received, a
        // length of 0, over kMaxChunkBytes or past the declared size. write appends (refusing anything past
        // the chunk's declared length). end closes the chunk and answers with the new received size. abort is
        // a dropped connection: what arrived is kept (it is in order and correct), the rest is not.
        Answer beginChunk(const QString& id, qint64 offset, qint64 length);
        bool   writeChunk(const QString& id, const char* data, qint64 n);
        Answer endChunk(const QString& id);
        void   abortChunk(const QString& id);

        // POST /drop/finish. Needs every byte; fsyncs; renames to `newName` when given (the page's "rename and
        // retry" after an "already there"), else to the name the upload started with. A name that exists by
        // now is refused and the part file is KEPT, so a retry under another name costs no re-upload.
        Answer finish(const QString& id, const QString& newName = QString());

        // Remove every stale part file (shouldSweep) directly inside `dirs`, except one with a chunk in flight;
        // a swept upload is forgotten. Returns how many files went. Pure enough to run from a worker thread
        // when given a snapshot: see sweepDirs.
        int sweep(const QStringList& dirs, const QDateTime& now);
        QSet<QString> busyIds() const;

        int     inProgress() const { return int(uploads_.size()); }
        QString partPathFor(const QString& id) const;    // test seam: "" when unknown

    private:
        struct Upload;
        std::function<qint64(const QString&)> freeBytes_;
        QHash<QString, std::shared_ptr<Upload>> uploads_;
        qint64 pendingBytesExcept(const QString& id) const;
    };

    // The sweep without an Uploads: for a worker thread handed the busy ids as a snapshot.
    int sweepDirs(const QStringList& dirs, const QDateTime& now, const QSet<QString>& skipIds);

    // The embedded page's bytes (kPageResource). Empty when the resource is missing.
    QByteArray pageHtml();
}
