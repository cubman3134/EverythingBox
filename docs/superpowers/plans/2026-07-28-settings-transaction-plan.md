# Settings Save/Discard Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Leaving a settings screen with changes asks Save / Discard / Keep editing, and Discard genuinely reverts — including settings that applied live.

**Architecture:** Immediate-apply stays completely unchanged. A new `SettingsTxn` core unit snapshots the settings-scope keys on entry to the settings area; Discard restores them and re-runs the side effects. No settings row changes how it writes, so live-preview rows (theme, display mode) need no special case.

**Tech Stack:** Qt 6.8.3 (Core, Widgets), C++17, CMake. Headless console probe for verification. No new dependencies.

## Global Constraints

- **Build config is always Release**, always a named target: `cmake --build build --config Release --target <name>`. A target-less build compiles ~43 probes and stalls.
- **Adding a source file requires ONE reconfigure.** Create a `.cpp` in the SAME step that adds it to CMake — CMake resolves source lists at configure time.
- **The suite script only RUNS pre-built exes.** Build targets first, then run it. A green suite on stale binaries has shipped before.
- **A new probe must be registered in THREE places** or it silently never runs: its `add_executable` in `native/CMakeLists.txt`, the loop list in `native/tools/run-headless-probes.sh` (~line 130), and the `--target` list in `.github/workflows/ci.yml` (~line 52).
- **Modal UI goes through the nav kit only** — `NavConfirm`/`NavMenu`/`Osk`. Never `QDialog`/`QMessageBox`/`QInputDialog`/top-level windows. `probe_nav` gates this.
- **Both settings builders.** Every hub screen exists in a themed (`PanelRow`) and a classic (QWidget `showPanel`) form. A change must land in both or it is unreachable in one mode.
- **Mutation testing is the standard of proof.** Break the implementation, confirm the probe FAILS, revert, confirm green. An assertion that passes under a broken implementation is not coverage.
- **Out-of-scope keys are exactly:** the `CloudMerge`-owned families `resume/`, `recent/`, `marks/`, `favorites/`, `playlists/`, `stats/`, `playstats/`, `deleted/`; plus `cloud/`, `device/`, `downloads`, `pcgames/`.
- **In scope despite being device-local:** `display/mode`, `roms/folder`, `library/folder`, `emulators/root`. A naive "exclude everything device-local" implementation is WRONG and the probe pins this.
- **The prompt reports a COUNT, never values.** Credential rows are masked and must never be rendered into a diff.
- **Never touch the real deployed app** at `C:\EverythingBox-app` or its ini. Copy to a scratch dir. Strip `cloud/*` and `sync/*` from any throwaway ini. Never print, log, echo or screenshot a credential value.

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
| `native/src/core/SettingsTxn.h` | The transaction: `inScope` predicate + `begin`/`isDirty`/`commit`/`rollback`/`active`. |
| `native/src/core/SettingsTxn.cpp` | Implementation. QtCore only, own file-local `store()`, so the probe links lean. |
| `native/tools/probe_settingstxn.cpp` | Headless probe. Sentinel `SETTINGSTXN-OK`. |

**Modified:**

| File | Change |
|---|---|
| `native/CMakeLists.txt` | `probe_settingstxn` target; `SettingsTxn.*` into the app target. |
| `native/tools/run-headless-probes.sh` | `probe_settingstxn SETTINGSTXN-OK` in the loop list. |
| `.github/workflows/ci.yml` | `probe_settingstxn` in the `--target` list. |
| `native/src/core/CloudSync.cpp` | `applySettingsJson` commits an open txn before writing. |
| `native/src/ui/MainWindow.cpp` | `begin()` on settings entry; the three-way prompt on both hub exits. |
| `native/src/ui/MainWindow.h` | Declaration for the shared leave helper. |

---

### Task 1: SettingsTxn core + probe_settingstxn

**Files:**
- Create: `native/src/core/SettingsTxn.h`, `native/src/core/SettingsTxn.cpp`, `native/tools/probe_settingstxn.cpp`
- Modify: `native/CMakeLists.txt`, `native/tools/run-headless-probes.sh`, `.github/workflows/ci.yml`

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces, for Tasks 2–3:
  - `bool SettingsTxn::inScope(const QString& key)`
  - `void SettingsTxn::begin()` — no-op when already active
  - `bool SettingsTxn::active()`
  - `int  SettingsTxn::dirtyCount()` — number of in-scope keys that differ from the snapshot
  - `bool SettingsTxn::isDirty()` — `dirtyCount() > 0`
  - `void SettingsTxn::commit()`
  - `void SettingsTxn::rollback()`
  - `void SettingsTxn::setIniPathForTesting(const QString& path)` — behind `#ifdef EB_SETTINGSTXN_TEST_SEAM`

Note `dirtyCount()` rather than only a bool: the prompt's message states how many settings changed, and a count is also a far more useful probe assertion than a bool.

- [ ] **Step 1: Write the probe first (RED)**

