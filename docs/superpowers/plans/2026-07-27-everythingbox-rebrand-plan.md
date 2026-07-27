# EverythingBox Rebrand Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Rename the product to EverythingBox with no remaining mention of the old name anywhere in the tracked tree, migrate existing installs without losing settings or synced data, and add the license and community files the repo has never had.

**Architecture:** The load-bearing identity strings collapse into one `AppBrand.h` so the name stops being a thousand scattered literals. A per-step, resumable `BrandMigration` moves an existing install, with each step's lookup tolerating the legacy name only until that step's flag lands. A CI grep gate then makes "no mentions left" a property the suite enforces rather than a claim.

**Tech Stack:** Qt 6.8.3, C++17, MSVC 2022, CMake, GitHub Actions, `gh` CLI.

## Global Constraints

- **Build:** `export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH"`; build dir `build`; **always `--config Release`**.
- **Build ONLY named targets.** Never a target-less `cmake --build build` — it builds ~42 probes and stalls. Adding a source or probe needs ONE reconfigure. Report BLOCKED past ~6 min with no progress.
- **When you add a `.cpp` to a CMake target, create the file in the SAME step** — CMake resolves source lists at configure time, so a missing file fails the *reconfigure*, not the build.
- **The suite only RUNS pre-built exes.** After touching a shared source, rebuild every target that compiles it before trusting a green run.
- **Suite:** `BUILD_DIR=build bash native/tools/run-headless-probes.sh` must print `ALL HEADLESS PROBES PASSED`.
- **Exact new identifiers** (use verbatim):

| Old | New |
|---|---|
| `MyMediaVault` / `My Media Vault` | `EverythingBox` |
| `MyMediaVault.exe` | `EverythingBox.exe` |
| project `MyMediaVaultNative`, target `mymediavault` | `EverythingBoxNative`, target `everythingbox` |
| `mymediavault.ini` | `everythingbox.ini` |
| Drive folder `MyMediaVault` | `EverythingBox` |
| `mymediavault-sync.zip` / `mymediavault-progress.json` | `everythingbox-sync.zip` / `everythingbox-progress.json` |
| `com.mymediavault.` | `com.everythingbox.` |
| `MMV_UITEST`, `MMV_UITEST_PIPE`, `MMV_NO_VERSION_BUMP` | `EB_UITEST`, `EB_UITEST_PIPE`, `EB_NO_VERSION_BUMP` |
| `MYMEDIAVAULT_BUILD_APP` | `EVERYTHINGBOX_BUILD_APP` |
| `X-MMV-Config` | `X-EB-Config` |
| description "My media scraper" | "EverythingBox — one place for your films, shows, music, books, comics and games." |

- **Probe sentinels (`NAV-OK`, `SUBS-OK`, …) are feature-named and do NOT change.**
- **Exactly two files may contain the old name** when this is done: `native/src/core/AppBrand.h` (its `Legacy` block) and `native/src/core/BrandMigration.cpp`. The grep gate enforces this.
- **The deployed install directory is NOT renamed** and the old executable is **not deleted** — the install is portable, so renaming the folder would mean moving every save and setting.
- **Commit messages** end with `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`. Note `MMV_NO_VERSION_BUMP=1` becomes `EB_NO_VERSION_BUMP=1` after Task 2 — use whichever matches the tree you are on.

## File Structure

| File | Responsibility |
|---|---|
| `native/src/core/AppBrand.h` | **New.** Every identity string, current and legacy. |
| `native/src/core/BrandMigration.{h,cpp}` | **New.** Per-step resumable migration of an existing install. |
| `native/tools/probe_brand.cpp` | **New.** Migration step logic. Sentinel `BRAND-OK`. |
| `native/tools/run-headless-probes.sh` | The old-name grep gate. |
| `LICENSE`, `CODE_OF_CONDUCT.md`, `CONTRIBUTING.md`, `.github/ISSUE_TEMPLATE/*`, `.github/PULL_REQUEST_TEMPLATE.md` | **New.** |

