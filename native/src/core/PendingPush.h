// The durable "this device owes the cloud a settings push" record, and the pure policy that decides when to
// retry it and when to stop (issue #34).
//
// WHY THIS IS A SEPARATE MODULE. Everything here is a decision, not an action: it takes facts (is a push owed,
// how many attempts have failed, what time is it, what did checkStatus say) and returns a verdict. No network,
// no CloudSync, no UI — QtCore only, so probe_cloudmerge exercises the whole policy without a socket and
// without touching a real Drive account. MainWindow owns the timers and the callbacks; this owns the answers.
//
// ---- The four decisions, stated -------------------------------------------------------------------------
//
// 1. WHERE THE INTENT LIVES: in the settings ini itself, under `device/push/`, and it stores NO PAYLOAD.
//    * Durable across a crash and a restart because it is an ordinary QSettings write.
//    * `device/` is already carved out of BOTH sync (CloudSync::isDeviceLocalKey) and the settings transaction
//      (SettingsTxn::inScope), which is exactly what this record needs: "this device owes a push" is
//      meaningless on another machine, and a Discard must never resurrect a pending state the user cannot see.
//      Note the sub-GROUP: `device/push/x`, never a flat `device/pushX`. probe_cloudmerge §1 asserts the only
//      direct child KEY of `device` is `id`, and a flat key would break it.
//    * It is deliberately NOT a second source of truth. The authoritative question — "does the cloud differ
//      from what is on this disk?" — is already answered by CloudSync::stateFingerprint() vs cloud/syncedHash,
//      i.e. checkStatus's localChanged. This record only says "a retry is DUE"; resolve() below then asks the
//      real question, and NothingToSend is the answer whenever the two agree. So the record cannot disagree
//      with the settings: at worst it schedules a retry that turns out to be a no-op. Losing the record
//      entirely degrades to the pre-#34 behaviour (the exit push catches it), never to a wrong upload.
//
// 2. WHAT IS RETRIED: the CURRENT settings at retry time, never a snapshot taken at Save time. The bundle is
//    a WHOLE-DOCUMENT upload, so re-pushing a stale snapshot is not "retrying the failed push" — it is
//    reverting every change made since, including changes made while offline. And a snapshot would be a
//    payload (a multi-megabyte zip of settings, addons and themes) that would have to be persisted, which is
//    precisely the second source of truth decision 1 avoids. THE COST, stated plainly: the retry is not of the
//    thing that failed. If the user edits a setting, goes offline, edits it again and the retry then succeeds,
//    the intermediate value never reaches the cloud. That intermediate state is not recoverable and is not
//    meant to be — the bundle has never carried history.
//
// 3. WHEN IT STOPS: bounded, backed off, and never silent.
//    * Exponential backoff from kBaseDelayMs, doubling per consecutive failure, capped at kMaxDelayMs. A
//      handheld that is offline for a day makes ~10 attempts, not thousands (the retry-storm warning).
//    * After kMaxAttempts consecutive failures the record reads gaveUp() and NOTHING automatic runs again.
//      Only a user action restarts it — the Cloud Sync panel's Retry, a manual Sync now, or a fresh sign-in.
//      gaveUp() is DERIVED from the attempt count, not stored, so there is no flag that can disagree with it.
//    * Sign-in expiry is parked IMMEDIATELY rather than backed off, because it needs a human and no amount of
//      patience fixes it (the "retrying forever that never resolves" the issue calls out). due() reports
//      NeedsSignIn, which the panel renders as a sign-in prompt, not as "retrying".
//    * Both parked states are surfaced in the Cloud Sync panel (themed AND classic). Give-up is a thing the
//      user is told, not a thing that quietly happens.
//
// 4. CONFLICT: a peer may have pushed while this device was offline, so the retry is checkStatus-FIRST and
//    never a blind upload. resolve() maps the status onto a plan:
//      * !localChanged            -> NothingToSend. We owe nothing; a peer's change is the pull chain's job.
//      * localChanged, remote same -> Push.
//      * localChanged AND remoteChanged -> PullThenPush: take the peer's bundle FIRST (applyRemote, which
//        already holds off device-local and per-item keys), then push what is left.
//    WHY IT CANNOT OSCILLATE. The failure this shape is written against is the #58 one: a VALUE-LEVEL
//    comparator where two sides' repairs each lose to the other's bytes forever. There is no comparator here.
//    applyRemote sets cloud/syncedHash to the remote's own hash, so after a pull this device's baseline IS the
//    bytes on Drive; the following push then makes Drive equal this device. Either way the next checkStatus
//    reports localChanged == false and the very next resolve() returns NothingToSend. The fixed point is
//    reached in ONE round and the loop cannot re-enter it: a retry attempt performs at most one PullThenPush
//    and never re-arms from its own completion — the next attempt is a fresh timer tick that must first pass
//    due(). The only way to keep pushing is for the local settings to keep genuinely changing, which is the
//    user editing them.
#pragma once
#include <QString>
#include <QtGlobal>

