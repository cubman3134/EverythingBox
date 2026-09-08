#include "TrackerQueue.h"
#include "AppBrand.h"
#include "AppPaths.h"
#include "ProfileStore.h"
#include "TrackerRules.h"

#include <QDateTime>
#include <QMetaType>
#include <QObject>
#include <QSettings>

#include <utility>

using namespace tracker;

// The portable everythingbox.ini, shared with Settings/TrackerLinks/the trackers themselves. Coherence with
// any other QSettings on the same file comes from every writer calling sync() — the posture TrackerLinks.cpp
// documents.
static QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                       QSettings::IniFormat);
    return s;
}

QVector<Update> TrackerQueue::load(Id id)
{
    return decodeQueue(store().value(queueKey(ProfileStore::currentId(), id)).toString().toUtf8());
}

void TrackerQueue::save(Id id, const QVector<Update>& q)
{
    store().setValue(queueKey(ProfileStore::currentId(), id), QString::fromUtf8(encodeQueue(q)));
    store().sync();
}

int TrackerQueue::count(Id id) { return load(id).size(); }

bool TrackerQueue::enqueue(Id id, const Update& u)
{
    QVector<Update> q = load(id);
    // FURTHEST WINS, per item. An earlier chapter arriving late changes nothing and writes nothing.
    if (!coalesce(q, u)) return false;
    applyQueueCap(q);
    save(id, q);
    return true;
}

int TrackerQueue::nextSendable(const QVector<Update>& q, Id id, qint64 nowMs, qint64* waitMsOut)
{
    const QString profile = ProfileStore::currentId();
    qint64 soonest = -1;
    for (int i = 0; i < q.size(); ++i)
    {
        const qint64 last = store().value(lastSentKey(profile, id, q[i].itemKey), 0).toLongLong();
        if (debounceAllows(last, nowMs)) return i;
        const qint64 waitMs = kDebounceMs - (nowMs - last);
        if (soonest < 0 || waitMs < soonest) soonest = waitMs;
    }
    if (waitMsOut) *waitMsOut = soonest;
    return -1;
}

void TrackerQueue::removeDelivered(Id id, const Update& u)
{
    QVector<Update> q = load(id);
    // BY IDENTITY, not by index: the queue is re-read after the request, and a page turn during it may have
    // coalesced a FURTHER update onto this item. Dropping index 0 would then throw that away.
    for (int i = 0; i < q.size(); ++i)
    {
        if (q[i].itemKey == u.itemKey && q[i].mediaId == u.mediaId && q[i].unit <= u.unit)
        { q.remove(i); break; }
    }
    save(id, q);
}

qint64 TrackerQueue::lastSentMs(Id id, const QString& itemKey)
{
    return store().value(lastSentKey(ProfileStore::currentId(), id, itemKey), 0).toLongLong();
}

void TrackerQueue::noteSent(Id id, const QString& itemKey, qint64 whenMs)
{
    store().setValue(lastSentKey(ProfileStore::currentId(), id, itemKey), whenMs);
    store().sync();
}

QString TrackerQueue::lastError(Id id)
{
    return store().value(lastErrorKey(ProfileStore::currentId(), id)).toString();
}

void TrackerQueue::setLastError(Id id, const QString& message)
{
    const QString key = lastErrorKey(ProfileStore::currentId(), id);
    if (message.isEmpty()) store().remove(key); else store().setValue(key, message);
    store().sync();
}

// ---- the dropped-update notices (issue #328) ------------------------------------------------------------

QStringList TrackerQueue::dropped(Id id)
{
    const QVariant v = store().value(droppedKey(ProfileStore::currentId(), id));
    // QSettings' ini backend reads a one-element list back as a bare string, so both spellings are accepted
    // - a single dropped update is by far the common case and must not read back as nothing.
    if (v.metaType().id() == QMetaType::QStringList) return v.toStringList();
    const QString one = v.toString();
    return one.isEmpty() ? QStringList{} : QStringList{ one };
}

void TrackerQueue::noteDropped(Id id, const QString& message)
{
    const QString m = message.trimmed();
    if (m.isEmpty()) return;                  // not a notice
    QStringList all = dropped(id);
    if (!all.isEmpty() && all.constLast() == m) return;   // the same refusal twice says one thing, not two
    all.push_back(m);
    while (all.size() > kDroppedMax) all.removeFirst();    // bounded, oldest first out
    store().setValue(droppedKey(ProfileStore::currentId(), id), all);
    store().sync();
}

void TrackerQueue::clearDropped(Id id)
{
    store().remove(droppedKey(ProfileStore::currentId(), id));
    store().sync();
}

QString TrackerQueue::droppedNotice(const QStringList& messages)
{
    if (messages.isEmpty()) return QString();
    // THE NEWEST FEW, spelled out. Each sentence names the update it is about, which is the only part
    // anybody can act on; the rest are counted, because a settings panel is not a log.
    const int kShown = 3;
    const int extra = qMax(0, messages.size() - kShown);
    QStringList shown;
    for (int i = qMax(0, messages.size() - kShown); i < messages.size(); ++i) shown << messages.at(i);
    QString out = shown.join(QStringLiteral("  "));
    if (extra > 0)
        out += QStringLiteral("  ") + QObject::tr("%n earlier update(s) were dropped too.", nullptr, extra);
    return out;
}

void TrackerQueue::forgetAccount(Id id)
{
    store().remove(queueKey(ProfileStore::currentId(), id));
    store().remove(lastErrorKey(ProfileStore::currentId(), id));
    // ...and the notices, which are about updates this account's queue held. A fresh link starts with
    // nothing owed and nothing to be told about.
    store().remove(droppedKey(ProfileStore::currentId(), id));
    store().sync();
}

