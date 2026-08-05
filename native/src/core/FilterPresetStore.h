// Per-profile saved filter presets for the game library (issue #63). A preset is a user-named gamefilter::
// Filter; the Games category surfaces each one as a virtual shelf (peer of Favourites/Recents) that lists the
// games it matches. Stored as a JSON list in everythingbox.ini, keyed by the active profile — exactly the
// posture FavoritesStore/ItemMarks use (QtCore only, no Quick/Widgets), so it rides the same profile-scoped
// storage favourites do.
//
// Layout: filterpresets/<profile>/items -> JSON array of { name, filter:{…}, ts }. The name is the preset's
// identity (save() upserts by name); ts is the epoch second it was last written (kept for a future multi-
// device merge — see the note below).
//
// CLOUD SYNC IS DEFERRED (issue #63 calls it a "candidate", not a requirement). This store is LOCAL ONLY: it
// does not tombstone deletes or wire a CloudMerge serializer, because a synced preset store inherits the
// husk/tombstone resurrection rules (#132/#166) — a deleted preset must not come back from a peer — which is
// its own deliverable. The `ts` field is here so that follow-up has a per-item stamp to merge on, and nothing
// more. Until then, add/remove/rename are plain local writes.
#pragma once
#include "GameFilter.h"
#include <QString>
#include <QVector>
#include <functional>

struct FilterPreset
{
    QString            name;   // user-visible, the preset's identity (unique per profile)
    gamefilter::Filter filter;
    qint64             ts = 0; // epoch seconds last written (reserved for a future multi-device merge)
};

namespace FilterPresetStore
{
    QVector<FilterPreset> list();                 // for the active profile, newest first
    void save(const FilterPreset& preset);        // upsert by name (a re-save replaces + re-dates)
    void remove(const QString& name);
    bool rename(const QString& oldName, const QString& newName); // false if oldName missing or newName taken
    bool exists(const QString& name);
    FilterPreset get(const QString& name);        // empty name/filter when absent

    // UI refresh hook, mirroring FavoritesStore::setChangeHook: fired after every mutation so the home can
    // rebuild its shelves. A std::function, not a Qt signal (the store stays QtCore-clean); unset in probes.
    void setChangeHook(std::function<void()> hook);
}
