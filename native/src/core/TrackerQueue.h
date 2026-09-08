// THE ONE OFFLINE QUEUE, SHARED BY EVERY TRACKER (issue #156, increment 2).
//
// Increment 1 put the queue rules in TrackerRules (coalesce, the cap, the debounce, the encoding) and their
// PERSISTENCE inside AniListTracker as a handful of statics. Increment 2 needs the same persistence for
// MyAnimeList, and Tracker.h is explicit that there is not to be a second queue — so the statics moved here
// and became Id-parameterised, and AniListTracker now calls them instead of owning them.
//
// NOTHING ABOUT THE BEHAVIOUR CHANGED. Every key is still tracker::queueKey / lastSentKey / lastErrorKey,
// still per profile, still per tracker; every decision is still the TrackerRules function it was. What
// changed is that one implementation of it exists rather than one per provider, which is the difference
// between two trackers sharing a queue and two trackers each having one.
//
// PER TRACKER, THOUGH — not per app. A user with AniList and MyAnimeList both connected has two queues, one
// each, keyed by tracker::Id, because a chapter accepted by one and refused by the other has to stay pending
// on exactly the one that refused it. That is the same reason ScrobbleQueue is per provider.
//
// DEVICE-LOCAL. Everything here is under tracker::stateKeyPrefix(), which CloudSync excludes: merging two
// devices' pending queues would deliver the same progress twice.
#pragma once
#include "Tracker.h"

#include <QString>
#include <QVector>

namespace TrackerQueue
{
    // The pending updates for `id` on the current profile. TOTAL: a malformed row is skipped and a
    // malformed document reads back empty, so a corrupted ini costs the queue and not the launch.
    QVector<tracker::Update> load(tracker::Id id);
    void save(tracker::Id id, const QVector<tracker::Update>& q);
    int  count(tracker::Id id);

    // Coalesce `u` in (furthest wins, per item), apply the cap, and write. Returns true when the queue
    // really changed — false means an earlier unit arrived late and nothing was written, which is what
    // stops a page-turn storm rewriting the ini once per page.
    bool enqueue(tracker::Id id, const tracker::Update& u);

    // The index of the first row whose ITEM is outside its debounce window, or -1 when every row is still
    // inside one. On -1, `waitMsOut` (when given) receives the shortest wait until one opens, so a caller
    // can wake exactly then rather than polling.
    int nextSendable(const QVector<tracker::Update>& q, tracker::Id id, qint64 nowMs, qint64* waitMsOut);

    // Drop the row `u` delivered, BY IDENTITY and never by index: the queue is re-read after a request, and
    // a page turn during it may have coalesced a FURTHER update onto the same item. Only a row at or below
    // `u.unit` is removed, so that further update survives.
    void removeDelivered(tracker::Id id, const tracker::Update& u);

    // When the last accepted push for `itemKey` went out, and the stamp for one that just did.
    qint64 lastSentMs(tracker::Id id, const QString& itemKey);
    void   noteSent(tracker::Id id, const QString& itemKey, qint64 whenMs);

    // The user-facing line for the settings status. NEVER a credential and never a request — see
    // AniListTracker.h and MyAnimeListTracker.h. Empty clears it.
    QString lastError(tracker::Id id);
    void    setLastError(tracker::Id id, const QString& message);

    // Disconnecting an account drops that account's pending progress and its error line. The next account
    // has not agreed to receive what this one queued. The per-item LINKS are deliberately kept — they
    // describe the media, not the account.
    void forgetAccount(tracker::Id id);
}