---

### Task 1: `AppBrand.h` and the core identity strings

**Files:**
- Create: `native/src/core/AppBrand.h`
- Modify: every file naming the ini, the Drive folder/files, the addon prefix, the User-Agent, or the config header. Find them with:
  `git grep -ln 'mymediavault\.ini\|"MyMediaVault"\|mymediavault-sync\|mymediavault-progress\|com\.mymediavault\|X-MMV-Config' -- native/src`

**Interfaces:**
- Produces: `AppBrand::{kName,kIniFile,kDriveFolder,kSyncZip,kProgressDoc,kAddonPrefix,kUserAgent,kConfigHeader,kEnvPrefix}` and `AppBrand::Legacy::{kName,kIniFile,kDriveFolder,kSyncZip,kProgressDoc,kAddonPrefix}`. Tasks 2–4 use these.

- [ ] **Step 1: Write the header**

Create `native/src/core/AppBrand.h` exactly as the spec's section 1 specifies (`kName` … `kEnvPrefix`, plus the `Legacy` namespace). Copy it verbatim from `docs/superpowers/specs/2026-07-27-everythingbox-rebrand-design.md`.

- [ ] **Step 2: Replace the identity literals with the constants**

For each file the grep above lists, replace the literal with the constant. `QStringLiteral("mymediavault.ini")` becomes `QLatin1String(AppBrand::kIniFile)` (or `QString::fromLatin1(...)` where a `QString` is required) — match each call site's existing type rather than forcing one form.

**Do not** touch comments or log prose in this task; Task 4 sweeps those. This task is only the strings code depends on.

**The addon prefix has three consumers** and all three must move together: the reserved-namespace guard in `AddonManager::installPackage`, the first-party exclusion used by the sync bundle, and `GameMetaAggregator`'s per-provider ordering. A partial change here would let a third-party package claim a bundled addon's id.

- [ ] **Step 3: Rename the bundled addon ids**

Every `native/addons/*/manifest.json` carries `com.mymediavault.<name>`. Rename each to `com.everythingbox.<name>`, and update the registry index at `native/registry/mymediavault-addons/index.json` (**and the directory name itself** → `everythingbox-addons`).

> Installed copies under the old id are handled by Task 3's migration; do not add compatibility here.

> **The registry directory rename changes a public URL.** The default add-on registry is fetched from a raw GitHub path containing `mymediavault-addons`, so renaming the directory moves that URL. Combined with Task 6's repository rename, an app build that predates this change would fetch a 404. GitHub redirects both repository renames and — in practice — raw paths, but the directory rename is a genuine break for any older build. Note it in your report; it is acceptable here because the app and the registry ship from the same repository, so they move together.

- [ ] **Step 3b: The worker must accept BOTH config headers — this one crosses a boundary a commit cannot**

`native/addon-protocol/aiocatalog-worker/src/worker.js` reads the per-user config header. Renaming it to `X-EB-Config` in the app AND the worker source is correct — but **a Cloudflare Worker already deployed from that source keeps reading `X-MMV-Config` until it is redeployed**. In that window the app sends the new header, the live worker looks for the old one, and every per-user credential arrives empty: the add-on appears to work and silently returns nothing.

So the worker reads the new header and **falls back to the old one**:

```js
// Accept both: a Worker already deployed from an earlier revision of this file still reads the old header,
// and the app is updated independently of when this Worker is redeployed. Prefer the new name; the fallback
// can be deleted once every deployment has been refreshed.
const cfg = req.headers.get("X-EB-Config") || req.headers.get("X-MMV-Config");
```

This is the **only** compatibility shim in the rebrand, and it is why the gate in Task 4 needs a third exemption rather than two.

- [ ] **Step 4: Build and run the suite**

