#include "SaveSync.h"

#include "CloudSync.h"
#include "SaveMeta.h"
#include "Tombstones.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStringList>
#include <QTimer>
#include <QUrl>

using SaveSyncPlan::Act;
using SaveSyncPlan::Decision;
using SaveSyncPlan::Entry;

namespace {

// The shared index of what the cloud holds. It IS the remote side of the plan: Drive's own file list is not
// consulted per-run, because a listing gives no content hash and a hash is what the rules compare.
const QLatin1String kIndexName("saves-index.json");
const QLatin1String kBaselineName("save-baseline.json");
// Tombstone namespace. Saves are not per-profile — a save belongs to the machine, not to a viewer profile.
const QLatin1String kTombStore("saves");

// Drive is ONE flat folder here (CloudSync's primitives take no parent path), and findFile builds its query
// by substituting the name into a quoted Drive query string. So a save's relative path cannot be the Drive
// name as-is: '/' would be meaningless there and an apostrophe in a ROM name would break the query outright.
// Percent-encoding gives a flat, deterministic, query-safe name, and it is reversible if anyone ever needs
// to read the folder by hand.
QString driveNameFor(const QString& rel)
{
    return QStringLiteral("save-") + QString::fromLatin1(QUrl::toPercentEncoding(rel));
}

QString sha256Of(const QByteArray& data)
{
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

// Empty on ANY failure to read — callers must treat that as "unknown", never as "empty file".
QString sha256File(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QString();
    QCryptographicHash h(QCryptographicHash::Sha256);
    if (!h.addData(&f)) return QString();
    return QString::fromLatin1(h.result().toHex());
}

Entry entryFromDisk(const QString& name, const QString& absPath)
{
    Entry e;
    e.name = name;
    const QFileInfo fi(absPath);
    if (!fi.exists()) return e;                 // sha stays empty => !present()
    e.sha     = sha256File(absPath);            // empty if unreadable => !present(); the caller decides
    e.mtimeMs = fi.lastModified().toMSecsSinceEpoch();
    e.size    = fi.size();
    return e;
}

QByteArray entriesToJson(const QHash<QString, Entry>& m)
{
    QJsonObject root;
    for (auto it = m.constBegin(); it != m.constEnd(); ++it)
    {
        const Entry& e = it.value();
        QJsonObject o;
        o.insert(QStringLiteral("sha"), e.sha);
        o.insert(QStringLiteral("mtimeMs"), static_cast<double>(e.mtimeMs));
        o.insert(QStringLiteral("size"), static_cast<double>(e.size));
        if (!e.deviceId.isEmpty()) o.insert(QStringLiteral("deviceId"), e.deviceId);
        root.insert(it.key(), o);
    }
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

// `ok` distinguishes "no such document" from "a document we could not understand". A corrupt index parsed as
// an empty one would look exactly like a cloud with no saves in it, which is a shape the rules are allowed
// to delete against.
QHash<QString, Entry> entriesFromJson(const QByteArray& data, bool* ok = nullptr)
{
    QHash<QString, Entry> out;
    if (ok) *ok = true;
    if (data.trimmed().isEmpty()) return out;
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(data, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) { if (ok) *ok = false; return out; }
    const QJsonObject root = doc.object();
    for (auto it = root.constBegin(); it != root.constEnd(); ++it)
    {
        const QJsonObject o = it.value().toObject();
        Entry e;
        e.name     = it.key();
        e.sha      = o.value(QStringLiteral("sha")).toString();
        e.mtimeMs  = static_cast<qint64>(o.value(QStringLiteral("mtimeMs")).toDouble());
        e.size     = static_cast<qint64>(o.value(QStringLiteral("size")).toDouble());
        e.deviceId = o.value(QStringLiteral("deviceId")).toString();
        if (!e.present()) continue;              // an entry with no hash carries no decidable information
        out.insert(e.name, e);
    }
    return out;
}

// A relative path as the manifest keys it: forward slashes, no leading separator, no "./".
QString normalizeRel(const QString& rel)
{
    QString s = QDir::fromNativeSeparators(rel).trimmed();
    while (s.startsWith(QLatin1String("./"))) s.remove(0, 2);
    while (s.startsWith(QLatin1Char('/')))    s.remove(0, 1);
    return s;
}

} // namespace

// One reconcile in flight. Held by shared_ptr because every step of it is a network callback away from the
// next, and the run must outlive each of them.
struct SaveSync::Run
{
    QString folderId;
    QHash<QString, Entry> local, remote, baseline;
    QHash<QString, Entry> nextBase, nextRemote;   // what we will PUBLISH, built only from successes
    QVector<Decision>     decisions;
    QSet<QString>         settled;                // names this run actually finished, for dirty_ retirement
    int  idx = 0, uploaded = 0, downloaded = 0, conflicts = 0;
    bool failed = false;
    bool remoteMoved = false;                     // the manifest needs republishing
    std::function<void(bool, int, int, int)> cb;
};

SaveSync::SaveSync(CloudSync* cloud, QString root, QString deviceId, QObject* parent)
    : QObject(parent), cloud_(cloud), root_(std::move(root)), deviceId_(std::move(deviceId))
{
    debounce_ = new QTimer(this);
    debounce_->setSingleShot(true);
    debounce_->setInterval(kUploadDebounceMs);
    connect(debounce_, &QTimer::timeout, this, [this] { pushDirty([](bool) {}); });
}

SaveSync::~SaveSync() = default;

QString SaveSync::baselinePath() const { return root_ + QLatin1Char('/') + kBaselineName; }

bool SaveSync::firstRun() const { return !QFileInfo::exists(baselinePath()); }

// ---- local state -------------------------------------------------------------------------------------

QHash<QString, Entry> SaveSync::scanLocal(QSet<QString>* unreadable) const
{
    QHash<QString, Entry> out;
    // Both trees, RECURSIVELY: new saves are written under saves/<system>/ (save-sync T4) while saves made
    // before that change stay flat, and the relative path INCLUDING the subdirectory is the sync name.
    const QStringList tops{ QStringLiteral("saves"), QStringLiteral("states") };
    for (const QString& top : tops)
    {
        const QDir base(root_ + QLatin1Char('/') + top);
        if (!base.exists()) continue;
        QDirIterator it(base.absolutePath(), QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext())
        {
            const QString path = it.next();
            const QString rel  = top + QLatin1Char('/') + base.relativeFilePath(path);
            // A .conflict-* copy is local recovery only. Syncing it would multiply one conflict across every
            // device, and it would come back as a "new" save on the machine that made it.
            if (SaveSyncPlan::isConflictArtifact(rel)) continue;
            const Entry e = entryFromDisk(rel, path);
            if (!e.present()) { if (unreadable) unreadable->insert(rel); continue; }
            out.insert(rel, e);
        }
    }
    return out;
}

QHash<QString, Entry> SaveSync::readBaseline() const
{
    QFile f(baselinePath());
    if (!f.open(QIODevice::ReadOnly)) return {};
    return entriesFromJson(f.readAll());
}

bool SaveSync::writeBaseline(const QHash<QString, Entry>& m) const
{
    // QSaveFile, and not negotiable: this file is what tells the next run what already happened. A truncated
    // one parses as empty, an empty one that still EXISTS is a non-firstRun with no history, and the run
    // after that would re-upload everything and mis-read tombstones.
    QDir().mkpath(root_);
    QSaveFile f(baselinePath());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const QByteArray json = entriesToJson(m);
    if (f.write(json) != json.size()) { f.cancelWriting(); return false; }
    return f.commit();
}

// ---- the Drive seam ----------------------------------------------------------------------------------

void SaveSync::resolveFolder(std::function<void(const QString&)> cb)
{
    if (!cloud_) { cb(QString()); return; }
    cloud_->ensureFolder(std::move(cb));
}

void SaveSync::fetchManifest(const QString& folderId,
                             std::function<void(bool, const QHash<QString, Entry>&)> cb)
{
    if (!cloud_) { cb(false, {}); return; }
    cloud_->findFile(folderId, kIndexName, [this, cb](bool listOk, const QString& id, const QString&, const QString&) {
        // listOk==false is "we could not reach Drive", NOT "there is nothing there". Reading it as an empty
        // cloud would hand plan() a remote side with every file missing — and with a tombstone that is a
        // local delete. Fail the run instead.
        if (!listOk)     { cb(false, {}); return; }
        if (id.isEmpty()) { cb(true,  {}); return; }   // proven absent: no saves have ever been synced
        cloud_->downloadFile(id, [this, cb](bool ok, const QByteArray& data) {
            if (!ok) { cb(false, {}); return; }
            bool parsed = false;
            const QHash<QString, Entry> m = entriesFromJson(data, &parsed);
            if (!parsed) { emit log(QStringLiteral("save sync: the cloud index is unreadable")); cb(false, {}); return; }
            cb(true, m);
        });
    });
}

void SaveSync::putManifest(const QString& folderId, const QHash<QString, Entry>& manifest,
                           std::function<void(bool)> cb)
{
    if (!cloud_) { cb(false); return; }
    const QByteArray json = entriesToJson(manifest);
    cloud_->findFile(folderId, kIndexName, [this, folderId, json, cb](bool listOk, const QString& id, const QString&, const QString&) {
        // Never create on a failed lookup: that mints a SECOND saves-index.json in the same folder and
        // findFile would then return whichever Drive listed first.
        if (!listOk) { cb(false); return; }
        cloud_->uploadFile(folderId, id, kIndexName, QStringLiteral("application/json"), json,
                           sha256Of(json), [cb](const QString& newId) { cb(!newId.isEmpty()); });
    });
}

void SaveSync::downloadInto(const QString& folderId, const QString& name, const QString& destPath,
                            std::function<void(bool)> cb)
{
    if (!cloud_) { cb(false); return; }
    cloud_->findFile(folderId, driveNameFor(name), [this, name, destPath, cb](bool listOk, const QString& id, const QString&, const QString&) {
        if (!listOk || id.isEmpty())
        { emit log(QStringLiteral("save sync: %1 is in the index but not in the cloud folder").arg(name)); cb(false); return; }
        cloud_->downloadFile(id, [this, name, destPath, cb](bool ok, const QByteArray& data) {
            if (!ok) { emit log(QStringLiteral("save sync: download of %1 failed").arg(name)); cb(false); return; }
            QDir().mkpath(QFileInfo(destPath).absolutePath());
            // QSaveFile again: a save half-written by an interrupted download is worse than no download.
            QSaveFile out(destPath);
            if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate) || out.write(data) != data.size() || !out.commit())
            { emit log(QStringLiteral("save sync: could not write %1").arg(destPath)); cb(false); return; }
            cb(true);
        });
    });
}

