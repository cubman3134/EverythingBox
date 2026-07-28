# Theme Onboarding Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make every new profile pick its own theme at first run, cut the shipped theme set to Triple + Channels, and give both onboarding and Appearance a real live preview.

**Architecture:** A new Qt-Core-only core unit `ThemeChoice` becomes the single owner of the theme setting, moving it from one global key to `themedHome/theme/<profileId>` and absorbing the eleven scattered read/write sites in `MainWindow.cpp`. A one-time migration folds two moves together (global→per-profile, and the `XMB`→`Triple` folder rename) so nobody's appearance changes and nobody is re-asked. One new themed surface, `ThemePickerHost`, serves both the forced first-run pick and Appearance.

**Tech Stack:** Qt 6.8.3 (Widgets, Qml/Quick, QuickWidgets), C++17, CMake. Headless console probes for verification. No new dependencies.

## Amendment (after Task 1 review — binding on Tasks 2–5)

Task 1's review found two Critical defects in this plan's own Task 1 code. Both are fixed in the
shipped `ThemeChoice`, and **the code in Task 1 below is now historical — the header on disk is the
truth.** Two contract changes bind the remaining tasks:

1. **`needsPick` takes ONE argument: `bool needsPick(const QString& stored)`,** and returns true only
   when nothing is stored. It no longer consults the installed list. Reason: the theme key is not in
   `CloudSync::isDeviceLocalKey`'s carve-out, so it SYNCS — under the old two-argument rule a device
   missing a theme folder would force a pick, write, and silently overwrite the other device's
   choice. `resolve()` already covers a missing folder invisibly, which is the correct behaviour.
   Do not restore the old rule; it looks more thorough and is wrong.
2. **`ThemeChoice` gained two seams:** `runMigrationForIds(const QStringList&)` and
   `setIniPathForTesting(const QString&)`, both used by `probe_theme` to exercise the ini-backed half
   hermetically. Leave them alone.

Also fixed there, for context: `QSettings::remove("themedHome/theme")` removes the key *and its whole
subtree*, so the migration was deleting the per-profile values it had just written.

## Global Constraints

These bind every task. Values are verbatim from the spec.

- **Build config is always Release.** `cmake --build build --config Release --target <name>`. Never a target-less build (it compiles ~43 probes and stalls).
- **Build only named targets.** Adding a source file requires ONE reconfigure. **Create a `.cpp` in the same step that adds it to CMake** — CMake resolves source lists at configure time.
- **The suite script only RUNS pre-built exes.** A green suite on stale binaries has shipped before. Build the targets, then run the suite.
- **A new probe must be registered in THREE places** or it silently never runs: its `add_executable` in `native/CMakeLists.txt`, the loop list in `native/tools/run-headless-probes.sh:130`, and the `--target` list in `.github/workflows/ci.yml:52`.
- **Modal UI goes through the nav kit** (`src/ui/nav` — `NavMenu`/`NavConfirm`/`Osk`) only. Never `QDialog`/`QMessageBox`/`QInputDialog`/top-level windows. `probe_nav` gates this.
- **Two settings builders.** `MainWindow::openGeneralSettings()` and `openAppearance()` each have a themed (default-reachable) and a classic QWidget path. A user-facing setting must be added to BOTH or it is unreachable by default.
- **Mutation testing is the standard of proof.** For each probe assertion group: break the implementation, confirm the probe FAILS, revert, confirm green. An assertion that passes under a broken implementation is not coverage.
- **Fallback theme folder name is exactly `Triple`.** Constant `ThemeChoice::kFallbackTheme`.
- **Setting key format is exactly `themedHome/theme/<profileId>`**, with an empty profile id mapping to `themedHome/theme/default`.
- **Legacy global key is exactly `themedHome/theme`** — read only by the migration, then removed.
- **Migration flag key is exactly `device/themeChoiceMigrated`** and must join the `isDeviceLocalKey` carve-out.
- **Never touch the real deployed app** at `C:\EverythingBox-app` or its ini during development. Copy to a scratch dir and work only there. Strip `cloud/*` and `sync/*` from any throwaway ini. Never print, log, echo or screenshot any credential value — report credentials only as "configured"/"not configured".

**Build command (Git Bash):**

```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH"
cmake --build build --config Release --target <targets>
```

---

## File Structure

**Created:**

| File | Responsibility |
|---|---|
| `native/src/core/ThemeChoice.h` | The per-profile theme key: pure decisions (`needsPick`, `resolve`, `planMigration`, `renameLegacyFolder`) + ini accessors. The ONLY declared owner of the setting. |
| `native/src/core/ThemeChoice.cpp` | Implementation. Qt-Core only (its own file-local `store()`), so it links into a lean headless probe. |
| `native/tools/probe_theme.cpp` | Headless probe. Sentinel `THEME-OK`. Pins the decision tables. |
| `native/src/theme2/ThemePickerHost.h` | The reusable theme-picker surface: list zone + no-focus live preview. Two entry modes. |
| `native/src/theme2/ThemePickerHost.cpp` | Implementation. |
| `native/src/theme2/qml/ThemePicker.qml` | The QML for that surface. Follows `elements/SettingsPanel.qml` verbatim for style resolution and form-factor tokens. |

**Modified:**

| File | Change |
|---|---|
| `native/CMakeLists.txt` | `probe_theme` target; `ThemeChoice.*` and `ThemePickerHost.*` into the app target. |
| `native/resources/theme2.qrc` | `ThemePicker.qml` entry. |
| `native/tools/run-headless-probes.sh:130` | `probe_theme THEME-OK` in the loop list. |
| `.github/workflows/ci.yml:52` | `probe_theme` in the `--target` list. |
| `native/src/theme2/ThemeEngine.cpp:233` | Empty-fallback `"Default"` → `"Triple"`. |
| `native/src/core/CloudSync.cpp:486` | `device/themeChoiceMigrated` is already covered by the existing `device/` prefix rule — verify, don't duplicate. |
| `native/src/ui/MainWindow.cpp` | Eleven theme-key sites → `ThemeChoice`; migration call at startup; forced pick at `chooseProfile()`; Appearance rewiring in both builders. |
| `native/src/ui/MainWindow.h` | Declarations for the new private helpers. |

**Deleted:** `native/themes2/Default/`, `native/themes2/Grid/`, `native/themes2/Lumen/`, `native/themes2/Midnight/`.
**Renamed:** `native/themes2/XMB/` → `native/themes2/Triple/`.

---

### Task 1: ThemeChoice core + probe_theme

**Files:**
- Create: `native/src/core/ThemeChoice.h`, `native/src/core/ThemeChoice.cpp`
- Create: `native/tools/probe_theme.cpp`
- Modify: `native/CMakeLists.txt` (probe target + app sources), `native/tools/run-headless-probes.sh:130`, `.github/workflows/ci.yml:52`

**Interfaces:**
- Consumes: `ProfileStore::list()` / `ProfileStore::currentId()` from `native/src/core/ProfileStore.h`.
- Produces, for Tasks 2–4:
  - `ThemeChoice::kFallbackTheme` → `"Triple"`
  - `QString ThemeChoice::keyFor(const QString& profileId)`
  - `bool ThemeChoice::needsPick(const QString& stored, const QStringList& installed)`
  - `QString ThemeChoice::resolve(const QString& stored, const QStringList& installed)`
  - `QString ThemeChoice::renameLegacyFolder(const QString& stored)`
  - `QHash<QString,QString> ThemeChoice::planMigration(const QString& legacyGlobal, const QStringList& profileIds, const QHash<QString,QString>& existing)`
  - `QString ThemeChoice::forProfile(const QString& profileId)`
  - `void ThemeChoice::setForProfile(const QString& profileId, const QString& folder)`
  - `void ThemeChoice::runMigration()`

