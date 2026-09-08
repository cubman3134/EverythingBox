// Per-profile consumption stats — the generalization of PlayStats (which owns GAME playtime) to the media
// and reading tracks that accrued NOTHING before: seconds watched/listened and pages read, per title and per
// category, per profile. Fed at the app's EXISTING seams — the PlaybackSession persistResume ≥5s heartbeat
// (forward-only Δposition, clamped) and the reader page-turn edges (PdfView/ComicView/EbookView) — so there
// are no new timers. Games are NOT migrated here; the Stats panel reads their hours from PlayStats at display.
//
// Backed by the portable everythingbox.ini (same AppPaths::dataDir() posture as PlayStats/ItemMarks —
// QtCore only, no Quick/Widgets), all namespaced by the active profile id (or "default"):
//   stats/<profile>/<deviceId>/items/<hash>  -> JSON { mediaSeconds, pagesRead, lastActivity, title }  (one blob/title)
//   stats/<profile>/<deviceId>/cat/<cat>/{seconds|pages}  -> per-category rollups (video|audio|reading), kept at accrual
//
// Accumulators are DEVICE-NAMESPACED (mdsync T3): each device only ever WRITES its own <deviceId> namespace,
// so a multi-device sync unions namespaces verbatim and can never double-count. The readers (get/rollups/
// topTitles) roll those namespaces up PER FIELD, and which rule a field takes follows from what the field
// MEANS (issue #295):
//   * mediaSeconds - a LIFETIME TOTAL, so it SUMS. Half an hour on the box plus half an hour on the handheld
//     is an hour watched, and that is the intended answer.
//   * pagesRead    - a HIGH-WATER MARK (the furthest page ever reached), so it takes the MAXIMUM. Summed, two
//     devices forty pages into the same hundred-page book read as eighty per cent, and the reading progress
//     built on it (#134 increment 2) called Finished on a book nobody finished.
// Either way a SINGLE-DEVICE install reads exactly what the un-namespaced store used to hold, so the public
// API contract is unchanged. A one-time stamped migrate() folds pre-upgrade un-namespaced keys into this
// device's namespace. History written BEFORE the fix stays as it is: an already-summed value cannot be told
// from a legitimately large one, so the honest repair is to take the max of what each device stores and to
// leave the old numbers alone rather than guess at them.
//
// Item keys are the SAME identities the seams already carry (media resume identity, reader path keys). They are
// hashed (MD5-over-UTF8 hex — the ItemMarks/SyncOffsets lesson) BEFORE use as an ini group leaf so keys that
// differ only in empty/duplicate '/' separators — or a URL-shaped key — never alias. The store owns key
// sanitization; callers pass their natural keys. An empty key is a no-op on every writer and reads back {}.
//
// Accrual rules (enforced by the store):
//   * media  — addMediaSeconds accrues an already-clamped forward-only Δ; the SEAM computes Δ vs its own
//              lastAccruedPos_ and clamps to [0, 30] per heartbeat (a seek-forward can't dump minutes). The
//              store additionally floors any secs<=0 to a no-op, so a stale/negative Δ is junk-free.
//   * pages  — addPagesRead is HIGH-WATER: it stores the max page index ever reached per title and accrues only
//              max(0, page - storedHighWater). Re-reading a page or paging backward never accrues or decrements.
//
// Cache: a lazy per-profile QHash<hash, Totals> is built on first read and reused (the ItemMarks template) so
// get()/rollups/topTitles cost a ProfileStore::currentId() read + a cheap profile compare (the self-healing
// profile-switch check), not a full group re-scan per call. Category rollups are cached alongside. invalidate()
// drops it (wired at chooseProfile beside ItemMarks::invalidate); this store's own writers invalidate for you.
#pragma once
#include <QString>
#include <QVector>
#include <QPair>

namespace ConsumptionStats
{
    struct Totals
    {
        qint64  mediaSeconds = 0; // accrued watch/listen seconds for this title
        qint64  pagesRead    = 0; // high-water pages read for this title
        qint64  lastActivity = 0; // epoch seconds of the most recent accrual (0 = never)
        QString title;            // last title seen on accrual (so the panel needs no reverse lookup)
    };

    // Media seam: `secs` is the ALREADY-clamped forward-only Δ for one heartbeat; category is "video" or
    // "audio". secs<=0 or an empty key is a no-op. Updates the title + lastActivity + the category rollup.
    void addMediaSeconds(const QString& key, const QString& category, qint64 secs, const QString& title);

    // Reader seam: `page` is the current page index; the store keeps a per-title high-water mark and accrues
    // only max(0, page - highWater) — revisits/regressions never accrue or decrement. Empty key is a no-op.
    void addPagesRead(const QString& key, int page, const QString& title);

    // Rolled up across the device namespaces by the per-field rule above: seconds SUM, pages take the MAX.
    Totals  get(const QString& key);                 // cached; empty/unknown key -> default {}
    qint64  categorySeconds(const QString& category); // "video" | "audio" rollup (summed across devices)
    // The "reading" rollup, and the one place pages still SUM across devices - deliberately. This counter
    // accrues each device's NEW GROUND (addPagesRead adds max(0, page - highWater)), so it is a lifetime count
    // of pages turned on this account rather than a position in any one book. It feeds the Stats panel's
    // total and never a per-title fraction, so #295's early-Finished cannot reach it.
    qint64  categoryPages();                          // "reading" rollup (all readers; summed across devices)

    // One-time, stamped, idempotent migration (mdsync T3): fold the legacy un-namespaced accumulator keys
    // (stats/<profile>/items/* and stats/<profile>/cat/*) into THIS device's namespace
    // (stats/<profile>/<deviceId>/...) for EVERY profile. Guarded per profile by a stats/<profile>/schema
    // stamp (the PlaylistStore precedent), so a second call is a no-op. Call once at startup, before any
    // CloudMerge serialize; the read/write funnels also fold the current profile lazily.
    void migrate();

    // Top-N titles for a category ("reading" | "video" | "audio"), sorted by the relevant metric (pagesRead for
    // reading, mediaSeconds for video/audio), descending. Returns <naturalKeyIsHashed?no — the HASHED key>,Totals.
    QVector<QPair<QString, Totals>> topTitles(const QString& category, int n);

    void invalidate(); // drop the cache (profile switch / external ini change)
}
