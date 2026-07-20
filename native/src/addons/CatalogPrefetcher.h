// Warms the catalog-result cache in the background so opening a media menu is instant instead of showing a
// "loading…" spinner on first entry. It fires ordinary AddonManager::requestCatalog calls (page 1) for every
// enabled source × its catalogs and lets the results land in AddonManager's existing catalogCache_ via the
// manager's own catalogReady lambda — the prefetcher adds NO storage of its own; it only paces the requests.
//
// Pacing: a FIFO queue drained at most kMaxInFlight (3) requests at a time, so a warm-up never floods the JS
// worker pool or the network and never competes hard with a user's own browsing. A staggered resweep timer
// (~25 min ± jitter, scaled by MMV_PREFETCH_TTL_S via the manager's TTL) refreshes entries before they expire;
// each resweep skips anything still comfortably within the cache TTL, so a warm cache costs nothing. It also
// resweeps whenever the source set or a source's enabled state changes. Failures are logged and never retried
// within a sweep — a source that's down simply gets another chance on the next resweep.
#pragma once
#include <QObject>
#include <QHash>
#include <QQueue>
#include <QSet>
#include <QString>

class AddonManager;
struct LoadedAddon;
class QTimer;

class CatalogPrefetcher : public QObject
{
    Q_OBJECT
public:
    explicit CatalogPrefetcher(AddonManager* mgr, QObject* parent = nullptr);

    void start();    // post-first-paint kick: sweep every enabled source × catalog (page 1) into the cache
    void resweep();  // re-run the sweep (triggered by timer / source changes); skips entries still fresh

    // ---- testability seams (probe_addon) ----
    void setPeriodicResweep(bool enabled) { periodic_ = enabled; } // false = only explicit resweep() sweeps
    int  inFlight() const { return inFlight_.size(); } // requests currently outstanding (capped at kMaxInFlight)
    int  queued()   const { return queue_.size(); }    // jobs waiting for a slot
    int  issued()   const { return issued_; }          // cumulative requestCatalog calls this object has made
    bool idle()     const { return inFlight_.isEmpty() && queue_.isEmpty(); }

    static constexpr int kMaxInFlight = 3;

private:
    struct Job { LoadedAddon* src = nullptr; QString catalogId; QString key; };

    void enqueueSweep();                       // append a job per enabled source × catalog, skipping fresh/dupes
    void pump();                               // start jobs until kMaxInFlight are in flight or the queue drains
    void onCatalogReady(int reqId);            // free the slot for one of OUR reqIds and pump the next
    void armTimer();                           // (re)arm the staggered resweep timer off the manager's TTL
    QString jobKey(LoadedAddon* src, const QString& catalogId) const;

    AddonManager* mgr_ = nullptr;
    QTimer* timer_ = nullptr;
    QQueue<Job> queue_;
    QHash<int, QString> inFlight_;   // our outstanding reqId -> job key
    QSet<QString> active_;           // job keys currently queued or in flight (dedupe across resweeps)
    int issued_ = 0;
    bool periodic_ = true;
};
