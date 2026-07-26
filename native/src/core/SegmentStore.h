// The LEARNED tier of intro/credits detection: ranges the user marked once, remembered against a season.
//
// Keyed "<seriesKey>|s<N>" with a bare "<seriesKey>" fallback, because openings genuinely change between
// seasons (this is why Jellyfin fingerprints per season) — but most shows never change theirs, so put()
// writes BOTH keys and an unmarked season inherits the most recent mark instead of demanding a fresh one.
// Device-local JSON, never synced: it is a small personal preference, not library metadata.
//
// put() and forget() do SYNCHRONOUS file I/O on the calling thread (the SubtitleCache precedent). They are
// called from user actions — marking or clearing one range — so the write is a single small JSON file, but
// they must not be called from a hot loop or a worker thread expecting them to be cheap or synchronised.
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
    // false when the file could not be written (see the QSaveFile note in the .cpp). The store stays valid
    // in memory either way — a failed save loses the write, never the ranges already learned.
    bool save() const;

    // Per TYPE, exactly like MediaSegments::resolve: every type the season entry supplies, then each type it
    // does NOT supply filled from the series-level fallback. Whole-list fallback would mean that marking the
    // credits of an unmarked season silently retired an intro that had been inheriting and working.
    QVector<MediaSegments::Segment> lookup(const QString& seriesKey, int season) const;
    // Replaces any segment of the SAME type at that scope and leaves other types alone, so marking credits
    // never clobbers a learned intro. Writes the season key and the bare series key together. Returns save().
    bool put(const QString& seriesKey, int season, const MediaSegments::Segment& seg);
    // Drops this season's entry AND every row this season contributed to the series-level entry, so a mark
    // can actually be taken back. Rows another season wrote survive, so lookup still falls back to the most
    // recent OTHER mark rather than going dark. Returns save().
    bool forget(const QString& seriesKey, int season);

    // season <= 0 means "unknown", which keys the bare series entry rather than a bogus "|s0".
    static QString keyFor(const QString& seriesKey, int season)
    {
        return season > 0 ? seriesKey + QStringLiteral("|s") + QString::number(season) : seriesKey;
    }

private:
    // Provenance — WHICH season wrote this row — lives here and nowhere else. Without it the bare series
    // entry is indistinguishable from a copy of the thing being deleted, so forget() cannot tell "an older
    // season's mark worth inheriting" from "the mark the user just asked to remove" and lookup() resurrects
    // it byte-identical. It is deliberately NOT a field on MediaSegments::Segment: that type is shared with
    // the .edl and chapter providers and must stay a pure range. lookup() strips it on the way out.
    struct Row
    {
        MediaSegments::Segment seg;
        int                    srcSeason = 0;   // 0 = unknown (a pre-provenance file); never matched by forget
    };

    // Replace the same-type row in place, or append. Shared by put()'s two writes. A member rather than a
    // free helper only because Row is private.
    static void upsert(QVector<Row>& list, const Row& row);

    QString file_;
    QHash<QString, QVector<Row>> byKey_;
};