Create `native/tools/probe_settingstxn.cpp`:

```cpp
// Headless check of the settings save/discard transaction (issue #26). SettingsTxn snapshots the
// settings-scope keys when the settings area is entered; Discard restores them. The load-bearing part is
// the SCOPE predicate: a whole-ini snapshot would clobber cloud sync, stats accrual and resume positions
// that are written while a panel is open — so this pins which keys the transaction may touch, and proves a
// key outside that scope written mid-transaction survives rollback untouched.
//
// Prints SETTINGSTXN-OK on success; any failure prints SETTINGSTXN-FAIL <cond> and exits non-zero.
#include "SettingsTxn.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QString>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "SETTINGSTXN-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

static QString iniPath()
{
    return QDir::tempPath() + QStringLiteral("/eb-probe-settingstxn.ini");
}

static void freshIni()
{
    QFile::remove(iniPath());
    SettingsTxn::setIniPathForTesting(iniPath());
}

static void put(const QString& k, const QString& v)
{
    QSettings s(iniPath(), QSettings::IniFormat);
    s.setValue(k, v); s.sync();
}

static QString get(const QString& k)
{
    QSettings s(iniPath(), QSettings::IniFormat);
    return s.value(k).toString();
}

static bool has(const QString& k)
{
    QSettings s(iniPath(), QSettings::IniFormat);
    return s.contains(k);
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ---- 1. inScope: the families that must be EXCLUDED --------------------------------------------
    // Everything CloudMerge owns. These are written continuously by playback, marking and stats accrual
    // while a settings panel is open; rolling them back would be data loss, not a nuisance.
    for (const char* k : { "resume/abc", "recent/p1", "marks/p1/items", "favorites/p1",
                           "playlists/p1", "stats/p1/dev", "playstats/p1/dev", "deleted/x" })
        CHECK(SettingsTxn::inScope(QString::fromLatin1(k)) == false);
    // OAuth tokens: signing in is not a setting you discard.
    CHECK(SettingsTxn::inScope(QStringLiteral("cloud/refreshToken")) == false);
    // This install's identity and one-shot migration flags.
    CHECK(SettingsTxn::inScope(QStringLiteral("device/id")) == false);
    CHECK(SettingsTxn::inScope(QStringLiteral("device/themeChoiceMigrated")) == false);
    // Catalogs written by background download / import.
    CHECK(SettingsTxn::inScope(QStringLiteral("downloads")) == false);
    CHECK(SettingsTxn::inScope(QStringLiteral("downloads/items")) == false);
    CHECK(SettingsTxn::inScope(QStringLiteral("pcgames/abc/exe")) == false);

    // ---- 2. inScope: DEVICE-LOCAL BUT IN SCOPE ----------------------------------------------------
    // These are the cases a naive "exclude everything CloudSync::isDeviceLocalKey covers" implementation
    // gets WRONG. They are per-device AND they are settings rows a user must be able to discard.
    CHECK(SettingsTxn::inScope(QStringLiteral("display/mode")) == true);
    CHECK(SettingsTxn::inScope(QStringLiteral("roms/folder")) == true);
    CHECK(SettingsTxn::inScope(QStringLiteral("library/folder")) == true);
    CHECK(SettingsTxn::inScope(QStringLiteral("emulators/root")) == true);
    // Ordinary settings.
    CHECK(SettingsTxn::inScope(QStringLiteral("subs/language")) == true);
    CHECK(SettingsTxn::inScope(QStringLiteral("themedHome/theme/p1")) == true);
    CHECK(SettingsTxn::inScope(QStringLiteral("playback/autoplayNext")) == true);
    // A prefix that merely STARTS like an excluded one must not be excluded by a sloppy startsWith.
    CHECK(SettingsTxn::inScope(QStringLiteral("statsPanel/lastTab")) == true);
    CHECK(SettingsTxn::inScope(QStringLiteral("recentlyUsed/x")) == true);

    // ---- 3. begin / isDirty / dirtyCount ----------------------------------------------------------
    freshIni();
    put(QStringLiteral("subs/language"), QStringLiteral("en"));
    SettingsTxn::begin();
    CHECK(SettingsTxn::active() == true);
    // Nothing touched -> clean. Leaving without changing anything must NEVER prompt.
    CHECK(SettingsTxn::isDirty() == false);
    CHECK(SettingsTxn::dirtyCount() == 0);

    put(QStringLiteral("subs/language"), QStringLiteral("fr"));
    CHECK(SettingsTxn::isDirty() == true);
    CHECK(SettingsTxn::dirtyCount() == 1);

    // Changed and changed BACK reads clean: isDirty compares values, it does not count edits.
    put(QStringLiteral("subs/language"), QStringLiteral("en"));
    CHECK(SettingsTxn::isDirty() == false);
    CHECK(SettingsTxn::dirtyCount() == 0);
    SettingsTxn::commit();
    CHECK(SettingsTxn::active() == false);

    // ---- 4. rollback restores, and LEAVES OUT-OF-SCOPE KEYS ALONE ---------------------------------
    // This is the assertion the whole scope predicate exists for.
    freshIni();
    put(QStringLiteral("subs/language"), QStringLiteral("en"));
    put(QStringLiteral("playback/autoplayNext"), QStringLiteral("true"));
    put(QStringLiteral("resume/movie1"), QStringLiteral("120"));
    SettingsTxn::begin();
    put(QStringLiteral("subs/language"), QStringLiteral("fr"));          // in scope, changed
    put(QStringLiteral("resume/movie1"), QStringLiteral("999"));         // OUT of scope, changed mid-txn
    put(QStringLiteral("stats/p1/dev"), QStringLiteral("42"));           // OUT of scope, created mid-txn
    CHECK(SettingsTxn::dirtyCount() == 1);   // only the in-scope change counts
    SettingsTxn::rollback();
    CHECK(get(QStringLiteral("subs/language")) == QStringLiteral("en"));   // restored
    CHECK(get(QStringLiteral("playback/autoplayNext")) == QStringLiteral("true")); // untouched
    CHECK(get(QStringLiteral("resume/movie1")) == QStringLiteral("999"));  // SURVIVES — not clobbered
    CHECK(get(QStringLiteral("stats/p1/dev")) == QStringLiteral("42"));    // SURVIVES — not removed
    CHECK(SettingsTxn::active() == false);

    // ---- 5. rollback REMOVES an in-scope key that did not exist at begin() ------------------------
    freshIni();
    SettingsTxn::begin();
    put(QStringLiteral("subs/language"), QStringLiteral("de"));
    CHECK(SettingsTxn::dirtyCount() == 1);
    SettingsTxn::rollback();
    CHECK(has(QStringLiteral("subs/language")) == false);

    // ---- 6. nested begin() is a NO-OP ------------------------------------------------------------
    // Hub -> Appearance -> theme picker all call begin(); they must share ONE transaction so Discard
    // from any depth reverts the whole visit. A reset would silently make earlier changes permanent.
    freshIni();
    put(QStringLiteral("subs/language"), QStringLiteral("en"));
    SettingsTxn::begin();
    put(QStringLiteral("subs/language"), QStringLiteral("fr"));
    SettingsTxn::begin();                                    // must NOT re-snapshot
    put(QStringLiteral("playback/autoplayNext"), QStringLiteral("false"));
    CHECK(SettingsTxn::dirtyCount() == 2);                   // both changes still tracked
    SettingsTxn::rollback();
    CHECK(get(QStringLiteral("subs/language")) == QStringLiteral("en"));  // the FIRST change reverted too
    CHECK(has(QStringLiteral("playback/autoplayNext")) == false);

    // ---- 7. idempotence + inactive safety --------------------------------------------------------
    freshIni();
    put(QStringLiteral("subs/language"), QStringLiteral("en"));
    SettingsTxn::begin();
    put(QStringLiteral("subs/language"), QStringLiteral("fr"));
    SettingsTxn::rollback();
    SettingsTxn::rollback();                                 // second rollback is a harmless no-op
    CHECK(get(QStringLiteral("subs/language")) == QStringLiteral("en"));
    // Calling the mutators with no open transaction must not crash or corrupt.
    SettingsTxn::commit();
    CHECK(SettingsTxn::isDirty() == false);
    CHECK(SettingsTxn::dirtyCount() == 0);

    QFile::remove(iniPath());
    CHECK(QFile::exists(iniPath()) == false);   // no residue

    if (failures == 0) { std::puts("SETTINGSTXN-OK"); return 0; }
    std::fprintf(stderr, "SETTINGSTXN: %d check(s) failed\n", failures);
    return 1;
}
```

