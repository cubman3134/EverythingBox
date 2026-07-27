// Per-file sync of emulator saves and save states against Drive, using CloudSync's primitives. Replaces the
// old whole-zip snapshot: one Drive file per save, so an F2 press uploads ONE state instead of re-uploading
// addons, themes and settings alongside it.
//
// The rules live in SaveSyncPlan; this file is transport and ordering only. Four properties here are
// load-bearing, and each is a rule rather than a behaviour because its failure mode is unrecoverable:
//
//   * When the REMOTE copy loses a conflict its bytes exist only in the cloud and we are about to overwrite
//     that name — so we download the loser and write it as .conflict-… BEFORE uploading the winner.
//     Uploading first would make "the loser is preserved" a lie in exactly the case that matters.
//   * The baseline is rewritten from what actually SUCCEEDED, never from the plan. A failed upload recorded
//     as synced makes the next run believe the cloud holds a file it does not — which then reads as "the
//     other device deleted it".
//   * "Could not reach Drive" is never allowed to read as "the cloud is empty". An empty remote view plus a
//     tombstone is a local delete, so a swallowed network error would erase saves.
//   * Neither the index nor a save blob is ever written as a BLIND whole-document overwrite. Both carry a
//     content hash in Drive's appProperties; a run remembers the hash it read and refuses to write over a
//     different one. Without that, two devices that publish from the same starting view leave the index
//     permanently disagreeing with the blob — each believing it is in sync — which is silent, permanent
//     divergence, i.e. the whole failure class this class exists to remove.
//
// THE CLOUD INDEX (saves-index.json) is the remote side of the rules, and it carries deletions as well as
// files, because tombstones that stay local can never delete anything: the other device simply re-uploads
// and the delete is undone even on the machine that made it.
//
//   { "v": 2,
//     "files": { "<rel path>": { "sha": …, "mtimeMs": …, "size": …, "deviceId": … }, … },
//     "tombs": [ { "key": "<rel path>", "ts": <epoch SECONDS> }, … ] }
//
// The tombstone semantics are Tombstones'/CloudMerge's, unchanged: a tombstone whose ts is >= the entry's
// own timestamp suppresses that entry, a strictly newer re-add beats an older tombstone, and every run
// imports the peer's tombstones locally (with their own ts) so this device re-propagates their delete.
// A document with no "v" is the pre-tombstone shape — the root IS the file map — and still reads.
//
// NOT THREAD-SAFE: hashes files and writes JSON on the calling thread. GUI-thread use only.
#pragma once
#include "SaveSyncPlan.h"   // Entry/Act/Decision are in this class's interface

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVector>
#include <functional>
#include <memory>

class CloudSync;
class QTimer;

class SaveSync : public QObject
{
    Q_OBJECT
public:
    // How long a save write is allowed to coalesce before it is pushed. Save states arrive in bursts (F2
    // held, an emulator flushing SRAM every few seconds), and each one otherwise costs a round trip.
    static constexpr int kUploadDebounceMs = 10000;

    // A published deletion. `ts` is epoch SECONDS — the same unit and meaning Tombstones stores.
    struct Tomb
    {
        QString key;
        qint64  ts = 0;
    };

    // `root` is the folder holding saves/ and states/ (AppPaths::dataDir()); `deviceId` is the same id the
    // progress sync uses (mdsync T1), so conflict copies name a device the user can recognise.
    SaveSync(CloudSync* cloud, QString root, QString deviceId, QObject* parent = nullptr);
    ~SaveSync() override;

    // Full reconcile: pull the manifest, plan, execute. cb reports how many files moved each way and how
    // many conflicts were preserved. ok is false if ANY step failed — the caller should retry later, and the
    // baseline will have advanced only over the files that genuinely made it.
    void syncNow(std::function<void(bool ok, int uploaded, int downloaded, int conflicts)> cb);

    // A save was just written. Coalesces into one debounced upload of THAT file (kUploadDebounceMs).
    //
    // `relPath` is relative to `root` and MUST INCLUDE the "saves/" or "states/" prefix ("saves/snes/Zelda.srm",
    // "states/Zelda.state1") — it is the same name scanLocal produces and the same key the index uses. A bare
    // filename matches nothing on disk, so the push finds no such file, the name never retires from the dirty
    // set, and the save reaches the cloud only at the next full syncNow.
    void markDirty(const QString& relPath);
    // Push anything still pending. Called at exit, inside the existing shutdown watchdog.
    void flush(std::function<void(bool ok)> cb);

    // Record that the user deliberately deleted a save, so it is not restored on the next sync. The tombstone
    // is published in the index, so the delete reaches every device rather than being undone by the first peer
    // that re-uploads. `relPath` carries the same "saves/"|"states/" prefix markDirty documents above.
    void recordDelete(const QString& relPath);

