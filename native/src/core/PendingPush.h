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
//
// 5. NO TIMER UPLOADS AN OPEN SETTINGS VISIT (review round 1). SettingsTxn is snapshot-and-restore: an edit
//    writes THROUGH to the ini immediately and the snapshot is what a Discard puts back. So while a visit is
//    open, the bytes on disk — and therefore CloudSync's fingerprint, and therefore checkStatus's
//    localChanged — are a state the user has NOT confirmed and may be about to reject. A timer firing in that
//    window would upload it, and on the PullThenPush arm it would also take the Discard away entirely, because
//    CloudSync::applySettingsJson force-commits an open transaction. (That force-commit is a deliberate trade
//    against a PEER's bundle arriving mid-visit; nothing about it licenses this device's own timer to do the
//    same to itself.) due() therefore answers Deferred, and it does so BEFORE the manual override, because the
//    post-Save timer is manual: "the user pressed Save three seconds ago" is not consent to upload the
//    different, still-open edits they went back in and started making. Deferred is not a drop — the caller
//    re-arms, so the owed record survives the visit and is attempted the moment it closes. This is the same
//    invariant the push TRIGGER states ("the user never confirmed the state that resulted, so this device does
//    not upload it"), now enforced on the timers and not only on the trigger.
//
//    WHAT THIS DOES NOT COVER, deliberately: a push the user is asking for AT THIS INSTANT — the Cloud panel's
//    "Retry sync" and "Sync now". Those are unreachable except from INSIDE the settings area, so the
//    transaction is open by construction whenever they are pressed, and blocking on it would leave the user
//    pressing a button that visibly does nothing. They are the same explicit make-the-cloud-match lever "Sync
//    now" has always been, and the caller reflects that by not reporting a visit for them. The distinction the
//    flag draws is therefore not "is a transaction open" but "is this state one nobody has confirmed AND
//    nobody is asking for" — hence the parameter's name.
//
// A NOTE ON THE CLOCK. lastAttemptMs is wall clock, and wall clock moves backwards (an RTC that read ahead,
// then an NTP correction). A timestamp in the FUTURE makes dueAtMs unreachable, so a naive `now < dueAt ->
// Wait` would re-arm forever without ever attempting, with the panel cheerfully saying "retrying
// automatically" — a permanent silent stall that give-up never rescues because no attempt is ever made. due()
// treats lastAttemptMs > nowMs as DUE NOW: one un-backed-off attempt, whose outcome rewrites the stamp with
// the corrected clock, and the record is healthy again.
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
        Deferred,     // the state on disk is UNCONFIRMED (a settings visit is open) — ask again shortly
        NeedsSignIn,  // parked: the account needs re-authentication, retrying cannot fix it
        GaveUp,       // parked: kMaxAttempts consecutive failures, only a user action resumes
        Attempt       // go
    };
    // `unconfirmedEditsOpen` — a settings visit is open (SettingsTxn::active()) and no live user action stands
    // behind this attempt; see decision 5 above. A parameter rather than a call into SettingsTxn so this stays
    // a pure function of stated facts, and with no default: every caller must answer it, because the caller
    // that forgets is the one that uploads a state the user is still deciding about.
    Due due(const State& s, bool signedIn, bool manual, qint64 nowMs, bool unconfirmedEditsOpen);

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
    // Whether the OAuth token refresh could be completed, and if not, whose problem it is.
    //
    // THE DISCRIMINATOR IS THE OAuth ERROR CODE AND THE HTTP STATUS, not "the body parsed" (review round 1).
    // The earlier rule — any JSON object without an access_token is a rejection — reads Google's rate limit,
    // its 5xx, and any JSON-speaking proxy's error page as a REVOKED GRANT. That parks the device at the FIRST
    // failure with "your sign-in expired": no backoff, no automatic recovery, and a prompt the user cannot act
    // on because nothing is actually wrong with their account. Which is precisely the failure the Offline
    // direction was written to avoid, left open in the other one.
    //
    // So Expired requires POSITIVE evidence that the grant itself is dead: a 4xx carrying RFC 6749 §5.2's
    // `invalid_grant` (revoked, expired, or reused) or `invalid_client` (the client credentials no longer
    // work). Everything else backs off. The asymmetry is deliberate — a transient error mislabelled Expired
    // strands the device until a human notices, while a genuine expiry mislabelled Offline still surfaces
    // within the attempt cap as "retrying has stopped", which is visible and user-clearable.
    //
    //   haveRefreshToken — is there a stored grant to refresh at all
    //   httpStatus       — the reply's HTTP status; 0 when nothing arrived (dead network, DNS, TLS)
    //   oauthError       — the body's `error` field, empty when it carried none (or was not JSON at all)
    //   haveAccessToken  — the reply actually carried an access_token
    // The body is classified and dropped: no part of it is logged, stored, or returned.
    enum class Auth { Ok, Offline, Expired };
    Auth classifyRefresh(bool haveRefreshToken, int httpStatus, const QString& oauthError, bool haveAccessToken);

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
