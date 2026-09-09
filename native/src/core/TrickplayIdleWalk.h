// TrickplayIdleWalk — the seek-preview job's SECOND trigger (issue #302): a bounded, resumable walk of the
// local video library, run only when the machine is genuinely idle.
//
// #85 made preview strips between playbacks, so a film had one from its SECOND viewing on. This walks the
// library instead of waiting to be given a file, which turns previews into a property of the library rather
// than a reward for watching something twice. It widens #85's TRIGGER and inherits every one of #85's
// MANNERS unchanged — one file at a time, off the GUI thread, cancelled the instant anything else wants the
// machine, whole grids or nothing, the same cache key and the same size bound.
//
// THE DECISION IS NOT THIS CLASS'S. Every start, and every continuation, goes through
// TrickplayIdle::evaluate(), which is pure and is driven over its whole input space by probe_trickplay. This
// class is the plumbing around that decision: a poll cadence, a cursor, a bounded per-tick budget, and the
// cancel. It re-asks on EVERY tick, including while a walk is running, so pressing play stops the job within
// one tick rather than at the end of the film.
//
// WHAT IT DOES ON THE GUI THREAD, and why that is a short list. Per tick: read the conditions (all cheap),
// and — only if they say Go — look at up to kProbePerTick candidates, each costing a directory listing and
// one small sidecar read. Nothing is decoded, nothing is scaled and no file is opened for its content. The
// generation itself is TrickplayGen's worker thread, exactly as #85 left it.
//
// WHERE IT PICKS UP. The cursor is the last path completed, kept under "previews/idleCursor" — the same
// device-local prefix as the cache bound (#301), which is right: a walk of THIS machine's library is not a
// fact about the account. Interrupted after the third of ten files, it continues at the fourth. Reaching the
// end clears the cursor, so the next idle period starts at the top and picks up whatever has been added
// since — which costs nothing on an already-swept library, because every file answers Complete without a
// decoder ever being created.
//
// WHAT IT DOES NOT DO. It never evicts. If the cache is at its bound the walk STOPS; making room for a film
// nobody has asked for by deleting the strip of one somebody watched last night is the cache working against
// its owner. Eviction stays exactly where #85 put it: on the path where the user IS watching something.
#pragma once
#include <QObject>
#include <QString>
#include <QStringList>

#include "TrickplayIdle.h"

#include <functional>

class QTimer;
class TrickplayGen;

class TrickplayIdleWalk : public QObject
{
    Q_OBJECT
public:
    // How many candidates one tick may look at before giving the GUI thread back. A whole library is not
    // examined in one go: a 2000-file collection that is already swept would otherwise be 2000 sidecar reads
    // on the GUI thread every tick, which is precisely the kind of impoliteness this feature is trying not
    // to be. The cursor advances over the ones it did look at, so the next tick continues rather than
    // restarting.
    static constexpr int kProbePerTick = 8;
    // The poll cadence. Long enough to be free, short enough that pressing play stops a running walk while
    // the player is still opening its file. It is a CADENCE and never the decision — see TrickplayIdle.h.
    static constexpr int kPollMs = 5000;
    // How many ticks a "the cache is at its bound" belief survives before the worker is allowed to measure
    // again. Five minutes: long enough that a genuinely full cache is not re-walked, short enough that space
    // freed by #85's eviction is noticed within one sitting. See tick() for why the belief must expire.
    static constexpr int kFullRetryTicks = 60;

    TrickplayIdleWalk(TrickplayGen* gen, QObject* parent = nullptr);

    // Everything the walk needs from the app, injected rather than reached for, so this class knows nothing
    // about MainWindow. `conditions` is polled every tick; `library` is asked only when a tick is about to
    // look for work, and must return absolute local paths (any order — this sorts them).
    void setConditionsProvider(std::function<TrickplayIdle::Conditions()> fn);
    void setLibraryProvider(std::function<QStringList()> fn);

    void start();   // begin polling (no-op if already started)

    // The last verdict this walk reached, for a log line and for the uitest state snapshot. Never shown to
    // the user: a background courtesy has nothing to say to anybody.
    TrickplayIdle::Verdict lastVerdict() const { return last_; }
    QString cursor() const;

private slots:
    void tick();

private:
    void stopWork(TrickplayIdle::Verdict why);
    void setCursor(const QString& path);

    TrickplayGen* gen_   = nullptr;
    QTimer*       timer_ = nullptr;
    std::function<TrickplayIdle::Conditions()> conditions_;
    std::function<QStringList()>               library_;
    QStringList   sorted_;         // this sweep's snapshot of the library, in walk order
    bool          working_ = false;  // an item of OURS is with the generator
    int           sinceMeasure_ = 0; // ticks since the cache-size belief was last allowed to expire
    TrickplayIdle::Verdict last_ = TrickplayIdle::Verdict::IdleOff;
};
