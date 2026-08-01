// Per-item metadata overrides — the user's corrections to a wrong scrape (issue #24).
//
// WHY THIS IS NOT IN MetaCache. MetaCache is a CACHE: everything in <dataDir>/metadata/<sha1(key)>/ is
// regenerable from the providers, it is deliberately excluded from sync, and any of it may be re-fetched,
// evicted by the image cap, or deleted wholesale by MetaCache::remove(). A user's correction is the opposite
// kind of thing — it is intent, it can never be re-derived, and an override that a refresh silently discards
// is worse than no override at all because the user is never told. So the override lives in the portable
// everythingbox.ini beside the other per-item stores (marks/favourites/playlists/resume), which is also the
// only place CloudMerge can carry it between devices. Issue #24 asked for "no new store"; that is the one
// point of its technical note this deliberately declines, and the composite still runs THROUGH the MetaCache
// read primitives so every surface that already reads the cache sees the correction with no further edits.
//
// Layout — GLOBAL, not per profile (same posture as resume/*, unlike marks/*): a mis-scraped item is wrong
// for the whole household, not for one viewer, so a correction made on one profile must show on all of them.
//   metaoverrides/items/<md5(key)>  -> compact JSON blob { title, subtitle, overview, image, updatedAt }
//
// The item key is the SAME identity every other per-item store uses — MetaCache::keyFor(item) (the stable
// addon id, else the url/path) — hashed with the SAME full MD5-hex-over-UTF8 as ItemMarks. No fifth scheme.
//
// FIELD SEMANTICS. A field is overridden when it is present and non-empty; an empty edit CLEARS that field's
// override and the scraped value comes back. There is therefore exactly ONE spelling for "not overridden"
// (absent), never a choice between absent and "" — see the convergence note below. Values are trimmed at
// WRITE time, so two devices that typed the same correction with different incidental whitespace store
// byte-identical records.
//
// RESET TO SCRAPED. reset() does NOT delete the record; it stores a timestamp-only husk {"updatedAt": now}.
// That is the difference between this store and ItemMarks (which removes an all-default blob) and it is
// deliberate: deleting the row would let the next merge with a peer that still holds the old override
// RESURRECT the thing the user just reset. The husk is newer, so it wins the merge and propagates the reset.
// A husk composites as "no override", which is exactly "show the scraped values".
//
// Husks are therefore NEVER compacted, unlike Tombstones (compact(30) at every merge): a husk has to outlive
// any peer's stale copy of the override it reset, and "any peer" has no expiry — a device that has been off
// for a year still holds that copy. So this store grows by one ~45-byte record per item ever reset and never
// shrinks. Accepted deliberately: ten thousand resets is well under a megabyte of ini, and what it buys off
// is the user's reset silently undoing itself the day a long-dormant device syncs.
//
// AND THEREFORE A HUSK IS ONLY EVER WRITTEN WHERE A RECORD EXISTED TO CLEAR (issue #132). Because a husk is
// permanent, dated NOW and carried to every device, storing one for an item that carries no correction records
// an event that did not happen — and, being newer, it wins the merge against another device's genuine older
// correction and deletes it. set() therefore refuses to create a row from an all-empty override; only a real
// clear leaves a husk. ItemMarks::saveItem carries the same guard, and the rule both answer to is stated next
// to CloudMerge::remoteReplaces: in a store that merges by timestamp, "cleared" and "never known" must not have
// the same representation — but neither may a NON-clear be spelled as a clear.
//
// CONVERGENCE. CloudMerge carries the store as "metaoverrides": {"<hash>": <blob>} and merges per hash with
// remoteReplaces() — newest updatedAt wins, and on an EQUAL timestamp the lexically-greater canonical bytes
// decide, order-independently. Two devices converge because the record has exactly one canonical spelling:
// omit-empty + trimmed at write, compact + key-sorted on serialize. Note deliberately NOT normalizing
// anything at comparison time (the #58 addonId lesson): this blob carries no add-on id and no device-local
// path, so a normalization here would be motion no mutation could kill. The reason it carries no device-local
// path is a design choice, not luck — see the "left out" note on local-file artwork below.
//
// LEFT OUT ON PURPOSE: (a) local-FILE artwork — a path is device-local, so two devices could never converge
// on one and the winner of a tie would be a file the loser does not have; the image override is a URL.
// (b) re-picking the MATCH (searching providers and choosing another result) — the match id IS the key this
// record is stored under, so rewriting it would move the item out from under its own override; that needs a
// re-key/migration plus a provider-search surface. (c) sortTitle — nothing in the app sorts by title today,
// so the field would be stored and never read.
#pragma once
#include "../addons/AddonModels.h"

#include <QJsonObject>
#include <QString>
#include <functional>

namespace MetaOverrides
{
    struct Override
    {
        QString title;      // replaces the scraped title everywhere the item is shown
        QString subtitle;   // the line under the title (year / rating / runtime for video, system for games)
        QString overview;   // synopsis on the detail card
        QString image;      // poster/thumb URL; leads the "poster" and "thumb" art roles too
        qint64  updatedAt = 0;

        // No field set. Ignores updatedAt, so a reset husk is empty (= "show the scraped values") while still
        // being a real, newer, propagating record.
        bool isEmpty() const;
    };

    // ---- pure: canonical record <-> JSON ----------------------------------------------------------------
    Override    fromJson(const QJsonObject& o);
    QJsonObject toJson(const Override& ov);   // trimmed + omit-empty: ONE canonical spelling per record
    Override    normalized(const Override& ov); // trim every field (what set() stores)

    // ---- pure: the composite rule ------------------------------------------------------------------------
    // Override wins over scraped, field by field; an unset field leaves the scraped value alone. These are the
    // whole merge-with-scraped-data rule and are what the probe pins.
    QString pick(const QString& override, const QString& scraped);
    void    applyTo(const Override& ov, MediaDetail& d);
    void    applyTo(const Override& ov, MediaItem& it);
    void    applyTo(const Override& ov, MediaArt& art); // image -> front of the poster/thumb candidate lists
    // The themed row map a theme binds through (`selected.title`, `selected.image`, `selected.poster`, the
    // `images` sub-map…). Some surfaces assemble that map from several sources — a session art cache, a
    // gamelist entry, the scrape cache — so the correction is composited over the FINISHED map rather than
    // over each source, which is also the only way one hook covers all of them.
    void    applyTo(const Override& ov, QVariantMap& row);

    // ---- store ------------------------------------------------------------------------------------------
    QString  hashKey(const QString& key);   // md5-hex of the UTF-8 key (ItemMarks' scheme, same key space)
    Override get(const QString& key);       // absent/empty key -> a default (all-clear) Override
    bool     has(const QString& key);       // is any field overridden for this item
    // Normalizes, stamps updatedAt, persists. An all-empty override CLEARS the item — but only where a record
    // exists to clear; on an un-overridden item it writes nothing at all (see the husk note above).
    void     set(const QString& key, const Override& ov);
    void     reset(const QString& key);     // "reset to scraped": a newer, empty, still-propagating record
    int      count();                       // items carrying at least one overridden field
    void     clearAll();                    // reset EVERY overridden item (husks, not deletions)

    void invalidate();                      // drop the cache (external ini change / after a cloud merge)

    // Multi-device sync trigger, same contract as ItemMarks::setChangeHook: a std::function fired after every
    // mutation so MainWindow can (re)arm the debounced push. Unset in headless probes (fires nothing).
    void setChangeHook(std::function<void()> hook);
}
