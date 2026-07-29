// The per-profile 4-digit passcode (issue #30): the pure core of "can this person open this profile?".
//
// WHAT THIS IS, AND WHAT IT IS NOT. This is a SOFT LOCK — the thing that keeps a sibling out of your watch
// history. It is NOT security, and no UI string may imply otherwise:
//   * the ini sits next to the executable, world-readable, and the hash is one line of it;
//   * the keyspace is 10,000, so an offline attacker recovers the code in milliseconds;
//   * the recovery routes below are DELIBERATELY openable by the profile's owner without an admin.
// Nothing genuinely sensitive is gated on this. The rate limiting exists because it is nearly free and stops
// casual guessing at the couch, not because it makes the lock strong.
//
// DISTINCT FROM THE GLOBAL PARENTAL PIN. Settings::checkParentalPin gates LEAVING a restricted profile and
// opening Settings; this gates ENTERING a profile. Different question, different value, different salt — and
// AppBrand::Legacy::kParentalPinSalt is untouched by this file, because that salt is an input to a hash
// already written to every existing user's ini (renaming it would silently break every parental lock in the
// field). This unit uses AppBrand::kProfilePasscodeSalt, which has never been written before.
//
// Everything above `attempts()` is a pure function of its arguments — no clock, no store, no Qt beyond
// QString — which is what lets probe_passcode pin the whole policy table headlessly.
#pragma once
#include <QString>

namespace ProfilePasscode
{
    // ---- Policy constants (the probe pins the behaviour these produce, not the numbers themselves) ----
    inline constexpr int    kLength         = 4;       // exactly four digits; not configurable
    inline constexpr int    kFreeAttempts   = 4;       // wrong tries allowed before the first lockout
    inline constexpr qint64 kBaseLockoutMs  = 30000;   // the 5th wrong try costs 30 s
    inline constexpr qint64 kMaxLockoutMs   = 300000;  // ...doubling per try, capped at 5 min
    inline constexpr int    kResetWaitSecs  = 60;      // the "forgot passcode" self-service wait (see below)

    // ---- Format + hashing -------------------------------------------------------------------------
    // Exactly kLength ASCII digits. Deliberately strict: a pad that can only emit digits still has to agree
    // with a stored hash produced elsewhere (a synced profile from another device, a hand-edited ini), and
    // "0012" and "12" must never be the same code.
    bool isValidFormat(const QString& code);

    // SHA-256(salt + profileId + ':' + code), hex. Returns an EMPTY string for a malformed code — an
    // unsettable code must never produce a storable hash, because an empty hash is exactly how "no passcode"
    // is represented and a caller that stored a hash of garbage would lock the profile with a code nobody
    // could type.
    //
    // profileId is IN the hash on purpose: without it, two profiles that chose the same four digits store
    // byte-identical hashes, and anyone reading the ini learns that fact for free.
    QString hash(const QString& profileId, const QString& code);

    // Constant-input comparison of `code` against a stored hash. False for an empty stored hash (no passcode
    // set is not "any code opens it") and for a malformed code.
    bool verify(const QString& storedHash, const QString& profileId, const QString& code);

    // ---- The picker gate (issue #30 part 1) -------------------------------------------------------
    // Must the "Who's using EverythingBox?" picker be shown at launch? Always-ask is the DEFAULT; the only
    // escape hatch is a single-profile install that opted into skipping — and even then a passcode on that
    // one profile wins, because skipping the picker would skip the only surface that asks for the code.
    bool mustShowPicker(int profileCount, bool skipWhenSingle, bool singleHasPasscode);

    // ---- What the entry screen offers -------------------------------------------------------------
    // LOCKOUT RECOVERY, decided here rather than left emergent (see the report for the reasoning):
    //   * a global parental PIN is set  -> that PIN is the override, and the timed reset is NOT offered.
    //     Otherwise a kid could walk around the parent's PIN by waiting a minute.
    //   * no parental PIN, ORDINARY profile -> a TIMED SELF-SERVICE RESET (kResetWaitSecs of on-screen
    //     waiting, cancellable, no data touched) removes the passcode. "Reinstall" is not a recovery story,
    //     and a recovery code the user must write down is a worse one for a lock this soft. The wait is
    //     friction, which is all this lock has ever been.
    //   * RESTRICTED (kids) profile -> the timed reset is NEVER offered, with or without a parental PIN.
    //     A self-service reset on a kids profile is a self-service way out of the kids profile: the child it
    //     is aimed at is exactly the person sitting in front of the countdown with sixty seconds to spare.
    //     Recovery for a restricted profile is the parental-PIN override, full stop. That deliberately
    //     creates an unrecoverable state for a household that sets a kid passcode with NO parental PIN, so
    //     the UI must say so at the moment the passcode is SET (MainWindow::profilePasscodeMenu warns and
    //     offers to set a PIN there and then) — the cost is accepted, being surprised by it is not.
    struct EntryOptions
    {
        bool needCode         = false;  // this profile has a passcode at all
        bool offerParentalPin = false;  // show "Use the parental PIN" on the pad
        bool offerTimedReset  = false;  // show "Forgot passcode?" on the pad
    };
    EntryOptions entryOptions(bool hasPasscode, bool parentalPinSet, bool restricted);