- [ ] **Step 1: Write the probe first (RED)**

Create `native/tools/probe_theme.cpp`:

```cpp
// Headless check of the per-profile theme choice (roadmap #57). ThemeChoice owns the theme setting: the
// per-profile key, whether a profile still owes us a pick, what to actually render, and the one-time
// migration that carries both the global->per-profile move and the XMB->Triple folder rename. Every decision
// below is a pure function over its arguments (no ini, no filesystem), so this pins the tables verbatim and
// the UI layers can never drift from them.
//
// Prints THEME-OK on success; any failure prints THEME-FAIL <cond> and exits non-zero.
#include "ThemeChoice.h"

#include <QHash>
#include <QString>
#include <QStringList>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "THEME-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

int main()
{
    const QStringList kShipped = { QStringLiteral("Channels"), QStringLiteral("Triple") };

    // ---- 1. keyFor: the exact key format, including the empty-profile-id case ------------------------
    CHECK(ThemeChoice::keyFor(QStringLiteral("abc123")) == QStringLiteral("themedHome/theme/abc123"));
    CHECK(ThemeChoice::keyFor(QString()) == QStringLiteral("themedHome/theme/default"));

    // ---- 2. needsPick: unset, set-and-installed, set-but-UNINSTALLED --------------------------------
    // A profile with nothing stored owes a pick.
    CHECK(ThemeChoice::needsPick(QString(), kShipped) == true);
    // A profile whose theme is installed does not.
    CHECK(ThemeChoice::needsPick(QStringLiteral("Triple"), kShipped) == false);
    CHECK(ThemeChoice::needsPick(QStringLiteral("Channels"), kShipped) == false);
    // A profile whose stored theme is no longer on disk DOES owe a pick — this is the case a naive
    // "is it empty" check gets wrong, and it is why `installed` is a parameter at all.
    CHECK(ThemeChoice::needsPick(QStringLiteral("Grid"), kShipped) == true);
    // ...but not if that theme is still installed (the migrated-user case: cut from the shipped set,
    // still on their disk, must NOT be re-asked).
    CHECK(ThemeChoice::needsPick(QStringLiteral("Grid"),
                                 { QStringLiteral("Channels"), QStringLiteral("Grid"),
                                   QStringLiteral("Triple") }) == false);

    // ---- 3. resolve: all four ordering steps --------------------------------------------------------
    // (a) stored, when installed.
    CHECK(ThemeChoice::resolve(QStringLiteral("Channels"), kShipped) == QStringLiteral("Channels"));
    // (b) the fallback, when the stored theme is gone.
    CHECK(ThemeChoice::resolve(QStringLiteral("Grid"), kShipped) == QStringLiteral("Triple"));
    CHECK(ThemeChoice::resolve(QString(), kShipped) == QStringLiteral("Triple"));
    // (c) the FIRST installed theme, when the fallback itself isn't installed. A user who deleted
    //     Triple and kept only a community theme must land on that theme, not on a name that is
    //     nowhere on disk.
    CHECK(ThemeChoice::resolve(QStringLiteral("Grid"), { QStringLiteral("Aurora") })
          == QStringLiteral("Aurora"));
    CHECK(ThemeChoice::resolve(QString(), { QStringLiteral("Aurora"), QStringLiteral("Zed") })
          == QStringLiteral("Aurora"));
    // (d) nothing installed at all -> empty. Callers already handle this (MainWindow.cpp:3951).
    CHECK(ThemeChoice::resolve(QStringLiteral("Grid"), {}).isEmpty());
    CHECK(ThemeChoice::resolve(QString(), {}).isEmpty());
    // resolve NEVER returns a folder that isn't installed — the invariant the whole function exists for.
    CHECK(kShipped.contains(ThemeChoice::resolve(QStringLiteral("Nonexistent"), kShipped)));

    // ---- 4. renameLegacyFolder: the XMB -> Triple move ---------------------------------------------
    CHECK(ThemeChoice::renameLegacyFolder(QStringLiteral("XMB")) == QStringLiteral("Triple"));
    CHECK(ThemeChoice::renameLegacyFolder(QStringLiteral("Channels")) == QStringLiteral("Channels"));
    CHECK(ThemeChoice::renameLegacyFolder(QStringLiteral("Grid")) == QStringLiteral("Grid"));
    CHECK(ThemeChoice::renameLegacyFolder(QString()).isEmpty());

    // ---- 5. planMigration: the table --------------------------------------------------------------
    const QStringList twoProfiles = { QStringLiteral("p1"), QStringLiteral("p2") };

    // (a) A legacy global value fans out to every profile that has none.
    {
        const QHash<QString, QString> out =
            ThemeChoice::planMigration(QStringLiteral("Grid"), twoProfiles, {});
        CHECK(out.size() == 2);
        CHECK(out.value(QStringLiteral("p1")) == QStringLiteral("Grid"));
        CHECK(out.value(QStringLiteral("p2")) == QStringLiteral("Grid"));
    }

    // (b) The rename rides along: a legacy global of XMB lands as Triple.
    {
        const QHash<QString, QString> out =
            ThemeChoice::planMigration(QStringLiteral("XMB"), { QStringLiteral("p1") }, {});
        CHECK(out.value(QStringLiteral("p1")) == QStringLiteral("Triple"));
    }

    // (c) An EXISTING per-profile value is never overwritten by the global.
    {
        QHash<QString, QString> existing;
        existing.insert(QStringLiteral("p1"), QStringLiteral("Channels"));
        const QHash<QString, QString> out =
            ThemeChoice::planMigration(QStringLiteral("Grid"), { QStringLiteral("p1") }, existing);
        CHECK(out.isEmpty());
    }

    // (d) ...but an existing value still gets the folder rename applied.
    {
        QHash<QString, QString> existing;
        existing.insert(QStringLiteral("p1"), QStringLiteral("XMB"));
        const QHash<QString, QString> out =
            ThemeChoice::planMigration(QStringLiteral("Grid"), { QStringLiteral("p1") }, existing);
        CHECK(out.size() == 1);
        CHECK(out.value(QStringLiteral("p1")) == QStringLiteral("Triple"));
    }

    // (e) No legacy global and no stored value -> nothing written, so needsPick stays true and the
    //     profile gets the forced pick. This is the genuinely-fresh-install case.
    {
        const QHash<QString, QString> out =
            ThemeChoice::planMigration(QString(), { QStringLiteral("p1") }, {});
        CHECK(out.isEmpty());
        CHECK(ThemeChoice::needsPick(QString(), kShipped) == true);
    }

    // (f) No profiles at all -> nothing to write, no crash.
    CHECK(ThemeChoice::planMigration(QStringLiteral("Grid"), {}, {}).isEmpty());

    // ---- 6. IDEMPOTENCE: applying the plan and re-running produces nothing -------------------------
    // The migration is flag-guarded in practice, but it must ALSO be naturally idempotent — a flag that
    // fails to persist (a crash between write and sync) must not corrupt anything on the second run.
    {
        QHash<QString, QString> existing;
        const QHash<QString, QString> first =
            ThemeChoice::planMigration(QStringLiteral("XMB"), twoProfiles, existing);
        for (auto it = first.constBegin(); it != first.constEnd(); ++it)
            existing.insert(it.key(), it.value());
        const QHash<QString, QString> second =
            ThemeChoice::planMigration(QStringLiteral("XMB"), twoProfiles, existing);
        CHECK(second.isEmpty());
        // ...and a third run over the SAME state is still empty.
        CHECK(ThemeChoice::planMigration(QStringLiteral("XMB"), twoProfiles, existing).isEmpty());
    }

    if (failures == 0) { std::puts("THEME-OK"); return 0; }
    std::fprintf(stderr, "THEME: %d check(s) failed\n", failures);
    return 1;
}
```

