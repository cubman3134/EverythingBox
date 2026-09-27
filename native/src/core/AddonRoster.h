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
// REGISTRY REFERENCES (increment 2). Add-on code never rides sync. A folder add-on installed from a registry is
// a kind-"registry" record — registry index URL, entry id, the version installed — and a receiving device
// installs it FROM THAT REGISTRY, through the same install path the registry browser uses, and only when it has
// that registry configured itself. A sideloaded folder add-on is device-only and is not in the roster at all.
//
// REGISTRY SOURCES (increment 4). The registries the user ADDED — add-on registries and theme registries — are
// roster records too, of kind "registrySource", keyed by the list and the normalised index URL
// ("registrySource:addons:<url>" / "registrySource:themes:<url>"). One kind, told apart by a field: every rule
// is the same for both lists, so two kinds would be two copies of the same code. The key carries the list and
// the URL, so a record whose extra fields a build before this one dropped still reads back as what it is.
// THE ROSTER IS THE STORE: registrySources() is what every browser reads, addRegistrySource()/
// removeRegistrySource() are the only writers (a dated record, or a dated tombstone), and the old list keys
// (registry/addonsExtras, registry/themesExtras) are gone — a device's own copy is adopted once as ts-0 adds, and
// an older peer's copy arriving in its heavy bundle is adopted the same way, never written raw. The BUILT-IN
// registries are never records and can never be tombstoned: they are always configured, whatever a peer says.
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
        // kind "registry" (issue #77, increment 2): a folder add-on installed from a registry, synced as a
        // REFERENCE — never as code. A receiving device installs it from `registry` itself, and only when it
        // has that registry configured too. Empty on every other record, and then not serialised at all, so an
        // increment-1 record keeps its exact bytes (the merge's equal-ts tie-break compares canonical bytes).
        QString registry;     // the registry's index.json URL
        QString entry;        // the registry entry's id — the folder it installs into
        QString version;      // the version the RECORDING device installed; informational, never compared
        bool isRegistry() const { return !registry.isEmpty(); }
        // kind "registrySource" (increment 4): a registry the user added. Both are derived from the KEY, which is
        // the authority; empty on every other record.
        QString sourceList;   // "addons" or "themes"
        QString sourceUrl;    // the registry's index URL, normalised
        bool isRegistrySource() const { return !sourceList.isEmpty(); }
    };

    // ---- the registries the user added (increment 4) --------------------------------------------------------
    // Two lists. Themes also serve decoration packs: one community registry format, one list (#187).
    enum class RegistryList { Addons, Themes };
    // The built-in index of each list. Never a roster record, never tombstoned; always configured.
    QString builtInRegistryUrl(RegistryList list);
    // True for the built-in index — and, for add-ons under EB_UITEST, for the EB_ADDON_REGISTRY_URL fixture that
    // stands in for it (AddonManager::registryUrlFor).
    bool isBuiltInRegistry(RegistryList list, const QString& url);
    QString normalizeRegistryUrl(const QString& raw);          // trimmed; scheme and host lower-cased
    QString registrySourceKey(RegistryList list, const QString& url);
    // The user-added registries of `list`: normalised, built-ins excluded, oldest first (ts, then key) — the
    // same order on every device. Adopts this device's old list key first, once (see REGISTRY SOURCES).
    QStringList registrySources(RegistryList list);
    // A dated record (strictly newer than any tombstone on it), or a dated tombstone (strictly newer than the
    // record); both fire the change hook. Refused, with nothing written, for an empty URL or a built-in; add is a
    // no-op for a registry already held, remove for one not held. `now` <= 0 = the current epoch second.
    bool addRegistrySource(RegistryList list, const QString& url, qint64 now = 0);
    bool removeRegistrySource(RegistryList list, const QString& url, qint64 now = 0);
    // The old per-list ini keys — carved out of the heavy bundle both ways (CloudSync).
    bool isLegacyRegistryKey(const QString& iniKey);
    // A roster key naming a BUILT-IN registry as a source: never merged in, as a record or as a tombstone.
    bool isBuiltInRegistryKey(const QString& key);
    // An OLDER peer's list, or this device's own pre-roster list: ADDS at ts 0 — never over a tombstone, never a
    // built-in, never a removal. This device's own edits are reconciled first; the change hook fires when a record
    // was added, and so does the return value.
    bool adoptLegacyRegistrySources(RegistryList list, const QStringList& urls);
    // The value an older build's bundle carries for such a key (a QVariant's string form), as a list of URLs.
    QStringList parseLegacyRegistryValue(const QString& value);

    // ---- the add-ons in THIS device's folder (issue #77, increment 2) --------------------------------------
    // Add-on CODE never rides sync. What a folder add-on is decides what syncs about it:
    //   * first-party (the reserved id prefix): ships with the build; its on/off flag syncs, nothing else;
    //   * registry-installed (its folder holds a provenance file): syncs as a kind-"registry" record;
    //   * sideloaded (anything else — a local .addon import, a folder dropped in): DEVICE-ONLY. It is kept
    //     out of the roster entirely, flag included, and the add-on list shows it under "Not synced".
    // The provenance file is written by the registry install, is part of the folder, and so goes with it: an
    // uninstall, or a local .addon import over the same id, leaves no stale claim behind.
    struct Provenance
    {
        QString registry;     // the registry index URL it was installed from
        QString entry;        // the registry entry id
        QString version;      // the version actually installed (the downloaded manifest's own "version")
        qint64  installedAt = 0;
        bool valid() const { return !registry.isEmpty() && !entry.isEmpty(); }
    };
    struct LocalAddon
    {
        QString dir;          // absolute folder path
        QString id;           // its manifest.json "id" (the folder name when the manifest has none)
        bool    firstParty = false;
        Provenance provenance;   // valid() = registry-installed
        bool sideloaded() const { return !firstParty && !provenance.valid(); }
    };
    QString provenanceFileName();                    // the file inside an add-on folder that records its origin
    Provenance readProvenance(const QString& addonDir);
    bool writeProvenance(const QString& addonDir, const Provenance& p);
    bool isFirstPartyId(const QString& manifestId);  // the reserved namespace, current or previous brand
    // Where the folder add-ons live. AddonManager sets its own root (EB_ADDONS_ROOT in the probes); unset, it is
    // <data>/addons.
    void setAddonsRoot(const QString& root);
    QString addonsRoot();
    QVector<LocalAddon> localAddons();               // every folder under the root that has a manifest.json

    // A registry record this device does not have installed: no folder add-on with its key as manifest id, and
    // no folder named after its entry. Suppressed records are not pending. Sorted by key.
    QVector<Record> pendingRegistryInstalls();
    // An explicit removal (AddonManager::removeAddon of a registry-installed add-on). A registry record's
    // absence from the folder can mean "not installed here YET" — a reference from a device whose registry this
    // one lacks — so reconcile() never infers a removal from it; the uninstall says so itself. Tombstones `key`
    // at a stamp newer than its record, drops the record, and fires the change hook.
    void recordRemoval(const QString& key);

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
    // each flag is written where it differs. A registry-installed folder whose record a tombstone removed is
    // UNINSTALLED (its provenance file first, so a folder that will not delete is left behind as a device-only
    // add-on rather than one the next reconcile would re-add). Installing a registry record this device lacks
    // is not done here: that is a network fetch, and AddonManager::applyMergedRoster does it. With dryRun
    // nothing is written. Returns true when the live keys or the folders change (or would).
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
