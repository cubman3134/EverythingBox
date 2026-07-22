// Per-profile consumption stats — the generalization of PlayStats (which owns GAME playtime) to the media
// and reading tracks that accrued NOTHING before: seconds watched/listened and pages read, per title and per
// category, per profile. Fed at the app's EXISTING seams — the PlaybackSession persistResume ≥5s heartbeat
// (forward-only Δposition, clamped) and the reader page-turn edges (PdfView/ComicView/EbookView) — so there
// are no new timers. Games are NOT migrated here; the Stats panel reads their hours from PlayStats at display.
//
// Backed by the portable mymediavault.ini (same AppPaths::dataDir() posture as PlayStats/ItemMarks —
// QtCore only, no Quick/Widgets), all namespaced by the active profile id (or "default"):
//   stats/<profile>/items/<hash>  -> JSON { mediaSeconds, pagesRead, lastActivity, title }  (one blob/title)
//   stats/<profile>/cat/<cat>/{seconds|pages}  -> per-category rollups (video|audio|reading), kept at accrual
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

    Totals  get(const QString& key);                 // cached; empty/unknown key -> default {}
    qint64  categorySeconds(const QString& category); // "video" | "audio" rollup
    qint64  categoryPages();                          // "reading" rollup (all readers)

    // Top-N titles for a category ("reading" | "video" | "audio"), sorted by the relevant metric (pagesRead for
    // reading, mediaSeconds for video/audio), descending. Returns <naturalKeyIsHashed?no — the HASHED key>,Totals.
    QVector<QPair<QString, Totals>> topTitles(const QString& category, int n);

    void invalidate(); // drop the cache (profile switch / external ini change)
}
