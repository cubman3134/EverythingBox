// The per-profile themed-home theme choice (roadmap #57). Before this, `themedHome/theme` was ONE global key
// shared by every profile, while the classic colour theme was already per-profile (Theme.cpp:114) — the two
// systems disagreed, and "force a pick for new profiles" was meaningless. This unit is the single owner of the
// setting: the key format, the two decisions the UI makes about it, and the one-time migration.
//
// The decisions are PURE — they take `stored` and `installed` as arguments rather than reading the ini or the
// filesystem — so probe_theme pins the tables with no I/O and no Qt GUI. QtCore only (its own file-local
// store(), the idiom every other core store uses), so the probe links lean.
#pragma once
#include <QHash>
#include <QString>
#include <QStringList>

namespace ThemeChoice
{
    // The shipped fallback theme FOLDER name. Was "Default" until the shipped set was cut to Triple +
    // Channels; "Default" is no longer bundled, so a fallback naming it would point at nothing.
    inline constexpr const char* kFallbackTheme = "Triple";

    // The theme2 folder that was renamed. The folder's theme.json already declared "name": "Triple" and the
    // community registry already used the folder name Triple — the local tree was the odd one out. A folder
    // name is a STORED VALUE, so the rename is only safe because the migration carries it.
    inline constexpr const char* kRenamedFrom = "XMB";

    // The live key's group prefix — the base keyFor() builds "<base>/<profileId>" from. It happens to spell
    // the same string as kLegacyGlobalKey, but they are DIFFERENT things: this one is a live group prefix,
    // that one is a dead scalar key. Building the live key out of the legacy constant would make the
    // migration's "nothing else may name it" rule a lie and would tie the two together forever.
    inline constexpr const char* kKeyBase = "themedHome/theme";

    // The legacy GLOBAL key. Read by the migration, then removed. Nothing else may name it.
    inline constexpr const char* kLegacyGlobalKey = "themedHome/theme";

    // "this install's ini has been migrated" — device-local (it describes work done against THIS ini), so it
    // is covered by CloudSync::isDeviceLocalKey's existing "device/" prefix rule and must never sync.
    inline constexpr const char* kMigratedFlag = "device/themeChoiceMigrated";

    // "themedHome/theme/<profileId>"; an empty id maps to ".../default", matching ThemeStore::currentName().
    QString keyFor(const QString& profileId);

    // ---- pure decisions (no ini, no filesystem) ---------------------------------------------------------
    // Does this profile still owe us a pick? ONLY when nothing is stored. Deliberately NOT "…or the stored
    // folder isn't installed here", which looks more thorough and is wrong: "themedHome/theme/<profileId>"
    // is not in CloudSync::isDeviceLocalKey's carve-out (CloudSync.cpp:486), so this value SYNCS. A device
    // that merely lacks the theme folder would force a pick, write the answer, and that write would sync
    // back and silently overwrite the choice the user made on their other device. A missing folder is a
    // per-device rendering fact, not a lost choice — resolve() already covers it gracefully and invisibly.
    // Do not restore the `installed` check here; put it in resolve(), where it belongs.
    bool needsPick(const QString& stored);

    // What to actually render, in order: the stored folder if installed; else kFallbackTheme if installed;
    // else the first installed folder; else empty (nothing is installed — callers already handle that, see
    // MainWindow.cpp:3951). NEVER returns a folder that is not in `installed`.
    QString resolve(const QString& stored, const QStringList& installed);

    // kRenamedFrom -> kFallbackTheme; every other value (including empty) passes through unchanged.
    QString renameLegacyFolder(const QString& stored);

    // The migration table. Given the legacy global value, the profile ids, and the per-profile values already
    // stored, return ONLY the per-profile values that need WRITING. An existing value is never replaced by
    // the global — it just gets renameLegacyFolder applied. Naturally idempotent: feeding the result back in
    // as `existing` returns empty.
    QHash<QString, QString> planMigration(const QString& legacyGlobal,
                                          const QStringList& profileIds,
                                          const QHash<QString, QString>& existing);

    // ---- ini-backed accessors ---------------------------------------------------------------------------
    QString forProfile(const QString& profileId);                          // "" when unset
    void    setForProfile(const QString& profileId, const QString& folder);
    void    runMigration();   // flag-guarded AND naturally idempotent; safe to call on every startup

    // runMigration()'s body, with the profile ids passed in instead of read from ProfileStore::list(). Split
    // out because the ZERO-profiles case is the one that used to DESTROY the legacy value (an empty id list
    // fanned it out to nothing and then deleted it), and it can only be pinned by driving the id list
    // directly — ProfileStore reads its own ini, which a hermetic probe has no way to seed.
    void    runMigrationForIds(const QStringList& profileIds);

    // ---- test-only seam ---------------------------------------------------------------------------------
    // Redirects THIS unit's ini to `path`; an empty path restores the app's real ini. It exists so
    // probe_theme can exercise the ini-backed half (forProfile/setForProfile/runMigrationForIds) against a
    // scratch file in the temp dir, hermetically — that half is where the destructive-migration bug lived and
    // it had no coverage at all.
    //
    // PRODUCTION CANNOT CALL THIS: only probe_theme defines EB_THEMECHOICE_TEST_SEAM (native/CMakeLists.txt),
    // so in the app the symbol does not exist and a call is a compile error. A comment alone was not enough —
    // an accidental production call would silently redirect every theme read and write for the whole process
    // lifetime, and the assert that would have caught it is compiled out of the Release build we ship.
#ifdef EB_THEMECHOICE_TEST_SEAM
    void    setIniPathForTesting(const QString& path);
#endif
}