- [ ] **Step 2: Create the header in the same step it enters CMake**

Create `native/src/core/SettingsTxn.h`:

```cpp
// The settings save/discard transaction (issue #26). Before this, every settings row was immediate-apply —
// all 34 Settings::set* accessors are `store().setValue(k,v); store().sync();` — so there was no pending
// state, nothing to discard, and Back was indistinguishable from Save.
//
// The design is SNAPSHOT AND RESTORE, not buffer and flush. Immediate-apply is kept completely unchanged,
// which is the point: the rows most worth protecting are the ones that MUST apply live (the theme previews
// live; display mode re-lays out the surface you are standing on). A pending map would have to special-case
// exactly those, and Discard would then be a lie for them. Snapshotting instead means live rows need NO
// special case at all — the theme previews because the write genuinely happened — and Discard is a restore.
//
// QtCore only, own file-local store(), so probe_settingstxn links lean.
#pragma once
#include <QString>

namespace SettingsTxn
{
    // Is this key owned by the settings screens? THE LOAD-BEARING PREDICATE. A whole-ini snapshot would be
    // a DATA-LOSS bug: cloud sync, stats accrual, resume positions and the download catalog are all written
    // while a settings panel is open, and rollback would clobber them.
    //
    // Note this is deliberately NOT "exclude everything device-local" — display/mode, roms/folder,
    // library/folder and emulators/root are per-device AND are settings rows a user must be able to
    // discard. CloudSync::isDeviceLocalKey is the precedent for this SHAPE of predicate, not its contents.
    bool inScope(const QString& key);

    // Snapshot every in-scope key. A begin() while already active is a NO-OP, not a reset: hub ->
    // Appearance -> theme picker all call it, and they must share ONE transaction so Discard from any depth
    // reverts the whole visit. Re-snapshotting would silently make earlier changes permanent.
    void begin();
    bool active();

    // How many in-scope keys differ from the snapshot. Compares VALUES, not edits, so changing something
    // and changing it back reads clean and never prompts. The prompt states this count — never the values,
    // which would leak masked credential rows.
    int  dirtyCount();
    bool isDirty();

    void commit();     // drop the snapshot
    void rollback();   // restore every differing in-scope key; remove in-scope keys created during the txn

#ifdef EB_SETTINGSTXN_TEST_SEAM
    // Probe-only: redirect the store to a scratch ini. Gated so production cannot call it — an unguarded
    // call would silently redirect every settings read/write for the process lifetime.
    void setIniPathForTesting(const QString& path);
#endif
}
```