- [ ] **Step 2: Create the header in the same step it enters CMake**

Create `native/src/core/ThemeChoice.h`:

```cpp
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
```

- [ ] **Step 3: Create the implementation**

Create `native/src/core/ThemeChoice.cpp`:

```cpp
#include "ThemeChoice.h"

#include "AppBrand.h"
#include "AppPaths.h"
#include "ProfileStore.h"

#include <QSettings>

// The file-local store() idiom every other core unit uses (ProfileStore.cpp:14, CloudMerge.cpp:21) — the
// shared portable ini, opened once. Keeping it local is what lets this TU stay QtCore-only.
static QSettings& store()
{
    static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                       QSettings::IniFormat);
    return s;
}

QString ThemeChoice::keyFor(const QString& profileId)
{
    return QLatin1String(kLegacyGlobalKey) + QStringLiteral("/")
         + (profileId.isEmpty() ? QStringLiteral("default") : profileId);
}

QString ThemeChoice::renameLegacyFolder(const QString& stored)
{
    return stored == QLatin1String(kRenamedFrom) ? QString::fromLatin1(kFallbackTheme) : stored;
}

bool ThemeChoice::needsPick(const QString& stored, const QStringList& installed)
{
    return stored.isEmpty() || !installed.contains(stored);
}

QString ThemeChoice::resolve(const QString& stored, const QStringList& installed)
{
    if (!stored.isEmpty() && installed.contains(stored)) return stored;
    const QString fallback = QString::fromLatin1(kFallbackTheme);
    if (installed.contains(fallback)) return fallback;
    return installed.value(0);   // empty when nothing is installed
}

QHash<QString, QString> ThemeChoice::planMigration(const QString& legacyGlobal,
                                                   const QStringList& profileIds,
                                                   const QHash<QString, QString>& existing)
{
    QHash<QString, QString> out;
    const QString global = renameLegacyFolder(legacyGlobal);
    for (const QString& id : profileIds)
    {
        const QString have = existing.value(id);
        if (!have.isEmpty())
        {
            // An existing choice is authoritative — the global never overwrites it. It only needs writing
            // back if the folder rename changed it.
            const QString renamed = renameLegacyFolder(have);
            if (renamed != have) out.insert(id, renamed);
            continue;
        }
        // No per-profile value: inherit the legacy global, if there was one. If there wasn't, write nothing
        // — needsPick then stays true and this profile gets the forced pick, which is correct for a genuinely
        // fresh install.
        if (!global.isEmpty()) out.insert(id, global);
    }
    return out;
}

QString ThemeChoice::forProfile(const QString& profileId)
{
    return store().value(keyFor(profileId)).toString();
}

void ThemeChoice::setForProfile(const QString& profileId, const QString& folder)
{
    store().setValue(keyFor(profileId), folder);
    store().sync();
}

void ThemeChoice::runMigration()
{
    if (store().value(QLatin1String(kMigratedFlag), false).toBool()) return;

    QStringList ids;
    QHash<QString, QString> existing;
    for (const Profile& p : ProfileStore::list())
    {
        ids << p.id;
        const QString v = store().value(keyFor(p.id)).toString();
        if (!v.isEmpty()) existing.insert(p.id, v);
    }

    const QString legacyGlobal = store().value(QLatin1String(kLegacyGlobalKey)).toString();
    const QHash<QString, QString> plan = planMigration(legacyGlobal, ids, existing);
    for (auto it = plan.constBegin(); it != plan.constEnd(); ++it)
        store().setValue(keyFor(it.key()), it.value());

    // The global is gone once its value has been fanned out. Removing it is what makes a later accidental
    // read of the legacy key fail loudly instead of silently serving a stale device-wide theme.
    store().remove(QLatin1String(kLegacyGlobalKey));
    store().setValue(QLatin1String(kMigratedFlag), true);
    store().sync();
}
```

- [ ] **Step 4: Register the probe target in CMake**

In `native/CMakeLists.txt`, immediately after the `probe_onboarding` block (which ends at the
`target_link_libraries(probe_onboarding ...)` line, currently line 420), add:

```cmake
    # Headless test for the per-profile theme choice (roadmap #57): the key format, needsPick, resolve's
    # four-step ordering, the XMB->Triple rename, and the migration table incl. idempotence. ThemeChoice.cpp
    # is QtCore-only (its own file-local store(), AppPaths/AppBrand are header-only), and the only other TU it
    # needs is ProfileStore for the profile id list — so this links leaner than any other probe here.
    add_executable(probe_theme tools/probe_theme.cpp
        src/core/ThemeChoice.cpp src/core/ThemeChoice.h
        src/core/ProfileStore.cpp src/core/ProfileStore.h)
    target_include_directories(probe_theme PRIVATE src src/core)
    target_link_libraries(probe_theme PRIVATE Qt6::Core)
```

Also add `ThemeChoice.cpp`/`ThemeChoice.h` to the main app target's source list, alongside the other
`src/core/*.cpp` entries.

- [ ] **Step 5: Register the probe in the suite script**

In `native/tools/run-headless-probes.sh`, line 130, add `"probe_theme THEME-OK"` to the `for p in ...`
list. Put it immediately after `"probe_brand BRAND-OK"`, before the closing `; do`.

- [ ] **Step 6: Register the probe in CI**

In `.github/workflows/ci.yml`, line 52, append `probe_theme` to the end of the `--target` list.

- [ ] **Step 7: Configure and build (RED expected first)**

```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH"
cmake -S native -B build -DEVERYTHINGBOX_BUILD_APP=ON \
  -DCMAKE_PREFIX_PATH="C:/Qt/6.8.3/msvc2022_64" \
  -DMPV_INCLUDE_DIR="C:/mpv-dev/include" -DMPV_LIBRARY="C:/mpv-dev/libmpv.lib" \
  -DSDL2_INCLUDE_DIR="C:/SDL2/include" -DSDL2_LIBRARY="C:/SDL2/lib/x64/SDL2.lib"
cmake --build build --config Release --target probe_theme
```

Expected: builds clean.

- [ ] **Step 8: Run the probe**

```bash
./build/Release/probe_theme.exe
```

Expected: `THEME-OK`, exit 0.

- [ ] **Step 9: Mutation-test every assertion group**

For each mutation: apply it, rebuild `probe_theme`, run it, confirm it FAILS with the named check,
then revert and confirm `THEME-OK` again. **Record the observed failing line for each in the report.**

| # | Mutation in `ThemeChoice.cpp` | Must fail |
|---|---|---|
| 1 | `needsPick`: `return stored.isEmpty();` (drop the installed check) | the `needsPick("Grid", kShipped) == true` check |
| 2 | `resolve`: delete the `installed.contains(fallback)` branch | the `resolve("Grid", kShipped) == "Triple"` check |
| 3 | `resolve`: `return installed.value(0);` unconditionally | the `resolve("Channels", kShipped) == "Channels"` check |
| 4 | `renameLegacyFolder`: `return stored;` (no-op) | the `renameLegacyFolder("XMB")` and planMigration (b)/(d) checks |
| 5 | `planMigration`: drop the `!have.isEmpty()` guard so the global always wins | the planMigration (c) check |
| 6 | `planMigration`: insert `global` even when empty | the planMigration (e) check |

