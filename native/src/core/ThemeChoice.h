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

    // The legacy GLOBAL key. Read by the migration, then removed. Nothing else may name it.
    inline constexpr const char* kLegacyGlobalKey = "themedHome/theme";

    // "this install's ini has been migrated" — device-local (it describes work done against THIS ini), so it
    // is covered by CloudSync::isDeviceLocalKey's existing "device/" prefix rule and must never sync.
    inline constexpr const char* kMigratedFlag = "device/themeChoiceMigrated";

    // "themedHome/theme/<profileId>"; an empty id maps to ".../default", matching ThemeStore::currentName().
    QString keyFor(const QString& profileId);

    // ---- pure decisions (no ini, no filesystem) ---------------------------------------------------------
    // Does this profile still owe us a pick? True when nothing is stored, OR the stored folder is no longer
    // installed. The second case matters: a user who deleted their theme must be asked again, not silently
    // moved. It is also why `installed` is a parameter — an "is it empty" check gets this wrong.
    bool needsPick(const QString& stored, const QStringList& installed);

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
}