```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH" && cmake --build build --config Release --target mymediavault && BUILD_DIR=build bash native/tools/run-headless-probes.sh
```
Expected: clean build, `ALL HEADLESS PROBES PASSED`. (The target is still `mymediavault` until Task 2.)

- [ ] **Step 5: Commit**

```bash
git add native/src/core/AppBrand.h native/src native/addons native/registry
git commit -m "refactor: collect every identity string into AppBrand.h"
```

---

### Task 2: Build system, harness, and environment

**Files:**
- Modify: `native/CMakeLists.txt` (project name, target name, output name, the `MYMEDIAVAULT_BUILD_APP` option and every reference)
- Modify: `native/tools/uitest.py`, `native/tools/run-headless-probes.sh`, `.github/workflows/ci.yml`, `.github/workflows/release.yml`, and any script referencing `MMV_*`
- Modify: `native/src/core/UiTestServer.cpp` and wherever `MMV_UITEST` / `MMV_UITEST_PIPE` are read
- Modify: the pre-commit hook that honours `MMV_NO_VERSION_BUMP`

**Interfaces:**
- Consumes: `AppBrand.h` (Task 1).
- Produces: target `everythingbox`, executable `EverythingBox.exe`, `EVERYTHINGBOX_BUILD_APP`, `EB_UITEST`, `EB_UITEST_PIPE`, `EB_NO_VERSION_BUMP`. Every later task's build commands use these.

- [ ] **Step 1: Rename the CMake project, target and output**

```cmake
project(EverythingBoxNative VERSION <keep the current patch> LANGUAGES C CXX) # keep in sync with kAppVersion in src/main.cpp
```
Rename `qt_add_executable(mymediavault …)` → `everythingbox`, every `target_*` referencing it, and set the output name so the binary is `EverythingBox.exe`. Rename the option `MYMEDIAVAULT_BUILD_APP` → `EVERYTHINGBOX_BUILD_APP` **and every `if()` that tests it** — a missed one silently stops building the app.

- [ ] **Step 2: Rename the environment variables and every consumer**

`MMV_UITEST` → `EB_UITEST`, `MMV_UITEST_PIPE` → `EB_UITEST_PIPE`, `MMV_NO_VERSION_BUMP` → `EB_NO_VERSION_BUMP`.

**Every consumer is in-repo, so this is atomic — no aliases.** Find them all first:
```bash
git grep -ln 'MMV_UITEST\|MMV_NO_VERSION_BUMP\|MYMEDIAVAULT_BUILD_APP'
```
and update each, including `native/tools/uitest.py`, both workflow files, and the pre-commit hook. **Report the full list you changed** — a missed consumer means the UI-test harness silently stops connecting.

- [ ] **Step 3: Reconfigure from scratch and build**

The target name changed, so the existing cache is stale:
```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH" && cmake -S native -B build -DEVERYTHINGBOX_BUILD_APP=ON && cmake --build build --config Release --target everythingbox
```
Expected: `EverythingBox.exe` in `build/Release`.

- [ ] **Step 4: Run the suite**

`run-headless-probes.sh` locates binaries by name; confirm it still finds them.
```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH" && BUILD_DIR=build bash native/tools/run-headless-probes.sh
```
Expected: `ALL HEADLESS PROBES PASSED`.

- [ ] **Step 5: Commit**

```bash
git add native/CMakeLists.txt native/tools .github native/src
git commit -m "build: rename project, target, executable, and the EB_* environment"
```

---

### Task 3: `BrandMigration` + `probe_brand`

**Files:**
- Create: `native/src/core/BrandMigration.{h,cpp}`, `native/tools/probe_brand.cpp`
- Modify: `native/CMakeLists.txt` (app sources + the probe target), `native/tools/run-headless-probes.sh:~131` (runner list), `.github/workflows/ci.yml` (target list)
- Modify: `native/src/main.cpp` (run the migration before settings are read)

**Interfaces:**
- Consumes: `AppBrand::Legacy::*`.
- Produces: `BrandMigration::{Step, done(Step), run(cb)}`.