If any mutation does NOT produce a failure, the corresponding assertion is not real coverage — fix
the assertion before proceeding.

- [ ] **Step 10: Run the full suite**

```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH"
bash native/tools/run-headless-probes.sh
```

Expected: `THEME-OK` present in the output and `ALL HEADLESS PROBES PASSED`. If other probes are
stale, build them first — the script only RUNS pre-built exes.

- [ ] **Step 11: Commit**

```bash
git add native/src/core/ThemeChoice.h native/src/core/ThemeChoice.cpp \
        native/tools/probe_theme.cpp native/CMakeLists.txt \
        native/tools/run-headless-probes.sh .github/workflows/ci.yml
git commit -m "feat: ThemeChoice — per-profile theme key, resolve/needsPick, migration table"
```

---

### Task 2: Cut the shipped set and collapse the eleven call sites

**Files:**
- Delete: `native/themes2/Default/`, `native/themes2/Grid/`, `native/themes2/Lumen/`, `native/themes2/Midnight/`
- Rename: `native/themes2/XMB/` → `native/themes2/Triple/`
- Modify: `native/src/theme2/ThemeEngine.cpp:233`
- Modify: `native/src/ui/MainWindow.cpp` (eleven sites + the migration call), `native/src/ui/MainWindow.h`

**Interfaces:**
- Consumes: every symbol from Task 1's Produces list.
- Produces, for Tasks 3–4: `QString MainWindow::currentThemeFolder() const` — the ONE widget-side
  resolution helper. Returns `ThemeChoice::resolve(ThemeChoice::forProfile(ProfileStore::currentId()),
  ThemeEngine::availableThemes())`.

- [ ] **Step 1: Rename the theme folder with git so history follows**

```bash
git mv native/themes2/XMB native/themes2/Triple
git rm -r native/themes2/Default native/themes2/Grid native/themes2/Lumen native/themes2/Midnight
ls native/themes2/
```

Expected: `Channels`, `THEME_FORMAT.md`, `Triple`.

- [ ] **Step 2: Verify the surviving manifests still declare the right names**

```bash
grep -m1 '"name"' native/themes2/Triple/theme.json native/themes2/Channels/theme.json
```

Expected: `"name": "Triple"` and `"name": "Channels"`. The `Triple` folder's manifest already said
`Triple` before the rename — do not edit it.

- [ ] **Step 3: Change the ThemeEngine empty-fallback**

In `native/src/theme2/ThemeEngine.cpp`, line 233, replace:

```cpp
    if (out.isEmpty()) out << QStringLiteral("Default");
```

with:

```cpp
    // "Default" is no longer a bundled theme (the shipped set is Triple + Channels), so a fallback naming it
    // would hand callers a folder that is nowhere on disk. ThemeChoice::kFallbackTheme is the one name.
    if (out.isEmpty()) out << QString::fromLatin1(ThemeChoice::kFallbackTheme);
```

Add `#include "../core/ThemeChoice.h"` to the includes at the top of that file.

- [ ] **Step 4: Add the resolution helper to MainWindow**

In `native/src/ui/MainWindow.h`, in the private section near the other themed-home declarations
(around line 433), add:

```cpp
    // The ONE widget-side theme resolution (roadmap #57). Every site that used to read
    // themedHome/theme and hand-roll a "not installed -> first" fallback now calls this; ThemeChoice
    // owns the key and the ordering, so the eleven copies of that logic cannot drift apart again.
    QString currentThemeFolder() const;
```

In `native/src/ui/MainWindow.cpp`, immediately above `bool MainWindow::themedHomeEnabled() const`
(currently line 3980), add:

```cpp
QString MainWindow::currentThemeFolder() const
{
    return ThemeChoice::resolve(ThemeChoice::forProfile(ProfileStore::currentId()),
                                ThemeEngine::availableThemes());
}
```

Add `#include "../core/ThemeChoice.h"` to `MainWindow.cpp`'s includes.

- [ ] **Step 5: Replace all six READ sites**

Each of these is the same two-line pattern — a `store().value("themedHome/theme", "Default")` read
followed by an `if (!themes.contains(...))` fallback. Replace the pair with one
`currentThemeFolder()` call at each of: **lines 4013–4014, 4021–4022, 4678–4679, 4917–4918,
5000–5001, and 8017**.

For example, at 4021–4022, replace:

```cpp
    QString themeName = store().value(QStringLiteral("themedHome/theme"), QStringLiteral("Default")).toString();
    if (!themes.contains(themeName)) themeName = themes.value(0, QStringLiteral("Default"));
```

with:

```cpp
    QString themeName = currentThemeFolder();
```

Where the surrounding code no longer uses its local `themes` variable after the substitution, remove
that variable too — a leftover `const QStringList themes = ThemeEngine::availableThemes();` with no
remaining reader is dead code the reviewer will (correctly) flag.

- [ ] **Step 6: Replace all five WRITE sites**

At **lines 4056, 4814, 4933** (the three theme-cycler writes) and **5065, 5163** (the two Appearance
writes), replace:

```cpp
    store().setValue(QStringLiteral("themedHome/theme"), next); store().sync();
```

with:

```cpp
    ThemeChoice::setForProfile(ProfileStore::currentId(), next);
```

(At 5065 and 5163 the variable is named `folder`, not `next` — use the local name that is already
there.) `setForProfile` syncs internally, so drop the trailing `store().sync()`.

- [ ] **Step 7: Wire the migration into startup**

The migration must run before anything reads a theme, and after `ProfileStore` is usable. In
`native/src/ui/MainWindow.cpp`, in the `MainWindow` constructor, immediately after the existing
`ProfileStore::migrateIcons()` call (the established one-time-migration slot), add:

```cpp
    ThemeChoice::runMigration();   // global themedHome/theme -> per-profile, plus the XMB->Triple rename
```

If `ProfileStore::migrateIcons()` is not called from the constructor, place `runMigration()` as the
first statement after the `store()` is first touched in the constructor body — it only needs
`ProfileStore::list()`, which reads the ini directly.

- [ ] **Step 8: Verify no legacy key reader survives**

```bash
grep -n 'themedHome/theme' native/src/ui/MainWindow.cpp
```

Expected: **no output.** Every reference now goes through `ThemeChoice`. Then:

```bash
grep -rn 'QStringLiteral("Default")' native/src/ui/MainWindow.cpp native/src/theme2/ThemeEngine.cpp
```

Expected: **no output.** (`native/src/core/Theme.cpp` keeps its `"Default"` — that is the separate,
still-correct classic colour theme, not the theme2 folder.)

- [ ] **Step 9: Confirm the migration flag is covered by the device-local carve-out**

```bash
grep -n 'device/' native/src/core/CloudSync.cpp | head -5
```

Expected: the existing `key.startsWith(QStringLiteral("device/"))` prefix rule at line ~507. Because
`kMigratedFlag` is `device/themeChoiceMigrated`, it is already covered — **do not add a duplicate
entry**. Confirm by reading the line; do not assume.

- [ ] **Step 10: Build the app and the probes**

```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH"
cmake --build build --config Release --target everythingbox probe_theme probe_theme2 probe_navqml
```

Expected: clean build.