    // ---- Who may change a profile's `restricted` flag ---------------------------------------------
    // The flag stopped being a preference the moment entryOptions started withholding the timed reset from
    // restricted profiles: turning it OFF hands that reset back, so an un-gated toggle IS a passcode-reset
    // bypass — "enter any unlocked profile, Settings, un-restrict the kid, wait sixty seconds". The rule
    // lives HERE, next to the entryOptions rule it protects, so the two cannot drift and so both settings
    // builders ask the same question of one answer.
    //
    //   * a parental PIN exists -> the PIN, in BOTH directions. It is the household's answer to "who decides
    //     what a kids profile is", and clearing the flag is as much that question as setting it.
    //   * no PIN, turning it OFF on a profile that HAS a passcode -> THAT PROFILE'S PASSCODE. Refusing
    //     outright would be a harder lock than the one being protected (a PIN-less household could never
    //     un-restrict anything again); allowing it is the bypass. Charging the passcode makes un-restricting
    //     cost exactly what the reset it restores would have cost — so it gives a person who can already
    //     open the profile nothing new, and gives a person who cannot nothing at all.
    //   * otherwise -> free, exactly as before. No reset exists to bypass.
    enum class RestrictedGate { Free, ParentalPin, ProfilePasscode };
    RestrictedGate restrictedChangeGate(bool parentalPinSet, bool turningOn, bool hasPasscode);

    // ---- Rate limiting ----------------------------------------------------------------------------
    // Per-profile, PER-DEVICE and PERSISTED: an in-memory counter is reset by quitting the app, which is one
    // keypress away on a TV remote and would make the limit decorative.
    struct Attempts
    {
        int    fails         = 0;  // consecutive wrong tries since the last success/reset
        qint64 lockedUntilMs = 0;  // ms since epoch; 0 = not locked
    };

    bool   lockedOut(const Attempts& a, qint64 nowMs);
    qint64 lockRemainingMs(const Attempts& a, qint64 nowMs);   // 0 when not locked

    // Make a STORED state safe to act on. Pure, so the probe pins it without a clock or an ini.
    //   * a negative fail count clamps to 0 (a hand-edited one would otherwise grant unlimited free tries);
    //   * a lockedUntilMs further out than nowMs + kMaxLockoutMs clamps to nowMs + kMaxLockoutMs.
    // The second one is not paranoia about hand-editing: TV boxes routinely boot with a wildly wrong clock
    // and correct it from the network seconds later. A lockout stamped during that window is written with a
    // deadline years in the future, and it SURVIVES the correction — a permanent lock on a real user's
    // profile, produced by nothing but a clock. kMaxLockoutMs is the longest lockout this policy can ever
    // legitimately impose, so anything beyond it cannot be one.
    //
    // attempts() applies this on read AND PERSISTS the result. Clamping without persisting would be worse
    // than not clamping: every read would re-clamp to now + kMaxLockoutMs, so the deadline would walk
    // forward with the clock and the lockout would never expire at all.
    Attempts sanitized(const Attempts& raw, qint64 nowMs);

    // The escalation: the first kFreeAttempts failures cost nothing, then kBaseLockoutMs doubling per further
    // failure, capped at kMaxLockoutMs. Pure — `nowMs` is the caller's clock.
    Attempts afterFailure(const Attempts& a, qint64 nowMs);
    Attempts cleared();   // the post-success / post-reset state

    // THE composite decision, and the reason the pieces above are not enough on their own: the lockout is
    // checked BEFORE the code is compared, so a caller that hammers the pad learns nothing during a lockout —
    // not even "wrong" versus "right" — and a locked-out correct guess does not open the profile. It also
    // means the caller cannot forget to update the attempt state, because evaluate() does it in `io`.
    enum class Outcome { Accepted, Rejected, LockedOut };
    Outcome evaluate(const QString& storedHash, const QString& profileId, const QString& code,
                     Attempts& io, qint64 nowMs);

    // ---- Attempt-state persistence (device-local) -------------------------------------------------
    // Stored under "profilepass/<profileId>/..." — carved out of CloudSync (a lockout on the living-room TV
    // must not follow you to the phone) and out of SettingsTxn (a settings Discard must not clear a lockout).
    // The HASH itself is not here: it lives in the profile record (ProfileStore), which SYNCS, so setting a
    // passcode once covers every device.
    //
    // attempts() returns sanitized() state and writes the clamp back when it changed anything — see above.
    Attempts attempts(const QString& profileId);
    void     setAttempts(const QString& profileId, const Attempts& a);
    void     clearAttempts(const QString& profileId);

    // The ini group this unit owns. INLINE (header-only) on purpose: CloudSync's device-local carve-out and
    // SettingsTxn's scope predicate both have to know it, and both are deliberately lean translation units
    // whose probes link a hand-picked source list. Header-only means they can share the ONE definition
    // without either of them — or their probes — growing a link dependency on this .cpp.
    inline constexpr const char* kAttemptKeyPrefix = "profilepass/";
    inline bool isAttemptKey(const QString& key)
    {
        return key.startsWith(QLatin1String(kAttemptKeyPrefix));
    }

#ifdef EB_PROFILEPASSCODE_TEST_SEAM
    // Probe-only ini redirect (the EB_THEMECHOICE_TEST_SEAM / EB_SETTINGSTXN_TEST_SEAM idiom). Compiled ONLY
    // for probe_passcode: without the macro the setter is not declared, so a production call is a compile
    // error rather than a silent process-wide redirect.
    void setIniPathForTesting(const QString& path);
#endif
}
