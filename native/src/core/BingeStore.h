// Remembers which release the user chose for a series, by Stremio's own `bingeGroup` — the mechanism that
// exists so the next episode keeps using the same source. Device-local JSON; never synced.
//
// EPISODES ONLY: a movie has no next episode, so a movie's choice is neither stored nor consulted. Keys are
// the "tt…" prefix of a "tt…:S:E" id — the same series convention MediaSegments::keyFor uses.
// NOT THREAD-SAFE: put() writes the file synchronously on the calling thread. GUI-thread use only.
#pragma once
#include <utility>   // std::move (do not rely on a transitive Qt include)
#include <QHash>
#include <QString>

class BingeStore
{
public:
    explicit BingeStore(QString filePath) : file_(std::move(filePath)) {}
    void load();
    bool save() const;

    // The remembered bingeGroup for this series, or empty.
    QString lookup(const QString& seriesKey) const;
    // Overwrite (the newest choice wins). Returns false on invalid input or a failed write.
    bool put(const QString& seriesKey, const QString& bingeGroup);

    // "tt123:2:7" -> "tt123". Empty for a movie or any id without the S:E tail, which is what makes this
    // episodes-only without the caller having to remember the rule.
    static QString seriesKeyFor(const QString& imdbStreamId);

    // The release already chosen for THIS id's series, if any — the ONE definition every caller shares (the
    // browse page's Play, the manual picker, the next-episode hand-off), so the memory can't be honoured on
    // one path and silently ignored on another. lookup() answers empty for an empty key and seriesKeyFor()
    // answers empty for a movie, so no caller needs an "is it an episode?" test of its own; a null store
    // simply means no memory.
    static QString preferredGroup(const BingeStore* store, const QString& imdbStreamId)
    { return store ? store->lookup(seriesKeyFor(imdbStreamId)) : QString(); }

private:
    QString file_;
    QHash<QString, QString> byKey_;
};