- [ ] **Step 11: Run the suite**

```bash
bash native/tools/run-headless-probes.sh
```

Expected: `ALL HEADLESS PROBES PASSED`, including `THEME-OK` and `THEME2-OK`.

- [ ] **Step 12: Commit**

```bash
git add -A native/themes2 native/src/theme2/ThemeEngine.cpp \
        native/src/ui/MainWindow.cpp native/src/ui/MainWindow.h
git commit -m "feat: cut the shipped themes to Triple + Channels; route every theme read/write through ThemeChoice"
```

---

### Task 3: ThemePickerHost — the reusable picker surface

**Files:**
- Create: `native/src/theme2/ThemePickerHost.h`, `native/src/theme2/ThemePickerHost.cpp`
- Create: `native/src/theme2/qml/ThemePicker.qml`
- Modify: `native/resources/theme2.qrc`, `native/CMakeLists.txt`

**Interfaces:**
- Consumes: `MainWindow::currentThemeFolder()` (Task 2); `ThemeChoice::setForProfile` (Task 1);
  `ThemeEngine::availableThemes()`, `ThemeEngine::themeDisplayName(folder)`,
  `ThemeEngine::themesRoot()`, `ThemeEngine::buildView(...)`, `ThemeEngine::rootItem(widget)`.
- Produces, for Task 4:
  - `ThemePickerHost(QWidget* parent)` — constructed once in the `MainWindow` ctor, added to `stack_`.
  - `void ThemePickerHost::present(const QString& title, const QString& currentFolder, bool mustChoose, std::function<void(const QString& folder)> onPicked, std::function<void()> onBack)`
  - `void ThemePickerHost::setStyle(const QVariantMap& style)`
  - `QString ThemePickerHost::title() const` — for a `themedPanelIsTop`-style late-async gate.
  - `static QVariantList ThemePickerHost::previewItems()` — the shared synthetic sample data.

- [ ] **Step 1: Add the QML file to the resource bundle in the same step it is created**

In `native/resources/theme2.qrc`, add inside the `<qresource prefix="/theme2">` block, immediately
after the `ThemeView.qml` line:

```xml
    <file alias="ThemePicker.qml">../src/theme2/qml/ThemePicker.qml</file>
```

A `.qml` that is not listed here is silently not embedded and the host will fail to load at runtime
with an obscure "component not ready" error.

- [ ] **Step 2: Create the QML**

Create `native/src/theme2/qml/ThemePicker.qml`:

```qml
// ThemePicker — the themed theme-chooser surface rendered by ThemePickerHost, used BOTH as the forced
// first-run step and from Appearance. A list of installed themes on the left; the right pane is filled by the
// host with a real ThemeEngine::buildView of the highlighted theme, so the preview IS the renderer and a
// community theme previews for free.
//
// It reads two context properties the host installs: `nav` (a Vertical `themeRows` zone — the ONLY zone this
// surface registers) and `picker` (title + resolved settingsPanel style + the theme display names + the
// current index + whether Back is allowed). Style resolution and the form-factor tokens follow
// elements/SettingsPanel.qml verbatim.
//
// THE PREVIEW IS NOT A NAV ZONE. It is a live QQuickWidget parented in by the host with Qt::NoFocus, and it
// is registered in no zone: a focusable live view inside a nav surface takes the cursor and strands the user
// in a preview they cannot leave.
import QtQuick

Rectangle {
    id: root
    focus: true

    readonly property real ffs:     (typeof form !== "undefined" && form) ? form.uiScale : 1
    readonly property real density: (typeof form !== "undefined" && form) ? form.density : 1
    readonly property int  safeInset: Math.round(Math.min(width, height) * ((typeof form !== "undefined" && form) ? form.safeAreaFrac : 0))
    readonly property real safeTop:    (typeof safeArea !== "undefined" && safeArea) ? safeArea.top : 0
    readonly property real safeBottom: (typeof safeArea !== "undefined" && safeArea) ? safeArea.bottom : 0

    readonly property var g:  (typeof nav !== "undefined") ? nav : null
    readonly property var st: (typeof picker !== "undefined" && picker && picker.style) ? picker.style : ({})
    function col(key, def) { return (st && st[key] !== undefined && st[key] !== "") ? st[key] : def }

    readonly property color cBg:     col("background",  "#0F1216")
    readonly property color cPanel:  col("panel",       "#161A20")
    readonly property color cRow:    col("row",         "#1A1F27")
    readonly property color cRowSel: col("rowSelected", "#243244")
    readonly property color cAccent: col("accent",      "#3A6FB0")
    readonly property color cText:   col("text",        "#E6ECF3")
    readonly property color cDim:    col("dim",         "#9AA6B2")

    color: cBg

    readonly property var names: (typeof picker !== "undefined" && picker) ? picker.names : []
    readonly property int  sel:  g ? g.indexFor("themeRows") : 0

    // The host reads this to know where to place the preview QQuickWidget (scene coords -> widget coords).
    property alias previewX: previewSlot.x
    property alias previewY: previewSlot.y
    property alias previewW: previewSlot.width
    property alias previewH: previewSlot.height

    Text {
        id: heading
        x: Math.round(28 * ffs) + safeInset
        y: Math.round(22 * ffs) + safeInset + safeTop
        text: (typeof picker !== "undefined" && picker) ? picker.title : ""
        color: cText
        font.pixelSize: Math.round(26 * ffs)
        font.bold: true
    }

    Text {
        id: hint
        anchors.left: heading.left
        anchors.top: heading.bottom
        anchors.topMargin: Math.round(6 * ffs)
        text: (typeof picker !== "undefined" && picker && picker.mustChoose)
              ? qsTr("Pick a look for this profile. You can change it later in Settings ▸ Appearance.")
              : qsTr("Previews live. Press Enter to keep the highlighted theme.")
        color: cDim
        font.pixelSize: Math.round(14 * ffs)
    }

    Rectangle {
        id: listPane
        x: heading.x
        anchors.top: hint.bottom
        anchors.topMargin: Math.round(18 * ffs)
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Math.round(28 * ffs) + safeInset + safeBottom
        width: Math.round(300 * ffs)
        color: cPanel
        radius: Math.round(10 * ffs)

        ListView {
            id: list
            anchors.fill: parent
            anchors.margins: Math.round(8 * ffs)
            clip: true
            model: root.names
            currentIndex: root.sel
            highlightFollowsCurrentItem: true
            // Keep the cursor visible when the list is longer than the pane (a user with many community
            // themes) — the same reason NavMenu had to learn to scroll.
            onCurrentIndexChanged: positionViewAtIndex(currentIndex, ListView.Contain)

            delegate: Rectangle {
                width: list.width
                height: Math.round(46 * ffs * density)
                radius: Math.round(7 * ffs)
                color: index === root.sel ? cRowSel : cRow
                border.width: index === root.sel ? Math.max(1, Math.round(2 * ffs)) : 0
                border.color: cAccent

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left
                    anchors.leftMargin: Math.round(14 * ffs)
                    anchors.right: parent.right
                    anchors.rightMargin: Math.round(10 * ffs)
                    elide: Text.ElideRight
                    text: modelData
                    color: cText
                    font.pixelSize: Math.round(15 * ffs)
                }

                MouseArea {
                    anchors.fill: parent
                    onClicked: if (root.g) root.g.select("themeRows", index)
                    onDoubleClicked: if (root.g) root.g.activate("themeRows", index)
                }
            }
        }
    }

    // The slot the host fills with the live preview widget. Deliberately an empty Item: nothing in QML draws
    // the preview, because the preview is a real QQuickWidget the host parents over this rectangle.
    Item {
        id: previewSlot
        anchors.left: listPane.right
        anchors.leftMargin: Math.round(20 * ffs)
        anchors.top: listPane.top
        anchors.bottom: listPane.bottom
        anchors.right: parent.right
        anchors.rightMargin: Math.round(28 * ffs) + safeInset
    }
}
```

