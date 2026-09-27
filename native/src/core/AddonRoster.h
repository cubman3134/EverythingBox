// THE ADD-ON ROSTER, AS A MERGED STORE (issue #77, increment 1).
//
// What it is: the remote (subscribe-by-URL) add-ons this install has, and the on/off flag of every add-on.
// AddonManager keeps both where it always has — `addon.remote.urls` (a JSON array of base URLs) and
// `addon.enabled.<manifest id>` in everythingbox.ini — and reads them live. Those two are the LIVE keys.
//
// Why a second representation: until #77 the live keys rode the heavy settings bundle, which is a
// last-writer-wins snapshot. A device that pushed late silently erased a subscription another device had
// added in between. A merge needs a timestamp per entry and a dated record of every removal, and the live
// keys have neither, so this store keeps a STAMPED SHADOW of them — one record per add-on — plus tombstones
// (Tombstones, store "roster/<scope>"), and CloudMerge's `roster` section merges that shadow exactly the way
// favourites and presets are merged: union by key, newest ts wins, a tombstone at-or-after an entry's ts
// suppresses it, equal ts decided by the order-independent tie-break. After a merge the shadow is PROJECTED
// back onto the live keys, so AddonManager never has to know the shadow exists.
//
// ONE AUTHORITY. The live keys are carved out of the heavy bundle in both directions (CloudSync), so the merge
// is the only thing that moves them between devices. A peer on an older build still sends them in its
// bundle; applySettingsJson hands that snapshot to adoptLegacySnapshot(), which takes its entries as ADDS at
// ts 0 (never as removals, and never over a local tombstone) instead of writing them raw.
//
// KEYING. A record is keyed by the add-on's manifest id when known (read from the cached manifest), else by
// its normalised base URL. That is #80's rule — a manifest id already installed under another URL is a
// re-configure, not a second add-on — carried into the merge: two devices that configured Torrentio
// differently hold ONE record, and the newer URL wins. The enabled flag lives in the same record, so one ts
// covers the URL and the flag together. A record with an empty url is an enabled flag for an add-on that is
// not a remote source on the device that wrote it (a bundled or installed folder add-on).
//
// SCOPE. The document shape is per scope, { "<scope>": {items, tombs} }, like the per-profile categories. On
// main the roster is DEVICE-WIDE — AddonManager has one add-on set that every profile shares — so the only
// scope written is kScope. A later per-profile roster slots in beside it without a format change.
//
// HOW A LOCAL EDIT GETS ITS STAMP. reconcile() compares the live keys with the shadow and turns every
// difference into a stamped edit: a live URL with no record is an add, a record whose URL left the live list
// is a removal (tombstoned now), a flipped flag is a change. AddonManager calls touched() after each write,
// so an edit is stamped when it happens; serialize and merge reconcile first too, which is what catches the
// writers that do not go through AddonManager — a settings Discard reverting the live keys, or an older build
// run against this ini. The very first reconcile (no shadow yet) is a BACKFILL: it stamps what is there at
// ts 0 ("unknown time"), so any real tombstone or dated edit on another device beats it.
//
// QtCore only, over the shared portable everythingbox.ini, like every other store.
#pragma once
#include <QString>
#include <QStringList>
#include <QHash>
#include <QVector>
#include <functional>

class QJsonObject;

namespace AddonRoster
{
    // The one scope written today (see SCOPE above).
    inline constexpr const char* kScope = "all";

    struct Record
    {
        QString key;          // manifest id, else the normalised base URL
        QString url;          // the remote source's base URL; empty = an enabled flag only
        bool    enabled = true;
        qint64  ts = 0;       // epoch seconds of the edit; 0 = backfilled or adopted, time unknown
    };

    // ---- names ---------------------------------------------------------------------------------------------
    QString itemsKey();                              // "roster/<scope>/items"
    QString tombStore();                             // "roster/<scope>" (its tombstones live under deleted/)
    QString normalizeBase(const QString& raw);       // AddonManager's rule: trim, drop /manifest.json and '/'
    QString manifestCacheKey(const QString& base);   // "addon.remote.manifest.<md5 of base>"
    QString keyForUrl(const QString& base);          // the cached manifest's id, else the base itself
    // The LIVE keys this store shadows — addon.remote.urls and addon.enabled.<id>. CloudSync keeps them off
    // the heavy bundle in both directions.
    bool isLiveKey(const QString& key);

    // ---- the shadow ----------------------------------------------------------------------------------------
    QVector<Record> records();                       // sorted by key
    void saveRecords(QVector<Record> recs);          // sorts by key before writing
    QJsonObject toJson(const Record& r);
    Record fromJson(const QJsonObject& o);           // url normalised; a record with no key is returned empty

    // Turn live-vs-shadow differences into stamped edits (see HOW A LOCAL EDIT GETS ITS STAMP). `now` <= 0
    // means the current epoch second. Returns true when the shadow or the tombstones changed.
    bool reconcile(qint64 now = 0);
    // reconcile() + the change hook when anything changed. AddonManager calls this after every live write.
    void touched();

    // Write the shadow onto the live keys: surviving URLs keep their place, a reconfigured one is replaced in
    // place, a suppressed one is dropped (with its cached manifest), new ones are appended in key order, and
    // each flag is written where it differs. With dryRun nothing is written. Returns true when the live keys
    // change (or would).
    bool project(bool dryRun = false);

    // An OLD peer's heavy-bundle snapshot (its addon.remote.urls JSON text and its addon.enabled.* flags):
    // adopted as ADDS at ts 0 — an entry this device already holds, by URL or by key, is left alone, and one
    // this device has tombstoned is not adopted — then projected. Never a removal: the snapshot has no dates.
    void adoptLegacySnapshot(const QString& urlsJson, const QHash<QString, bool>& enabled);

    // Multi-device sync trigger, the FavoritesStore::setChangeHook contract: fired after a local edit.
    void setChangeHook(std::function<void()> hook);

    // ORDERING (issue #77): the bundled-add-on migrations (AddonManager::seedDefaultStremioSources) must run
    // before the first roster merge, or a merge could hand a fresh device the pre-migration roster of a peer
    // before its own one-shot removals have had their say. AddonManager marks the process once its startup
    // migrations have run; CloudMerge skips the roster section until then (the next pull carries it again).
    void markMigrationsRun();
    bool migrationsRun();
}