    // The flat, query-safe Drive name a save is stored under. Drive is ONE flat folder here (CloudSync's
    // primitives take no parent path) and findFile substitutes the name into a quoted Drive query, so a
    // relative path cannot be the Drive name as-is: '/' is meaningless there and an apostrophe in a ROM name
    // ("Link's Awakening") would break the query outright. Percent-encoding is flat, deterministic, query-safe
    // and reversible. Public so the probe addresses the in-memory cloud through the REAL mapping.
    static QString driveNameFor(const QString& rel);

signals:
    // A conflict was resolved. `keptAs` is the preserved losing copy's filename; `title` is the game name
    // from SaveMeta when known, else the raw filename.
    void conflictKept(const QString& title, const QString& keptAs);
    void log(const QString& line);

protected:
    // ---- the test seam ------------------------------------------------------------------------------
    // The seam is one level DOWN, in CloudSync: its four Drive primitives (ensureFolder/findFile/uploadFile/
    // downloadFile) are virtual, a headless probe substitutes an in-memory Drive, and EVERYTHING in this file
    // then runs for real — the index compare-and-swap, the torn-write guard, both listOk guards, driveNameFor
    // and QSaveFile's atomicity included. The seam used to sit at uploadFrom/downloadInto, one level up, which
    // meant the fake REPLACED two of the three rules the header calls load-bearing and they were asserted
    // nowhere. Nothing in production substitutes anything.
    QHash<QString, SaveSyncPlan::Entry> scanLocal(QSet<QString>* unreadable = nullptr,
                                                  const QSet<QString>& only = {}) const;
    // `ok` is false when a baseline file EXISTS but could not be parsed. That is not the same as "no baseline":
    // an empty-but-existing baseline is a non-firstRun with no history, which is a shape the rules may delete
    // against. Callers treat !ok as firstRun.
    QHash<QString, SaveSyncPlan::Entry> readBaseline(bool* ok = nullptr) const;
    bool    writeBaseline(const QHash<QString, SaveSyncPlan::Entry>&) const;
    bool    firstRun() const;          // exactly "no baseline file exists yet"
    QString baselinePath() const;

    // Fires INSIDE uploadFrom's torn-write window — between the hash of the bytes we read and the re-hash
    // from disk. Null in production and never set there; it exists because that window is a few microseconds
    // wide, so the guard cannot otherwise be exercised by a test that only touches files from outside.
    std::function<void(const QString& srcPath)> midReadHook_;

private:
    struct Run;   // one reconcile in flight

    // ---- transport. Not virtual: see the seam note above. -------------------------------------------
    void resolveFolder(std::function<void(const QString& folderId)> cb);
    void fetchManifest(const QString& folderId,
                       std::function<void(bool ok, const QHash<QString, SaveSyncPlan::Entry>&,
                                          const QVector<Tomb>&, const QString& indexHash)> cb);
    // `expectHash` is the index's stateHash as this run READ it. Publishing is a whole-document overwrite, so
    // a hash that has moved means another device republished in between and our document is built on a stale
    // view — `stale` comes back true and NOTHING is written.
    void putManifest(const QString& folderId, const QHash<QString, SaveSyncPlan::Entry>& manifest,
                     const QVector<Tomb>& tombs, const QString& expectHash,
                     std::function<void(bool ok, bool stale)> cb);
    // Fetch `name`'s cloud bytes and write them at `destPath` (atomically; parents created).
    void downloadInto(const QString& folderId, const QString& name, const QString& destPath,
                      std::function<void(bool ok)> cb);
    // Send `srcPath` as `name`. Reports the entry ACTUALLY sent — the baseline is written from this, not from
    // what the scan believed, and not from the plan. `expectRemoteSha` is the hash the index claimed for this
    // save when the run read it: a blob that no longer carries it was PATCHed by another device after we
    // planned, so uploading would overwrite bytes the conflict rule never got to compare.
    void uploadFrom(const QString& folderId, const QString& name, const QString& srcPath,
                    const QString& expectRemoteSha,
                    std::function<void(bool ok, const SaveSyncPlan::Entry& sent)> cb);

    // Both entry points funnel here: `only` empty means every file, otherwise the run is restricted to those
    // names. The debounced push is a restricted run rather than a bare upload precisely so it still obeys
    // the conflict rule — a save written here while another device wrote the same file must not be a silent
    // overwrite just because it arrived through the fast path.
    void begin(const QSet<QString>& only, std::function<void(bool, int, int, int)> cb, int attempt = 0);
    void seedAgreed(const std::shared_ptr<Run>& r) const;
    void step(std::shared_ptr<Run> r);
    void publish(std::shared_ptr<Run> r);
    void finish(std::shared_ptr<Run> r, bool writeBase);

    void executeConflict(const SaveSyncPlan::Decision& d,
                         const SaveSyncPlan::Entry& localE,
                         const SaveSyncPlan::Entry& remoteE,
                         const QString& folderId,
                         std::function<void(bool ok, const SaveSyncPlan::Entry& applied)> done);

    void pushDirty(std::function<void(bool ok)> cb);

    CloudSync* cloud_ = nullptr;
    QString    root_, deviceId_;
    QTimer*    debounce_ = nullptr;
    QSet<QString> dirty_;
    bool       busy_ = false;   // one reconcile at a time; two would race on the baseline and the manifest
    bool       warnedNoFolder_ = false;   // "no Drive folder" is said once per session, not once per autosave
};