- [ ] **Step 3: Create the implementation**

Create `native/src/core/SettingsTxn.cpp`:

```cpp
#include "SettingsTxn.h"

#include "AppBrand.h"
#include "AppPaths.h"

#include <QHash>
#include <QSettings>
#include <QStringList>

namespace {

QString g_iniOverride;

QSettings& store()
{
    static QString path = g_iniOverride.isEmpty()
        ? AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile)
        : g_iniOverride;
    static QSettings s(path, QSettings::IniFormat);
    return s;
}

bool g_active = false;
QHash<QString, QVariant> g_snapshot;   // key -> value at begin(); absent-at-begin keys are simply not here
QStringList g_snapshotKeys;            // the in-scope keys present at begin(), for removal detection

} // namespace

bool SettingsTxn::inScope(const QString& key)
{
    // The CloudMerge-owned per-item stores. Written continuously by playback, marking and stats accrual
    // while a panel is open — rolling these back is data loss. Matches CloudSync::isPerItemStoreKey.
    static const char* kExcludedPrefixes[] = {
        "resume/", "recent/", "marks/", "favorites/", "playlists/", "stats/", "playstats/", "deleted/",
        "cloud/",      // OAuth tokens — signing in is not a setting you discard
        "device/",     // this install's identity + one-shot migration flags
        "pcgames/",    // catalog written by the PC-game importer
    };
    for (const char* p : kExcludedPrefixes)
        if (key.startsWith(QLatin1String(p))) return false;
    // "downloads" is a bare key AND a family; both are the background download catalog. Matched exactly or
    // as "downloads/..." so a sibling like "downloadsPanel/x" is NOT swept up.
    if (key == QLatin1String("downloads") || key.startsWith(QLatin1String("downloads/"))) return false;
    return true;
}

void SettingsTxn::begin()
{
    if (g_active) return;   // nested panels share the outermost transaction — see the header
    g_snapshot.clear();
    g_snapshotKeys.clear();
    for (const QString& k : store().allKeys())
        if (inScope(k)) { g_snapshot.insert(k, store().value(k)); g_snapshotKeys << k; }
    g_active = true;
}

bool SettingsTxn::active() { return g_active; }

int SettingsTxn::dirtyCount()
{
    if (!g_active) return 0;
    int n = 0;
    // Changed or removed since begin().
    for (const QString& k : g_snapshotKeys)
        if (store().value(k) != g_snapshot.value(k)) ++n;
    // Created since begin().
    for (const QString& k : store().allKeys())
        if (inScope(k) && !g_snapshot.contains(k)) ++n;
    return n;
}

bool SettingsTxn::isDirty() { return dirtyCount() > 0; }

void SettingsTxn::commit()
{
    g_snapshot.clear();
    g_snapshotKeys.clear();
    g_active = false;
}

void SettingsTxn::rollback()
{
    if (!g_active) return;
    // Remove in-scope keys that did not exist at begin(). Collect first — removing while iterating
    // allKeys() would be mutating the container being read.
    QStringList created;
    for (const QString& k : store().allKeys())
        if (inScope(k) && !g_snapshot.contains(k)) created << k;
    for (const QString& k : created) store().remove(k);
    // Restore every changed key.
    for (const QString& k : g_snapshotKeys)
        if (store().value(k) != g_snapshot.value(k)) store().setValue(k, g_snapshot.value(k));
    store().sync();
    commit();
}

#ifdef EB_SETTINGSTXN_TEST_SEAM
void SettingsTxn::setIniPathForTesting(const QString& path)
{
    g_iniOverride = path;
}
#endif
```

