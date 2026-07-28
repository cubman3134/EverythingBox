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

#include <QCoreApplication>
#include <QCryptographicHash>
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

    // SALT SEPARATION — the constraint the whole feature is built around. AppBrand::Legacy::kParentalPinSalt
    // ("mmv-parental:") is an input to a hash already written to every existing user's ini; this scheme must
    // use its OWN salt and leave that one alone. Recomputed here from the literal rather than by including
    // AppBrand.h, so that a future edit which "unifies" the two salts fails HERE instead of silently making
    // every parental PIN in the field equal to a profile passcode.
    {
        const QByteArray parentalIn = QByteArray("mmv-parental:") + kCodeA.toUtf8();
        const QString parentalHash =
            QString::fromLatin1(QCryptographicHash::hash(parentalIn, QCryptographicHash::Sha256).toHex());
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
        const EntryOptions none = entryOptions(/*hasPasscode*/ false, /*parentalPinSet*/ false);
        CHECK(!none.needCode && !none.offerParentalPin && !none.offerTimedReset);
        const EntryOptions noneWithPin = entryOptions(false, true);
        CHECK(!noneWithPin.needCode && !noneWithPin.offerParentalPin && !noneWithPin.offerTimedReset);
        // With a parental PIN set, THAT is the override and the timed reset is withheld — otherwise a kid
        // walks around the parent's PIN by waiting out a countdown.
        const EntryOptions withPin = entryOptions(true, true);
        CHECK(withPin.needCode && withPin.offerParentalPin && !withPin.offerTimedReset);
        // With no parental PIN there must still be a way out that keeps the profile's data, so the timed
        // self-service reset appears. The two routes are mutually exclusive by construction.
        const EntryOptions noPin = entryOptions(true, false);
        CHECK(noPin.needCode && !noPin.offerParentalPin && noPin.offerTimedReset);
        CHECK(!(noPin.offerParentalPin && noPin.offerTimedReset));
        CHECK(!(withPin.offerParentalPin && withPin.offerTimedReset));
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
