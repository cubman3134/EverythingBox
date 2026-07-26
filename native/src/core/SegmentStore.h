// The LEARNED tier of intro/credits detection: ranges the user marked once, remembered against a season.
//
// Keyed "<seriesKey>|s<N>" with a bare "<seriesKey>" fallback, because openings genuinely change between
// seasons (this is why Jellyfin fingerprints per season) — but most shows never change theirs, so put()
// writes BOTH keys and an unmarked season inherits the most recent mark instead of demanding a fresh one.
// Device-local JSON, never synced: it is a small personal preference, not library metadata.
#pragma once
#include "MediaSegments.h"

#include <utility>   // std::move (do not rely on a transitive Qt include)
#include <QHash>
#include <QString>
#include <QVector>

class SegmentStore
{
public:
    explicit SegmentStore(QString filePath) : file_(std::move(filePath)) {}
    void load();
    void save() const;

    // The season's segments, else the series-level fallback, else empty.
    QVector<MediaSegments::Segment> lookup(const QString& seriesKey, int season) const;
    // Replaces any segment of the SAME type at that scope and leaves other types alone, so marking credits
    // never clobbers a learned intro. Writes the season key and the bare series key together.
    void put(const QString& seriesKey, int season, const MediaSegments::Segment& seg);
    // Drops this season's entry. The series-level entry survives, so lookup falls back rather than going dark.
    void forget(const QString& seriesKey, int season);

    // season <= 0 means "unknown", which keys the bare series entry rather than a bogus "|s0".
    static QString keyFor(const QString& seriesKey, int season)
    {
        return season > 0 ? seriesKey + QStringLiteral("|s") + QString::number(season) : seriesKey;
    }

private:
    QString file_;
    QHash<QString, QVector<MediaSegments::Segment>> byKey_;
};
