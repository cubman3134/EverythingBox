#include "TrackerFanout.h"
#include "TrackerLinks.h"

using namespace tracker;

QVector<Tracker*> TrackerFanout::active(const QVector<Tracker*>& all)
{
    QVector<Tracker*> out;
    out.reserve(all.size());
    for (Tracker* t : all)
    {
        // A null is skipped rather than dereferenced: the caller's list is built out of members that may not
        // have been constructed yet on an early path.
        if (!t) continue;
        if (!t->configured() || !t->connected()) continue;   // the feature being off is not a failure
        out.push_back(t);
    }
    return out;
}

TrackerFanout::Result TrackerFanout::push(const QVector<Tracker*>& all, const QString& itemKey, Kind kind,
                                          int unit, bool completes)
{
    Result r;
    if (itemKey.isEmpty() || unit <= 0) return r;
    Q_UNUSED(kind);   // the KIND a push uses is the LINK's, not the caller's guess — see below

    for (Tracker* t : active(all))
    {
        // ITS OWN LINK. The same series is a different media id on every tracker, and the link store is
        // keyed by (Id, itemKey) precisely so nothing here has to correlate them.
        const TrackerLinks::Link link = TrackerLinks::get(t->id(), itemKey);
        if (!link.linked())
        {
            ++r.unlinked;
            if (link.declined) ++r.declined;
            else r.needLink.push_back(t);
            continue;   // NEVER `return`: one tracker being unlinked says nothing about the next one
        }
        // The app's own side of the reconciliation moves first, and monotonically: if this push cannot go
        // out for an hour, the next pull must still know we are ahead. Per tracker, because each tracker's
        // account is at its own place in the series.
        TrackerLinks::noteLocalProgress(t->id(), itemKey, unit);
        Update u;
        u.itemKey = itemKey;
        u.mediaId = link.mediaId;
        // The LINK's kind, not the caller's. A series linked as manga on one tracker and anime on the other
        // (an adaptation the user linked deliberately) has to be pushed to each as what it IS there, or the
        // write goes to the wrong endpoint and MAL answers 404.
        u.kind = link.kind;
        u.unit = unit;
        u.completes = completes;
        // Debounced and queued INSIDE the implementation, per tracker. Cheap, never blocks, and cannot
        // throw — so nothing here can stop the loop reaching the tracker after it.
        t->pushProgress(u);
        ++r.pushed;
    }
    return r;
}