- [ ] **Step 1: Write the header**

```cpp
// Moves an install created under the previous name onto the current one. ONE-SHOT by design (the alternative,
// permanently reading both names, was considered and declined) — but PER-STEP and resumable, because the
// failure mode of a single all-or-nothing migration is a half-migrated account, which is the hardest state to
// recover from. Each step carries its own device-local flag, the steps run safest-first, and each step's
// lookup tolerates the legacy name ONLY until that step's flag is set. The tolerance retires itself.
#pragma once
#include <functional>

namespace BrandMigration
{
    enum class Step { LocalIni, AddonIds, DriveFolder, DriveFiles };

    // Device-local flags: a synced flag would mark OTHER machines as already migrated.
    bool done(Step);
    // Idempotent — safe on every launch. cb(true) when every step has completed.
    void run(std::function<void(bool allDone)> cb);
}
```

- [ ] **Step 2: Write the failing probe**

Create `native/tools/probe_brand.cpp` with the `CHECK` macro / `failures` counter / sentinel shape used by `native/tools/probe_savesync.cpp`. Cover, against a `QTemporaryDir`:

```cpp
    // 1. The ini step COPIES and never destroys the legacy file before the new one is verified.
    //    This is the step that can lose every setting, so it is asserted directly and mutation-tested.
    CHECK(QFileInfo::exists(legacyIni), "the legacy ini still exists after migration");
    CHECK(QFileInfo::exists(newIni),    "the new ini was created");
    CHECK(readKey(newIni, "roms/folder") == QStringLiteral("D:/roms"), "settings survived the copy");

    // 2. Idempotence — running twice changes nothing and does not re-copy over a newer file.
    writeKey(newIni, "roms/folder", QStringLiteral("D:/changed"));
    runMigration();
    CHECK(readKey(newIni, "roms/folder") == QStringLiteral("D:/changed"),
          "a second run does NOT clobber the migrated ini with the legacy one");

    // 3. Resumability — a step whose flag is unset runs again; one whose flag is set does not.
    CHECK(BrandMigration::done(BrandMigration::Step::LocalIni), "the completed step is flagged");
    clearFlag(BrandMigration::Step::LocalIni);
    runMigration();
    CHECK(BrandMigration::done(BrandMigration::Step::LocalIni), "an unset flag causes the step to re-run");

    // 4. Nothing to migrate is success, not failure.
    CHECK(migrateOnFreshInstall(), "a fresh install with no legacy data completes immediately");
```

- [ ] **Step 3: Create the sources, wire all three registration points, and watch it FAIL**

Create `BrandMigration.{h,cpp}` (the `.cpp` initially just `#include "BrandMigration.h"`) in the **same step** as the CMake edit. Add the sources to the app target; add:

```cmake
    # Headless test for the one-shot brand migration: per-step flags, idempotence, resumability, and that the
    # ini step never destroys the legacy file before the new one is verified.
    add_executable(probe_brand tools/probe_brand.cpp
        src/core/BrandMigration.cpp src/core/BrandMigration.h
        src/core/AppPaths.cpp       src/core/AppPaths.h)
    target_include_directories(probe_brand PRIVATE src src/core)
    target_link_libraries(probe_brand PRIVATE Qt6::Core)
```
Append `"probe_brand BRAND-OK"` to the runner list and `probe_brand` to the CI target list. Then:

```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH" && cmake -S native -B build -DEVERYTHINGBOX_BUILD_APP=ON && cmake --build build --config Release --target probe_brand
```
Expected: link errors — the functions are declared, not defined.

- [ ] **Step 4: Implement the four steps**

Flags live under the `device/` settings prefix (already carved out of sync — see `CloudSync::isDeviceLocalKey`), e.g. `device/brandMigrated/localIni`.