// ================= the credential store (issue #326) =====================================================
//
// Both trackers had written these out against the same five Tracker.h keys. One copy now, Id-parameterised.
// Nothing here logs and nothing here is ever put in a message — see the header.

QString TrackerQueue::clientId(Id id) { return store().value(clientIdKey(id)).toString(); }

QString TrackerQueue::clientSecret(Id id) { return store().value(clientSecretKey(id)).toString(); }

void TrackerQueue::setClientId(Id id, const QString& v)
{
    store().setValue(clientIdKey(id), v.trimmed());
    store().sync();
}

void TrackerQueue::setClientSecret(Id id, const QString& v)
{
    store().setValue(clientSecretKey(id), v.trimmed());
    store().sync();
}

QString TrackerQueue::accessToken(Id id) { return store().value(accessKey(id)).toString(); }

bool TrackerQueue::hasAccessToken(Id id) { return !accessToken(id).isEmpty(); }

QString TrackerQueue::refreshToken(Id id) { return store().value(refreshKey(id)).toString(); }

bool TrackerQueue::tokenFresh(Id id, qint64 nowSec, qint64 skewSec)
{
    const qint64 expiry = store().value(expiryKey(id), 0).toLongLong();
    // 0 = unknown expiry (see storeTokens). The skew keeps a token that expires mid-flight from being used
    // for the request it would fail.
    return expiry <= 0 || expiry - skewSec > nowSec;
}

void TrackerQueue::storeTokens(Id id, const QString& access, const QString& refresh,
                               qint64 expiresInSec, qint64 nowSec)
{
    store().setValue(accessKey(id), access);
    // A refresh reply may legitimately omit the refresh token; keeping the old one is correct, blanking it
    // would unlink the account on the next expiry.
    if (!refresh.isEmpty()) store().setValue(refreshKey(id), refresh);
    store().setValue(expiryKey(id), expiresInSec > 0 ? nowSec + expiresInSec : 0);
    store().sync();
}

void TrackerQueue::clearTokens(Id id)
{
    store().remove(accessKey(id));
    store().remove(refreshKey(id));
    store().remove(expiryKey(id));
    store().sync();
}

// ================= the drain loop (issue #326) ===========================================================

TrackerQueue::Sender::Sender(SendSpec spec) : spec_(std::move(spec))
{
    if (!spec_.now) spec_.now = [] { return QDateTime::currentMSecsSinceEpoch(); };
}

void TrackerQueue::Sender::reset() { failures_ = 0; lastWaitMs_ = 0; }

void TrackerQueue::Sender::arm(qint64 delayMs)
{
    lastWaitMs_ = delayMs > 0 ? delayMs : 0;
    if (spec_.wait) spec_.wait(int(lastWaitMs_));
}

void TrackerQueue::Sender::drain()
{
    if (sending_ || !spec_.send) return;
    if (!spec_.ready || !spec_.ready()) return;
    QVector<Update> q = load(spec_.id);
    if (q.isEmpty()) { arm(0); return; }   // nothing pending: stop the timer rather than wake for nothing

    const qint64 now = spec_.now();
    qint64 soonest = -1;
    const int idx = nextSendable(q, spec_.id, now, &soonest);
    if (idx < 0)
    {
        // Everything queued is inside its item's debounce window. Wake exactly when the earliest one opens,
        // rather than polling: a binge-reader would otherwise have a timer firing every second.
        arm(qBound<qint64>(1000, soonest, kDebounceMs));
        return;
    }

    const Update u = q[idx];
    sending_ = true;
    spec_.send(u, [this, u](Reply r) {
        sending_ = false;
        if (r.accepted)
        {
            failures_ = 0;
            // Removed by IDENTITY, not by index — see TrackerQueue::removeDelivered.
            removeDelivered(spec_.id, u);
            noteSent(spec_.id, u.itemKey, spec_.now());
            setLastError(spec_.id, QString());
            if (spec_.pushed) spec_.pushed(u.itemKey, u.unit);
            if (spec_.changed) spec_.changed();
            drain();   // keep going; the next item's debounce is checked afresh
            return;
        }

        ++failures_;
        const SendVerdict v = classifySend(spec_.policy, r.status, r.retryAfterSec, failures_);
        if (v.permanent)
        {
            // THE UNWEDGE. This provider will never accept this row, so it is DROPPED rather than left to
            // block the head of an ordered queue for ever — every later chapter behind it would be lost
            // too. Said out loud in the status line, because a queue that quietly discards somebody's
            // progress is worse than one that wedges: at least a wedge is eventually noticed.
            removeDelivered(spec_.id, u);
            const QString said = spec_.droppedMessage ? spec_.droppedMessage(u) : QString();
            setLastError(spec_.id, said);
            // ...AND IT WAITS FOR THE USER (issue #328). The status line is read by whoever has the settings
            // panel open, which for a background sync is nobody; this is the same sentence, kept until a
            // panel has actually shown it.
            noteDropped(spec_.id, said);
            if (spec_.changed) spec_.changed();
            drain();   // ...and the rows behind it go out now, which is the whole point of dropping it
            return;
        }
        // A message ABOUT the failure, never the request — see both tracker headers.
        setLastError(spec_.id, v.reauth ? spec_.reauthMessage : spec_.retryMessage);
        arm(qBound<qint64>(1000, v.delayMs, spec_.policy.maxMs));
        if (spec_.changed) spec_.changed();
    });
}
