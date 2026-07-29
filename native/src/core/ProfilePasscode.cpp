#include "ProfilePasscode.h"

#include "AppBrand.h"
#include "AppPaths.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QSettings>

namespace {

// Test-only redirect (see the header). The statics, the setter and the branch in store() are compiled ONLY
// for probe_passcode; in the app build none of it exists. Deleting the cached QSettings rather than only
// re-pointing a path is the load-bearing half: a function-local static QSettings is constructed exactly once,
// so a path captured on first use would pin every later probe case to the FIRST case's file and the cases
// would silently share state while looking independent.
#ifdef EB_PROFILEPASSCODE_TEST_SEAM
QString    g_testIniPath;
QSettings* g_testStore = nullptr;
#endif

QSettings& store()
{
#ifdef EB_PROFILEPASSCODE_TEST_SEAM
    if (!g_testIniPath.isEmpty())
    {
        if (!g_testStore) g_testStore = new QSettings(g_testIniPath, QSettings::IniFormat);
        return *g_testStore;
    }
#endif
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                       QSettings::IniFormat);
    return s;
}

// Built from the one prefix the carve-outs also match on, so a rename cannot leave the keys inside the
// sync/transaction exclusions and the writers outside them.
QString failKey(const QString& id)
{
    return QLatin1String(ProfilePasscode::kAttemptKeyPrefix) + id + QStringLiteral("/fails");
}
QString untilKey(const QString& id)
{
    return QLatin1String(ProfilePasscode::kAttemptKeyPrefix) + id + QStringLiteral("/until");
}

} // namespace

#ifdef EB_PROFILEPASSCODE_TEST_SEAM
void ProfilePasscode::setIniPathForTesting(const QString& path)
{
    delete g_testStore;
    g_testStore   = nullptr;
    g_testIniPath = path;
}
#endif

bool ProfilePasscode::isValidFormat(const QString& code)
{
    if (code.size() != kLength) return false;
    for (const QChar c : code)
        // isDigit() would accept Arabic-Indic and every other Unicode decimal digit, which no pad emits and
        // which would hash differently from the ASCII digits the user believes they typed.
        if (c < QLatin1Char('0') || c > QLatin1Char('9')) return false;
    return true;
}

QString ProfilePasscode::hash(const QString& profileId, const QString& code)
{
    if (!isValidFormat(code)) return QString();   // never mint a hash nobody can reproduce — see the header
    const QByteArray in = QByteArray(AppBrand::kProfilePasscodeSalt)
                        + profileId.toUtf8() + QByteArray(":") + code.toUtf8();
    return QString::fromLatin1(QCryptographicHash::hash(in, QCryptographicHash::Sha256).toHex());
}

bool ProfilePasscode::verify(const QString& storedHash, const QString& profileId, const QString& code)
{
    if (storedHash.isEmpty()) return false;       // "no passcode set" is not "every code opens it"
    const QString h = hash(profileId, code);
    return !h.isEmpty() && h == storedHash;
}

bool ProfilePasscode::mustShowPicker(int profileCount, bool skipWhenSingle, bool singleHasPasscode)
{
    // Zero profiles (create one) and two-or-more (genuinely ambiguous) always ask; the preference only ever
    // speaks for the exactly-one case, and a passcode on that one profile overrides it — skipping the picker
    // would skip the ONLY surface that asks for the code, i.e. silently disable the lock.
    if (profileCount != 1) return true;
    return !(skipWhenSingle && !singleHasPasscode);
}

ProfilePasscode::EntryOptions ProfilePasscode::entryOptions(bool hasPasscode, bool parentalPinSet,
                                                            bool restricted)
{
    EntryOptions o;
    if (!hasPasscode) return o;                   // no gate at all: no code, no recovery rows
    o.needCode         = true;
    o.offerParentalPin = parentalPinSet;
    // Exclusive with the parental PIN by design, and withheld OUTRIGHT on a kids profile — a sixty-second
    // countdown is not an obstacle to the one person a kids profile exists to hold, who has the remote and
    // the afternoon. See the header for why that unrecoverable corner is accepted (and warned about).
    o.offerTimedReset  = !parentalPinSet && !restricted;
    return o;
}

ProfilePasscode::RestrictedGate ProfilePasscode::restrictedChangeGate(bool parentalPinSet, bool turningOn,
                                                                      bool hasPasscode)
{
    if (parentalPinSet) return RestrictedGate::ParentalPin;   // both directions — see the header
    // No PIN. Turning it ON takes nothing away that a passcode holder had; turning it OFF hands back the
    // timed reset, which is the bypass, so it costs the profile's own code.
    if (!turningOn && hasPasscode) return RestrictedGate::ProfilePasscode;
    return RestrictedGate::Free;
}

