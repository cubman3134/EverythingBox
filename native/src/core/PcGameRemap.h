// Moving the user's per-item records from the OLD per-launcher game ids ("steam:1145360", "gog:1207658930",
// "epic:<appName>", "bnet:<code>") onto the merged id the catalog now builds — pcgame::itemId, which is the
// one function both this unit and pcGamesCatalog call so the two cannot drift apart.
// Everything the user personally accrued against a game — hidden/completion/tags, play time, sessions, the
// star, a resume position — is keyed on the id. Change the id without moving the records and every one of
// them is silently orphaned: the play time reads zero, the star is gone, the completion mark is gone. This
// unit is the half of the merge that keeps those.
//
// WHY IT IS REPEATABLE, NOT A ONE-SHOT MIGRATION
// ---------------------------------------------
// ItemMarks, ConsumptionStats and PlayStats all store a record under a HASH of the caller key
// (ItemMarks.cpp:46, ConsumptionStats.cpp:96, PlayStats.cpp:29). Storage therefore holds md5/sha1 tokens and
// nothing else, and a hash is one-way: there is NO way to enumerate "every steam: record" and rewrite it.
// The only way to find a record is to already know the id — which means deriving candidate old ids from the
// CURRENT library (the four launcher scans + the downloaded-games store) and hashing each one.
//
// A game the user does not have installed right now contributes no candidate id, so its records cannot be
// located. A one-shot pass would therefore mark itself done and strand those records forever — the user
// reinstalls a game six months later and finds 200 hours of play time gone. Running the remap on every
// library refresh instead costs a few hundred hash lookups and migrates a reinstalled game the moment it
// reappears. That is why applyRemap is idempotent by construction rather than guarded by a schema stamp:
// running it twice equals running it once, so it is safe to run always.
//
// THE FOUR RULES THIS FILE IS BUILT AROUND
// ----------------------------------------
//   1. An id with no merged destination is ABSENT from the table — never mapped to an empty string. A
//      caller that trusted an empty value would write "" as a storage key and destroy the record.
//   2. A record is never removed until its replacement has been written AND read back successfully.
//   3. When the destination already holds a record, the two are MERGED, never overwritten. Two launcher
//      entries collapsing into one game means two records collapsing into one; discarding either loses real
//      play time, a favourite or a completion mark. The per-store rules are documented in the .cpp.
//   4. Running the remap TWICE equals running it once — INCLUDING after a failed run. Rule 2 alone does not
//      give this: playstats writes three keys from a SUM, and a run that wrote one of them and then hit a
//      write error left the destination already summed with the source still present, so the next refresh
//      re-summed it. That inflated play time on every refresh thereafter, and it only happened on the path
//      nobody exercises. A per-record journal marker now carries the absolute values, so a retry COMMITS
//      the pending move rather than recomputing it. See remapPlayStats in the .cpp.
#pragma once
#include <QHash>
#include <QPair>
#include <QString>
#include <QVector>
#include <functional>

namespace pcgame
{
    // old id -> merged id, for every entry that HAS a merged id. `oldIdToTitle` is the current library
    // flattened to (id, title) pairs.
    //
    // THE DESTINATION IS pcgame::itemId(title) AND NOTHING ELSE. It is not computed here, and there is
    // deliberately no metadata argument: this function used to take a title->igdb map and prefer the id
    // it supplied, while pcGamesCatalog keyed on the title alone. The moment a caller populated that map
    // — which the parameter openly invited — every record would have been moved to an id no catalog
    // lookup performs, stranding the user's favourites, marks and play time under an unreachable key,
    // silently. Removing the parameter is the fix: a caller that has metadata now gets a COMPILE ERROR
    // instead of a data loss. See pcgame::itemId for why the id is title-only.
    //
    // An entry with an empty id or an empty title is OMITTED (rule 1). An entry that already carries the
    // merged id maps to ITSELF, so feeding the table's own output back in is a fixed point — that is what
    // makes running this on every library refresh safe.
    QHash<QString, QString> remapTable(const QVector<QPair<QString, QString>>& oldIdToTitle);

    // Move every per-item record from each old id to its merged id, across EVERY profile (and, for the
    // device-namespaced accumulators, every device namespace) present in the ini — not just the active one:
    // a record belongs to whichever profile accrued it, and the remap runs once for all of them.
    //
    // Self-maps are skipped. Missing records are skipped. A destination that already holds a record is
    // merged into, not overwritten. Safe to call on every library refresh.
    void applyRemap(const QHash<QString, QString>& table);

    // applyRemap rewrites the ini UNDERNEATH the stores, which hold lazily-built in-memory caches keyed on
    // the old hashes (ItemMarks::invalidate / ConsumptionStats::invalidate). It cannot call them itself
    // without dragging ProfileStore/Settings/Tombstones into every probe that links this unit, so the app
    // hands it the invalidator once at startup. Unset in probes, which read the ini back directly.
    void setRemapCacheInvalidator(std::function<void()> fn);

#ifdef EB_PCGAMEID_TEST_SEAM
    // Test-only ini redirect, same rule and same macro as PcGameId::setIniPathForTesting: without the
    // define this symbol does not exist, so a production call is a compile error rather than a silent
    // process-wide redirect. Load-bearing here for issue #42 — a probe run whose data dir is the exe's own
    // folder would otherwise leave an everythingbox.ini in build/Release for the next run to read.
    void setRemapIniPathForTesting(const QString& path);

    // How many times applyRemap has flushed the ini since the last reset. The first-entry migration runs
    // on the GUI thread, and a QSettings::sync() is a whole-file rewrite — so "one sync per record"
    // is a per-record disk write, and a library with hundreds of migrating records stalls visibly the
    // first time the folder is opened. The count is the cheap, deterministic way to assert the flushes
    // are BATCHED (bounded by the number of passes) instead of scaling with the library, which is a
    // property no wall-clock measurement can pin repeatably on CI.
    int  remapSyncCount();
    void resetRemapSyncCount();
#endif
}
