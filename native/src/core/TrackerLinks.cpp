#include "TrackerLinks.h"
#include "AppBrand.h"
#include "AppPaths.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>

// Shares the portable everythingbox.ini with Settings/SpeedStore/LyricOffsetStore (same AppPaths::dataDir()
// posture). Coherence with any other QSettings on the same file comes from every writer calling sync().
static QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                       QSettings::IniFormat);
    return s;
}

static std::function<void()>& hook()
{
    static std::function<void()> h;
    return h;
}

void TrackerLinks::setChangeHook(std::function<void()> h) { hook() = std::move(h); }

QString TrackerLinks::itemsGroup() { return tracker::linkKeyPrefix() + QStringLiteral("items"); }

QString TrackerLinks::hashFor(tracker::Id id, const QString& itemKey)
{
    // The tracker token is part of the HASHED INPUT, not a path segment: an item key is arbitrary user data
    // and could otherwise be crafted to land on another tracker's leaf.
    const QString material = tracker::idToken(id) + QLatin1Char('\n') + itemKey;
    return QString::fromLatin1(
        QCryptographicHash::hash(material.toUtf8(), QCryptographicHash::Md5).toHex().left(10));
}

static QString rowKey(tracker::Id id, const QString& itemKey)
{
    return TrackerLinks::itemsGroup() + QLatin1Char('/') + TrackerLinks::hashFor(id, itemKey);
}

QString TrackerLinks::encode(const Link& l)
{
    QJsonObject o;
    o.insert(QStringLiteral("media"), l.mediaId);
    o.insert(QStringLiteral("kind"), tracker::kindToken(l.kind));
    o.insert(QStringLiteral("title"), l.title);
    o.insert(QStringLiteral("total"), l.totalUnits);
    o.insert(QStringLiteral("local"), l.localUnits);
    o.insert(QStringLiteral("declined"), l.declined);
    o.insert(QStringLiteral("updatedAt"), double(l.updatedAt));
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

TrackerLinks::Link TrackerLinks::decode(const QString& blob)
{
    Link l;
    if (blob.isEmpty()) return l;
    const QJsonObject o = QJsonDocument::fromJson(blob.toUtf8()).object();
    l.mediaId = o.value(QStringLiteral("media")).toString();
    l.kind = o.value(QStringLiteral("kind")).toString() == QLatin1String("manga") ? tracker::Kind::Manga
                                                                                  : tracker::Kind::Anime;
    l.title = o.value(QStringLiteral("title")).toString();
    l.totalUnits = o.value(QStringLiteral("total")).toInt();
    l.localUnits = o.value(QStringLiteral("local")).toInt();
    l.declined = o.value(QStringLiteral("declined")).toBool();
    l.updatedAt = static_cast<qint64>(o.value(QStringLiteral("updatedAt")).toDouble());
    return l;
}

TrackerLinks::Link TrackerLinks::get(tracker::Id id, const QString& itemKey)
{
    if (itemKey.isEmpty()) return Link{};
    return decode(store().value(rowKey(id, itemKey)).toString());
}

static void write(tracker::Id id, const QString& itemKey, const TrackerLinks::Link& l)
{
    store().setValue(rowKey(id, itemKey), TrackerLinks::encode(l));
    store().sync();
}

void TrackerLinks::set(tracker::Id id, const QString& itemKey, const QString& mediaId, tracker::Kind kind,
                       const QString& title, int totalUnits)
{
    if (itemKey.isEmpty() || mediaId.isEmpty()) return;  // an item with no identity has nowhere to remember
    Link l;
    l.mediaId = mediaId;
    l.kind = kind;
    l.title = title;
    l.totalUnits = qMax(0, totalUnits);
    l.declined = false;   // choosing a match answers the question a refusal declined; see the header
    l.updatedAt = QDateTime::currentSecsSinceEpoch();
    write(id, itemKey, l);
    if (hook()) hook()();
}

void TrackerLinks::clear(tracker::Id id, const QString& itemKey)
{
    if (itemKey.isEmpty()) return;
    Link l = get(id, itemKey);
    if (!l.linked() && !l.declined) return;      // nothing to unlink; do not arm a sync push for a no-op
    // The HUSK, not a removal: a peer holding the old link must lose to a newer record, and a deleted ini
    // row would simply be re-merged back in from that peer's copy on the next sync.
    l.mediaId.clear();
    l.title.clear();
    l.totalUnits = 0;
    l.localUnits = 0;     // the app's side of the reconciliation belonged to the link that is being dropped
    l.declined = false;   // unlinking is not refusing — the next progress event may offer the prompt again
    l.updatedAt = QDateTime::currentSecsSinceEpoch();
    write(id, itemKey, l);
    if (hook()) hook()();
}

void TrackerLinks::decline(tracker::Id id, const QString& itemKey)
{
    if (itemKey.isEmpty()) return;
    Link l = get(id, itemKey);
    if (l.declined) return;
    l.declined = true;
    l.updatedAt = QDateTime::currentSecsSinceEpoch();
    write(id, itemKey, l);
    if (hook()) hook()();
}

bool TrackerLinks::noteLocalProgress(tracker::Id id, const QString& itemKey, int unit)
{
    if (itemKey.isEmpty() || unit <= 0) return false;
    Link l = get(id, itemKey);
    if (!l.linked()) return false;      // nothing for the number to be the progress of
    if (unit <= l.localUnits) return false;  // monotonic: re-reading an early chapter regresses nothing
    l.localUnits = unit;
    l.updatedAt = QDateTime::currentSecsSinceEpoch();
    write(id, itemKey, l);
    if (hook()) hook()();
    return true;
}

bool TrackerLinks::shouldPrompt(tracker::Id id, const QString& itemKey)
{
    if (itemKey.isEmpty()) return false;
    const Link l = get(id, itemKey);
    return !l.linked() && !l.declined;
}