- [ ] **Step 3: Create the host header**

Create `native/src/theme2/ThemePickerHost.h`:

```cpp
// ThemePickerHost — the ONE theme-chooser surface, used both as the forced first-run step and from
// Appearance. A persistent stack page constructed in the MainWindow ctor (like ThemedPanelHost and
// ReaderChromeHost), so it can present PRE-HOME: at first run openHome() has not run and home_ does not
// exist yet.
//
// It owns:
//   * a NavGraph with a single Vertical `themeRows` zone, exposed to the QML as `nav`;
//   * a PickerBridge (`picker` context property) carrying the title, the resolved settingsPanel style, the
//     theme display names, and whether Back is allowed;
//   * the live preview: a real ThemeEngine::buildView of the highlighted theme, rebuilt on selection change
//     and reparented over the QML's preview slot.
//
// THE PREVIEW IS REGISTERED IN NO NAV ZONE and is created Qt::NoFocus. A focusable live view inside a nav
// surface takes the cursor and strands the user in a preview they cannot leave — the single constraint this
// class exists to hold.
#pragma once
#include <QVariantList>
#include <QVariantMap>
#include <QWidget>
#include <functional>

class NavGraph;
class QQuickWidget;

// The `picker` context property the QML reads.
class PickerBridge : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString title READ title NOTIFY changed)
    Q_PROPERTY(QVariantMap style READ style NOTIFY changed)
    Q_PROPERTY(QStringList names READ names NOTIFY changed)
    Q_PROPERTY(bool mustChoose READ mustChoose NOTIFY changed)
public:
    using QObject::QObject;
    QString title() const { return title_; }
    QVariantMap style() const { return style_; }
    QStringList names() const { return names_; }
    bool mustChoose() const { return mustChoose_; }

    void set(const QString& t, const QStringList& n, bool must);
    void setStyleMap(const QVariantMap& s);
signals:
    void changed();
private:
    QString title_;
    QVariantMap style_;
    QStringList names_;
    bool mustChoose_ = false;
};

class ThemePickerHost : public QWidget
{
    Q_OBJECT
public:
    explicit ThemePickerHost(QWidget* parent = nullptr);

    // Present the picker. `currentFolder` seeds the highlighted row. `mustChoose` = the forced first-run
    // mode: Back runs onBack (the caller wires the quit-confirm), and there is no other exit.
    // onPicked receives the chosen FOLDER name (not the display name).
    void present(const QString& title, const QString& currentFolder, bool mustChoose,
                 std::function<void(const QString& folder)> onPicked,
                 std::function<void()> onBack);

    void setStyle(const QVariantMap& style);
    QString title() const { return titleText_; }
    NavGraph* graph() const { return graph_; }

    // The synthetic sample data both this surface and the classic Appearance preview render. At first run
    // there is no library and no home_, so the synthetic path is the ONLY path that works there — sharing it
    // is what stops the two previews drifting.
    static QVariantList previewItems();

protected:
    void resizeEvent(QResizeEvent* e) override;

private:
    void rebuildPreview();          // tear down + rebuild the live preview for the selected row
    void layoutPreview();           // position the preview widget over the QML's preview slot

    QQuickWidget* view_ = nullptr;  // the picker chrome
    QQuickWidget* preview_ = nullptr; // the live theme preview (NoFocus, no nav zone)
    NavGraph* graph_ = nullptr;
    PickerBridge* bridge_ = nullptr;
    QString titleText_;
    QStringList folders_;           // folder names, index-aligned with the bridge's display names
    std::function<void(const QString&)> onPicked_;
    std::function<void()> onBack_;
};
```

- [ ] **Step 4: Create the host implementation**

Create `native/src/theme2/ThemePickerHost.cpp`. Follow `ThemedPanelHost.cpp` verbatim for the
QQuickWidget construction, the `nav`/bridge context-property installation, and the graph wiring. The
parts specific to this class:

```cpp
QVariantList ThemePickerHost::previewItems()
{
    // The four inherent categories, so an XMB/Triple theme shows its cross and a Grid-style theme shows
    // tiles. Matches what the classic Appearance preview has always used as its empty-library fallback.
    QVariantList items;
    for (const char* n : { "Video", "Games", "Audio", "Reading" })
        items << QVariantMap{ { QStringLiteral("title"), QString::fromLatin1(n) },
                              { QStringLiteral("accent"), QStringLiteral("#3E8E7E") } };
    return items;
}

void ThemePickerHost::rebuildPreview()
{
    if (preview_) { preview_->deleteLater(); preview_ = nullptr; }
    const int idx = graph_ ? graph_->indexFor(QStringLiteral("themeRows")) : 0;
    const QString folder = folders_.value(idx);
    if (folder.isEmpty()) return;

    QVariantMap system; system.insert(QStringLiteral("name"), QStringLiteral("EverythingBox"));
    QWidget* w = ThemeEngine::buildView(ThemeEngine::themesRoot() + QStringLiteral("/") + folder,
                                        previewItems(), system, this);
    preview_ = qobject_cast<QQuickWidget*>(w);
    if (!preview_) { if (w) w->deleteLater(); return; }

    // THE constraint: the preview must never take the cursor. NoFocus on the widget, and it is registered
    // in no nav zone, so arrows/Enter always reach the list.
    preview_->setFocusPolicy(Qt::NoFocus);
    preview_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    if (QQuickItem* r = ThemeEngine::rootItem(preview_))
        { r->setProperty("categories", previewItems()); r->setProperty("catIndex", 0); }
    preview_->show();
    layoutPreview();
}

void ThemePickerHost::layoutPreview()
{
    if (!preview_ || !view_) return;
    QQuickItem* root = view_->rootObject();
    if (!root) return;
    // The QML exposes the slot's geometry; mirror it onto the overlaid widget.
    const int x = int(root->property("previewX").toReal());
    const int y = int(root->property("previewY").toReal());
    const int w = int(root->property("previewW").toReal());
    const int h = int(root->property("previewH").toReal());
    if (w > 0 && h > 0) { preview_->setGeometry(x, y, w, h); preview_->raise(); }
}

void ThemePickerHost::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    if (view_) view_->setGeometry(rect());
    layoutPreview();   // the slot moved with the chrome
}
```

Wire the graph so that:
- selection change on `themeRows` calls `rebuildPreview()`;
- activation on `themeRows` reads `folders_.value(index)` and calls `onPicked_(folder)`;
- the graph's root-back calls `onBack_()`.

`present()` fills `folders_` from `ThemeEngine::availableThemes()`, builds the display-name list with
`ThemeEngine::themeDisplayName(folder)` index-aligned to it, seeds the graph's selection to
`folders_.indexOf(currentFolder)` (clamped to 0 when absent), sets the bridge, then calls
`rebuildPreview()`.

- [ ] **Step 5: Add the sources to CMake**

In `native/CMakeLists.txt`, add `src/theme2/ThemePickerHost.cpp` and `src/theme2/ThemePickerHost.h`
to the main app target's source list, alongside `src/theme2/ThemedPanelHost.cpp`.

- [ ] **Step 6: Build**

