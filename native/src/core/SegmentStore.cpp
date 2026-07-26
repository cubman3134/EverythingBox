#include "SegmentStore.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <algorithm>

// Replace the same-type entry in place, or append. Shared by put()'s two writes (season key + series key).
// Replacing carries the NEW provenance too: the row now belongs to whichever season last wrote it, which is
// what makes forget() able to remove exactly its own contribution.
void SegmentStore::upsert(QVector<Row>& list, const Row& row)
{
    for (Row& r : list)
        if (r.seg.type == row.seg.type) { r = row; return; }
    list.push_back(row);
}

void SegmentStore::load()
{
    byKey_.clear();
    QFile f(file_);
    if (!f.open(QIODevice::ReadOnly)) return;              // no file yet is a normal empty store
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    for (auto it = root.constBegin(); it != root.constEnd(); ++it)
    {
        QVector<Row> list;
        for (const QJsonValue v : it.value().toArray())
        {
            const QJsonObject o = v.toObject();
            const auto ty = MediaSegments::typeFromString(o.value(QStringLiteral("t")).toString());
            if (!ty) continue;                             // an unknown type from a newer build is skipped
            const double s = o.value(QStringLiteral("s")).toDouble();
            const double e = o.value(QStringLiteral("e")).toDouble();
            if (e <= s) continue;
            // A file written before provenance existed has no "src". Default 0 = unknown origin, which no
            // forget() can match — the safe direction: never guess that an old row belongs to the season
            // being cleared and delete something the user still wants.
            const int src = o.value(QStringLiteral("src")).toInt(0);
            list.push_back(Row{ MediaSegments::Segment{ s, e, *ty }, src });
        }
        if (!list.isEmpty()) byKey_.insert(it.key(), list);
    }
}

bool SegmentStore::save() const
{
    QJsonObject root;
    for (auto it = byKey_.constBegin(); it != byKey_.constEnd(); ++it)
    {
        QJsonArray arr;
        for (const Row& r : it.value())
        {
            QJsonObject o;
            o.insert(QStringLiteral("s"), r.seg.start);
            o.insert(QStringLiteral("e"), r.seg.end);
            o.insert(QStringLiteral("t"), MediaSegments::typeToString(r.seg.type));
            o.insert(QStringLiteral("src"), r.srcSeason);
            arr.append(o);
        }
        root.insert(it.key(), arr);
    }
    // QSaveFile (the MetaCache/GamelistStore pattern): write to a temp sibling and rename on commit(), so a
    // failed open, a short write (disk full, AV lock) or a crash mid-write leaves the PREVIOUS file intact.
    // A plain WriteOnly|Truncate would have already emptied the only copy of ranges the user hand-marked,
    // and load() turns truncated JSON into a silently empty store.
    const QByteArray data = QJsonDocument(root).toJson(QJsonDocument::Compact);
    QSaveFile f(file_);
    if (!f.open(QIODevice::WriteOnly)) return false;
    if (f.write(data) != data.size()) { f.cancelWriting(); return false; }
    return f.commit();
}

QVector<MediaSegments::Segment> SegmentStore::lookup(const QString& seriesKey, int season) const
{
    if (seriesKey.isEmpty()) return {};

    QVector<MediaSegments::Segment> out;
    const auto haveType = [&out](MediaSegments::SegmentType t) {
        for (const MediaSegments::Segment& s : out)
            if (s.type == t) return true;
        return false;
    };
    const auto take = [&](const QVector<Row>& rows) {
        for (const Row& r : rows)
            if (!haveType(r.seg.type)) out.push_back(r.seg);   // provenance is stripped here
    };

    const QString exactKey = keyFor(seriesKey, season);
    take(byKey_.value(exactKey));                          // everything this season says, per type
    if (exactKey != seriesKey)
        take(byKey_.value(seriesKey));                     // and only the types it is silent about
    return out;
}

bool SegmentStore::put(const QString& seriesKey, int season, const MediaSegments::Segment& seg)
{
    if (seriesKey.isEmpty() || seg.end <= seg.start) return false;
    const Row row{ seg, season };
    upsert(byKey_[keyFor(seriesKey, season)], row);
    if (season > 0) upsert(byKey_[seriesKey], row);        // the next unmarked season inherits this
    return save();
}

bool SegmentStore::forget(const QString& seriesKey, int season)
{
    if (seriesKey.isEmpty()) return false;
    const QString exactKey = keyFor(seriesKey, season);
    byKey_.remove(exactKey);
    if (exactKey != seriesKey)
    {
        // …and withdraw this season's contribution to the shared fallback, or lookup() hands the very range
        // that was just deleted straight back and no public call sequence can ever clear it.
        const auto it = byKey_.find(seriesKey);
        if (it != byKey_.end())
        {
            QVector<Row>& rows = it.value();
            rows.erase(std::remove_if(rows.begin(), rows.end(),
                                      [season](const Row& r) { return r.srcSeason == season; }),
                       rows.end());
            if (rows.isEmpty()) byKey_.erase(it);
        }
    }
    return save();
}