1. **`LocalIni`** — if `everythingbox.ini` is absent and `mymediavault.ini` exists, **copy** it (`QFile::copy`, never `rename`), reopen the copy and confirm a known key reads back, then set the flag. The legacy file is retained as a backup and never read once the flag is set.
2. **`AddonIds`** — for each installed addon directory whose manifest id starts with `AppBrand::Legacy::kAddonPrefix`, rewrite the id to `AppBrand::kAddonPrefix`. Until this flag is set, the reserved-namespace guard and the first-party exclusion must accept **both** prefixes.
3. **`DriveFolder`** — rename the Drive folder. Until the flag is set, folder lookup accepts **either** name. A failure leaves everything in place and retries next launch.
4. **`DriveFiles`** — rename `mymediavault-sync.zip` and `mymediavault-progress.json`. Same tolerance.

Call `BrandMigration::run` from `main.cpp` **before** settings are first read and before the startup cloud pull, so the migrated ini is the one everything sees.

- [ ] **Step 5: Build, run, and mutation-test the ini step**

```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH" && cmake --build build --config Release --target probe_brand everythingbox && ./build/Release/probe_brand.exe
```
Expected: `BRAND-OK`.

Then **change `QFile::copy` to `QFile::rename`**, rebuild, run. Expected: the "legacy ini still exists" assertion FAILS. Revert, rebuild, confirm `BRAND-OK`. Report both outputs — this is the step that can lose every setting.

- [ ] **Step 6: Commit**

```bash
git add native/src/core/BrandMigration.h native/src/core/BrandMigration.cpp native/tools/probe_brand.cpp native/CMakeLists.txt native/tools/run-headless-probes.sh .github/workflows/ci.yml native/src/main.cpp
git commit -m "feat: per-step resumable migration from the previous brand"
```

---

### Task 4: The prose sweep and the grep gate

**Files:**
- Modify: every remaining tracked file containing the old name (comments, log strings, docs, specs, plans, READMEs, `native/addon-protocol/`)
- Modify: `native/tools/run-headless-probes.sh` (the gate)

- [ ] **Step 1: Add the gate FIRST, so it tells you what is left**

Append to `native/tools/run-headless-probes.sh`, following the shape of the `qml no-direct-selection-writes` gate at `:135-156`:

```bash
# Old-brand gate: the product was renamed, and "no mentions remain" has to be a property the suite enforces
# rather than a claim someone made once. THREE files may still name the previous brand, each for a stated
# reason: AppBrand.h's Legacy block and BrandMigration.cpp exist to migrate installs that predate the rename,
# and the aiocatalog Worker accepts the old config header because a Worker already deployed from it keeps
# reading that header until someone redeploys it. Anything else is a leftover — or a new one someone just
# typed — and it fails here.
echo "=== old-brand references ==="
brand_hits="$(cd "$HERE/../.." && git grep -I -n -i -e 'mymediavault' -e 'my media vault' -e '\bMMV\b' -e 'MMV_' \
  -- . ':(exclude)native/src/core/AppBrand.h' ':(exclude)native/src/core/BrandMigration.cpp' \
       ':(exclude)native/addon-protocol/aiocatalog-worker/src/worker.js' || true)"
if [ -n "$brand_hits" ]; then
  echo "$brand_hits"
  echo "FAIL: old-brand references (the previous name survives outside AppBrand.h/BrandMigration.cpp)"
  fail=1
else
  echo "PASS: old-brand references"
fi
```

- [ ] **Step 2: Run it and work the list down**

```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH" && BUILD_DIR=build bash native/tools/run-headless-probes.sh 2>&1 | sed -n '/old-brand/,/PASS\|FAIL/p'
```
Expected initially: a long list. Work through it until the gate passes.

**Rename the historical SDD docs too** (`docs/superpowers/plans/`, `specs/`, `perf/`). They are living reference — someone following an old plan's build command needs the command that works *now*, not the one that worked then.

