#include "FollowSnapshot.h"
#include "AppBrand.h"
#include "AppPaths.h"
#include "ProfileStore.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QSettings>

static QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                       QSettings::IniFormat);
    return s;
}

static QString profileId()
{
    const QString id = ProfileStore::currentId();
    return id.isEmpty() ? QStringLiteral("default") : id;
}

// The same MD5-hex-over-UTF8 leaf ItemMarks/MetaOverrides/MissedDismiss use. See the header for why a raw
// item id may not be an ini leaf.
static QString leaf(const QString& itemId)
{
    return QString::fromLatin1(QCryptographicHash::hash(itemId.toUtf8(), QCryptographicHash::Md5).toHex());
}

static QString snapKey(const QString& itemId)
{
    return QStringLiteral("followsnap/") + profileId() + QStringLiteral("/") + leaf(itemId);
}

static QString cycleKey() { return QStringLiteral("followsnap/") + profileId() + QStringLiteral("/lastCycle"); }

static QJsonObject pendingToJson(const FollowSnapshot::Pending& p)
{
    QJsonObject o;
    o.insert(QStringLiteral("id"), p.id);
    o.insert(QStringLiteral("title"), p.title);
    o.insert(QStringLiteral("subtitle"), p.subtitle);
    o.insert(QStringLiteral("thumbnailUrl"), p.thumbnailUrl);
    o.insert(QStringLiteral("type"), p.type);
    o.insert(QStringLiteral("url"), p.url);
    o.insert(QStringLiteral("mime"), p.mime);
    o.insert(QStringLiteral("foundAt"), static_cast<double>(p.foundAt));
    o.insert(QStringLiteral("count"), p.count);
    return o;
}

static FollowSnapshot::Pending pendingFromJson(const QJsonObject& o)
{
    FollowSnapshot::Pending p;
    p.id           = o.value(QStringLiteral("id")).toString();
    p.title        = o.value(QStringLiteral("title")).toString();
    p.subtitle     = o.value(QStringLiteral("subtitle")).toString();
    p.thumbnailUrl = o.value(QStringLiteral("thumbnailUrl")).toString();
    p.type         = o.value(QStringLiteral("type")).toString();
    p.url          = o.value(QStringLiteral("url")).toString();
    p.mime         = o.value(QStringLiteral("mime")).toString();
    p.foundAt      = static_cast<qint64>(o.value(QStringLiteral("foundAt")).toDouble());
    p.count        = o.value(QStringLiteral("count")).toInt(1);
    return p;
}

FollowSnapshot::Snapshot FollowSnapshot::get(const QString& itemId)
{
    Snapshot s;
    if (itemId.isEmpty()) return s;
    const QJsonObject o =
        QJsonDocument::fromJson(store().value(snapKey(itemId)).toString().toUtf8()).object();
    s.checked     = static_cast<qint64>(o.value(QStringLiteral("checked")).toDouble());
    s.fingerprint = o.value(QStringLiteral("fp")).toString();
    for (const QJsonValue& v : o.value(QStringLiteral("seen")).toArray())
    {
        const QString id = v.toString();
        if (!id.isEmpty()) s.seen << id;
    }
    for (const QJsonValue& v : o.value(QStringLiteral("pending")).toArray())
    {
        const Pending p = pendingFromJson(v.toObject());
        if (!p.id.isEmpty()) s.pending << p;
    }
    return s;
}

static void write(const QString& itemId, const FollowSnapshot::Snapshot& s)
{
    QJsonArray seen;
    for (const QString& id : s.seen) seen.append(id);
    QJsonArray pending;
    for (const FollowSnapshot::Pending& p : s.pending) pending.append(pendingToJson(p));
    QJsonObject o;
    o.insert(QStringLiteral("checked"), static_cast<double>(s.checked));
    o.insert(QStringLiteral("seen"), seen);
    if (!s.fingerprint.isEmpty()) o.insert(QStringLiteral("fp"), s.fingerprint);
    o.insert(QStringLiteral("pending"), pending);
    store().setValue(snapKey(itemId), QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)));
    store().sync();
}

void FollowSnapshot::record(const QString& itemId, const QStringList& seenAfter,
                            const QString& fingerprintAfter, const QVector<Pending>& found, qint64 nowSecs)
{
    if (itemId.isEmpty()) return;
    Snapshot s = get(itemId);
    s.seen        = seenAfter;
    s.fingerprint = fingerprintAfter;
    s.checked     = nowSecs;
    // Append, de-duplicated by child id: a child found two cycles ago and still not dealt with stays exactly
    // once, with the foundAt it FIRST got (its place in the newest-first shelf must not drift upward every
    // time the source is asked again).
    QSet<QString> have;
    for (const Pending& p : std::as_const(s.pending)) have.insert(p.id);
    for (const Pending& p : found)
    {
        if (p.id.isEmpty() || have.contains(p.id)) continue;
        have.insert(p.id);
        s.pending << p;
    }
    write(itemId, s);
}

void FollowSnapshot::markAllSeen(const QString& itemId)
{
    if (itemId.isEmpty()) return;
    Snapshot s = get(itemId);
    if (s.pending.isEmpty()) return;
    s.pending.clear();
    write(itemId, s);
}

void FollowSnapshot::clearPending(const QString& itemId, const QString& childId)
{
    if (itemId.isEmpty() || childId.isEmpty()) return;
    Snapshot s = get(itemId);
    const int before = int(s.pending.size());
    for (int i = int(s.pending.size()) - 1; i >= 0; --i)
        if (s.pending[i].id == childId) s.pending.remove(i);
    if (int(s.pending.size()) == before) return;
    write(itemId, s);
}

void FollowSnapshot::forget(const QString& itemId)
{
    if (itemId.isEmpty()) return;
    store().remove(snapKey(itemId));
    store().sync();
}

qint64 FollowSnapshot::lastCycleAt()
{
    return static_cast<qint64>(store().value(cycleKey(), 0).toLongLong());
}

void FollowSnapshot::setLastCycleAt(qint64 nowSecs)
{
    store().setValue(cycleKey(), static_cast<qlonglong>(nowSecs));
    store().sync();
}

FollowSnapshot::Pending FollowSnapshot::fromChild(const follow::Child& c, qint64 nowSecs)
{
    Pending p;
    p.id           = c.id;
    p.title        = c.title;
    p.subtitle     = c.subtitle;
    p.thumbnailUrl = c.thumbnailUrl;
    p.type         = c.type;
    p.url          = c.url;
    p.mime         = c.mime;
    p.foundAt      = nowSecs;
    p.count        = 1;
    return p;
}
