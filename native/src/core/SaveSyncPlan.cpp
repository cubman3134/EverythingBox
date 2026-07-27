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
//
// It only has to be antisymmetric on the inputs plan() can actually construct, and plan() has already
// `continue`d on L.sha == R.sha before calling this — so the shas always differ and one of the two lines
// below always decides. A deviceId tie-break used to sit on the end; it was unreachable AND wrong (equal
// sha with equal deviceId returns false for BOTH views, so the two devices swap copies instead of
// converging). If a later task wants this as a shared primitive over arbitrary pairs, promote it then and
// give the total order its own direct assertions.
bool localWinsOver(const Entry& local, const Entry& remote)
{
    if (!nearlySimultaneous(local.mtimeMs, remote.mtimeMs)) return local.mtimeMs > remote.mtimeMs;
    if (local.sha != remote.sha)                            return local.sha > remote.sha;
    return false;   // unreachable from plan(); see above.
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

        const bool tombstoned = tombstones.contains(name);

        if (!L.present())
        {
            // Absent locally. A tombstone is the ONLY evidence that this was a deliberate delete; without
            // one, a missing file is a restore. Getting this backwards deletes saves the user still wants.
            //
            // A tombstone authorises deleting the copy we tombstoned, and nothing else. If the cloud copy
            // has moved since the baseline then another device wrote it after our delete, and honouring the
            // tombstone would destroy that save outright — no conflict is declared, so no preserved copy
            // exists either. Update beats delete: the worst case is a file the user deletes twice.
            if (!firstRun && tombstoned && !remoteChanged)
            { d.act = Act::DeleteRemote; d.reason = QStringLiteral("deleted locally (tombstoned)"); }
            else
            { d.act = Act::Download;
              d.reason = firstRun    ? QStringLiteral("first run: cloud-only save")
                       : tombstoned  ? QStringLiteral("tombstoned here, but updated in the cloud since")
                                     : QStringLiteral("missing locally, no tombstone"); }
            out.push_back(d);
            continue;
        }
        if (!R.present())
        {
            // Mirror of the above, and the mirror matters: ONE undifferentiated tombstone set serves both
            // directions, so a tombstone written when this device deleted the file is still set when the
            // user plays again and recreates it. A present local file whose content differs from the
            // baseline was written after the tombstone and logically contradicts it — never delete it.
            if (!firstRun && tombstoned && !localChanged)
            { d.act = Act::DeleteLocal; d.reason = QStringLiteral("deleted remotely (tombstoned)"); }
            else
            { d.act = Act::Upload;
              d.reason = firstRun    ? QStringLiteral("first run: local-only save")
                       : tombstoned  ? QStringLiteral("tombstoned, but recreated here since")
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
        else if (localChanged) { d.act = Act::Upload;   d.reason = QStringLiteral("changed on this device"); }
        else
        {
            // remoteChanged is implied, so this is exhaustive and never falls through to a silent None:
            // we are past `L.sha == R.sha`, so the two sides differ, so they cannot BOTH equal B.sha —
            // !localChanged (L.sha == B.sha) therefore forces R.sha != B.sha.
            // (A fourth "the baseline disagrees with both sides" branch used to sit here. By the same
            // argument it was unreachable, so it could never be asserted on; it is gone rather than left
            // as untestable code claiming to handle a shape plan() cannot build.)
            d.act = Act::Download; d.reason = QStringLiteral("changed on another device");
        }
        out.push_back(d);
    }
    return out;
}

QString SaveSyncPlan::conflictName(const QString& name, const QString& deviceId, qint64 mtimeMs)
{
    // `name` is a path RELATIVE to the saves root ("Zelda.srm", "saves/Zelda.srm", "saves/snes/Zelda.srm"),
    // and completeBaseName() drops the directory. Losing it would write the preserved copy to the wrong
    // place AND make saves/nes/Zelda.srm and saves/snes/Zelda.srm produce ONE output name, so preserving
    // the second would overwrite the first — the exact copy the conflict rule exists to keep. Carry the
    // directory through verbatim, separator included.
    const int slash = qMax(name.lastIndexOf(QLatin1Char('/')), name.lastIndexOf(QLatin1Char('\\')));
    const QString dir = slash >= 0 ? name.left(slash + 1) : QString();

    const QFileInfo fi(name);
    const QString base = fi.completeBaseName();
    const QString ext  = fi.suffix();
    const QString when = QDateTime::fromMSecsSinceEpoch(mtimeMs, Qt::UTC)
                             .toString(QStringLiteral("yyyyMMdd-HHmmss"));
    QString out = dir + base + QStringLiteral(".conflict-") + deviceId + QLatin1Char('-') + when;
    if (!ext.isEmpty()) out += QLatin1Char('.') + ext;
    return out;
}

bool SaveSyncPlan::isConflictArtifact(const QString& name)
{
    return name.contains(QStringLiteral(".conflict-"));
}
