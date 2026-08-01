// Moves an install created under the previous name onto the current one. ONE-SHOT by design (the alternative,
// permanently reading both names, was considered and declined) — but PER-STEP and resumable, because the
// failure mode of a single all-or-nothing migration is a half-migrated account, which is the hardest state to
// recover from. Each step carries its own device-local flag, the steps run safest-first, and each step's
// lookup tolerates the legacy name ONLY until that step's flag is set. The tolerance retires itself.
//
// NOT covered here, deliberately: the MOBILE data directory. QApplication::setApplicationName is still pinned
// to AppBrand::Legacy::kDisplayName (see the comment at that call site in main.cpp) because on Android/iOS
// AppPaths::dataDir() resolves through QStandardPaths::AppDataLocation, which incorporates applicationName —
// so renaming it MOVES the whole data directory. That is a recursive directory move of ini + saves + states +
// addons + themes, it cannot be exercised by a desktop headless probe (dataDir() only branches under
// Q_OS_ANDROID/Q_OS_IOS), and it cannot use the flag mechanism below unmodified, because the flags live INSIDE
// the directory being moved. It is a separate, device-tested step; until it lands, the pin IS the tolerance.
#pragma once
#include <QString>
#include <QStringList>
#include <functional>

namespace BrandMigration
{
    // Safest-first. LocalIni runs before anything else can create the new ini: a flag write is itself a write
    // to everythingbox.ini, so a step that ran first would leave the file existing and LocalIni's guard would
    // have to decide between "already migrated" and "about to be lost".
    enum class Step { LocalIni, AddonIds, DriveFolder, DriveFiles };

    // Device-local flags: a synced flag would mark OTHER machines as already migrated. Stored under the
    // "device/" prefix, which CloudSync::isDeviceLocalKey already carves out of the synced bundle.
    bool done(Step);
    // Production only ever SETS a flag, and only after the step's work is verified. Clearing exists so the
    // probe can prove resumability — an unset flag must make the step run again without damaging what the
    // first run produced.
    void setDone(Step, bool done);

    // Idempotent — safe on every launch. cb(allDone) fires when nothing remains to migrate: both local steps
    // complete, and the two Drive steps complete OR Drive is not configured / not signed in on this device
    // (in which case their flags stay UNSET, so signing in later still runs them).
    void run(std::function<void(bool allDone)> cb);

    // ---- the hop BEFORE this one, from the product's original name -------------------------------------
    // goliath.ini -> the previous brand's ini, one namespace further back. NOT a Step and NOT flagged: its
    // own guard is that its destination does not yet exist, and it must run before migrateLocalIni so the
    // install it produces is indistinguishable from a native previous-brand one.
    //
    // Unlike the hop below it, this one rewrites the addon namespace in VALUES as well as keys, and applies
    // no addon-id-keyed exclusion. That is deliberate and load-bearing — see the argument at the definition
    // (#121) before changing it; the two hops are NOT symmetric, and making them so is a regression.
    //
    // Lives here rather than as a static in main.cpp so probe_brand can drive the real function instead of a
    // re-spelling of it. Returns false only if the copy could not be made.
    bool migrateGoliathIni(const QString& dataDir);

    // ---- the local half, exposed against an explicit data directory ------------------------------------
    // run() calls these with AppPaths::dataDir(); the probe calls them against a QTemporaryDir so the
    // copy/verify/rewrite logic is asserted on a real filesystem without touching the running install. Each
    // returns true when the step is complete (including "there was nothing to migrate"). The FLAGS always
    // live in AppPaths::dataDir() — they describe this device, not the directory passed in.
    bool migrateLocalIni(const QString& dataDir);
    bool migrateAddonIds(const QString& dataDir);

    // ---- per-add-on state, reunited with the id the add-on actually reports ----------------------------
    // Deliberately NOT a Step and NOT flagged. Its subject is the set of ids that actually LOADED, which the
    // steps above cannot know: a remote add-on has no folder to inspect and no manifest until one has been
    // fetched and cached, possibly not until a later launch. A one-shot flag would retire the repair before
    // the add-on it exists for was ever seen. Idempotent instead, and cheap enough to run on every reload —
    // AddonManager::reload() calls it once its sources are built. `installedIds` is manifest ids, verbatim.
    // Returns how many values were carried across; 0 on the ordinary no-op run.
    int reconcileAddonConfig(const QString& dataDir, const QStringList& installedIds);

    // ---- add-on ids stored INSIDE per-profile content, re-pointed at the ids that loaded ---------------
    // The same repair as reconcileAddonConfig, on the other surface it has. Favourites and playlists persist
    // the id of the add-on an item came from inside their JSON blob (FavoritesStore's "addonId",
    // PlaylistStore's per-entry "addonId"), and that id is a foreign key into whatever manifest.id says —
    // not a brand string of ours. The symptom is one step louder than a blank Configure field: opening the
    // favourite resolves no source add-on at all and says so.
    //
    // Covers EVERY profile, not just the active one — the stores are namespaced per profile, so a repair
    // scoped to whoever happens to be signed in would leave every other profile broken with no second
    // chance. Enumerated from the ini's own groups rather than from ProfileStore, so data belonging to a
    // profile that has since been deleted is still reached (and this stays a QtCore-only TU).
    //
    // Same contract as reconcileAddonConfig: not a Step, not flagged, idempotent, safe on every reload.
    // Returns how many stored references were re-pointed; 0 on the ordinary no-op run.
    int reconcileAddonRefs(const QString& dataDir, const QStringList& installedIds);
}