**Note for the implementer:** `store()` caches its `QSettings` in a function-local static, so
`setIniPathForTesting` must be called before the first `store()` use, and the probe calls `freshIni()`
before any other access. If a single static cannot be re-pointed between the probe's cases, change
`store()` to construct per call (it is a probe-only cost) or hold a `std::unique_ptr<QSettings>` that
`setIniPathForTesting` resets. Make the probe's seven independent cases actually independent — if they
share one cached ini they are not testing what they claim.

- [ ] **Step 4: Register the probe target in CMake**

In `native/CMakeLists.txt`, after the `probe_theme` block, add:

```cmake
    # Headless test for the settings save/discard transaction (issue #26): the scope predicate (incl. the
    # device-local-but-in-scope cases a naive implementation gets wrong), dirty counting by VALUE, rollback
    # fidelity, and — the assertion the whole predicate exists for — that an out-of-scope key written mid
    # transaction survives rollback untouched. SettingsTxn.cpp is QtCore-only with its own file-local
    # store(), so this links against Qt6::Core alone.
    add_executable(probe_settingstxn tools/probe_settingstxn.cpp
        src/core/SettingsTxn.cpp src/core/SettingsTxn.h)
    target_include_directories(probe_settingstxn PRIVATE src src/core)
    target_compile_definitions(probe_settingstxn PRIVATE EB_SETTINGSTXN_TEST_SEAM)
    target_link_libraries(probe_settingstxn PRIVATE Qt6::Core)
```

Also add `src/core/SettingsTxn.cpp` and `src/core/SettingsTxn.h` to the main app target's source list,
alongside the other `src/core/*.cpp` entries. The app target must **not** define
`EB_SETTINGSTXN_TEST_SEAM`.

- [ ] **Step 5: Register in the suite script**

In `native/tools/run-headless-probes.sh` (~line 130), add `"probe_settingstxn SETTINGSTXN-OK"` to the
`for p in ...` list, immediately after `"probe_theme THEME-OK"`.

- [ ] **Step 6: Register in CI**

In `.github/workflows/ci.yml` (~line 52), append `probe_settingstxn` to the `--target` list.

- [ ] **Step 7: Configure and build**

```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH"
cmake -S native -B build -DEVERYTHINGBOX_BUILD_APP=ON \
  -DCMAKE_PREFIX_PATH="C:/Qt/6.8.3/msvc2022_64" \
  -DMPV_INCLUDE_DIR="C:/mpv-dev/include" -DMPV_LIBRARY="C:/mpv-dev/libmpv.lib" \
  -DSDL2_INCLUDE_DIR="C:/SDL2/include" -DSDL2_LIBRARY="C:/SDL2/lib/x64/SDL2.lib"
cmake --build build --config Release --target probe_settingstxn
```

Expected: builds clean.

- [ ] **Step 8: Run the probe**

```bash
./build/Release/probe_settingstxn.exe
```

Expected: `SETTINGSTXN-OK`, exit 0, and no `eb-probe-settingstxn.ini` left in `%TEMP%`.

- [ ] **Step 9: Mutation-test every assertion group**

Apply each mutation, rebuild, run, confirm it FAILS with the named check, revert, confirm `SETTINGSTXN-OK`.
**Record the observed failing line for each.**

| # | Mutation in `SettingsTxn.cpp` | Must fail |
|---|---|---|
| 1 | `inScope`: `return true;` (no exclusions) | the out-of-scope family checks in §1 AND the `resume/movie1` survival check in §4 |
| 2 | `inScope`: also exclude anything starting `display/` | the `display/mode == true` check in §2 |
| 3 | `inScope`: use `key.startsWith("stats")` instead of `"stats/"` | the `statsPanel/lastTab == true` check in §2 |
| 4 | `begin`: drop the `if (g_active) return;` guard | the nested-begin checks in §6 |
| 5 | `dirtyCount`: count snapshot keys only (drop the created-key loop) | the §5 removal check (dirtyCount would read 0) |
| 6 | `rollback`: skip the `created` removal loop | the `has("subs/language") == false` check in §5 |
| 7 | `rollback`: restore ALL snapshot keys unconditionally rather than only differing ones | nothing — **expected survivor.** Confirm and state it: restoring an unchanged key to its own value is a no-op, so no assertion can distinguish it. If you can cheaply make it observable, do; otherwise record it as benign. |

If a mutation other than #7 does NOT fail, the corresponding assertion is not real coverage — strengthen
it before proceeding.

- [ ] **Step 10: Run the full suite**

```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH"
bash native/tools/run-headless-probes.sh
```

Expected: `SETTINGSTXN-OK` present and `ALL HEADLESS PROBES PASSED`.

- [ ] **Step 11: Commit**

```bash
git add native/src/core/SettingsTxn.h native/src/core/SettingsTxn.cpp \
        native/tools/probe_settingstxn.cpp native/CMakeLists.txt \
        native/tools/run-headless-probes.sh .github/workflows/ci.yml
git commit -m "feat: SettingsTxn — snapshot/restore for settings save-discard"
```

