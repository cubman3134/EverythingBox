// MOVING WHAT THE USER ALREADY BANKED ONTO SERVER-QUALIFIED IDS (issue #160, increment 1).
//
// Jellyfin.h argues why an item reference must carry the server it came from. This is the half that repairs
// references already written in the old, single-server shape — and it lands FIRST, before any multi-server
// UI, because the moment a second server can be added an unqualified row becomes unattributable for ever.
//
// It is MusicRemap's shape deliberately, down to the Batch class and the four rules, because the property
// that made that unit safe is the property that matters here: A MIGRATION THAT SILENTLY LOSES A USER'S DATA
// IS FAR WORSE THAN ONE THAT LEAVES A FEW ROWS BEHIND ON OLD KEYS.
//
// ==================================================================================================
// WHAT THIS IS HONESTLY FOR — SAID PLAINLY, BECAUSE IT CHANGES HOW IT SHOULD BE READ
// ==================================================================================================
// #83's single-server client has not shipped, so no released build has ever written a `jf:<itemId>` row and
// there is, today, nothing on any user's disk for this to move. It exists anyway, and lands first anyway,
// for two reasons that are not the same:
//   * IT MAKES THE OLD SHAPE UNWRITABLE RATHER THAN MERELY DISCOURAGED. With the reader, the table and the
//     sweep in the tree and under a probe, a row in the bare shape has a defined, tested destination; without
//     them, "we will qualify ids later" is a promise, and the later never has the information it needs.
//   * IT IS THE ORDER THE ISSUE ASKS FOR, and the order is the whole point: the id decision cannot be
//     retrofitted after two servers exist, because nothing in a bare row says which of them it came from.
// So read the probe's migration cases as a specification of a repair, not as evidence about existing installs.
//
// ==================================================================================================
// THE ONE-WAY HASH, AND WHY THE TABLE IS BUILT FROM THE READABLE STORES
// ==================================================================================================
// Three of the six stores key on a DIGEST of the item reference (resume on md5-10, marks on md5, play stats
// on sha1). A digest cannot be walked backwards, so those stores cannot be enumerated: there is no way to
// look at `resume/9f86d0818` and discover the id it was filed under.
//
// The other three — favourites, playlists and recents — store the reference LITERALLY, in JSON. So the
// table is built by reading those, and then applied to all six: the hashed stores are swept by hashing both
// ends of each mapping, exactly as MusicRemap does. The consequence is stated rather than hidden: a resume
// position for an item that appears in NO favourite, playlist or recents row is not enumerable and STAYS
// WHERE IT IS. It is not lost — it is under a key nothing looks up any more — and that is the conservative
// side of rule 1. In practice the case that matters most (a Continue Watching row and its resume position)
// is enumerable by construction, because the recents row IS the enumeration.
//
// ==================================================================================================
// WHICH SERVER AN UNQUALIFIED ROW BELONGS TO — AND WHEN THAT QUESTION HAS NO ANSWER
// ==================================================================================================
// An unqualified row came from "the server that was configured", so:
//   * EXACTLY ONE server configured -> every legacy row gets that server's id. This is the upgrade case.
//   * NO server configured -> nothing to attribute it to. Nothing is written.
//   * TWO OR MORE servers configured -> the row is AMBIGUOUS, and the migration DOES NOT GUESS. Guessing
//     would file your friend's resume position against your copy of the film, which is the exact corruption
//     the qualified id exists to prevent — and it is silent. The rows stay as they are.
// migrateSingleServer() is that rule, and it is the only entry point the app calls.
//
// ==================================================================================================
// THE FOUR RULES (MusicRemap's, unchanged)
// ==================================================================================================
//   1. A reference with no destination is ABSENT from the table — never mapped to an empty string. Anything
//      that is not a legacy Jellyfin reference (a file path, an addon item id, a Subsonic key, an already-
//      qualified id) is left exactly as it is.
//   2. A record is never removed until its replacement has been written and flushed without error.
//   3. A destination that already holds a record is MERGED into, never overwritten.
//   4. RUNNING IT TWICE EQUALS RUNNING IT ONCE, and that is structural rather than stamped: after one run
//      no legacy reference remains in the three readable stores, so the second run builds an EMPTY table and
//      applyMigration returns before it opens the ini. There is no "already migrated" flag to get out of
//      step with the data, which is the failure mode a stamp introduces.
#pragma once
#include <QHash>
#include <QString>
#include <QStringList>

namespace JellyfinMigrate
{
    // Old reference -> new reference. One map, applied to every store this unit sweeps.
    struct Table
    {
        QHash<QString, QString> map;
        bool isEmpty() const { return map.isEmpty(); }
    };

    // PURE. No settings, no network, no clock — the mapping is decided entirely away from the records it
    // will rewrite, which is what makes it probe-testable over a table of strings.
    //
    // Every entry of `storedIds` that is a LEGACY reference maps to its qualified form under `serverId`.
    // Everything else is absent (rule 1), including an already-qualified id — which is what makes a second
    // run a no-op. An invalid `serverId` produces an EMPTY table rather than a table of half-formed ids.
    Table tableFor(const QStringList& storedIds, const QString& serverId);

    // Every reference the three READABLE stores hold, across every profile in the ini, in no particular
    // order and with duplicates removed. The enumeration the header describes; the input to tableFor.
    QStringList storedIds();

    // Move every record from each old reference to its new one, across every profile (and, for the
    // device-namespaced play stats, every device namespace) present in the ini. Self-maps are skipped,
    // missing records are skipped, a destination that already holds a record is merged into. Returns
    // immediately on an empty table without opening the store.
    void applyMigration(const Table& table);

    // THE ONE ENTRY POINT THE APP CALLS, and the whole of the "which server" rule above: with exactly one
    // configured server it enumerates, builds and applies; with none or several it does nothing at all.
    // Safe and cheap to call on every load — with nothing to move it does not so much as open the ini.
    void migrateSingleServer(const QStringList& configuredServerIds);

#ifdef EB_JELLYFIN_TEST_SEAM
    // Test-only ini redirect, the same macro and the same rule as MusicRemap's: without the define the
    // symbol does not exist, so a production call is a compile error rather than a silent process-wide
    // redirect. Load-bearing here because this unit WRITES records — a probe run against the app's real ini
    // would move a user's data.
    void setIniPathForTesting(const QString& path);
#endif
}
