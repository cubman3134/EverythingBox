// The LEARNED tier of intro/credits detection: ranges the user marked once, remembered against a season.
//
// Keyed "<seriesKey>|s<N>", one entry per marked season, and nothing else. Openings genuinely change between
// seasons (this is why Jellyfin fingerprints per season), but most shows never change theirs, so an unmarked
// season INHERITS a marked one rather than demanding a fresh mark. That inheritance is DERIVED by lookup()
// from the season entries themselves — put() never keeps a second, series-level copy of the mark. Deriving it
// is what makes forget() a plain remove of one key: there is no copy left behind to resurrect a range the
// user just cleared, and no other season's contribution to destroy while clearing this one.
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
    // false when the file could not be written (see the QSaveFile note in the .cpp). The in-memory store is
    // updated BEFORE the save is attempted, so a failed save does not lose the write: the range stays learned
    // for this run and the next successful save persists it. What a failure costs is durability, not data.
    bool save() const;

    // Per TYPE, exactly like MediaSegments::resolve: every type the requested season's own entry supplies,
    // then each type it does NOT supply, taken from the nearest other marked season. Whole-list fallback
    // would mean that marking the credits of an unmarked season silently retired an intro that had been
    // inheriting and working. See the .cpp for why the fallback is the nearest season and not the highest.
    QVector<MediaSegments::Segment> lookup(const QString& seriesKey, int season) const;

    // Replaces any segment of the SAME type for that season and leaves other types alone, so marking credits
    // never clobbers a learned intro. Writes exactly ONE key — keyFor(seriesKey, season). Returns false
    // either because the input was rejected (empty seriesKey, or a range that does not run forwards), in
    // which case nothing was stored and no save was attempted, or because save() failed. The caller cannot
    // tell those apart from the return value and does not need to: false means "this is not on disk".
    bool put(const QString& seriesKey, int season, const MediaSegments::Segment& seg);
    // Removes exactly the entry for that season and nothing else, because nothing else holds a copy of it.
    // Every other season keeps its mark, and lookup() goes on inheriting from whichever of them is now
    // nearest — so clearing one season never silently drops the skip a different season was using.
    // Returns save().
    bool forget(const QString& seriesKey, int season);

    // season <= 0 means "unknown", which keys the bare series entry rather than a bogus "|s0".
    static QString keyFor(const QString& seriesKey, int season)
    {
        return season > 0 ? seriesKey + QStringLiteral("|s") + QString::number(season) : seriesKey;
    }

private:
    QString file_;
    // Rows are plain ranges: everything lookup() needs to rank a mark is already in its KEY. A row carried a
    // "src" season for as long as the series-level copy existed and had to be told apart from the season that
    // wrote it; both are gone, and an old file's stray "src" is ignored on load.
    QHash<QString, QVector<MediaSegments::Segment>> byKey_;
};
