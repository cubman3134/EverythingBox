// Decides what should happen to each save file when this device and the cloud disagree. PURE — no network,
// no filesystem, no QSettings — so probe_savesync can drive every row of the table as data.
//
// Saves used to ride inside the whole-app sync zip, applied wholesale with an "always take the cloud" rule at
// startup: two devices playing the same game silently lost one side's saves, and any single save write
// re-uploaded the entire bundle. This file is the replacement rule. Two of its properties are load-bearing
// and are stated as rules rather than behaviours, because their failure modes are unrecoverable:
//   * firstRun NEVER deletes, in either direction.
//   * a conflict NEVER destroys the losing copy.
//   * a tombstone NEVER beats a copy that changed since the baseline, in either direction. A delete is
//     authorised against the bytes that were tombstoned and nothing else; anything newer is a resurrection
//     or another device's save, and update-beats-delete is the recoverable direction.
#pragma once
#include <QHash>
#include <QSet>
#include <QString>
#include <QVector>

namespace SaveSyncPlan
{
    // Ranges within which two clocks are treated as agreeing. Beyond this, newest genuinely wins.
    inline constexpr qint64 kSkewWindowMs = 5000;

    // One file's state on one side. An absent file is represented by an empty `sha`, NOT by omission —
    // callers may pass it either way and plan() treats both identically.
    struct Entry
    {
        // Path RELATIVE to the saves root: "Zelda.srm", "saves/Zelda.srm", "saves/snes/Zelda.srm".
        // NOT read by plan() — the QHash key is authoritative for identity, and every lookup goes through
        // it. A caller that inserts under a key differing from entry.name gets sane behaviour here and
        // confusing behaviour downstream, so keep the two equal. It IS read by conflictName().
        QString name;
        QString sha;         // content hash; empty = not present on this side
        qint64  mtimeMs = 0;
        qint64  size = 0;
        QString deviceId;    // who last wrote it (remote side only); carried for logs, not used to decide

        bool present() const { return !sha.isEmpty(); }
    };

    enum class Act
    {
        None,           // already in step
        Upload,         // local is the newer or only copy
        Download,       // remote is the newer or only copy
        Conflict,       // both changed since the baseline and differ
        DeleteRemote,   // deleted locally, with a tombstone to prove it
        DeleteLocal     // deleted remotely, with a tombstone to prove it
    };

    struct Decision
    {
        QString name;
        Act     act = Act::None;
        // Conflict only: true when the LOCAL copy keeps the real name. This is what tells the transport
        // whether it must fetch the losing remote copy before overwriting it — see the preservation rule.
        bool    localWins = false;
        QString reason;   // one line for the log; an action is never taken silently
    };

    // The whole rule. `firstRun` (no baseline exists yet) is a HARD no-delete mode.
    QVector<Decision> plan(const QHash<QString, Entry>& local,
                           const QHash<QString, Entry>& remote,
                           const QHash<QString, Entry>& baseline,
                           const QSet<QString>&         tombstones,
                           bool                         firstRun);

    // "Zelda.state1" -> "Zelda.conflict-<deviceId>-20260727-141530.state1"
    // "saves/snes/Zelda.srm" -> "saves/snes/Zelda.conflict-<deviceId>-20260727-141530.srm"
    // The suffix goes before the extension so the file keeps its type, and carries BOTH the device and the
    // timestamp so two conflicts from two devices cannot collide. Any directory component is preserved, so
    // the copy lands beside its original and two systems' same-named saves stay distinct.
    QString conflictName(const QString& name, const QString& deviceId, qint64 mtimeMs);

    // A .conflict-* artifact is local recovery only. Syncing it would multiply one conflict across every
    // device, so it is excluded from the synced set entirely.
    bool isConflictArtifact(const QString& name);
}
