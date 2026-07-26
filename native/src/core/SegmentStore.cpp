#include "SegmentStore.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <algorithm>
#include <utility>   // std::pair

namespace
{
    // Replace the same-type segment in place, or append: marking credits must not disturb a learned intro.
    void upsert(QVector<MediaSegments::Segment>& list, const MediaSegments::Segment& seg)
    {
        for (MediaSegments::Segment& s : list)
            if (s.type == seg.type) { s = seg; return; }
        list.push_back(seg);
    }
}

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
            // A file written while the series-level copy existed carries a "src" season on every row. Nothing
            // reads it now that the fallback is derived from the season keys, so it is simply dropped on the
            // way in — an unrecognised field is never a load error. This file is the only copy of ranges the
            // user hand-marked; refusing to load one over a field we no longer need would throw them away.
            list.push_back(MediaSegments::Segment{ s, e, *ty });
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
    const auto take = [&](const QVector<MediaSegments::Segment>& rows) {
        for (const MediaSegments::Segment& s : rows)
            if (!haveType(s.type)) out.push_back(s);
    };

    const QString exactKey = keyFor(seriesKey, season);
    take(byKey_.value(exactKey));                          // everything this season says, per type

    // …and, for the types it is silent about, the NEAREST other marked season of the same series: the closest
    // one BELOW the requested season if there is one, else the closest above. Nearest and not highest is
    // deliberate — if seasons 1-3 share an opening and season 5 changed it, an unmarked season 2 should
    // inherit season 1's mark, not season 5's; a highest-season rule would hand every earlier season the
    // newest range, which is exactly the one known to be wrong for them.
    const int     want   = season > 0 ? season : 0;
    const QString prefix = seriesKey + QStringLiteral("|s");
    QVector<std::pair<int, QString>> others;               // (season, key); 0 = the unknown-season mark
    for (auto it = byKey_.constBegin(); it != byKey_.constEnd(); ++it)
    {
        if (it.key() == exactKey) continue;                // already taken, and never its own fallback
        int n = 0;                                         // the bare key is this series at unknown season 0
        if (it.key() != seriesKey)
        {
            if (!it.key().startsWith(prefix)) continue;     // another series (or a longer key that shares it)
            bool ok = false;
            n = it.key().mid(prefix.size()).toInt(&ok);
            if (!ok || n <= 0) continue;
        }
        others.push_back({ n, it.key() });
    }
    // Rank: nearest below, then nearest above, then the unknown-season mark last of all — it still gets to
    // supply a type nobody else does, but never outranks a mark that names the season it came from. The
    // season number is unique per key, so ties are impossible and the QHash's iteration order cannot reach
    // the result.
    const auto rank = [want](int n) -> std::pair<int, int> {
        if (n <= 0)   return { 2, 0 };
        if (n < want) return { 0, want - n };
        return { 1, n - want };
    };
    std::sort(others.begin(), others.end(),
              [&rank](const std::pair<int, QString>& a, const std::pair<int, QString>& b) {
                  return rank(a.first) < rank(b.first);
              });
    for (const std::pair<int, QString>& o : others) take(byKey_.value(o.second));
    return out;
}

QVector<MediaSegments::Segment> SegmentStore::lookupExact(const QString& seriesKey, int season) const
{
    if (seriesKey.isEmpty()) return {};
    // Exactly the key put() writes, and nothing else — this is the ONE place that must not consult the derived
    // nearest-season fallback, so it does not share lookup()'s body.
    return byKey_.value(keyFor(seriesKey, season));
}

bool SegmentStore::put(const QString& seriesKey, int season, const MediaSegments::Segment& seg)
{
    if (seriesKey.isEmpty() || seg.end <= seg.start) return false;   // nothing stored, no save attempted
    upsert(byKey_[keyFor(seriesKey, season)], seg);
    return save();
}

bool SegmentStore::forget(const QString& seriesKey, int season)
{
    if (seriesKey.isEmpty()) return false;
    byKey_.remove(keyFor(seriesKey, season));   // the only place this mark lives
    return save();
}
