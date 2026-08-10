// Per-profile saved filter presets for the game library (issue #63). A preset is a user-named gamefilter::
// Filter; the Games category surfaces each one as a virtual shelf (peer of Favourites/Recents) that lists the
// games it matches. Stored as a JSON list in everythingbox.ini, keyed by the active profile — exactly the
// posture FavoritesStore/ItemMarks use (QtCore only, no Quick/Widgets), so it rides the same profile-scoped
// storage favourites do.
//
// Layout: filterpresets/<profile>/items -> JSON array of { id, name, filter:{…}, ts }. `id` is the STABLE
// merge identity (see below); `name` is the user-visible label (save() upserts by name); `ts` is the epoch
// second it was last written, the field the multi-device merge orders by.
//
// CLOUD SYNC (issue #184, the follow-up #63 deferred). This store now rides the CloudMerge document as a
// per-profile namespaced store — the same shape favourites/playlists use — so presets sync across a profile's
// devices. Two rules it inherits from #132/#166 and pays here:
//   * A DELETE LEAVES A DATED TOMBSTONE (Tombstones, store "filterpresets/<profile>", keyed by `id`), never a
//     bare row removal — a bare removal is indistinguishable from "never known", so a peer holding the old
//     copy would re-add the deleted preset on the next merge. The merge prefers the newer of {live edit,
//     delete} by `ts`, exactly as it does for marks/overrides/favourites.
//   * IDENTITY IS AN ID-STABLE FIELD, NOT THE NAME. The merge keys on `id`, and a RENAME is a mutable-name
//     edit that keeps the id — NOT a delete+add. Keying the merge on the name would make a rename look like a
//     "delete X, add Y" pair, and a concurrent edit of X on a peer would then survive alongside the added Y,
//     leaving a spurious duplicate the id-stable form cannot produce (a rename and a concurrent edit collapse
//     onto the one id, newest-ts wins). New presets get a random id at save(); a legacy #63 row (written
//     before this field existed) is given a DETERMINISTIC id derived from its name (same on every device, so
//     two peers' pre-existing copies of the same preset converge instead of duplicating) — see
//     FilterPresetStore::syncIdForName. The one cost of the id-stable form is that two devices that each
//     create a same-named preset OFFLINE get two ids and merge to two rows; that is favourites'/playlists'
//     accepted behaviour, and the right trade against the rename-resurrect race.
#pragma once
#include "GameFilter.h"
#include <QString>
#include <QVector>
#include <functional>

struct FilterPreset
{
    QString            id;     // stable merge identity (random at creation; deterministic for a legacy row). Mutable name rides on top.
    QString            name;   // user-visible label (unique per profile; save() upserts by name)
    gamefilter::Filter filter;
    qint64             ts = 0; // epoch seconds last written — the field CloudMerge orders presets by
};

namespace FilterPresetStore
{
    QVector<FilterPreset> list();                 // for the active profile, newest first
    void save(const FilterPreset& preset);        // upsert by name (a re-save replaces + re-dates)
    void remove(const QString& name);
    bool rename(const QString& oldName, const QString& newName); // false if oldName missing or newName taken
    bool exists(const QString& name);
    FilterPreset get(const QString& name);        // empty name/filter when absent

    // The DETERMINISTIC id a legacy #63 preset (one stored before the `id` field existed) is given, derived
    // from its name so every device computes the SAME id and two peers' pre-existing copies converge on merge
    // instead of duplicating. Pure, profile-independent — CloudMerge's serializer calls it to back-fill an
    // id-less raw row it reads straight off the ini, matching what list()/save() back-fill in-memory here.
    QString syncIdForName(const QString& name);

    // UI refresh hook, mirroring FavoritesStore::setChangeHook: fired after every mutation so the home can
    // rebuild its shelves. A std::function, not a Qt signal (the store stays QtCore-clean); unset in probes.
    void setChangeHook(std::function<void()> hook);
}
