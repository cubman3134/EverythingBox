#include "SaveSyncPlan.h"

#include <QDateTime>
#include <QFileInfo>

namespace {

using SaveSyncPlan::Entry;

// Two writes within kSkewWindowMs are treated as simultaneous: wall clocks on two machines are not
// trustworthy to finer than that, and pretending otherwise makes the winner depend on clock drift.
bool nearlySimultaneous(qint64 a, qint64 b)
{
    return qAbs(a - b) < SaveSyncPlan::kSkewWindowMs;
}

// TRUE when the local copy should keep the real name. Both devices run this over the SAME pair of entries
// (with local/remote swapped), so it must be antisymmetric — if it ever returned true for both views the two
// devices would each keep their own copy and diverge permanently, which is worse than having no rule.
bool localWinsOver(const Entry& local, const Entry& remote)
{
    if (!nearlySimultaneous(local.mtimeMs, remote.mtimeMs)) return local.mtimeMs > remote.mtimeMs;
    if (local.sha != remote.sha)           return local.sha > remote.sha;
    return local.deviceId > remote.deviceId;
}

} // namespace

QVector<SaveSyncPlan::Decision> SaveSyncPlan::plan(const QHash<QString, Entry>& local,
                                                   const QHash<QString, Entry>& remote,
                                                   const QHash<QString, Entry>& baseline,
                                                   const QSet<QString>&         tombstones,
                                                   bool                         firstRun)
{
    QVector<Decision> out;

    QSet<QString> names;
    for (auto it = local.constBegin();    it != local.constEnd();    ++it) names.insert(it.key());
    for (auto it = remote.constBegin();   it != remote.constEnd();   ++it) names.insert(it.key());
    for (auto it = baseline.constBegin(); it != baseline.constEnd(); ++it) names.insert(it.key());

    QStringList ordered(names.constBegin(), names.constEnd());
    ordered.sort();   // deterministic output; the caller logs these and probes compare them

    for (const QString& name : ordered)
    {
        if (isConflictArtifact(name)) continue;   // local recovery only — never synced

        const Entry L = local.value(name);
        const Entry R = remote.value(name);
        const Entry B = baseline.value(name);

        // Gone from both sides: nothing to move and nothing to delete. Treating a stale baseline entry as a
        // deletion would let it delete a file that no longer exists anywhere.
        if (!L.present() && !R.present()) continue;

        const bool localChanged  = L.sha != B.sha;
        const bool remoteChanged = R.sha != B.sha;

        Decision d;
        d.name = name;

        if (!L.present())
        {
            // Absent locally. A tombstone is the ONLY evidence that this was a deliberate delete; without
            // one, a missing file is a restore. Getting this backwards deletes saves the user still wants.
            if (!firstRun && tombstones.contains(name))
            { d.act = Act::DeleteRemote; d.reason = QStringLiteral("deleted locally (tombstoned)"); }
            else
            { d.act = Act::Download; d.reason = firstRun ? QStringLiteral("first run: cloud-only save")
                                                         : QStringLiteral("missing locally, no tombstone"); }
            out.push_back(d);
            continue;
        }
        if (!R.present())
        {
            if (!firstRun && tombstones.contains(name))
            { d.act = Act::DeleteLocal; d.reason = QStringLiteral("deleted remotely (tombstoned)"); }
            else
            { d.act = Act::Upload; d.reason = firstRun ? QStringLiteral("first run: local-only save")
                                                       : QStringLiteral("missing remotely, no tombstone"); }
            out.push_back(d);
            continue;
        }

        if (L.sha == R.sha) { continue; }                       // already identical, whatever the baseline said

        if (localChanged && remoteChanged)
        {
            d.act = Act::Conflict;
            d.localWins = localWinsOver(L, R);
            d.reason = QStringLiteral("both changed since last sync; %1 copy is newer")
                           .arg(d.localWins ? QStringLiteral("local") : QStringLiteral("cloud"));
        }
        else if (localChanged)  { d.act = Act::Upload;   d.reason = QStringLiteral("changed on this device"); }
        else if (remoteChanged) { d.act = Act::Download; d.reason = QStringLiteral("changed on another device"); }
        else
        {
            // Neither differs from the baseline yet they differ from each other — the baseline is stale or
            // wrong. Treat it as a conflict rather than guessing: the preservation rule keeps both copies.
            d.act = Act::Conflict;
            d.localWins = localWinsOver(L, R);
            d.reason = QStringLiteral("baseline disagrees with both sides");
        }
        out.push_back(d);
    }
    return out;
}

QString SaveSyncPlan::conflictName(const QString& name, const QString& deviceId, qint64 mtimeMs)
{
    const QFileInfo fi(name);
    const QString base = fi.completeBaseName();
    const QString ext  = fi.suffix();
    const QString when = QDateTime::fromMSecsSinceEpoch(mtimeMs, Qt::UTC)
                             .toString(QStringLiteral("yyyyMMdd-HHmmss"));
    QString out = base + QStringLiteral(".conflict-") + deviceId + QLatin1Char('-') + when;
    if (!ext.isEmpty()) out += QLatin1Char('.') + ext;
    return out;
}

bool SaveSyncPlan::isConflictArtifact(const QString& name)
{
    return name.contains(QStringLiteral(".conflict-"));
}
