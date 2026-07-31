#include "PendingPush.h"

#include "AppBrand.h"
#include "AppPaths.h"

#include <QSettings>

namespace {

// Own file-local store(), the SettingsTxn/ThemeChoice/ProfilePasscode idiom: this TU stays QtCore-only so it
// links into a headless probe without dragging Settings.cpp (and through it FormFactor) behind it.
QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                       QSettings::IniFormat);
    return s;
}

// A sub-GROUP under device/, never flat keys. probe_cloudmerge §1 pins that the only direct child KEY of
// `device` is `id`; a flat "device/pushAttempts" would break that assertion, and more importantly the group
// keeps the three fields of one record visibly together in the ini.
const char* kAttempts    = "device/push/attempts";
const char* kLastAttempt = "device/push/lastAttemptMs";
const char* kFailure     = "device/push/failure";

// Stored as words rather than the enum's integer value. An ini is a file a user can open, and a persisted
// enumerator is a number whose meaning silently changes the day someone inserts a value into the enum.
QString failureToString(PendingPush::Failure f)
{
    switch (f)
    {
        case PendingPush::Failure::Offline:     return QStringLiteral("offline");
        case PendingPush::Failure::AuthExpired: return QStringLiteral("auth");
        case PendingPush::Failure::None:        break;
    }
    return QString();
}

PendingPush::Failure failureFromString(const QString& s)
{
    if (s == QLatin1String("offline")) return PendingPush::Failure::Offline;
    if (s == QLatin1String("auth"))    return PendingPush::Failure::AuthExpired;
    return PendingPush::Failure::None;
}

} // namespace

qint64 PendingPush::backoffMs(int attempts)
{
    if (attempts <= 0) return 0;
    // Clamp the SHIFT, not just the result: kBaseDelayMs << 63 is undefined behaviour, and `attempts` comes
    // out of a file a user (or a corrupted write) can put any integer in.
    const int shift = attempts - 1;
    if (shift >= 31) return kMaxDelayMs;
    const qint64 d = kBaseDelayMs << shift;
    return d > kMaxDelayMs ? kMaxDelayMs : d;
}

qint64 PendingPush::dueAtMs(const State& s)
{
    return s.lastAttemptMs + backoffMs(s.attempts);
}

PendingPush::Due PendingPush::due(const State& s, bool signedIn, bool manual, qint64 nowMs)
{
    // No account, nothing to owe a push TO. Signing out clears the record anyway; this is the belt.
    if (!signedIn) return Due::Nothing;
    // A user action overrides every park and every backoff window — that is what makes give-up recoverable
    // without a restart, and it is checked BEFORE the owed test so a manual Retry works even from a clean
    // record (the panel only offers it when owed, but nothing here depends on the panel getting that right).
    if (manual) return Due::Attempt;
    if (!owed(s)) return Due::Nothing;
    // Auth BEFORE give-up, deliberately: an expired sign-in that has also burned through the attempt cap
    // should report the ACTIONABLE state ("sign in again"), not the generic one ("gave up").
    if (s.failure == Failure::AuthExpired) return Due::NeedsSignIn;
    if (gaveUp(s)) return Due::GaveUp;
    if (nowMs < dueAtMs(s)) return Due::Wait;
    return Due::Attempt;
}

PendingPush::Plan PendingPush::resolve(bool reached, bool listReached, bool localChanged, bool remoteChanged)
{
    // listReached is as load-bearing as reached: a failed bundle query returns an empty file id, and pushing
    // against an empty existingId POSTs a DUPLICATE bundle instead of PATCHing the real one. CloudSync::pushLocal
    // guards this too; refusing to even plan the push keeps the reason in one place.
    if (!reached || !listReached) return Plan::Unreachable;
    // The idempotence gate. Whatever this record thinks it owes, the fingerprint is the authority: if local
    // matches the synced baseline there is nothing to upload, however the state got there (an exit push, a
    // manual Sync now, a peer's bundle we already applied).
    if (!localChanged) return Plan::NothingToSend;
    return remoteChanged ? Plan::PullThenPush : Plan::Push;
}

PendingPush::State PendingPush::onOutcome(const State& before, Outcome o, qint64 nowMs)
{
    State after;
    switch (o)
    {
        case Outcome::Success:
            // A clean record, ZEROED rather than "attempts = 0 with the old timestamp left behind". due()
            // short-circuits on attempts == 0 so a stale timestamp would be harmless, but a record that reads
            // clean must LOOK clean in the ini — a leftover lastAttemptMs is the kind of thing a future reader
            // builds a wrong inference on.
            return after;
        case Outcome::Offline:
            after.failure = Failure::Offline;
            break;
        case Outcome::AuthExpired:
            after.failure = Failure::AuthExpired;
            break;
    }
    after.attempts      = before.attempts + 1;
    after.lastAttemptMs = nowMs;
    return after;
}

PendingPush::Auth PendingPush::classifyRefresh(bool haveRefreshToken, bool serverAnswered, bool serverRejected)
{
    // No stored grant at all: there is nothing to refresh and no network trip to make. Expired rather than
    // Offline, because the fix is a sign-in.
    if (!haveRefreshToken) return Auth::Expired;
    // We never heard back. That is the network's fault, not the account's — patience, not a sign-in prompt.
    if (!serverAnswered) return Auth::Offline;
    // The endpoint answered and said no (a revoked or expired grant). No amount of retrying changes that.
    if (serverRejected) return Auth::Expired;
    return Auth::Ok;
}

PendingPush::Outcome PendingPush::classifyPush(bool ok, Auth a)
{
    if (ok) return Outcome::Success;
    return a == Auth::Expired ? Outcome::AuthExpired : Outcome::Offline;
}

QString PendingPush::keyAttempts()    { return QString::fromLatin1(kAttempts); }
QString PendingPush::keyLastAttempt() { return QString::fromLatin1(kLastAttempt); }
QString PendingPush::keyFailure()     { return QString::fromLatin1(kFailure); }

PendingPush::State PendingPush::load()
{
    State s;
    s.attempts      = store().value(QString::fromLatin1(kAttempts), 0).toInt();
    s.lastAttemptMs = store().value(QString::fromLatin1(kLastAttempt), 0).toLongLong();
    s.failure       = failureFromString(store().value(QString::fromLatin1(kFailure)).toString());
    // A negative attempt count can only come from a hand-edited or corrupted ini, and it would make owed()
    // false while a failure string sits beside it. Normalise on the way in rather than letting every caller
    // wonder.
    if (s.attempts < 0) s.attempts = 0;
    if (s.attempts == 0) { s.lastAttemptMs = 0; s.failure = Failure::None; }
    return s;
}

void PendingPush::save(const State& s)
{
    if (!owed(s))
    {
        // Clearing REMOVES the keys rather than writing zeros: the clean state is the absence of the record,
        // so a fresh install and a device that has just synced look identical in the ini.
        store().remove(QString::fromLatin1(kAttempts));
        store().remove(QString::fromLatin1(kLastAttempt));
        store().remove(QString::fromLatin1(kFailure));
    }
    else
    {
        store().setValue(QString::fromLatin1(kAttempts), s.attempts);
        store().setValue(QString::fromLatin1(kLastAttempt), s.lastAttemptMs);
        store().setValue(QString::fromLatin1(kFailure), failureToString(s.failure));
    }
    // sync() here, not left to the caller: the entire point of this record is that it survives a crash, and a
    // QSettings write that has not been flushed does not.
    store().sync();
}