```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH"
cmake -S native -B build -DEVERYTHINGBOX_BUILD_APP=ON \
  -DCMAKE_PREFIX_PATH="C:/Qt/6.8.3/msvc2022_64" \
  -DMPV_INCLUDE_DIR="C:/mpv-dev/include" -DMPV_LIBRARY="C:/mpv-dev/libmpv.lib" \
  -DSDL2_INCLUDE_DIR="C:/SDL2/include" -DSDL2_LIBRARY="C:/SDL2/lib/x64/SDL2.lib"
cmake --build build --config Release --target everythingbox
```

Expected: clean build. (One reconfigure is required because sources were added.)

- [ ] **Step 7: Verify the QML actually embedded**

```bash
grep -c "ThemePicker.qml" native/resources/theme2.qrc
```

Expected: `1`. A missing entry is silent at build time and only fails at runtime.

- [ ] **Step 8: Commit**

```bash
git add native/src/theme2/ThemePickerHost.h native/src/theme2/ThemePickerHost.cpp \
        native/src/theme2/qml/ThemePicker.qml native/resources/theme2.qrc native/CMakeLists.txt
git commit -m "feat: ThemePickerHost — one theme-picker surface with a no-focus live preview"
```

---

### Task 4: The forced pick + Appearance rewiring in both builders

**Files:**
- Modify: `native/src/ui/MainWindow.cpp` (`chooseProfile` ~5692, themed Appearance ~5040–5070,
  classic Appearance ~5100–5167, the ctor), `native/src/ui/MainWindow.h`

**Interfaces:**
- Consumes: `ThemePickerHost::present(...)`, `ThemePickerHost::previewItems()`,
  `MainWindow::currentThemeFolder()`, `ThemeChoice::needsPick`, `ThemeChoice::setForProfile`.
- Produces: nothing further — this is the last behaviour task.

- [ ] **Step 1: Construct the host**

In the `MainWindow` constructor, next to where `themedPanelHost_` is constructed and added to
`stack_`, add the same for a new `themePickerHost_` member. Declare it in `MainWindow.h` beside
`themedPanelHost_`, inside the same `#ifdef EB_HAVE_QML` guard those members use.

- [ ] **Step 2: Add the forced-pick presenter**

In `native/src/ui/MainWindow.h`, private section:

```cpp
    // The forced first-run theme step (roadmap #57). Presented from chooseProfile when the newly-current
    // profile has no theme yet; on pick it stores the choice and runs the openHome() it displaced.
    void presentThemePick(std::function<void()> afterPick);
    QString themePickTitle() const;   // one title source, for the late-async top gate
```

In `native/src/ui/MainWindow.cpp`, near `presentOnboardingChoice` (~5384):

```cpp
QString MainWindow::themePickTitle() { return tr("Pick your look"); }

void MainWindow::presentThemePick(std::function<void()> afterPick)
{
#ifdef EB_HAVE_QML
    clearPanelPageConns();
    themePickerHost_->setStyle(settingsPanelStyle());
    NavOverlay::setThemeColors(settingsPanelStyle());
    themePickerHost_->present(
        themePickTitle(), currentThemeFolder(), /*mustChoose*/ true,
        [this, afterPick](const QString& folder) {
            ThemeChoice::setForProfile(ProfileStore::currentId(), folder);
            afterPick();
        },
        [this] { quitConfirmFromStartup(); });   // forced step: no escape, same contract as the profile picker
    stack_->setCurrentWidget(themePickerHost_);
    updateNavForPage();
    updateBackgroundMusic();
#else
    afterPick();   // classic build: no themed surface, resolve through ThemeChoice and carry on
#endif
}
```

- [ ] **Step 3: Inject at chooseProfile**

In `native/src/ui/MainWindow.cpp:5692`, `chooseProfile`, replace the `openHome();` line and the
comment above it with:

```cpp
    // Roadmap #57: a profile with no theme picks one BEFORE its home renders. Keying off the profile
    // (not a device flag) is what makes a SECOND profile created months later get its own pick, while a
    // migrated or restored profile — both of which carry a stored theme — never sees this.
    auto finish = [this] {
        openHome();   // render for the chosen profile (also the pre-home startup finish)
        QTimer::singleShot(0, this, [this] { maybeOfferTvMode(); });
    };
#ifdef EB_HAVE_QML
    if (themedHomeEnabled() && themePickerHost_ && ThemeChoice::needsPick(ThemeChoice::forProfile(id)))
        { presentThemePick(finish); return; }
#endif
    finish();
```

Delete the old trailing `openHome();` and its `maybeOfferTvMode` singleShot — they now live in
`finish`, which both branches run exactly once.

Note the argument: `ThemeChoice::forProfile(id)`, using the **id being switched to**, not
`ProfileStore::currentId()`. They are equal here because `setCurrent(id)` ran three lines above, but
naming `id` makes the dependency explicit and survives a future reorder.

- [ ] **Step 4: Rewire the THEMED Appearance panel**

In the themed branch of `openAppearance()`, replace the `choice(QStringLiteral("appr.theme"), ...)`
row and the `info(QStringLiteral("appr.applies"), ...)` row beneath it with a single action row:

```cpp
        action(QStringLiteral("appr.theme"), tr("Theme…   %1").arg(ThemeEngine::themeDisplayName(curFolder)));
```

Then in the same panel's activation handler, replace the whole `else if (id == QStringLiteral("appr.theme"))`
block with:

```cpp
                else if (id == QStringLiteral("appr.theme")) {
                    // The real preview lives in ThemePickerHost. This replaces the old "recolour this panel
                    // and call that the preview" approximation.
                    themePickerHost_->setStyle(settingsPanelStyle());
                    themePickerHost_->present(
                        tr("Theme"), currentThemeFolder(), /*mustChoose*/ false,
                        [this](const QString& folder) {
                            ThemeChoice::setForProfile(ProfileStore::currentId(), folder);
                            openAppearance();       // re-render Appearance with the new style + label
                        },
                        [this] { openAppearance(); });
                    stack_->setCurrentWidget(themePickerHost_);
                    updateNavForPage();
                }
```

The `themePairs`/`themeOpts` locals that only fed the removed Choice row become unused — delete
them. Keep `curFolder`, which the action row's label now uses; source it from
`currentThemeFolder()` rather than the old inline read.

- [ ] **Step 5: Rewire the CLASSIC Appearance panel**

The classic panel already has a working list + live preview — the only change is the key. At the
list-population read (~line 5112) replace:

```cpp
        const QString current = store().value(QStringLiteral("themedHome/theme"), QStringLiteral("Default")).toString();
```

with:

```cpp
        const QString current = currentThemeFolder();
```

and at the selection handler (~line 5163) replace the `store().setValue(...)` pair with:

```cpp
            ThemeChoice::setForProfile(ProfileStore::currentId(), folder);
```

Then replace the classic panel's inline synthetic-preview construction with the shared helper so the
two previews cannot drift:

```cpp
    QVariantList previewItems = home_ ? home_->categoryItems() : QVariantList();
    if (previewItems.isEmpty()) previewItems = ThemePickerHost::previewItems();
```

(The `home_ ?` guard is new and load-bearing: `openAppearance` is now reachable from a pre-home state
where `home_` is null.)

- [ ] **Step 6: Build**

```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH"
cmake --build build --config Release --target everythingbox probe_navqml probe_nav probe_theme
```

Expected: clean build.

- [ ] **Step 7: Run the suite**

```bash
bash native/tools/run-headless-probes.sh
```

