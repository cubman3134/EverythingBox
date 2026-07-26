#include "SegmentStore.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace {

// Replace the same-type entry in place, or append. Shared by put()'s two writes (season key + series key).
void upsert(QVector<MediaSegments::Segment>& list, const MediaSegments::Segment& seg)
{
    for (MediaSegments::Segment& s : list)
        if (s.type == seg.type) { s = seg; return; }
    list.push_back(seg);
}

} // namespace

void SegmentStore::load()
{
    byKey_.clear();
    QFile f(file_);
    if (!f.open(QIODevice::ReadOnly)) return;              // no file yet is a normal empty store
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    for (auto it = root.constBegin(); it != root.constEnd(); ++it)
    {
        QVector<MediaSegments::Segment> list;
        for (const QJsonValue v : it.value().toArray())
        {
            const QJsonObject o = v.toObject();
            const auto ty = MediaSegments::typeFromString(o.value(QStringLiteral("t")).toString());
            if (!ty) continue;                             // an unknown type from a newer build is skipped
            const double s = o.value(QStringLiteral("s")).toDouble();
            const double e = o.value(QStringLiteral("e")).toDouble();
            if (e <= s) continue;
            list.push_back(MediaSegments::Segment{ s, e, *ty });
        }
        if (!list.isEmpty()) byKey_.insert(it.key(), list);
    }
}

void SegmentStore::save() const
{
    QJsonObject root;
    for (auto it = byKey_.constBegin(); it != byKey_.constEnd(); ++it)
    {
        QJsonArray arr;
        for (const MediaSegments::Segment& s : it.value())
        {
            QJsonObject o;
            o.insert(QStringLiteral("s"), s.start);
            o.insert(QStringLiteral("e"), s.end);
            o.insert(QStringLiteral("t"), MediaSegments::typeToString(s.type));
            arr.append(o);
        }
        root.insert(it.key(), arr);
    }
    QFile f(file_);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

QVector<MediaSegments::Segment> SegmentStore::lookup(const QString& seriesKey, int season) const
{
    if (seriesKey.isEmpty()) return {};
    const QVector<MediaSegments::Segment> exact = byKey_.value(keyFor(seriesKey, season));
    if (!exact.isEmpty()) return exact;
    return byKey_.value(seriesKey);                        // the series-level fallback
}

void SegmentStore::put(const QString& seriesKey, int season, const MediaSegments::Segment& seg)
{
    if (seriesKey.isEmpty() || seg.end <= seg.start) return;
    upsert(byKey_[keyFor(seriesKey, season)], seg);
    if (season > 0) upsert(byKey_[seriesKey], seg);        // the next unmarked season inherits this
    save();
}

void SegmentStore::forget(const QString& seriesKey, int season)
{
    if (seriesKey.isEmpty()) return;
    byKey_.remove(keyFor(seriesKey, season));
    save();
}