void SaveSync::uploadFrom(const QString& folderId, const QString& name, const QString& srcPath,
                          std::function<void(bool, const Entry&)> cb)
{
    if (!cloud_) { cb(false, {}); return; }

    QFile f(srcPath);
    if (!f.open(QIODevice::ReadOnly))
    { emit log(QStringLiteral("save sync: could not read %1").arg(name)); cb(false, {}); return; }
    const QByteArray data = f.readAll();
    f.close();

    // Torn-write guard. An emulator flushing SRAM, or a state being written as we read it, would otherwise
    // publish half a file under the real name — and the next device to sync would take it as the truth.
    // Hash what we read, then hash the file again from disk: if they differ the file moved underneath us, so
    // abandon this upload and leave the entry dirty for the next pass.
    const QString sha   = sha256Of(data);
    const QString again = sha256File(srcPath);
    if (again.isEmpty() || again != sha)
    {
        emit log(QStringLiteral("save sync: %1 changed while being read — not uploading a torn copy").arg(name));
        cb(false, {});
        return;
    }

    Entry sent;
    sent.name     = name;
    sent.sha      = sha;
    sent.size     = data.size();
    sent.mtimeMs  = QFileInfo(srcPath).lastModified().toMSecsSinceEpoch();
    sent.deviceId = deviceId_;

    const QString driveName = driveNameFor(name);
    cloud_->findFile(folderId, driveName, [this, folderId, driveName, data, sha, sent, name, cb](bool listOk, const QString& id, const QString&, const QString&) {
        // Same reason as putManifest: uploading with an empty existingId CREATES, so a failed lookup would
        // leave two files with one name and later reads would pick between them arbitrarily.
        if (!listOk)
        { emit log(QStringLiteral("save sync: could not look up %1 in the cloud").arg(name)); cb(false, {}); return; }
        cloud_->uploadFile(folderId, id, driveName, QStringLiteral("application/octet-stream"), data, sha,
                           [this, name, sent, cb](const QString& newId) {
            if (newId.isEmpty())
            { emit log(QStringLiteral("save sync: upload of %1 failed").arg(name)); cb(false, {}); return; }
            cb(true, sent);
        });
    });
}

