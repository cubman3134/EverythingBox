// Headless test for the per-profile passcode core (issue #30): format, hashing + salt separation from the
// global parental PIN, the always-ask-at-launch decision, the recovery-route rule, the rate-limit escalation,
// the evaluate() composite, and the device-local attempt-state persistence.
//
// NOTHING IN HERE PRINTS A PASSCODE. The CHECK macro stringifies its condition, so every code is held in a
// NAMED constant and never written as a literal inside an assertion — a failing line prints "kCodeA", not
// four digits. The same rule applies to the app: no passcode, PIN or hash is ever logged.
//
// Every persistence case runs against its OWN scratch ini (eb-probe-passcode-<n>.ini): QSettings caches a
// QConfFile per path process-wide, so re-pointing at the same name after deleting the file leaves the
// previous case's keys alive in memory and the cases bleed into each other. Each case asserts up front that
// a key an earlier case wrote is absent — that assertion IS the independence proof.
//
// Prints PASSCODE-OK on success; any failure prints PASSCODE-FAIL <cond> and exits non-zero.
#include "ProfilePasscode.h"
#include "AppBrand.h"   // both salts, compared as constants (see §2) — never as re-typed literals

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QString>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "PASSCODE-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

using namespace ProfilePasscode;

// Named so no assertion ever stringifies the digits themselves.
static const QString kCodeA      = QStringLiteral("4821");
static const QString kCodeB      = QStringLiteral("4822");
static const QString kCodeZeros  = QStringLiteral("0012");
static const QString kCodeShort  = QStringLiteral("12");
static const QString kCodeLong   = QStringLiteral("48210");
static const QString kCodeAlpha  = QStringLiteral("48a1");
static const QString kCodeSpace  = QStringLiteral("48 1");
static const QString kCodeArabic = QStringLiteral("٤٨٢١");   // Arabic-Indic digits: isDigit() would accept these

static const QString kIdA = QStringLiteral("profile-aaaa");
static const QString kIdB = QStringLiteral("profile-bbbb");

