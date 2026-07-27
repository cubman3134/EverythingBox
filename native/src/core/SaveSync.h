// Per-file sync of emulator saves and save states against Drive, using CloudSync's primitives. Replaces the
// old whole-zip snapshot: one Drive file per save, so an F2 press uploads ONE state instead of re-uploading
// addons, themes and settings alongside it.
//
// The rules live in SaveSyncPlan; this file is transport and ordering only. Three properties here are
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
//
// NOT THREAD-SAFE: hashes files and writes JSON on the calling thread. GUI-thread use only.
#pragma once
#include "SaveSyncPlan.h"   // Entry/Act/Decision are in this class's interface

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
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

    // `root` is the folder holding saves/ and states/ (AppPaths::dataDir()); `deviceId` is the same id the
    // progress sync uses (mdsync T1), so conflict copies name a device the user can recognise.
    SaveSync(CloudSync* cloud, QString root, QString deviceId, QObject* parent = nullptr);
    ~SaveSync() override;

    // Full reconcile: pull the manifest, plan, execute. cb reports how many files moved each way and how
    // many conflicts were preserved. ok is false if ANY step failed — the caller should retry later, and the
    // baseline will have advanced only over the files that genuinely made it.
    void syncNow(std::function<void(bool ok, int uploaded, int downloaded, int conflicts)> cb);

    // A save was just written. Coalesces into one debounced upload of THAT file (kUploadDebounceMs).
    void markDirty(const QString& relPath);
    // Push anything still pending. Called at exit, inside the existing shutdown watchdog.
    void flush(std::function<void(bool ok)> cb);

    // Record that the user deliberately deleted a save, so it is not restored on the next sync.
    void recordDelete(const QString& relPath);

signals:
    // A conflict was resolved. `keptAs` is the preserved losing copy's filename; `title` is the game name
    // from SaveMeta when known, else the raw filename.
    void conflictKept(const QString& title, const QString& keptAs);
    void log(const QString& line);

protected:
    // ---- the transport seam ------------------------------------------------------------------------
    // Every call that actually touches Drive goes through one of these five, and they are virtual for one
    // reason: the ordering rules above are the whole point of this class and they cannot be asserted against
    // a live Google account. A headless probe subclasses SaveSync, substitutes an in-memory cloud, and drives
    // the real plan/execute/baseline code. Nothing in production overrides them.
    virtual void resolveFolder(std::function<void(const QString& folderId)> cb);
    virtual void fetchManifest(const QString& folderId,
                               std::function<void(bool ok, const QHash<QString, SaveSyncPlan::Entry>&)> cb);
    virtual void putManifest(const QString& folderId, const QHash<QString, SaveSyncPlan::Entry>& manifest,
                             std::function<void(bool ok)> cb);
    // Fetch `name`'s cloud bytes and write them at `destPath` (atomically; parents created).
    virtual void downloadInto(const QString& folderId, const QString& name, const QString& destPath,
                              std::function<void(bool ok)> cb);
    // Send `srcPath` as `name`. Reports the entry ACTUALLY sent — the baseline is written from this, not
    // from what the scan believed, and not from the plan.
    virtual void uploadFrom(const QString& folderId, const QString& name, const QString& srcPath,
                            std::function<void(bool ok, const SaveSyncPlan::Entry& sent)> cb);

    // `unreadable` (optional) collects names that exist but could not be hashed — a save the running
    // emulator holds open, typically. They are NOT reported as absent: "absent" means restore-or-delete,
    // and neither is a safe answer for a file we simply could not look at.
    QHash<QString, SaveSyncPlan::Entry> scanLocal(QSet<QString>* unreadable = nullptr) const;
    QHash<QString, SaveSyncPlan::Entry> readBaseline() const;
    bool    writeBaseline(const QHash<QString, SaveSyncPlan::Entry>&) const;
    bool    firstRun() const;          // exactly "no baseline file exists yet"
    QString baselinePath() const;

private:
    struct Run;   // one reconcile in flight

    // Both entry points funnel here: `only` empty means every file, otherwise the run is restricted to those
    // names. The debounced push is a restricted run rather than a bare upload precisely so it still obeys
    // the conflict rule — a save written here while another device wrote the same file must not be a silent
    // overwrite just because it arrived through the fast path.
    void begin(const QSet<QString>& only, std::function<void(bool, int, int, int)> cb);
    void seedAgreed(const std::shared_ptr<Run>& r, const QSet<QString>& only) const;
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
};