Expected: `ALL HEADLESS PROBES PASSED`.

- [ ] **Step 8: Commit**

```bash
git add native/src/ui/MainWindow.cpp native/src/ui/MainWindow.h
git commit -m "feat: force a theme pick for new profiles; Appearance opens the real picker in both builders"
```

---

### Task 5: Close-out — live verification, review, merge, redeploy

**Files:** no source changes expected; fixes found here are committed as they arise.

- [ ] **Step 1: Build a throwaway portable copy**

Never launch or modify the real install. Copy the deployed tree to scratch, then strip credentials:

```bash
SCRATCH="/c/Users/cubma/AppData/Local/Temp/claude/C--Users-cubma-Project-Goliath/7ae938b6-7f10-482b-ad59-ba50699a6c23/scratchpad/eb-t57"
rm -rf "$SCRATCH" && mkdir -p "$SCRATCH"
cp -r /c/EverythingBox-app/* "$SCRATCH"/
cp build/Release/everythingbox.exe "$SCRATCH"/EverythingBox.exe
rm -rf "$SCRATCH"/themes2 && cp -r native/themes2 "$SCRATCH"/themes2
ls "$SCRATCH"/themes2
```

Expected: `Channels`, `THEME_FORMAT.md`, `Triple`.

Then remove every `cloud/*` and `sync/*` line from **every** `.ini` in `$SCRATCH` — the current one
and any pre-rebrand file still sitting beside it. Verify none remain:

```bash
grep -c -E '^(cloud|sync)/' "$SCRATCH"/*.ini
```

Expected: `0`. **Never print an ini line that could carry a credential value.**

- [ ] **Step 2: Verify the fresh-profile forced pick**

Delete the scratch ini entirely to simulate a brand-new install, launch under the UI-test harness on
a renamed pipe so it cannot collide with any running instance, and drive it:

```bash
rm -f "$SCRATCH"/everythingbox.ini
EB_UITEST=1 EB_UITEST_PIPE=t57 "$SCRATCH"/EverythingBox.exe &
python native/tools/uitest.py --pipe t57 shot t57-01-firstrun.png
```

Walk through onboarding to profile creation, then confirm the theme picker appears **before** the
home screen, that it lists exactly Triple and Channels, that the preview changes with the selection,
and that Back does not escape it. Capture a screenshot at each step.

- [ ] **Step 3: Verify the preview never takes the cursor**

With the picker open, press Right repeatedly, then Down/Up, and confirm the selection stays in the
list and the preview never gains focus. This is the constraint most likely to be wrong and it is
invisible to any headless check. Capture a screenshot showing the selection still on a list row
after pressing Right.

- [ ] **Step 4: Verify a second profile gets its own pick**

From the running app, create a second profile and switch to it. Confirm the picker appears again for
that profile, and that switching back to the first profile does **not** re-ask.

- [ ] **Step 5: Verify the migration**

Stop the app. Seed a scratch ini with a legacy global value and two profiles (no per-profile theme
keys), relaunch, and confirm: no pick is forced, both profiles render the migrated theme, the global
key is gone, and `device/themeChoiceMigrated` is set. Repeat with the global set to `XMB` and
confirm both profiles resolve to `Triple`.

```bash
grep -n 'themedHome/theme' "$SCRATCH"/everythingbox.ini
```

Expected after migration: only `themedHome/theme/<id>` lines, no bare `themedHome/theme=`.

- [ ] **Step 6: Verify Appearance in both builders**

Themed: Settings ▸ Appearance ▸ `Theme…` opens the picker, a pick applies and returns to Appearance
with the label updated, Back returns without changing anything. Classic (`themedHome/enabled` off):
the list + preview still work and write the per-profile key.

- [ ] **Step 7: Clean up the throwaway with zero residue**

```bash
python native/tools/uitest.py --pipe t57 quit || true
rm -rf "$SCRATCH"
ls -d "$SCRATCH" 2>&1
```

Expected: "No such file or directory". Scope any process kill to the PID launched in Step 2 — never a
name-wide kill, which would take down the user's real instance.

- [ ] **Step 8: Full suite on the final tree**

```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH"
cmake --build build --config Release --target everythingbox probe_theme probe_theme2 probe_nav probe_navqml probe_onboarding probe_brand
bash native/tools/run-headless-probes.sh
```

Expected: `ALL HEADLESS PROBES PASSED`, including `THEME-OK` and `PASS: old-brand references`.

- [ ] **Step 9: Whole-branch review**

Generate the review package and dispatch the final reviewer on the most capable model:

```bash
git log --oneline $(git merge-base main HEAD)..HEAD
```

Feed the reviewer the package path, the spec, and this plan's Global Constraints.

- [ ] **Step 10: Merge and push**

```bash
git checkout main && git merge --no-ff <branch> && git push origin main
```

Absorb any upstream work first; if `main` has advanced, merge it in and re-run the suite on the
combined tree before pushing. A combined-tree build has caught a latent link break before.

- [ ] **Step 11: Redeploy and verify the binary**

```bash
cp build/Release/everythingbox.exe /c/EverythingBox-app/EverythingBox.exe
rm -rf /c/EverythingBox-app/themes2/Default /c/EverythingBox-app/themes2/Grid \
       /c/EverythingBox-app/themes2/Lumen /c/EverythingBox-app/themes2/Midnight
cp -r native/themes2/Triple /c/EverythingBox-app/themes2/Triple
md5sum build/Release/everythingbox.exe /c/EverythingBox-app/EverythingBox.exe
```

Expected: identical md5 sums.

**Ask the user before deleting the cut theme folders from the real install** — the migration
deliberately lets an existing user keep a cut theme, and removing them here contradicts that. If the
user has one of those themes selected, deleting it would force them into a pick. Default to leaving
them and only removing the stale `XMB` folder if `Triple` is confirmed present.

- [ ] **Step 12: Update the ledger**

Append one line per completed task to `.superpowers/sdd/progress.md`, and a close-out line recording
the merge commit, the deploy md5, and any follow-ups found.

---

## Self-Review

**Spec coverage:** §1 ThemeChoice → Task 1. §2 the cut → Task 2 Steps 1–3. The eleven call sites →
Task 2 Steps 4–6. Migration → Task 1 (table) + Task 2 Step 7 (wiring) + Task 5 Step 5 (live). §3
picker surface → Task 3. §4 forced pick → Task 4 Steps 2–3. §5 Appearance both builders → Task 4
Steps 4–5. §6 probe + mutation testing → Task 1 Steps 1, 9; live verify → Task 5. Device-local
carve-out → Task 2 Step 9. No gaps.

**Type consistency:** `kFallbackTheme`, `keyFor`, `needsPick`, `resolve`, `renameLegacyFolder`,
`planMigration`, `forProfile`, `setForProfile`, `runMigration` are spelled identically in Task 1's
header, its implementation, the probe, and every consumer in Tasks 2–4. `currentThemeFolder()` is
declared in Task 2 and consumed in Tasks 3–4. `ThemePickerHost::present` has the same five-parameter
signature in Task 3's header and both Task 4 call sites. `previewItems()` is static in Task 3 and
called unqualified-static from Task 4 Step 5.

**Known judgement call for the reviewer:** Task 4 Step 4 deletes the themed Appearance `Choice` row
in favour of an Action row. That removes the ability to cycle themes without leaving the panel. This
is intended — the Choice row's "preview" was only a panel recolour — but a reviewer may reasonably
flag the lost inline affordance, so it is called out here rather than discovered.
