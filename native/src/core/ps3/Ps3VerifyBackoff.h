#pragma once
#include <QDateTime>
#include <QString>

// Per-title retry bound for game updates whose install VERIFIES as failed — the version reached the
// target but the pkg's entry table disagreed (Ps3Pkg::verifyInstalled). That verdict is normally
// self-healing: the chain re-runs next launch and the pkg entries overwrite in place. But when it is
// a PERSISTENT false negative for a title (a future RPCS3 extractor change, a pkg shape the table
// check reads wrong), the chain re-downloads and re-installs hundreds of megabytes on EVERY launch,
// forever, and each attempt rolls PARAM.SFO back — the same unbounded pattern a persistent sha1
// mismatch already has, on a trigger the verify layer widened. A marker in tmpDir bounds it: one
// chain per title per backoff window, and a suppressed launch records NOTHING (the game boots
// unpatched), so success is still only ever recorded by an install the table actually verified.
namespace Ps3VerifyBackoff {

// 24h, not the firmware unit's 1h: a game update is optional — the game boots unpatched while the
// backoff holds, unlike firmware, which RPCS3 needs to boot anything at all — each attempt costs
// hundreds of megabytes, and the failure class this bounds is persistent by nature, so a retry an
// hour later almost never carries new information.
inline constexpr qint64 kRetryBackoffSecs = 24 * 3600;

// Where the marker for one title lives. titleId is already gated upstream by Ps3TitleId to a plain
// identifier (no separators, no traversal), so it is as safe a path component here as it is in the
// neighbouring ps3-heal-<titleId> marker.
QString markerPath(const QString& tmpDir, const QString& titleId);

// Arms the backoff for this title. Re-recording refreshes the timestamp, so a failure that keeps
// recurring keeps costing one chain per window rather than accumulating.
void record(const QString& tmpDir, const QString& titleId);

// True while a recorded failure still suppresses attempts. A future-dated marker — clock skew, a
// restored backup, a bad filesystem timestamp — reads STALE, not fresh, or updates stay suppressed
// until the wall clock catches up to the stamp. `now` is injectable so the window is testable
// without sleeping.
bool inBackoff(const QString& tmpDir, const QString& titleId,
               const QDateTime& now = QDateTime::currentDateTimeUtc());

} // namespace Ps3VerifyBackoff
