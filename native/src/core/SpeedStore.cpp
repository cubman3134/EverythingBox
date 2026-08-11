#include "SpeedStore.h"
#include "AppBrand.h"
#include "AppPaths.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDateTime>
#include <QSettings>

// Shares the portable everythingbox.ini with Settings/SyncOffsets/the CloudMerge stores (same AppPaths::dataDir()
// posture). Coherence with any other QSettings on the same file comes from every writer calling sync(); QSettings
// reloads on access when the on-disk file changed.
static QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                       QSettings::IniFormat);
    return s;
}

QString SpeedStore::itemsGroup() { return QStringLiteral("speed/items"); }

QString SpeedStore::hashFor(const QString& key)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Md5).toHex().left(10));
}

static QString itemKey(const QString& key)
{
    return SpeedStore::itemsGroup() + QLatin1Char('/') + SpeedStore::hashFor(key);
}

double SpeedStore::storedForItem(const QString& key)
{
    if (key.isEmpty()) return 0.0;
    const QByteArray raw = store().value(itemKey(key)).toString().toUtf8();
    if (raw.isEmpty()) return 0.0;
    // A malformed blob, or one carrying a non-positive rate, reads back as unset (0.0) — never a bogus 0x speed.
    const double r = QJsonDocument::fromJson(raw).object().value(QStringLiteral("rate")).toDouble();
    return r > 0.0 ? r : 0.0;
}

void SpeedStore::setForItem(const QString& key, double rate)
{
    if (key.isEmpty() || rate <= 0.0) return; // an empty key has no identity; a <=0 rate is meaningless
    QJsonObject o;
    o.insert(QStringLiteral("rate"), rate);
    o.insert(QStringLiteral("updatedAt"), double(QDateTime::currentSecsSinceEpoch()));
    store().setValue(itemKey(key), QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)));
    store().sync();
}

double SpeedStore::speedForItem(double storedForItem, double globalDefault, bool isMusic)
{
    if (storedForItem > 0.0) return storedForItem;              // an explicit per-item choice wins for anything
    if (isMusic)             return 1.0;                        // music stays 1x unless explicitly set
    return globalDefault > 0.0 ? globalDefault : 1.0;          // else the global default, guarded against 0/corrupt
}
