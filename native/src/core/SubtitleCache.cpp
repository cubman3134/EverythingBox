#include "SubtitleCache.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

void SubtitleCache::load()
{
    byKey_.clear();
    QFile f(file_);
    if (!f.open(QIODevice::ReadOnly)) return;
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    for (auto it = root.constBegin(); it != root.constEnd(); ++it)
        byKey_.insert(it.key(), it.value().toString());
}

void SubtitleCache::save() const
{
    QJsonObject root;
    for (auto it = byKey_.constBegin(); it != byKey_.constEnd(); ++it) root.insert(it.key(), it.value());
    QFile f(file_);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

QString SubtitleCache::lookup(const QString& key) const
{
    const QString p = byKey_.value(key);
    if (p.isEmpty() || !QFileInfo::exists(p)) return QString();   // deleted behind our back ⇒ re-fetch
    return p;
}

void SubtitleCache::put(const QString& key, const QString& srtPath)
{
    if (key.isEmpty() || srtPath.isEmpty()) return;
    byKey_.insert(key, srtPath);
    save();
}

void SubtitleCache::clear()
{
    byKey_.clear();
    save();
}