// ---- the reconcile -----------------------------------------------------------------------------------

void SaveSync::syncNow(std::function<void(bool, int, int, int)> cb)
{
    begin({}, std::move(cb));
}

void SaveSync::begin(const QSet<QString>& only, std::function<void(bool, int, int, int)> cb)
{
    if (busy_)
    {
        // Two reconciles at once would each read the baseline, each write it, and the loser's successes
        // would vanish — which is the "recorded as synced but is not" failure from the other direction.
        emit log(QStringLiteral("save sync: a sync is already running"));
        cb(false, 0, 0, 0);
        return;
    }
    busy_ = true;

    auto r = std::make_shared<Run>();
    r->cb = std::move(cb);

    resolveFolder([this, r, only](const QString& folderId) {
        if (folderId.isEmpty())
        { emit log(QStringLiteral("save sync: no Drive folder")); busy_ = false; r->cb(false, 0, 0, 0); return; }
        r->folderId = folderId;

        fetchManifest(folderId, [this, r, only](bool ok, const QHash<QString, Entry>& remote) {
            if (!ok)
            {
                emit log(QStringLiteral("save sync: could not read the cloud index — doing nothing"));
                busy_ = false; r->cb(false, 0, 0, 0); return;
            }

            QSet<QString> unreadable;
            r->remote   = remote;
            r->local    = scanLocal(&unreadable);
            r->baseline = readBaseline();

            QSet<QString> tombs;
            const QVector<Tombstones::Entry> ts = Tombstones::all(kTombStore);
            for (const Tombstones::Entry& t : ts) tombs.insert(t.key);

            // Failures leave their OLD baseline row untouched, so the next run re-plans them identically.
            r->nextBase   = r->baseline;
            r->nextRemote = r->remote;

            const QVector<Decision> all = SaveSyncPlan::plan(r->local, r->remote, r->baseline, tombs, firstRun());
            for (const Decision& d : all)
            {
                if (!only.isEmpty() && !only.contains(d.name)) continue;
                // A file we could not hash is neither present nor absent as far as the rules go, and both of
                // the answers plan() can give for "absent" (restore it, or delete it) are wrong for a save
                // the emulator merely has open. Skip the name entirely and try again next time.
                if (unreadable.contains(d.name))
                { emit log(QStringLiteral("save sync: %1 is in use — skipped this pass").arg(d.name)); continue; }
                r->decisions.push_back(d);
            }
            for (const QString& u : unreadable) emit log(QStringLiteral("save sync: could not read %1").arg(u));

            seedAgreed(r, only);
            step(r);
        });
    });
}

