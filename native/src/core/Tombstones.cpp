#include "Tombstones.h"
#include "AppPaths.h"

#include <QSettings>
#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>

// Shares the portable mymediavault.ini with the other stores (same AppPaths::dataDir() posture). Coherence
// with any other QSettings on the same file comes from every writer calling sync().
static QSettings& store_()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/mymediavault.ini"),
                       QSettings::IniFormat);
    return s;
}

namespace {

// Root group for a store's tombstones: "deleted/<store>". <store> may itself be a multi-level path (e.g.
// "favorites/default" or "marks/probeA/tagVocab") — QSettings nests it fine.
QString groupFor(const QString& store)
{
    return QStringLiteral("deleted/") + store;
}

// MD5-hex of the original key — a collision-safe ini leaf (keys can be URL/path-shaped; the original key is
// preserved in the value JSON so all() returns it intact).
QString hashLeaf(const QString& key)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Md5).toHex());
}

qint64 tsOf(const QString& jsonValue)
{
    const QJsonObject o = QJsonDocument::fromJson(jsonValue.toUtf8()).object();
    return static_cast<qint64>(o.value(QStringLiteral("ts")).toDouble());
}

} // namespace

void Tombstones::record(const QString& store, const QString& key)
{
    if (store.isEmpty() || key.isEmpty()) return;
    QJsonObject o;
    o.insert(QStringLiteral("key"), key);
    o.insert(QStringLiteral("ts"), static_cast<double>(QDateTime::currentSecsSinceEpoch()));
    store_().setValue(groupFor(store) + QLatin1Char('/') + hashLeaf(key),
                      QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)));
    store_().sync();
}

QVector<Tombstones::Entry> Tombstones::all(const QString& store)
{
    QVector<Entry> out;
    if (store.isEmpty()) return out;
    QSettings& s = store_();
    s.beginGroup(groupFor(store));
    const QStringList leaves = s.childKeys();
    for (const QString& leaf : leaves)
    {
        const QJsonObject o = QJsonDocument::fromJson(s.value(leaf).toString().toUtf8()).object();
        Entry e;
        e.key = o.value(QStringLiteral("key")).toString();
        e.ts  = static_cast<qint64>(o.value(QStringLiteral("ts")).toDouble());
        if (!e.key.isEmpty()) out.push_back(e);
    }
    s.endGroup();
    return out;
}

int Tombstones::compact(int olderThanDays)
{
    const qint64 cutoff = QDateTime::currentSecsSinceEpoch()
                          - static_cast<qint64>(olderThanDays) * 86400;
    QSettings& s = store_();
    s.beginGroup(QStringLiteral("deleted"));
    // allKeys() is recursive: it returns every leaf across all <store> namespaces (relative to "deleted"),
    // so compaction reaches arbitrarily-nested per-profile store names in one pass.
    const QStringList keys = s.allKeys();
    int dropped = 0;
    for (const QString& k : keys)
    {
        if (tsOf(s.value(k).toString()) < cutoff) { s.remove(k); ++dropped; } // strictly older-than: >N days
    }
    s.endGroup();
    s.sync();
    return dropped;
}