---

### Task 2: Rollback side effects + the CloudSync guard

**Files:**
- Modify: `native/src/core/SettingsTxn.h`, `native/src/core/SettingsTxn.cpp`, `native/src/core/CloudSync.cpp`
- Modify: `native/tools/probe_settingstxn.cpp` (one new case)

**Interfaces:**
- Consumes: everything from Task 1.
- Produces, for Task 3: `void SettingsTxn::setRollbackHook(std::function<void()> hook)` — invoked once at the end of a `rollback()` that actually restored something.

- [ ] **Step 1: Add the rollback hook**

Restoring a value is not enough when the setting drives visible state. Add to `SettingsTxn.h`:

```cpp
    // Run after a rollback that actually restored something. The UI installs this to re-apply side
    // effects: FormFactor::instance().refresh() for display/mode, and a re-render for the theme key.
    // A Discard that leaves the app LOOKING different is the failure this whole design exists to
    // prevent — restoring the stored value is only half the job.
    void setRollbackHook(std::function<void()> hook);
```

Add `#include <functional>`. In `SettingsTxn.cpp`, hold it in the anonymous namespace as
`std::function<void()> g_rollbackHook;`, and invoke it at the very end of `rollback()` — after
`commit()`, so the hook observes a closed transaction and cannot recurse into a live one:

```cpp
    const bool restored = !created.isEmpty() || changed > 0;   // track `changed` in the restore loop
    store().sync();
    commit();
    if (restored && g_rollbackHook) g_rollbackHook();
```

- [ ] **Step 2: Add the probe case for the hook**

Append to `probe_settingstxn.cpp` before the residue check:

```cpp
    // ---- 8. the rollback hook fires exactly once, and ONLY when something was restored -------------
    {
        freshIni();
        int hookCalls = 0;
        SettingsTxn::setRollbackHook([&hookCalls] { ++hookCalls; });

        // A rollback with nothing changed must NOT fire the hook — re-rendering the whole UI because a
        // user opened and closed Settings is a visible cost for no reason.
        put(QStringLiteral("subs/language"), QStringLiteral("en"));
        SettingsTxn::begin();
        SettingsTxn::rollback();
        CHECK(hookCalls == 0);

        // A rollback that restored something fires it exactly once, however many keys changed.
        SettingsTxn::begin();
        put(QStringLiteral("subs/language"), QStringLiteral("fr"));
        put(QStringLiteral("playback/autoplayNext"), QStringLiteral("false"));
        SettingsTxn::rollback();
        CHECK(hookCalls == 1);

        SettingsTxn::setRollbackHook(nullptr);   // leave no dangling capture for later cases
    }
```

- [ ] **Step 3: Guard CloudSync's remote apply**

`CloudSync::applySettingsJson` writes in-scope keys. If a remote bundle lands while a transaction is
open, a later rollback would silently undo **another device's** changes.

In `native/src/core/CloudSync.cpp`, at the top of `applySettingsJson`, add:

```cpp
    // A remote bundle writes settings-scope keys. If it lands while a settings transaction is open, a
    // later Discard would revert ANOTHER DEVICE's changes, not the user's. Commit the transaction first:
    // losing the ability to discard this visit is the correct trade against clobbering a peer.
    if (SettingsTxn::active()) SettingsTxn::commit();
```

Add `#include "SettingsTxn.h"`. Note `probe_onboarding`, `probe_brand`, `probe_sync` and
`probe_cloudmerge` link `CloudSync.cpp` — **add `src/core/SettingsTxn.cpp` to every probe target that
links `CloudSync.cpp`**, or those probes fail to link. Find them by grepping `CloudSync.cpp` in
`native/CMakeLists.txt`; do not guess the list.

- [ ] **Step 4: Build and run**

```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH"
cmake --build build --config Release --target probe_settingstxn probe_onboarding probe_brand probe_sync probe_cloudmerge everythingbox
./build/Release/probe_settingstxn.exe
```

Expected: all targets link, `SETTINGSTXN-OK`.

- [ ] **Step 5: Mutation-test the additions**

| # | Mutation | Must fail |
|---|---|---|
| 8 | fire the hook unconditionally (drop the `restored &&`) | the `hookCalls == 0` check in §8 |
| 9 | fire the hook once per restored key | the `hookCalls == 1` check in §8 |

- [ ] **Step 6: Full suite, then commit**

```bash
bash native/tools/run-headless-probes.sh
git add native/src/core/SettingsTxn.h native/src/core/SettingsTxn.cpp \
        native/src/core/CloudSync.cpp native/tools/probe_settingstxn.cpp native/CMakeLists.txt
git commit -m "feat: rollback side-effect hook + commit an open txn on remote apply"
```

Expected: `ALL HEADLESS PROBES PASSED`.

---

### Task 3: Wire the transaction and the three-way prompt

**Files:**
- Modify: `native/src/ui/MainWindow.cpp`, `native/src/ui/MainWindow.h`

