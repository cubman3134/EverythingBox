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
//
// ---------------------------------------------------------------------------------------------------------
// ISSUE #326 WIDENED IT from "the queue" to "everything two trackers were each keeping a copy of": the
// CREDENTIAL store and the DRAIN LOOP joined the persistence that moved here in increment 2. The reason is
// the one increment 2 gave and then only half-applied — a second provider had already made one copy of this
// into two, and Kitsu would have made it three. What is here now is:
//
//   * the queue itself (increment 2, unchanged);
//   * the ini access for a tracker's client id, secret and OAuth tokens, Id-parameterised, so the two
//     `static QSettings& store()` singletons and their ten near-identical accessors became one each;
//   * Sender — THE drain loop, and with it the ONE answer to "what does a failed push mean?".
//
// STILL Qt6::Core AND NO SOCKET. Sender takes its transport as a std::function, so the provider keeps its
// QNetworkAccessManager and probe_tracker can drive the whole loop from fixtures with nothing listening.
// That is what makes the head-of-queue unwedge assertable at all.
#pragma once
#include "Tracker.h"
#include "TrackerRules.h"

#include <QString>
#include <QVector>
#include <functional>

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

    // ---- the credential store, once, keyed by tracker::Id (issue #326) ---------------------------------
    //
    // The five keys are Tracker.h's (clientIdKey / clientSecretKey / accessKey / refreshKey / expiryKey), so
    // the sync carve-out and the settings transaction still classify them without knowing this file exists.
    // What moved here is only the ini ACCESS, which both trackers had written out identically.
    //
    // THESE ARE CREDENTIALS. Nothing here logs, and nothing here is ever put in a diagnostic message: a
    // tracker's last-error line is a sentence of our own (see TrackerQueue::setLastError above and the two
    // tracker headers). The #81 zero-config follow-up still lands in AniListTracker::clientId() /
    // MyAnimeListTracker::clientId(), which stay the one place "the client id" MEANS anything.
    QString clientId(tracker::Id id);
    QString clientSecret(tracker::Id id);
    void    setClientId(tracker::Id id, const QString& v);       // trimmed, then written
    void    setClientSecret(tracker::Id id, const QString& v);

    QString accessToken(tracker::Id id);
    bool    hasAccessToken(tracker::Id id);
    QString refreshToken(tracker::Id id);

    // Is the stored access token good for at least `skewSec` more seconds at `nowSec`? A stored expiry of 0
    // means UNKNOWN and answers TRUE: both services issue long-lived tokens and may omit expires_in, and
    // treating an unknown expiry as expired would refresh on every request against a token that is fine.
    bool tokenFresh(tracker::Id id, qint64 nowSec, qint64 skewSec);

    // Write a successful token reply. A reply that omits the refresh token leaves the stored one ALONE
    // rather than blanking it — blanking unlinks the account at the next expiry. `expiresInSec <= 0` stores
    // 0, i.e. "unknown", which tokenFresh reads as valid.
    void storeTokens(tracker::Id id, const QString& access, const QString& refresh,
                     qint64 expiresInSec, qint64 nowSec);
    // Forget the OAuth artefacts only. The typed client id/secret stay: the user pasted them, and a
    // disconnect is not a request to make them type them again.
    void clearTokens(tracker::Id id);

    // ---- THE DRAIN LOOP, once (issue #326) -------------------------------------------------------------
    //
    // Both trackers had written this out: read the queue, stop if it is empty, find the first row outside
    // its item's debounce window, send it, and on the answer either remove-and-continue or wait. The bodies
    // agreed line for line except in ONE decision — what a failure means — and that was the bug: MAL
    // classified, AniList retried everything, so one permanently-refused row at the head of AniList's
    // ordered queue blocked every update behind it for ever.
    //
    // The classification is now tracker::classifySend, asked with the provider's own SendPolicy, so there is
    // exactly one of it and both trackers get the same one.

    // What one send attempt answered with. `accepted` is the PROVIDER'S OWN test, not a status range:
    // AniList answers a refused mutation with HTTP 200 and a GraphQL error object.
    struct Reply
    {
        int    status = 0;           // HTTP status; 0 = there was no HTTP answer at all
        qint64 retryAfterSec = 0;    // Retry-After in seconds; 0 = absent, or the HTTP-date form
        bool   accepted = false;     // the write really landed
    };

    // Everything a provider supplies for its own queue to drain. Nothing here is optional except the
    // notifications: a spec with no `send` never sends, which is the correct behaviour for a tracker that
    // has not been wired up rather than a crash.
    struct SendSpec
    {
        tracker::Id         id = tracker::Id::AniList;
        tracker::SendPolicy policy;

        // configured() && connected(). Asked afresh on every drain, because both can change under us.
        std::function<bool()> ready;
        // The transport. Answers ASYNCHRONOUSLY in the app (a QNetworkReply) and synchronously in a probe;
        // Sender is written so that either is correct.
        std::function<void(const tracker::Update&, std::function<void(Reply)>)> send;

        // The three status-line sentences, supplied by the provider because they name it. NEVER built from
        // a request or a response body — see the credential rule in both tracker headers. The drop one is a
        // FUNCTION of the row because it has to say WHICH update was thrown away: a status line that admits
        // only that "an update" was dropped tells the user nothing they can act on.
        std::function<QString(const tracker::Update&)> droppedMessage;
        QString reauthMessage;
        QString retryMessage;

        std::function<void(const QString& itemKey, int unit)> pushed;   // one update was accepted
        std::function<void()> changed;                                  // the queue or the error line moved
        // Arm the provider's retry timer for `delayMs`, or STOP it when delayMs <= 0. A callback rather
        // than a QTimer owned here, so the timer keeps living and dying with the tracker QObject.
        std::function<void(int delayMs)> wait;
        // The clock, injectable so a probe can drive the debounce without sleeping. Defaults to the wall
        // clock in the constructor when left empty.
        std::function<qint64()> now;
    };

    class Sender
    {
    public:
        explicit Sender(SendSpec spec);

        // Send the first queued row whose item passes the debounce, then keep going. Re-entrant-safe
        // through `sending_`, exactly as both hand-written loops were.
        void drain();

        // Disconnecting an account forgets the consecutive-failure count with it: a fresh link is exactly
        // the moment it is worth trying again immediately.
        void reset();

        // For the settings surface and for a probe. `failures` is the consecutive-failure count the backoff
        // is doubling on; `lastWaitMs` is what the last arm asked for (0 = the timer was stopped).
        int    failures() const { return failures_; }
        qint64 lastWaitMs() const { return lastWaitMs_; }

    private:
        void arm(qint64 delayMs);

        SendSpec spec_;
        bool     sending_ = false;
        // In memory, not on disk: a restart is exactly the moment it is worth trying again immediately.
        int      failures_ = 0;
        qint64   lastWaitMs_ = 0;
    };
}
