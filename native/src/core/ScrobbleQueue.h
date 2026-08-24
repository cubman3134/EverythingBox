// THE OFFLINE SCROBBLE QUEUE (issue #192) — completed listens that have not been delivered yet, and the
// rules for keeping them.
//
// WHY THIS EXISTS RATHER THAN A RETRY TIMER. A retry that holds the listen in memory loses a flight's worth of
// listening to a single app restart, and a retry that re-stamps the listen with the moment it finally
// succeeded is worse than losing it: it tells the service you listened to a whole album at 11:47pm in one
// second. BOTH services accept BACKDATED submissions precisely so a client does not have to do either. So a
// completed listen is written to disk WITH THE MOMENT IT STARTED, and that timestamp is never touched again.
//
// WHY IT IS NOT TraktMissed. That file's name suggests it — it was the first place to look — but it is not an
// offline queue at all: `TraktMissed` is the pure JOIN that decides which already-aired episodes you have not
// watched, and it holds nothing pending. The Trakt side has no offline queue to reuse; its scrobbles are
// fire-and-forget against a live connection. What IS reused is the shape of the surrounding family — a pure
// header of rules, a thin QSettings-backed store, per-profile keys through one spelling, and a change hook
// rather than a Qt signal (MissedDismiss.cpp is the closest sibling and this file is deliberately its twin).
//
// ORDERING IS FIFO AND IS PART OF THE CONTRACT. Listens go out oldest-first, because that is the order they
// happened in and because a service that de-duplicates on (artist, title, listened_at) does so most reliably
// when a batch is monotone. `head()` returns the front, `dropFront()` removes exactly what was accepted, and
// nothing reorders in between — a partial success removes a prefix and leaves the rest in place, still in
// order.
//
// IT IS BOUNDED. A device left offline for a month with music playing would otherwise write an unbounded list
// into the ini. kMaxQueued is the cap, and it drops from the FRONT (the oldest) when it overflows: the newest
// listening is the listening most likely to still be worth submitting, and a cap that dropped the newest would
// make the queue permanently useless the moment it filled. Overflow is recorded, not silent — see dropped().
//
// PER PROFILE, DEVICE-LOCAL. Two people who share the box have two histories, and this device's undelivered
// listens are not another device's to deliver (they would be submitted twice). Scrobble::stateKeyPrefix()
// owns the carve-out for both sync and the settings transaction; see the note at the bottom of Scrobble.h.
#pragma once
#include "Scrobble.h"

#include <QString>
#include <QVector>
#include <functional>

namespace ScrobbleQueue
{
    // The most listens kept on disk. Roughly a week of continuous listening at three minutes a track, which
    // is far past any plausible offline stretch and still a small ini row.
    constexpr int kMaxQueued = 3000;

    // How many go out in one submission. Both services document batch limits around this size, and a smaller
    // batch also means a rejected batch costs less: one bad row poisons at most this many good ones before
    // the split-and-retry in the orchestrator isolates it.
    constexpr int kBatchSize = 50;

    // Everything below is scoped to `providerId` (ScrobbleProvider::id) as well as the active profile: a
    // listen queued for ListenBrainz must never be handed to Last.fm, which has neither seen it nor agreed to
    // de-duplicate it.

    // Append one completed listen. No-op for a Play with no timestamp — an unstamped listen is exactly the
    // thing the backdating contract cannot express, and submitting it would land at "now".
    void append(const QString& providerId, const Scrobble::Play& play);

    // The oldest `n` (at most), in the order they happened.
    QVector<Scrobble::Play> head(const QString& providerId, int n);

    // Remove the oldest `n`. Called with exactly the count the service accepted, never with "all".
    void dropFront(const QString& providerId, int n);

    int  count(const QString& providerId);
    void clear(const QString& providerId);

    // How many listens this queue has thrown away to stay under the cap, ever, for this profile. The settings
    // surface says so: a queue that silently ate a week of listening looks identical to one that delivered it.
    int  dropped(const QString& providerId);

    // The delivered counter — the "scrobbled N tracks" confidence indicator. It lives here rather than in
    // Settings because it is the same per-profile, device-local, background-written family as the queue, and a
    // second home for it would be a second thing to keep out of the sync bundle.
    int  delivered(const QString& providerId);
    void noteDelivered(const QString& providerId, int n);

    // The last thing that went wrong, for the same status line. Empty means "nothing has failed since the last
    // success". NEVER contains a credential: everything written here comes from ScrobbleResult::message, whose
    // contract forbids echoing the request. Cleared by a success.
    QString lastError(const QString& providerId);
    void    setLastError(const QString& providerId, const QString& message);

    // Multi-device sync trigger, matching FavoritesStore/MissedDismiss: a std::function, not a Qt signal, so
    // this stays QtCore-clean and probes can leave it unset. Fired only when something actually changed.
    void setChangeHook(std::function<void()> hook);

    // ---- pure, for the probe and for the carve-outs ---------------------------------------------------
    // The ini keys this store writes, built from Scrobble::stateKeyPrefix() so the sync carve-out and the
    // settings-transaction exclusion are written in terms of the same spelling the writer uses.
    QString queueKey(const QString& profileId, const QString& providerId);
    QString counterKey(const QString& profileId, const QString& providerId);
    QString droppedKey(const QString& profileId, const QString& providerId);
    QString errorKey(const QString& profileId, const QString& providerId);

    // Serialisation, exposed so the round-trip (and the timestamp preservation that is the whole point) is
    // assertable without touching the ini.
    QByteArray encode(const QVector<Scrobble::Play>& plays);
    QVector<Scrobble::Play> decode(const QByteArray& json);

    // The cap rule, pure: given a list and one more, what is kept. Drops from the FRONT. Returns how many were
    // dropped so the caller can record it.
    int applyCap(QVector<Scrobble::Play>& plays);
}
