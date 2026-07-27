#include "BingeStore.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStringList>

void BingeStore::load()
{
    byKey_.clear();
    QFile f(file_);
    if (!f.open(QIODevice::ReadOnly)) return;        // no file yet is a normal empty store
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    for (auto it = root.constBegin(); it != root.constEnd(); ++it)
    {
        const QString v = it.value().toString();
        if (!v.isEmpty()) byKey_.insert(it.key(), v);
    }
}

bool BingeStore::save() const
{
    QJsonObject root;
    for (auto it = byKey_.constBegin(); it != byKey_.constEnd(); ++it) root.insert(it.key(), it.value());
    // QSaveFile, not truncate-then-write: this file is the only record of a choice the user made by hand,
    // and a short write (disk full, AV lock) would otherwise leave truncated JSON that loads as empty.
    // Same reasoning as SegmentStore.
    QSaveFile f(file_);
    if (!f.open(QIODevice::WriteOnly)) return false;
    const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Compact);
    if (f.write(bytes) != bytes.size()) { f.cancelWriting(); return false; }
    return f.commit();
}

QString BingeStore::lookup(const QString& seriesKey) const
{
    return seriesKey.isEmpty() ? QString() : byKey_.value(seriesKey);
}

bool BingeStore::put(const QString& seriesKey, const QString& bingeGroup)
{
    if (seriesKey.isEmpty() || bingeGroup.isEmpty()) return false;
    byKey_.insert(seriesKey, bingeGroup);            // the newest choice wins
    return save();
}

QString BingeStore::seriesKeyFor(const QString& imdbStreamId)
{
    // "tt123:2:7" -> "tt123"; a movie ("tt123") or any other shape -> empty. Returning empty for a movie is
    // what makes this episodes-only WITHOUT every caller having to remember the rule.
    const QStringList p = imdbStreamId.split(QLatin1Char(':'));
    if (p.size() != 3) return QString();
    if (!p[0].startsWith(QLatin1String("tt"))) return QString();
    return p[0];
}