**Judgement, and say what you did:** a handful of hits are quotations of external things — a third-party URL, a Drive folder name inside a spec's prose describing the migration. Rename anything that is ours; if something genuinely cannot be renamed without becoming false, report it rather than forcing it, and we will decide whether it earns a third exemption.

- [ ] **Step 3: Update the app description**

Replace "My media scraper" wherever it appears in the tree with:
`EverythingBox — one place for your films, shows, music, books, comics and games.`
(The GitHub repository's own description is not in the tree; Task 6 handles it.)

- [ ] **Step 4: Full build and suite**

```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH" && T=$(grep -o 'add_executable([[:space:]]*probe_[a-z0-9_]*' native/CMakeLists.txt | sed 's/.*(\s*//' | tr '\n' ' ') && cmake --build build --config Release --target $T everythingbox && BUILD_DIR=build bash native/tools/run-headless-probes.sh
```
Expected: exit 0 and `ALL HEADLESS PROBES PASSED`, including `PASS: old-brand references`.

- [ ] **Step 5: Commit**

```bash
git add -A
git commit -m "docs: sweep the previous brand out of prose, and gate it in CI"
```

---

### Task 5: License and community files

**Files:**
- Create: `LICENSE`, `CODE_OF_CONDUCT.md`, `CONTRIBUTING.md`, `.github/ISSUE_TEMPLATE/bug_report.md`, `.github/ISSUE_TEMPLATE/feature_request.md`, `.github/PULL_REQUEST_TEMPLATE.md`

- [ ] **Step 1: `LICENSE`**

Standard MIT text, copyright the repository owner (take the name from `git config user.name`), year 2026. Then append, below the MIT text:

```
---

A note on dependencies

The MIT grant above covers the source code in this repository. It does not, and
cannot, relicense the third-party components a built binary links or bundles —
Qt and mpv are LGPL, and Duktape, miniz, LZMA and rcheevos carry their own terms.
If you redistribute a build, those licences apply to it alongside this one.
```

> This note exists because "use it any way you want" would otherwise imply something untrue about a distributed binary.

- [ ] **Step 2: `CODE_OF_CONDUCT.md`**

Contributor Covenant 2.1, verbatim, with the enforcement contact set to the address from `git config user.email` — already public in this repo's commit history, so this publishes nothing new.

- [ ] **Step 3: `CONTRIBUTING.md`**

Cover, concretely rather than generically:
- **Build:** the `PATH` export, `cmake -S native -B build -DEVERYTHINGBOX_BUILD_APP=ON`, `--config Release`, and **build named targets only** (a target-less build compiles ~42 probes).
- **The gate:** `BUILD_DIR=build bash native/tools/run-headless-probes.sh` must print `ALL HEADLESS PROBES PASSED` before a PR.
- **Nav-kit rule:** all modal UI goes through `src/ui/nav` — never `QDialog`/`QMessageBox`/`QInputDialog`/top-level windows.
- **Two settings builders:** `MainWindow::openGeneralSettings()` has a themed builder and a classic QWidget builder; the themed one is the default-reachable surface, so a user-facing setting must be added to **both**.
- **Tests:** a new pure component gets a probe, registered in three places — its `add_executable`, the runner list, and the CI target list. An unregistered probe silently never runs.
- **Commits:** conventional prefixes (`feat:`/`fix:`/`docs:`/`refactor:`).

- [ ] **Step 4: Issue and PR templates**

`bug_report.md`: what happened, what you expected, steps, platform + app version, and the relevant log lines.
`feature_request.md`: the problem, who it affects, what you tried.
`PULL_REQUEST_TEMPLATE.md`: what changed, why, how it was verified, which probes ran, and a checkbox for "the probe suite passes locally".

- [ ] **Step 5: Suite and commit**

```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH" && BUILD_DIR=build bash native/tools/run-headless-probes.sh
git add LICENSE CODE_OF_CONDUCT.md CONTRIBUTING.md .github
git commit -m "docs: add MIT licence, code of conduct, contributing guide and templates"
```

---

### Task 6: Close-out — live migration verify, merge, and the repo rename

- [ ] **Step 1: Live-verify the migration**

**Never launch or modify the deployed app at `C:\MyMediaVault-app` or its ini.** Copy it to a scratch dir and work only there. Never print a credential value.

1. The throwaway carries a legacy `mymediavault.ini` with recognisable settings (a distinctive ROMs folder, a theme). Launch `EverythingBox.exe`. Confirm `everythingbox.ini` appears **with those settings**, and `mymediavault.ini` is **still there**.
2. Launch again. Confirm it is a no-op — no re-copy, no clobber of anything changed since.
3. Confirm a bundled addon still loads after the id rewrite.
4. Confirm the app presents as "EverythingBox" — window title, About, settings.
5. **Drive steps are user-gated** (they need a signed-in account). If not signed in, record steps 3-4 of the migration as deferred. Do not claim them.

- [ ] **Step 2: Record the outcome in the spec**

Set `**Status:** Complete` in `docs/superpowers/specs/2026-07-27-everythingbox-rebrand-design.md`, stating which live steps ran and which were deferred.

- [ ] **Step 3: Merge**

```bash
git checkout main && git fetch origin && git merge origin/main --no-edit && git merge local/everythingbox --no-edit
```
On a version-line conflict take the **higher** patch. If upstream touched a file this branch renamed, verify the merge by reading — a rename plus an upstream edit is exactly where an auto-merge goes wrong.

- [ ] **Step 4: Build EVERY probe target, then the suite**

```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH" && T=$(grep -o 'add_executable([[:space:]]*probe_[a-z0-9_]*' native/CMakeLists.txt | sed 's/.*(\s*//' | tr '\n' ' ') && cmake --build build --config Release --target $T everythingbox && BUILD_DIR=build bash native/tools/run-headless-probes.sh
```
Expected: exit 0, `ALL HEADLESS PROBES PASSED`, including `PASS: old-brand references`.

- [ ] **Step 5: Push and delete the branch**

```bash
git push origin main && git branch -d local/everythingbox
```

- [ ] **Step 6: Rename the GitHub repository and its description — USER-AUTHORIZED**

The user explicitly authorized this. GitHub keeps a redirect from the old URL, so existing clones and links keep working, but the local remote should be updated anyway.

```bash
gh repo rename EverythingBox --repo cubman3134/MyMediaVault --yes
```
```bash
gh repo edit cubman3134/EverythingBox --description "EverythingBox — one place for your films, shows, music, books, comics and games."
```
```bash
git remote set-url origin https://github.com/cubman3134/EverythingBox.git && git remote -v && git fetch origin
```
Confirm the fetch succeeds against the new URL. **Report exactly what changed** — this is the one step that alters something outside the repository.

- [ ] **Step 7: Redeploy**

The executable name changed, so this deploys a **new** file beside the old one:
```bash
cp build/Release/EverythingBox.exe /c/MyMediaVault-app/EverythingBox.exe && md5sum build/Release/EverythingBox.exe /c/MyMediaVault-app/EverythingBox.exe
```
Then write `/c/MyMediaVault-app/RENAMED.txt`:

```
This app is now EverythingBox.

Run EverythingBox.exe. The old MyMediaVault.exe is left here on purpose — it is
not deleted for you — but do NOT run it: your settings have been migrated to
everythingbox.ini, so the old build would open a missing settings file and look
like a brand-new empty install. Delete it whenever you like.

This folder still has its old name. That is deliberate: the install is portable,
so renaming the folder would mean moving every save, state and add-on. You can
rename it by hand at any time — the app follows its own location.
```

**Do not delete `MyMediaVault.exe`.** Deleting a user's executable is the user's call.

- [ ] **Step 8: Update the ledger**

Append to `.superpowers/sdd/progress.md`: the merge commit, what live verification covered versus deferred, the repo rename, and any follow-ups.