// Rows where the two sides already AGREE are recorded even though nothing moved. plan() returns no decision
// for them, but leaving a stale baseline row in place is not harmless: with baseline "a" and both sides at
// "z", the next local edit reads as localChanged AND remoteChanged and manufactures a conflict out of two
// devices that were in step. Rows gone from both sides are dropped for the same reason in reverse.
void SaveSync::seedAgreed(const std::shared_ptr<Run>& r, const QSet<QString>& only) const
{
    QSet<QString> names;
    for (auto it = r->local.constBegin();    it != r->local.constEnd();    ++it) names.insert(it.key());
    for (auto it = r->remote.constBegin();   it != r->remote.constEnd();   ++it) names.insert(it.key());
    for (auto it = r->baseline.constBegin(); it != r->baseline.constEnd(); ++it) names.insert(it.key());

    for (const QString& name : names)
    {
        if (!only.isEmpty() && !only.contains(name)) continue;
        const Entry L = r->local.value(name);
        const Entry R = r->remote.value(name);
        if (!L.present() && !R.present())            { r->nextBase.remove(name); continue; }
        if (L.present() && R.present() && L.sha == R.sha) r->nextBase.insert(name, L);
    }
}

void SaveSync::step(std::shared_ptr<Run> r)
{
    if (r->idx >= r->decisions.size()) { publish(std::move(r)); return; }

    const Decision d = r->decisions[r->idx++];
    const QString  path = root_ + QLatin1Char('/') + d.name;
    // Every action is logged with the rule that produced it; an action is never taken silently.
    emit log(QStringLiteral("save sync: %1 — %2").arg(d.name, d.reason));

    switch (d.act)
    {
    case Act::Upload:
        uploadFrom(r->folderId, d.name, path, [this, r, d](bool ok, const Entry& sent) {
            if (ok)
            {
                ++r->uploaded;
                r->nextBase.insert(d.name, sent);       // what we SENT, not what the scan believed
                r->nextRemote.insert(d.name, sent);
                r->remoteMoved = true;
                r->settled.insert(d.name);
            }
            else r->failed = true;
            step(r);
        });
        return;

    case Act::Download:
        downloadInto(r->folderId, d.name, path, [this, r, d, path](bool ok) {
            if (ok)
            {
                ++r->downloaded;
                // From disk, not from the manifest: the baseline must describe the bytes that are actually
                // here. If the index lied about the hash, say so and let the next run reconcile it.
                const Entry got = entryFromDisk(d.name, path);
                if (got.sha != r->remote.value(d.name).sha)
                    emit log(QStringLiteral("save sync: %1 does not match the hash the index claimed").arg(d.name));
                r->nextBase.insert(d.name, got);
                r->settled.insert(d.name);
            }
            else r->failed = true;
            step(r);
        });
        return;

    case Act::Conflict:
        executeConflict(d, r->local.value(d.name), r->remote.value(d.name), r->folderId,
                        [this, r, d](bool ok, const Entry& applied) {
            if (ok)
            {
                ++r->conflicts;
                r->nextBase.insert(d.name, applied);
                if (d.localWins) { r->nextRemote.insert(d.name, applied); r->remoteMoved = true; }
                r->settled.insert(d.name);
            }
            else r->failed = true;
            step(r);
        });
        return;

    case Act::DeleteRemote:
        // CloudSync has no delete primitive, so the deletion is the index entry going away: the index IS the
        // remote side of the rules, and an entry absent from it is absent from the cloud. The blob is left
        // behind (harmless — a later re-upload of the same name PATCHes it rather than duplicating it), and
        // reclaiming it needs a Drive delete this class does not have. Recorded as a follow-up.
        r->nextRemote.remove(d.name);
        r->nextBase.remove(d.name);
        r->remoteMoved = true;
        r->settled.insert(d.name);
        step(r);
        return;

    case Act::DeleteLocal:
        if (QFile::exists(path) && !QFile::remove(path))
        { emit log(QStringLiteral("save sync: could not delete %1").arg(d.name)); r->failed = true; }
        else { r->nextBase.remove(d.name); r->settled.insert(d.name); }
        step(r);
        return;

    case Act::None:
        step(r);
        return;
    }
    step(r);
}