namespace PendingPush
{
    // Consecutive automatic failures before the retry parks itself. ~74 minutes of wall clock with the
    // backoff below — long enough to ride out a router reboot, short enough that a permanently-failing
    // account stops costing battery and Drive quota the same day.
    constexpr int    kMaxAttempts = 8;
    constexpr qint64 kBaseDelayMs = 30LL * 1000;        // first retry, 30 s after the failure
    constexpr qint64 kMaxDelayMs  = 30LL * 60 * 1000;   // ceiling, 30 min

    // Why the last attempt failed. None is the ONLY value valid with attempts == 0.
    enum class Failure { None, Offline, AuthExpired };

    // What an attempt did. Deliberately three values and not four: a plain Drive error and a dead network are
    // indistinguishable from the caller's side AND want identical treatment (back off and try again), so
    // giving them separate enumerators would be a distinction with no behaviour behind it.
    enum class Outcome { Success, Offline, AuthExpired };

    // The whole durable record. No payload, no settings value, no credential — three scalars.
    struct State
    {
        int     attempts      = 0;   // consecutive failures; 0 means nothing is owed
        qint64  lastAttemptMs = 0;   // epoch ms of the last failed attempt
        Failure failure       = Failure::None;
    };

    // A push is owed iff an attempt has failed and none has succeeded since. Derived, not stored: an `owed`
    // flag alongside a counter is two facts that can disagree, and the counter already carries the answer.
    inline bool owed(const State& s)   { return s.attempts > 0; }
    inline bool gaveUp(const State& s) { return s.attempts >= kMaxAttempts; }

    // Backoff for the NEXT attempt after `attempts` consecutive failures. 0 for attempts <= 0 (nothing owed
    // is due immediately), else kBaseDelayMs doubled per failure, clamped to kMaxDelayMs.
    qint64 backoffMs(int attempts);
    // Epoch ms at which the next automatic attempt becomes due.
    qint64 dueAtMs(const State& s);

    // Is an automatic (or user-forced) attempt appropriate right now?
    enum class Due
    {
        Nothing,      // signed out, or nothing owed — do not touch the network
        Wait,         // owed, but the backoff window has not elapsed
        NeedsSignIn,  // parked: the account needs re-authentication, retrying cannot fix it
        GaveUp,       // parked: kMaxAttempts consecutive failures, only a user action resumes
        Attempt       // go
    };
    Due due(const State& s, bool signedIn, bool manual, qint64 nowMs);

    // Given a resolved CloudSync::Status, what should the attempt actually do? See decision 4 above.
    enum class Plan
    {
        Unreachable,    // Drive did not answer (or the bundle query failed) — "absent" is UNPROVEN, do nothing
        NothingToSend,  // local matches the synced baseline — the idempotent no-op the issue asks for
        Push,
        PullThenPush    // a peer moved the remote AND we have local edits
    };
    Plan resolve(bool reached, bool listReached, bool localChanged, bool remoteChanged);

    // The state transition. Pure: takes the record before the attempt, returns the record after it.
    State onOutcome(const State& before, Outcome o, qint64 nowMs);

    // ---- failure classification ---------------------------------------------------------------------
    // Whether the OAuth token refresh could be completed, and if not, whose problem it is. `serverAnswered`
    // is "the token endpoint replied at all" and `serverRejected` is "it replied, and the reply was an error
    // rather than an access token" — Google answers a revoked or expired grant with an HTTP 400 body, and a
    // dead network with no body at all, so those two facts separate "needs a human" from "needs patience".
    enum class Auth { Ok, Offline, Expired };
    Auth classifyRefresh(bool haveRefreshToken, bool serverAnswered, bool serverRejected);

    // Map a push result onto an Outcome. A failure is AuthExpired ONLY when the token layer said so; every
    // other failure is retryable.
    Outcome classifyPush(bool ok, Auth a);

    // ---- durable storage (device/push/*) ------------------------------------------------------------
    // Reads/writes the shared portable ini directly, the same posture as every other core store.
    State load();
    void  save(const State& s);
    inline void clear() { save(State{}); }

    // The ini keys, exposed so the probe can assert the record's SHAPE (exactly these, all scalars, no value
    // ever copied out of a settings row) rather than trusting the implementation.
    QString keyAttempts();
    QString keyLastAttempt();
    QString keyFailure();
}