bool ProfilePasscode::lockedOut(const Attempts& a, qint64 nowMs)
{
    return a.lockedUntilMs > nowMs;
}

qint64 ProfilePasscode::lockRemainingMs(const Attempts& a, qint64 nowMs)
{
    return lockedOut(a, nowMs) ? (a.lockedUntilMs - nowMs) : 0;
}

ProfilePasscode::Attempts ProfilePasscode::afterFailure(const Attempts& a, qint64 nowMs)
{
    Attempts out;
    out.fails = a.fails + 1;
    if (out.fails <= kFreeAttempts)
    {
        // Still inside the free window. Carry ANY existing lockout forward rather than dropping it: a stored
        // state with a live lockout and a low fail count can only come from a hand-edited ini, and reading
        // that as "no lockout" would turn a corrupted counter into a bypass.
        out.lockedUntilMs = a.lockedUntilMs;
        return out;
    }
    const int steps = out.fails - kFreeAttempts - 1;      // 0 on the first punished failure
    qint64 ms = kBaseLockoutMs;
    // Doubling, but computed by loop with an early break: `kBaseLockoutMs << steps` is undefined behaviour
    // once a persisted (or tampered) fail count makes steps >= 63, and saturates to nonsense well before that.
    for (int i = 0; i < steps && ms < kMaxLockoutMs; ++i) ms *= 2;
    if (ms > kMaxLockoutMs) ms = kMaxLockoutMs;
    const qint64 until = nowMs + ms;
    out.lockedUntilMs = qMax(until, a.lockedUntilMs);     // never SHORTEN a lockout already running
    return out;
}

ProfilePasscode::Attempts ProfilePasscode::cleared()
{
    return Attempts{};
}

ProfilePasscode::Attempts ProfilePasscode::sanitized(const Attempts& raw, qint64 nowMs)
{
    Attempts a = raw;
    if (a.fails < 0) a.fails = 0;                 // a hand-edited negative would grant unlimited free tries
    // kMaxLockoutMs is the longest lockout the escalation can produce, so a deadline beyond now + that came
    // from something other than this policy — a forward-skewed clock at boot being the realistic one. Left
    // alone it is a years-long lock that survives the clock correction with no in-app way out.
    const qint64 ceiling = nowMs + kMaxLockoutMs;
    if (a.lockedUntilMs > ceiling) a.lockedUntilMs = ceiling;
    return a;
}

ProfilePasscode::Outcome ProfilePasscode::evaluate(const QString& storedHash, const QString& profileId,
                                                   const QString& code, Attempts& io, qint64 nowMs)
{
    // Lockout FIRST, before the comparison: during a lockout the caller learns nothing about the code it
    // supplied, and a correct guess does not open the profile. The state is left untouched — a locked-out
    // try is not a new failure, so hammering the pad cannot extend its own lockout forever.
    if (lockedOut(io, nowMs)) return Outcome::LockedOut;
    if (verify(storedHash, profileId, code)) { io = cleared(); return Outcome::Accepted; }
    io = afterFailure(io, nowMs);
    return Outcome::Rejected;
}

ProfilePasscode::Attempts ProfilePasscode::attempts(const QString& profileId)
{
    Attempts raw;
    raw.fails         = store().value(failKey(profileId), 0).toInt();
    raw.lockedUntilMs = store().value(untilKey(profileId), 0).toLongLong();
    const Attempts a = sanitized(raw, QDateTime::currentMSecsSinceEpoch());
    // PERSIST the clamp. Read-only clamping would re-derive the ceiling from the current clock on every
    // read, so a skew-stamped deadline would slide forward forever and never expire — see the header.
    if (a.fails != raw.fails || a.lockedUntilMs != raw.lockedUntilMs) setAttempts(profileId, a);
    return a;
}

void ProfilePasscode::setAttempts(const QString& profileId, const Attempts& a)
{
    if (a.fails == 0 && a.lockedUntilMs == 0) { clearAttempts(profileId); return; } // don't grow the ini
    store().setValue(failKey(profileId), a.fails);
    store().setValue(untilKey(profileId), a.lockedUntilMs);
    store().sync();
}

void ProfilePasscode::clearAttempts(const QString& profileId)
{
    store().remove(failKey(profileId));
    store().remove(untilKey(profileId));
    store().sync();
}