void SaveSync::publish(std::shared_ptr<Run> r)
{
    if (!r->remoteMoved) { finish(std::move(r), true); return; }

    putManifest(r->folderId, r->nextRemote, [this, r](bool ok) {
        if (!ok)
        {
            // The bytes went up but the index did not, so from the rules' point of view the cloud has not
            // changed. Advancing the baseline here would be the exact lie this class must not tell: the next
            // run would believe the cloud holds files its index does not list. Leave the baseline alone and
            // let everything replay — uploads are idempotent PATCHes.
            emit log(QStringLiteral("save sync: the cloud index could not be updated — baseline not advanced"));
            r->failed = true;
            finish(r, false);
            return;
        }
        finish(r, true);
    });
}

void SaveSync::finish(std::shared_ptr<Run> r, bool writeBase)
{
    if (writeBase && !writeBaseline(r->nextBase))
    {
        emit log(QStringLiteral("save sync: could not write %1").arg(kBaselineName));
        r->failed = true;
    }
    if (writeBase) for (const QString& n : r->settled) dirty_.remove(n);

    busy_ = false;
    r->cb(!r->failed, r->uploaded, r->downloaded, r->conflicts);
}

// ---- the conflict, whose ORDERING is the whole preservation promise -----------------------------------

void SaveSync::executeConflict(const Decision& d, const Entry& localE, const Entry& remoteE,
                               const QString& folderId,
                               std::function<void(bool ok, const Entry& applied)> done)
{
    const QString localPath = root_ + QLatin1Char('/') + d.name;

    if (!d.localWins)
    {
        // The LOCAL copy loses. Its bytes are already on this disk, so preserving it is a rename — do that
        // FIRST, then bring the winner down. If the rename fails we must NOT download, or we would overwrite
        // the copy we just failed to save.
        const QString kept = SaveSyncPlan::conflictName(d.name, deviceId_, localE.mtimeMs);
        if (!QFile::rename(localPath, root_ + QLatin1Char('/') + kept))
        {
            emit log(QStringLiteral("save conflict: could not preserve local copy of %1").arg(d.name));
            done(false, {});
            return;
        }
        emit conflictKept(SaveMeta::titleFor(d.name), kept);
        downloadInto(folderId, d.name, localPath, [this, d, kept, localPath, done](bool ok) {
            if (!ok)
            {
                // Abort means abort: put the local copy back under its real name so both sides are exactly
                // as they were and the next sync re-runs the whole conflict. Without this the real name is
                // simply missing until the next pass — recoverable, but a half-applied conflict all the same.
                if (!QFile::rename(root_ + QLatin1Char('/') + kept, localPath))
                    emit log(QStringLiteral("save conflict: %1 is preserved as %2 but could not be restored")
                                 .arg(d.name, kept));
                done(false, {});
                return;
            }
            done(true, entryFromDisk(d.name, localPath));
        });
        return;
    }

    // The REMOTE copy loses — and this is the case that can destroy data. Its bytes exist ONLY in the cloud,
    // and the very next thing we do is overwrite that name with ours. So fetch the loser and write it as a
    // .conflict-… copy BEFORE uploading. Uploading first would make "the loser is preserved" a lie in exactly
    // the case that matters, and it would look correct in review.
    const QString who  = remoteE.deviceId.isEmpty() ? QStringLiteral("cloud") : remoteE.deviceId;
    const QString kept = SaveSyncPlan::conflictName(d.name, who, remoteE.mtimeMs);
    downloadInto(folderId, d.name, root_ + QLatin1Char('/') + kept, [this, d, kept, localPath, folderId, done](bool ok) {
        if (!ok)
        {
            emit log(QStringLiteral("save conflict: could not preserve the cloud copy of %1 — NOT uploading").arg(d.name));
            done(false, {});
            return;
        }
        emit conflictKept(SaveMeta::titleFor(d.name), kept);
        // The preserved copy stays on disk even if this upload fails: deleting it would undo the one thing
        // we came here to guarantee. A retry re-fetches the same loser to the same name and overwrites it
        // with identical bytes.
        uploadFrom(folderId, d.name, localPath, [done](bool uok, const Entry& sent) { done(uok, sent); });
    });
}