**Interfaces:**
- Consumes: `SettingsTxn::begin/active/isDirty/dirtyCount/commit/rollback/setRollbackHook`.
- Produces: `void MainWindow::leaveSettingsArea(std::function<void()> proceed)` — the ONE exit gate.

- [ ] **Step 1: Install the rollback hook once**

In the `MainWindow` constructor, after the existing one-time migrations:

```cpp
    // Discard must revert the VISIBLE change, not just the stored value. display/mode drives the form
    // factor; the theme key drives the whole surface. Re-resolve both, then re-render.
    SettingsTxn::setRollbackHook([this] {
        FormFactor::instance().refresh();
        showHomeScreen();
    });
```

Add `#include "../core/SettingsTxn.h"`.

- [ ] **Step 2: Add the shared exit gate**

Declare in `native/src/ui/MainWindow.h`, private section:

```cpp
    // The ONE settings-area exit gate (issue #26). If the transaction is dirty, ask Save / Discard /
    // Keep editing and act on it; otherwise run `proceed` straight through. Both hub builders route
    // their root onBack through this, so all thirteen screens are covered in both modes at once.
    void leaveSettingsArea(std::function<void()> proceed);
```

Define in `native/src/ui/MainWindow.cpp`, next to `openSettingsHub`:

```cpp
void MainWindow::leaveSettingsArea(std::function<void()> proceed)
{
    const int n = SettingsTxn::dirtyCount();
    if (n <= 0) { SettingsTxn::commit(); proceed(); return; }   // clean exit NEVER prompts

    // The message states a COUNT, never the values — masked credential rows must not be rendered.
    const QString msg = tr("%n setting(s) changed.", "", n);
    const int choice = NavConfirm::ask(tr("Save changes?"), msg,
                                       { tr("Save"), tr("Discard"), tr("Keep editing") },
                                       /*focusIndex*/ 0,        // Save: the common case is one keypress
                                       /*cancelIndex*/ 2,       // Esc == Keep editing: dismissing changes nothing
                                       this);
    if (choice == 2) return;                       // Keep editing: stay put, transaction still open
    if (choice == 0) { SettingsTxn::commit(); }    // Save: the writes already happened
    else             { SettingsTxn::rollback(); }  // Discard: restore + the hook re-renders
    proceed();
}
```

- [ ] **Step 3: Begin the transaction on every settings entry**

