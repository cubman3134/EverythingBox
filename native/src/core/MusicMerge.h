// ONE LIBRARY OUT OF SEVERAL (issue #194, increment 1) — the local music library and every configured
// Subsonic server, folded into a single MusicLibrary::Index that #74's own browse builders render.
//
// Pure: indices in, an index out. No settings, no network, no UI, no clock. The only thing it reaches for is
// MusicId's override store, which is the user's own recorded verdicts and is the one input that cannot be a
// parameter without making every caller carry it.
//
// ==================================================================================================
// THE KEY OF A MERGED ROW IS ONE OF ITS SOURCES' OWN KEYS. NOTHING NEW IS MINTED.
// ==================================================================================================
// This is the decision the whole design rests on, so it is stated first. A merged artist or album is
// rendered under the key of the instance that was PICKED to represent it — a real MusicLibrary key, or a
// real Subsonic-qualified id. It is never a new "merged:" namespace.
//
// The consequence is that everything downstream keeps working with no change at all: MusicSupply::indexFor
// still routes the key to the supplier that owns it, MusicSupply::playUrl still turns it into a file path or
// a signed stream url, SubsonicClient still knows which server to fetch the album's tracks from, and every
// store that already keys on a music key — resume positions, consumption seconds, the queue-to-album map —
// is untouched. A "merged:" namespace would have required all of them to learn to un-merge a key before
// using it, which is a great deal of surface for the same picture on screen.
//
// The honest cost, and it is real: the merged identity is only as stable as the pick. Change the preferred
// source and a merged album is rendered under the other copy's key, so anything already banked under the
// first one stays there. That is the same limit PcGameId documents for its own effective id, it is bounded
// by "the user changed a setting", and it is why pickAutoSource is a total, deterministic function of the
// preference and the source order rather than of anything that varies run to run (see MusicId.h).
//
// ==================================================================================================
// FEWER THAN TWO SOURCES WITH CONTENT: THE INPUT IS RETURNED VERBATIM
// ==================================================================================================
// merge() short-circuits. With one supplier there is nothing to merge, and rebuilding the index anyway —
// re-grouping, re-sorting, recomputing counts — would put a whole new code path underneath a library that
// has not changed, where a difference of one row is a regression nobody would look for. So the one index is
// COPIED OUT, byte for byte, and `active` is false. probe_musicid pins that equality; it is what makes "a
// user with only a local library sees exactly what they see today" a checkable claim rather than an
// argument.
//
// ==================================================================================================
// WHY BUCKETING BY KEY IS COMPLETE, AND WHY THE PROBE MUST KEEP CHECKING IT
// ==================================================================================================
// The obvious implementation compares every instance with every other one — 500 local artists against 500
// remote ones is 250,000 predicate calls, each normalising four strings. So instead the candidate pairs come
// out of hash buckets: the normalised key, each MusicBrainz id, and the two sides of every "these ARE the
// same" verdict the user has recorded.
//
// That is complete only because of a structural property of MusicId's predicate: it can answer "merge" ONLY
// when the MBIDs agree, or the normalised keys are equal, or the user said so. Every bucket above is one of
// those three, so no merging pair can fall outside them. That property lives in another file, which is
// exactly the kind of coupling that rots — so probe_musicid drives merge() against a brute-force O(n^2)
// application of the predicate over the same inputs and requires identical grouping. If someone widens the
// predicate without widening the buckets, that probe goes red.
//
// ==================================================================================================
// TWO INSTANCES FROM THE SAME SOURCE NEVER MERGE
// ==================================================================================================
// A supplier has already grouped its own library; two rows it kept apart are two records it says the user
// owns. Fusing them here would hide one of them, which is the failure this whole feature is biased against,
// and it would do it to a source that was perfectly correct. The guard is enforced on the UNION, not just on
// the pair, so a transitive chain (local A - server B - local C) cannot smuggle two local albums into one
// group either.
#pragma once
#include "MusicLibrary.h"

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

namespace MusicMerge
{
    // One supplier. `id` is empty for the LOCAL library and is the server's uuid for a Subsonic server —
    // the same string MusicId::SourceRef and Subsonic::qualify use, so the preference can be expressed once.
    // A null or empty index is legal and contributes nothing (a server whose artists have not been fetched
    // yet is exactly that, and the Music root is reachable before any fetch lands).
    struct Source
    {
        QString                    id;
        const MusicLibrary::Index* index = nullptr;
    };

    struct Merged
    {
        MusicLibrary::Index idx;

        // A merged row's key -> EVERY instance key in its group, the primary FIRST and the rest in source
        // order. A key with no entry here is its own only instance; callers use instancesOf() rather than
        // testing that, so a single-source library needs no special case anywhere.
        QHash<QString, QStringList> artistGroup;
        QHash<QString, QStringList> albumGroup;

        // Every instance key -> the source it came from ("" == local). Populated for merged groups only, for
        // the same reason: an unlisted key belongs to whichever supplier its own shape says it does, which
        // MusicSupply can already answer.
        QHash<QString, QString> sourceOf;

        // False when merge() short-circuited: `idx` is one supplier's index verbatim and both maps are empty.
        bool active = false;

        // The instances of a merged row, primary first — or just {key} when it is not a merged row. Never
        // empty for a non-empty key.
        QStringList artistInstances(const QString& key) const;
        QStringList albumInstances(const QString& key) const;
    };

    // `preference` is MusicId's stored source preference ("local", "server", or a server id); it decides
    // which instance of each group is the one the merged row is keyed and played from.
    Merged merge(const QVector<Source>& sources, const QString& preference);
}