// ---- the debounced fast path --------------------------------------------------------------------------

void SaveSync::markDirty(const QString& relPath)
{
    const QString rel = normalizeRel(relPath);
    if (rel.isEmpty() || SaveSyncPlan::isConflictArtifact(rel)) return;
    dirty_.insert(rel);
    debounce_->start(kUploadDebounceMs);   // restart: a burst of saves costs one push, not one each
}

void SaveSync::recordDelete(const QString& relPath)
{
    const QString rel = normalizeRel(relPath);
    if (rel.isEmpty()) return;
    dirty_.remove(rel);
    Tombstones::record(kTombStore, rel);
    emit log(QStringLiteral("save sync: %1 deleted here").arg(rel));
}

void SaveSync::flush(std::function<void(bool)> cb)
{
    debounce_->stop();
    pushDirty(std::move(cb));
}

void SaveSync::pushDirty(std::function<void(bool)> cb)
{
    if (dirty_.isEmpty()) { cb(true); return; }
    if (busy_)
    {
        // Nothing is lost by deferring: dirty_ keeps the names, and a full reconcile would upload them
        // anyway because their content differs from the baseline.
        debounce_->start(kUploadDebounceMs);
        cb(false);
        return;
    }
    // A restricted reconcile, not a bare upload. The save that just landed here may also have been written
    // on another device, and the fast path must reach the same conflict-preserving answer the full pass does.
    const QSet<QString> only = dirty_;
    begin(only, [cb](bool ok, int, int, int) { cb(ok); });
}