`begin()` is a no-op while active, so calling it at every entry point is safe and is what makes
direct-entry paths (a theme's own Appearance button bypasses the hub) work.

Add `SettingsTxn::begin();` as the first statement after the parental-unlock check in
`MainWindow::openSettingsHub()`, and at the top of each settings panel opener that is reachable
**without** going through the hub.

**Find those by grep rather than trusting a list** — search `MainWindow.cpp` for calls to
`openAppearance`, `openGeneralSettings`, `openCloudSync`, `openDebug`, `openDownloadManager`,
`openLibrary`, `openStats`, `openRetroAchievements`, `openEmulatorManager`, `openEmulatorSettings`,
`openBiosCheck`, `openInputMapping` and identify which have callers outside `openSettingsHub`'s two
dispatch lambdas. Report the list you found.

- [ ] **Step 4: Route BOTH hub exits through the gate**

The themed hub's root `onBack` (~line 8325) and the classic hub's `onBack` (~line 8363) are
structurally identical: return to `panelReturnTo_`, else `showHomeScreen()`. Wrap each body in
`leaveSettingsArea`.

Themed:

```cpp
            [this] {
                leaveSettingsArea([this] {
                    clearPanelPageConns();
                    if (panelReturnTo_ == home_ || panelReturnTo_ == themedHome_) showHomeScreen();
                    else if (panelReturnTo_) stack_->setCurrentWidget(panelReturnTo_);
                    else showHomeScreen();
                });
            });
```

Classic:

```cpp
    }, [this] {
        leaveSettingsArea([this] {
            if (panelReturnTo_ == home_ || panelReturnTo_ == themedHome_) showHomeScreen();
            else stack_->setCurrentWidget(panelReturnTo_);
        });
    });
```

Line numbers WILL have drifted — locate by the lambda bodies shown, not by number.

- [ ] **Step 5: Check for other ways out of the settings area**

A settings panel that exits directly to a non-settings page bypasses the hub's onBack and would leave
the transaction open. Grep for `stack_->setCurrentWidget` and `showHomeScreen()` calls inside the
settings panel builders. Route any that leave the settings area through `leaveSettingsArea`, and
report every site you found and what you did with it.

An open transaction that is never closed is not merely untidy: the next entry's `begin()` is a no-op,
so a later Discard would revert changes from a previous, already-abandoned visit.

- [ ] **Step 6: Build**

```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH"
cmake --build build --config Release --target everythingbox probe_nav probe_navqml probe_settingstxn
```

Expected: clean build.

- [ ] **Step 7: Full suite**

```bash
bash native/tools/run-headless-probes.sh
```

Expected: `ALL HEADLESS PROBES PASSED`.

- [ ] **Step 8: Commit**

```bash
git add native/src/ui/MainWindow.cpp native/src/ui/MainWindow.h
git commit -m "feat: prompt Save/Discard/Keep editing when leaving settings with changes"
```

---

### Task 4: Close-out — live verification, review, merge, redeploy

- [ ] **Step 1: Build a throwaway**

```bash
SCRATCH="/c/Users/cubma/AppData/Local/Temp/claude/C--Users-cubma-Project-Goliath/7ae938b6-7f10-482b-ad59-ba50699a6c23/scratchpad/eb-t26"
rm -rf "$SCRATCH" && mkdir -p "$SCRATCH"
cp -r /c/EverythingBox-app/* "$SCRATCH"/
cp build/Release/EverythingBox.exe "$SCRATCH"/EverythingBox.exe
```

Then remove every `cloud/*` and `sync/*` line from **every** `.ini` in `$SCRATCH`. Verify:

```bash
grep -c -E '^(cloud|sync)/' "$SCRATCH"/*.ini
```

Expected: `0`. **Never print an ini line that could carry a credential value** — use key-name-only
output (`grep -o` on the key portion), never whole lines.

- [ ] **Step 2: Verify the six behaviours live**

Launch under the harness (`EB_UITEST=1`, unique `EB_UITEST_PIPE`, driven by `native/tools/uitest.py`),
capturing a screenshot or state snapshot for each:

1. Open Settings, change nothing, Back → **no prompt**.
2. Change a Choice row, Back → prompt appears, message states the count, Save is focused.
3. Choose **Discard** → the value reverts.
4. Change a value, Back, **Save** → it persists across a relaunch.
5. Change a value and change it **back**, Back → **no prompt**.
6. Change the **theme**, Back, Discard → the app looks as it did before. This is the one that proves
   the rollback hook works; a Discard that reverts the stored value but leaves the new theme on screen
   is a failure.

Also confirm **Esc on the prompt behaves as Keep editing** and changes nothing.

Scope any process kill to PIDs you launched. Clean up with zero residue, including screenshots — do
not leave PNGs in the repo root.

- [ ] **Step 3: Full suite on the final tree**

```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH"
cmake --build build --config Release --target everythingbox probe_settingstxn probe_theme probe_nav probe_navqml probe_onboarding probe_brand probe_sync probe_cloudmerge
bash native/tools/run-headless-probes.sh
```

Expected: `ALL HEADLESS PROBES PASSED`.

- [ ] **Step 4: Whole-branch review**

```bash
git log --oneline $(git merge-base main HEAD)..HEAD
```

Dispatch the final reviewer on the most capable model with the review package, the spec, and this
plan's Global Constraints. Tell it to hunt cross-task seams — especially any settings exit path that
does not route through `leaveSettingsArea`, and any key family that should or should not be in scope.

- [ ] **Step 5: Merge, push, redeploy**

```bash
git checkout main && git merge --no-ff <branch> && git push origin main
cp build/Release/EverythingBox.exe /c/EverythingBox-app/EverythingBox.exe
md5sum build/Release/EverythingBox.exe /c/EverythingBox-app/EverythingBox.exe
```

Expected: identical md5 sums. Absorb any upstream work first and re-run the suite on the combined tree
before pushing — a combined-tree build has caught a latent link break before.

- [ ] **Step 6: Update the ledger**

Append one line per completed task to `.superpowers/sdd/progress.md`, plus a close-out line recording
the merge commit, the deploy md5, and any follow-ups found.

---

## Self-Review

**Spec coverage:** §1 `SettingsTxn` → Task 1. §2 scope carve-out → Task 1 (`inScope` + probe §1/§2).
§3 rollback side effects → Task 2 (hook) + Task 3 Step 1 (installation). §4 panels/prompt → Task 3
Steps 2–5. §5 hazards → Task 2 Step 3 (CloudSync guard) and the count-not-values rule in Task 3 Step 2.
§6 testing → Task 1 Steps 1/9, Task 2 Steps 2/5, Task 4 Step 2. No gaps.

**Type consistency:** `inScope`, `begin`, `active`, `dirtyCount`, `isDirty`, `commit`, `rollback`,
`setRollbackHook`, `setIniPathForTesting` are spelled identically in Task 1's header, its
implementation, the probe, and Tasks 2–3. `leaveSettingsArea(std::function<void()>)` is declared in
Task 3 Step 2 and used in Steps 4–5.

**Known judgement calls for the reviewer:**
- Mutation #7 in Task 1 is an **expected survivor** — restoring an unchanged key to its own value is
  unobservable. Called out so it is adjudicated rather than discovered.
- `store()` caching its `QSettings` in a function-local static is flagged in Task 1 Step 3 as
  something the implementer may have to restructure for the probe's cases to be genuinely independent.
  If they leave it cached, the probe's seven cases share one ini and are weaker than they look.
