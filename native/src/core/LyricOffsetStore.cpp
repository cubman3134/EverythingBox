#include "LyricOffsetStore.h"
#include "AppBrand.h"
#include "AppPaths.h"
#include "../media/LyricSeek.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>

// Shares the portable everythingbox.ini with Settings/SpeedStore/the CloudMerge stores (same AppPaths::dataDir()
// posture). Coherence with any other QSettings on the same file comes from every writer calling sync(); QSettings
// reloads on access when the on-disk file changed.
static QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                       QSettings::IniFormat);
    return s;
}

QString LyricOffsetStore::itemsGroup() { return QStringLiteral("lyricoffset/items"); }

QString LyricOffsetStore::hashFor(const QString& key)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Md5).toHex().left(10));
}

static QString itemKey(const QString& key)
{
    return LyricOffsetStore::itemsGroup() + QLatin1Char('/') + LyricOffsetStore::hashFor(key);
}

double LyricOffsetStore::forItem(const QString& key)
{
    if (key.isEmpty()) return 0.0;
    const QByteArray raw = store().value(itemKey(key)).toString().toUtf8();
    if (raw.isEmpty()) return 0.0;
    // Sanitised on the way OUT as well as in: a blob hand-edited into the ini, or written by a build with a
    // different clamp, must not put an off-grid or unbounded offset into the sync maths.
    return LyricSeek::clampOffset(
        QJsonDocument::fromJson(raw).object().value(QStringLiteral("offset")).toDouble());
}

void LyricOffsetStore::setForItem(const QString& key, double offsetSec)
{
    if (key.isEmpty()) return; // an item with no identity has nowhere to remember anything
    QJsonObject o;
    o.insert(QStringLiteral("offset"), LyricSeek::clampOffset(offsetSec));
    o.insert(QStringLiteral("updatedAt"), double(QDateTime::currentSecsSinceEpoch()));
    store().setValue(itemKey(key), QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)));
    store().sync();
}