static int g_case = 0;
static QString iniPath() { return QDir::tempPath() + QStringLiteral("/eb-probe-passcode-%1.ini").arg(g_case); }
static void freshIni()
{
    ++g_case;
    QFile::remove(iniPath());
    setIniPathForTesting(iniPath());
}
static bool iniHas(const QString& key)
{
    QSettings s(iniPath(), QSettings::IniFormat);
    return s.contains(key);
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ---- 1. Format ---------------------------------------------------------------------------------
    CHECK(isValidFormat(kCodeA));
    CHECK(isValidFormat(kCodeZeros));                 // leading zeros are a code, not a number
    CHECK(!isValidFormat(kCodeShort));
    CHECK(!isValidFormat(kCodeLong));
    CHECK(!isValidFormat(QString()));
    CHECK(!isValidFormat(QStringLiteral("")));
    CHECK(!isValidFormat(kCodeAlpha));
    CHECK(!isValidFormat(kCodeSpace));
    // The reason isValidFormat hand-rolls the range check instead of calling QChar::isDigit(): isDigit() is
    // true for every Unicode decimal digit, so an Arabic-Indic string would be "valid", hash to something no
    // pad on earth can reproduce, and lock the profile permanently.
    CHECK(!isValidFormat(kCodeArabic));

    // ---- 2. Hashing --------------------------------------------------------------------------------
    const QString hA  = hash(kIdA, kCodeA);
    const QString hA2 = hash(kIdA, kCodeA);
    const QString hB  = hash(kIdA, kCodeB);
    const QString hOther = hash(kIdB, kCodeA);
    CHECK(!hA.isEmpty());
    CHECK(hA.size() == 64);                            // SHA-256, hex
    CHECK(hA == hA2);                                  // deterministic
    CHECK(hA != hB);                                   // different code -> different hash
    // The profile id is IN the hash: two profiles that pick the same four digits must not store identical
    // hashes, or the ini leaks "these two profiles share a passcode" to anyone who looks at it.
    CHECK(hA != hOther);
    CHECK(!hA.contains(kCodeA));                       // the code never survives into the stored value
    // A malformed code yields NO hash. An empty hash is exactly how "no passcode" is represented, so minting
    // one from garbage would either store an unopenable lock or (worse) read back as "unlocked".
    CHECK(hash(kIdA, kCodeShort).isEmpty());
    CHECK(hash(kIdA, kCodeAlpha).isEmpty());
    CHECK(hash(kIdA, QString()).isEmpty());

    // SALT SEPARATION — the constraint the whole feature is built around, asserted ON THE CONSTANTS.
    //
    // The obvious version of this test does not work, and finding that out is the point: comparing this
    // scheme's hash against a recomputed parental-PIN hash of the same digits PASSES even when the two salts
    // are made identical, because the profile id sits in the passcode input and the parental input has none.
    // Verified by mutation — pointing kProfilePasscodeSalt at the parental salt left that assertion green. An
    // assertion that survives the bug it is named after is not coverage, so the real property is pinned
    // directly: the two constants must be different strings, and this one must still be the value it shipped
    // as. Both are DATA, not names. The parental salt is an input to a hash in every existing user's ini
    // (renaming it makes no PIN match again); this one is an input to a hash in the SYNCED profile record
    // (renaming it makes no passcode match again, on every device at once).
    {
        const QByteArray mine     = QByteArray(AppBrand::kProfilePasscodeSalt);
        const QByteArray parental = QByteArray(AppBrand::Legacy::kParentalPinSalt);
        CHECK(mine != parental);
        CHECK(mine == QByteArray("eb-profile-pass:"));
        CHECK(!mine.isEmpty());
        // ...and the hashes differ too, which is the consequence users actually feel.
        const QString parentalHash = QString::fromLatin1(
            QCryptographicHash::hash(parental + kCodeA.toUtf8(), QCryptographicHash::Sha256).toHex());
        CHECK(hA != parentalHash);
        CHECK(hOther != parentalHash);
    }

    // ---- 3. verify ---------------------------------------------------------------------------------
    CHECK(verify(hA, kIdA, kCodeA));
    CHECK(!verify(hA, kIdA, kCodeB));
    CHECK(!verify(hA, kIdB, kCodeA));                  // right code, wrong profile
    CHECK(!verify(QString(), kIdA, kCodeA));           // no passcode set is NOT "any code opens it"
    CHECK(!verify(QStringLiteral(""), kIdA, kCodeA));
    CHECK(!verify(hA, kIdA, kCodeShort));
    CHECK(!verify(hA, kIdA, QString()));

    // ---- 4. mustShowPicker (issue #30 part 1) ------------------------------------------------------
    // Zero profiles: always (there is a profile to create).
    CHECK(mustShowPicker(0, false, false));
    CHECK(mustShowPicker(0, true, false));
    // Exactly one: the ONLY case the preference speaks for. Default (skip=false) asks — this is the headline
    // behaviour change; the old code was `profiles.size() != 1` and returned false here.
    CHECK(mustShowPicker(1, false, false));
    CHECK(!mustShowPicker(1, true, false));            // opted out, no passcode -> jump straight in
    // ...and a passcode on that one profile OVERRIDES the opt-out. Skipping the picker would skip the only
    // surface that asks for the code, i.e. silently disable the lock the user just set.
    CHECK(mustShowPicker(1, true, true));
    CHECK(mustShowPicker(1, false, true));
    // Two or more: always, preference or not.
    CHECK(mustShowPicker(2, true, false));
    CHECK(mustShowPicker(2, true, true));
    CHECK(mustShowPicker(5, true, false));

    // ---- 5. entryOptions: the recovery-route rule --------------------------------------------------
    {
        const EntryOptions none = entryOptions(/*hasPasscode*/ false, /*parentalPinSet*/ false,
                                               /*restricted*/ false);
        CHECK(!none.needCode && !none.offerParentalPin && !none.offerTimedReset);
        const EntryOptions noneWithPin = entryOptions(false, true, false);
        CHECK(!noneWithPin.needCode && !noneWithPin.offerParentalPin && !noneWithPin.offerTimedReset);
        // With a parental PIN set, THAT is the override and the timed reset is withheld — otherwise a kid
        // walks around the parent's PIN by waiting out a countdown.
        const EntryOptions withPin = entryOptions(true, true, false);
        CHECK(withPin.needCode && withPin.offerParentalPin && !withPin.offerTimedReset);
        // With no parental PIN there must still be a way out that keeps the profile's data, so the timed
        // self-service reset appears. The two routes are mutually exclusive by construction.
        const EntryOptions noPin = entryOptions(true, false, false);
        CHECK(noPin.needCode && !noPin.offerParentalPin && noPin.offerTimedReset);
        CHECK(!(noPin.offerParentalPin && noPin.offerTimedReset));
        CHECK(!(withPin.offerParentalPin && withPin.offerTimedReset));

        // A RESTRICTED (kids) profile is NEVER self-service resettable — the user's decision, fix round
        // finding 4. The timed reset is the one route a child could walk themselves, so it is withheld
        // whether or not a parental PIN exists; recovery is the parental-PIN override, full stop.
        const EntryOptions kidNoPin = entryOptions(true, /*parentalPinSet*/ false, /*restricted*/ true);
        CHECK(kidNoPin.needCode);
        CHECK(!kidNoPin.offerTimedReset);          // THE assertion: no countdown on a kids profile...
        CHECK(!kidNoPin.offerParentalPin);         // ...and with no PIN set there is no other route either
        const EntryOptions kidWithPin = entryOptions(true, /*parentalPinSet*/ true, /*restricted*/ true);
        CHECK(kidWithPin.needCode && kidWithPin.offerParentalPin && !kidWithPin.offerTimedReset);
        // Stated as the invariant rather than case-by-case: restricted implies no timed reset, always.
        for (int pin = 0; pin <= 1; ++pin)
            CHECK(!entryOptions(true, pin != 0, /*restricted*/ true).offerTimedReset);
        // ...and the ordinary-profile behaviour is UNCHANGED by the new argument (guards a fix that
        // accidentally withheld the reset from everyone).
        CHECK(entryOptions(true, false, /*restricted*/ false).offerTimedReset);
    }

    // ---- 6. Rate limiting --------------------------------------------------------------------------
    const qint64 t0 = 1700000000000LL;   // a fixed clock; nothing here reads the real one
    {
        Attempts a;
        CHECK(a.fails == 0 && a.lockedUntilMs == 0);
        CHECK(!lockedOut(a, t0));
        CHECK(lockRemainingMs(a, t0) == 0);

        // The free window: kFreeAttempts failures cost nothing but the counter.
        for (int i = 1; i <= kFreeAttempts; ++i)
        {
            a = afterFailure(a, t0);
            CHECK(a.fails == i);
            CHECK(a.lockedUntilMs == 0);
            CHECK(!lockedOut(a, t0));
        }
        // The first punished failure: the base lockout.
        a = afterFailure(a, t0);
        CHECK(a.fails == kFreeAttempts + 1);
        CHECK(a.lockedUntilMs == t0 + kBaseLockoutMs);
        CHECK(lockedOut(a, t0));
        CHECK(lockRemainingMs(a, t0) == kBaseLockoutMs);
        // Boundary: the lockout ends AT lockedUntilMs, not after it.
        CHECK(!lockedOut(a, t0 + kBaseLockoutMs));
        CHECK(lockedOut(a, t0 + kBaseLockoutMs - 1));
        CHECK(lockRemainingMs(a, t0 + kBaseLockoutMs) == 0);

        // Doubling from a clock AFTER the previous lockout expired (so qMax cannot mask the new value).
        const qint64 t1 = t0 + kBaseLockoutMs;
        a = afterFailure(a, t1);
        CHECK(a.lockedUntilMs == t1 + 2 * kBaseLockoutMs);
        const qint64 t2 = a.lockedUntilMs;
        a = afterFailure(a, t2);
        CHECK(a.lockedUntilMs == t2 + 4 * kBaseLockoutMs);
    }
    {
        // The cap holds, and a persisted/tampered fail count far past the doubling range neither overflows
        // nor produces a lockout in the past. (kBaseLockoutMs << steps is UB once steps >= 63 — the reason
        // afterFailure loops instead of shifting.)
        Attempts big; big.fails = 70;
        const Attempts next = afterFailure(big, t0);
        CHECK(next.fails == 71);
        CHECK(next.lockedUntilMs == t0 + kMaxLockoutMs);
        CHECK(next.lockedUntilMs > t0);
        // Monotone up to the cap, never past it.
        Attempts walk;
        qint64 prevSpan = -1;
        for (int i = 0; i < 20; ++i)
        {
            walk = afterFailure(walk, t0);
            const qint64 span = walk.lockedUntilMs == 0 ? 0 : walk.lockedUntilMs - t0;
            CHECK(span <= kMaxLockoutMs);
            CHECK(span >= prevSpan);
            prevSpan = span;
        }
        CHECK(prevSpan == kMaxLockoutMs);
    }
    {
        // A failure inside the free window must not DROP a lockout that is somehow already running (only a
        // hand-edited ini produces that state, and reading it as "not locked" would turn a corrupt counter
        // into a bypass). And a later failure must never SHORTEN a running lockout.
        Attempts odd; odd.fails = 1; odd.lockedUntilMs = t0 + 999999;
        const Attempts a = afterFailure(odd, t0);
        CHECK(a.lockedUntilMs == t0 + 999999);
        CHECK(lockedOut(a, t0));

        Attempts running; running.fails = kFreeAttempts + 1; running.lockedUntilMs = t0 + kMaxLockoutMs;
        const Attempts b = afterFailure(running, t0);
        CHECK(b.lockedUntilMs >= t0 + kMaxLockoutMs);
    }
    CHECK(cleared().fails == 0 && cleared().lockedUntilMs == 0);
    {
        // ---- 6b. sanitized(): a stored state can never mean a lock longer than the policy allows -------
        // Fix round finding 3. TV boxes boot with a wrong clock and correct it seconds later; a lockout
        // stamped in that window carries a deadline years out and SURVIVES the correction — a permanent lock
        // on a real profile with no in-app way back. kMaxLockoutMs is the longest lockout the escalation can
        // produce, so anything past now + that did not come from this policy.
        const qint64 kYear = 365LL * 24 * 60 * 60 * 1000;

        Attempts skewed; skewed.fails = kFreeAttempts + 1; skewed.lockedUntilMs = t0 + 5 * kYear;
        const Attempts fixed = sanitized(skewed, t0);
        CHECK(fixed.lockedUntilMs == t0 + kMaxLockoutMs);          // THE clamp
        CHECK(lockRemainingMs(fixed, t0) <= kMaxLockoutMs);        // ...so the wait is always survivable
        CHECK(!lockedOut(fixed, t0 + kMaxLockoutMs));              // ...and it genuinely expires
        CHECK(fixed.fails == skewed.fails);                        // the counter itself is not touched

        // A legitimate in-policy deadline is left EXACTLY alone — the clamp must not shorten a real lockout
        // (that would hand a guesser time back), and must not move the boundary case.
        Attempts legit; legit.lockedUntilMs = t0 + kBaseLockoutMs;
        CHECK(sanitized(legit, t0).lockedUntilMs == t0 + kBaseLockoutMs);
        Attempts atCap; atCap.lockedUntilMs = t0 + kMaxLockoutMs;
        CHECK(sanitized(atCap, t0).lockedUntilMs == t0 + kMaxLockoutMs);
        Attempts overCap; overCap.lockedUntilMs = t0 + kMaxLockoutMs + 1;
        CHECK(sanitized(overCap, t0).lockedUntilMs == t0 + kMaxLockoutMs);

        // The negative-fail-count clamp moved in here from attempts(); it is the same rule and still holds.
        Attempts neg; neg.fails = -5;
        CHECK(sanitized(neg, t0).fails == 0);
        // A clean state is a fixed point.
        CHECK(sanitized(cleared(), t0).fails == 0 && sanitized(cleared(), t0).lockedUntilMs == 0);
    }

    // ---- 7. evaluate(): the composite ---------------------------------------------------------------
    {
        Attempts a;
        CHECK(evaluate(hA, kIdA, kCodeA, a, t0) == Outcome::Accepted);
        CHECK(a.fails == 0 && a.lockedUntilMs == 0);

        CHECK(evaluate(hA, kIdA, kCodeB, a, t0) == Outcome::Rejected);
        CHECK(a.fails == 1);
        // A success clears the accumulated failures — the counter is CONSECUTIVE wrong tries.
        CHECK(evaluate(hA, kIdA, kCodeA, a, t0) == Outcome::Accepted);
        CHECK(a.fails == 0);

        // Burn through the free window into a lockout. Every one of these is REJECTED, including the one
        // that arms the lockout: evaluate tests the lock on the way IN, so the failure that creates it is
        // still answered honestly, and only the NEXT try is refused.
        for (int i = 0; i <= kFreeAttempts; ++i)
        {
            CHECK(evaluate(hA, kIdA, kCodeB, a, t0) == Outcome::Rejected);
            CHECK(a.fails == i + 1);
        }
        CHECK(lockedOut(a, t0));
        const Attempts snapshot = a;

        // THE assertion this composite exists for: while locked out, a CORRECT code does not open the
        // profile, and the attempt state does not move. A caller that checked verify() first (or updated the
        // counter before the lockout test) would return Accepted here, or would let a guesser extend their
        // own lockout by hammering — both are why this is one function rather than three the caller composes.
        CHECK(evaluate(hA, kIdA, kCodeA, a, t0) == Outcome::LockedOut);
        CHECK(a.fails == snapshot.fails);
        CHECK(a.lockedUntilMs == snapshot.lockedUntilMs);
        CHECK(evaluate(hA, kIdA, kCodeB, a, t0) == Outcome::LockedOut);
        CHECK(a.lockedUntilMs == snapshot.lockedUntilMs);   // hammering does not extend it

        // Once it expires, the correct code works again and resets the state.
        const qint64 after = a.lockedUntilMs;
        CHECK(evaluate(hA, kIdA, kCodeA, a, after) == Outcome::Accepted);
        CHECK(a.fails == 0 && a.lockedUntilMs == 0);
    }
    {
        // No passcode stored: evaluate must REJECT rather than accept anything. Callers are expected to skip
        // the gate entirely when the hash is empty (entryOptions says so), but a bug that reached here with
        // an empty hash must not become "any four digits open this profile".
        Attempts a;
        CHECK(evaluate(QString(), kIdA, kCodeA, a, t0) == Outcome::Rejected);
        CHECK(a.fails == 1);
    }

    // ---- 8. Attempt-state persistence (device-local) -----------------------------------------------
    freshIni();
    {
        CHECK(!iniHas(QStringLiteral("profilepass/") + kIdA + QStringLiteral("/fails")));
        Attempts a; a.fails = 3; a.lockedUntilMs = t0 + 5000;
        setAttempts(kIdA, a);
        const Attempts back = attempts(kIdA);
        CHECK(back.fails == 3);
        CHECK(back.lockedUntilMs == t0 + 5000);      // a 64-bit ms epoch must survive the ini round-trip
        // Untouched profiles read clean.
        CHECK(attempts(kIdB).fails == 0);
        CHECK(attempts(kIdB).lockedUntilMs == 0);
        // The keys are exactly the ones the sync + settings-transaction carve-outs match on.
        CHECK(iniHas(QStringLiteral("profilepass/") + kIdA + QStringLiteral("/fails")));
        CHECK(iniHas(QStringLiteral("profilepass/") + kIdA + QStringLiteral("/until")));

        clearAttempts(kIdA);
        CHECK(attempts(kIdA).fails == 0 && attempts(kIdA).lockedUntilMs == 0);
        CHECK(!iniHas(QStringLiteral("profilepass/") + kIdA + QStringLiteral("/fails")));
    }
    freshIni();
    {
        // Independence proof: the previous case's key must be absent in this file.
        CHECK(!iniHas(QStringLiteral("profilepass/") + kIdA + QStringLiteral("/fails")));
        // Storing a zeroed state REMOVES the keys rather than writing zeros — an ini that grows a pair of
        // keys per profile per successful unlock is a slow leak on a box with a big library.
        Attempts a; a.fails = 2;
        setAttempts(kIdA, a);
        CHECK(iniHas(QStringLiteral("profilepass/") + kIdA + QStringLiteral("/fails")));
        setAttempts(kIdA, cleared());
        CHECK(!iniHas(QStringLiteral("profilepass/") + kIdA + QStringLiteral("/fails")));
        CHECK(!iniHas(QStringLiteral("profilepass/") + kIdA + QStringLiteral("/until")));
    }
    freshIni();
    {
        // A hand-edited negative counter must not read back as "free tries" (it would make afterFailure walk
        // back up through the free window before locking again).
        QSettings s(iniPath(), QSettings::IniFormat);
        s.setValue(QStringLiteral("profilepass/") + kIdA + QStringLiteral("/fails"), -5);
        s.sync();
        setIniPathForTesting(iniPath());   // re-open so the seeded value is read
        CHECK(attempts(kIdA).fails == 0);
    }
    freshIni();
    {
        // A deadline stamped under a forward-skewed clock (the TV-box case, fix round finding 3) must not
        // survive as a years-long lock. attempts() clamps on read AND PERSISTS the clamp: read-only clamping
        // would re-derive the ceiling from the current clock every time, so the deadline would walk forward
        // with the clock and the lockout would never expire at all. The write-back is what makes it finite,
        // so it is asserted against the FILE, not just the returned struct.
        CHECK(!iniHas(QStringLiteral("profilepass/") + kIdA + QStringLiteral("/until")));
        const qint64 farFuture = 4102444800000LL;   // 2100-01-01, well past any real lockout
        {
            QSettings s(iniPath(), QSettings::IniFormat);
            s.setValue(QStringLiteral("profilepass/") + kIdA + QStringLiteral("/fails"), kFreeAttempts + 1);
            s.setValue(QStringLiteral("profilepass/") + kIdA + QStringLiteral("/until"), farFuture);
            s.sync();
        }
        setIniPathForTesting(iniPath());   // re-open so the seeded values are read
        const qint64 readAt = QDateTime::currentMSecsSinceEpoch();
        const Attempts got = attempts(kIdA);
        CHECK(got.lockedUntilMs < farFuture);
        CHECK(got.lockedUntilMs <= readAt + kMaxLockoutMs);
        // Persisted: a SECOND reader (a fresh QSettings on the same file) sees the clamped value, which is
        // what makes the lockout actually count down instead of being re-clamped forward forever.
        {
            QSettings s2(iniPath(), QSettings::IniFormat);
            CHECK(s2.value(QStringLiteral("profilepass/") + kIdA + QStringLiteral("/until")).toLongLong()
                  < farFuture);
        }
        // And an ordinary in-policy state is round-tripped BYTE-FOR-BYTE — the write-back must fire only
        // when the clamp actually changed something, or every read would rewrite the ini.
        const qint64 sane = QDateTime::currentMSecsSinceEpoch() + kBaseLockoutMs;
        Attempts ok; ok.fails = kFreeAttempts + 1; ok.lockedUntilMs = sane;
        setAttempts(kIdB, ok);
        CHECK(attempts(kIdB).lockedUntilMs == sane);
    }

    // ---- 9. isAttemptKey: what the carve-outs match ------------------------------------------------
    CHECK(isAttemptKey(QStringLiteral("profilepass/abc/fails")));
    CHECK(isAttemptKey(QStringLiteral("profilepass/abc/until")));
    CHECK(!isAttemptKey(QStringLiteral("profiles/list")));       // the hash lives here and MUST sync
    CHECK(!isAttemptKey(QStringLiteral("profiles/current")));
    CHECK(!isAttemptKey(QStringLiteral("profilepassword/x")));   // prefix, not a substring match
    CHECK(!isAttemptKey(QStringLiteral("parental/pinHash")));    // the global PIN is a different mechanism

    if (failures == 0) { std::printf("PASSCODE-OK\n"); return 0; }
    std::fprintf(stderr, "PASSCODE-FAIL total=%d\n", failures);
    return 1;
}
