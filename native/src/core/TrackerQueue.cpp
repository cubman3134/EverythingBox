#include "TrackerQueue.h"
#include "AppBrand.h"
#include "AppPaths.h"
#include "ProfileStore.h"
#include "TrackerRules.h"

#include <QSettings>

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

void TrackerQueue::forgetAccount(Id id)
{
    store().remove(queueKey(ProfileStore::currentId(), id));
    store().remove(lastErrorKey(ProfileStore::currentId(), id));
    store().sync();
}
