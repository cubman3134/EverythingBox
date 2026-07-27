# Rebrand to EverythingBox (+ license and community files) — Design

**Date:** 2026-07-27
**Status:** Draft — approved through brainstorming; awaiting user spec review before plan.
**Origin:** User request. Rename the product from MyMediaVault to EverythingBox with **no remaining mentions**
of `MyMediaVault` / `My Media Vault` / `MMV`, replace the "My media scraper" description, and add the
community files the repo has never had: a license, a code of conduct, contribution guidelines, and issue /
pull-request templates.

## The surface

Measured on the tracked tree: **214 files**, roughly **1,000 references** (`MyMediaVault` 190,
`mymediavault` 247, `My Media Vault` 68, `MMV` 342, `mmv` 170). Most of it is comments, log lines and docs —
mechanical. The parts that are not:

| Carrier | Where | Why it is not find-and-replace |
|---|---|---|
| `mymediavault.ini` | `AppPaths::dataDir()`, opened in ~20 files | It holds every setting the user has |
| Drive folder `"MyMediaVault"` | `CloudSync` | `drive.file` scope only sees folders the app created |
| `mymediavault-sync.zip`, `mymediavault-progress.json` | `CloudSync` | The synced bundle and merge document |
| `com.mymediavault.*` | `AddonManager`, `AddonContext`, `GameMetaAggregator`, every bundled addon | A **reserved namespace** the installer special-cases; renaming orphans installed copies |
| `MMV_UITEST`, `MMV_UITEST_PIPE`, `MMV_NO_VERSION_BUMP`, `MYMEDIAVAULT_BUILD_APP` | harness, CI, CMake | Every consumer is in-repo, so they can move atomically |
| `User-Agent: MyMediaVault`, `X-MMV-Config` | `AddonManager`, `addon-protocol/` | A remote addon may key on them |

The repo currently has **no** `LICENSE`, `CODE_OF_CONDUCT`, `CONTRIBUTING`, or `.github/` templates —
only `.github/workflows`.

## Decisions taken in brainstorming

| Question | Decision |
|---|---|
| Existing installs | **Migrate once on first run** (not permanent dual-read) — with per-step flags and transitional tolerance, see below |
| License | **MIT**, with an explicit note that bundled/linked dependencies keep their own terms |

## New identifiers

| Old | New |
|---|---|
| `MyMediaVault` / `My Media Vault` | `EverythingBox` |
| `MyMediaVault.exe` | `EverythingBox.exe` |
| CMake project `MyMediaVaultNative`, target `mymediavault` | `EverythingBoxNative`, target `everythingbox` |
| `mymediavault.ini` | `everythingbox.ini` |
| Drive folder `MyMediaVault` | `EverythingBox` |
| `mymediavault-sync.zip` / `mymediavault-progress.json` | `everythingbox-sync.zip` / `everythingbox-progress.json` |
| `com.mymediavault.*` | `com.everythingbox.*` |
| `MMV_UITEST`, `MMV_UITEST_PIPE`, `MMV_NO_VERSION_BUMP` | `EB_UITEST`, `EB_UITEST_PIPE`, `EB_NO_VERSION_BUMP` |
| `MYMEDIAVAULT_BUILD_APP` | `EVERYTHINGBOX_BUILD_APP` |
| `X-MMV-Config` | `X-EB-Config` |
| Description "My media scraper" | "EverythingBox — one place for your films, shows, music, books, comics and games." |

Probe sentinels (`NAV-OK`, `SUBS-OK`, …) are feature-named, not brand-named, and do not change.

## Design

### 1. `AppBrand.h` — one place the name lives (`native/src/core/AppBrand.h`)

The rename is a project *because* the name is a thousand scattered literals. The load-bearing identity
strings collapse into one header:

```cpp
// Every string that names this product. The rename that created this file touched 214 files and ~1000
// literals; collecting the load-bearing ones here means the next rename is a one-file change, and — more
// immediately — it is what makes "no mentions of the old name remain" a checkable property rather than a
// claim. Comments and prose are renamed textually; these are the ones code actually depends on.
namespace AppBrand
{
    inline constexpr const char* kName        = "EverythingBox";
    inline constexpr const char* kIniFile     = "everythingbox.ini";
    inline constexpr const char* kDriveFolder = "EverythingBox";
    inline constexpr const char* kSyncZip     = "everythingbox-sync.zip";
    inline constexpr const char* kProgressDoc = "everythingbox-progress.json";
    inline constexpr const char* kAddonPrefix = "com.everythingbox.";
    inline constexpr const char* kUserAgent   = "EverythingBox";
    inline constexpr const char* kConfigHeader= "X-EB-Config";
    inline constexpr const char* kEnvPrefix   = "EB_";

    // The previous identity. Referenced ONLY by BrandMigration and by the lookups that tolerate it until
    // migration is confirmed. Nothing else in the tree may name these — the probe gate enforces that.
    namespace Legacy
    {
        inline constexpr const char* kName        = "MyMediaVault";
        inline constexpr const char* kIniFile     = "mymediavault.ini";
        inline constexpr const char* kDriveFolder = "MyMediaVault";
        inline constexpr const char* kSyncZip     = "mymediavault-sync.zip";
        inline constexpr const char* kProgressDoc = "mymediavault-progress.json";
        inline constexpr const char* kAddonPrefix = "com.mymediavault.";
    }
}
```

### 2. `BrandMigration` — one-shot, but **per-step and resumable** (`native/src/core/BrandMigration.{h,cpp}`)

The user chose migrate-once over permanent dual-read. The risk of that choice is a **half-migrated install**
— particularly if the Drive half fails partway. This design removes that failure mode without reverting to
dual-read: each step carries its own device-local flag, the steps are ordered safest-first, and each step's
*lookup* tolerates the legacy name **only until that step's flag is set**.

```cpp
namespace BrandMigration
{
    // Each step is independently flagged so a failure resumes rather than stranding the install. Flags are
    // device-local (the "device/" carve-out) — a synced flag would mark OTHER machines as already migrated.
    enum class Step { LocalIni, AddonIds, DriveFolder, DriveFiles };

    bool done(Step);          // has this step completed on this device?
    void run(std::function<void(bool allDone)> cb);   // idempotent; safe to call on every launch
}
```

| Order | Step | Failure behaviour |
|---|---|---|
| 1 | **Copy** `mymediavault.ini` → `everythingbox.ini` | Copy, never move. The legacy file is **kept as a backup** and is never read again once the flag is set; a crash mid-copy leaves the original untouched and the step retries |
| 2 | Rewrite `com.mymediavault.*` addon ids | The reserved-namespace guard and the aggregator accept **both** prefixes until the flag is set |
| 3 | Rename the Drive folder | Folder lookup accepts **either** name until the flag is set |
| 4 | Rename the sync bundle and progress document | Same tolerance |

The tolerance is **transitional and retires itself** — it is not the permanent dual-read the user declined.
`drive.file` scope permits renaming files the app itself created, so steps 3-4 should succeed; the fallback
exists because "should" is not "will", and because a token can expire mid-run.

### 3. The grep gate — how "no mentions left" becomes enforceable

`native/tools/run-headless-probes.sh` gains a check, in the same shape as the existing
`qml no-direct-selection-writes` and `retroview srm-path` gates: the tracked tree must contain **zero**
occurrences of `MyMediaVault`, `My Media Vault`, `mymediavault`, or `MMV`, with exactly **three** exemptions —
`AppBrand.h`'s `Legacy` block, `BrandMigration.cpp`, and the aiocatalog Worker (which keeps reading the old
config header for already-deployed instances, per the edge table below). All three are named explicitly, so a
new occurrence anywhere else fails CI rather than quietly accumulating.

This is the difference between "I renamed everything" and "nothing can slip back in."

### 4. Community files

- **`LICENSE`** — MIT, plus a short plain-English note: the MIT grant covers *this project's* code; Qt and
  mpv are LGPL, and Duktape, miniz, LZMA and rcheevos carry their own terms, so a distributed binary is not
  unencumbered. Saying so is more useful than letting "use it any way you want" imply something untrue.
- **`CODE_OF_CONDUCT.md`** — Contributor Covenant 2.1. The enforcement contact is the repository owner's
  git committer address, which is already public in every commit in this repo's history — so this publishes
  nothing new. If a separate address is preferred, it is a one-line change.
- **`CONTRIBUTING.md`** — the build recipe (Qt/mpv paths, Release config, named targets only), the probe
  suite as the gate, the nav-kit rule, the two-settings-builders rule, and the commit convention.
- **`.github/ISSUE_TEMPLATE/bug_report.md`** and `feature_request.md`, and **`PULL_REQUEST_TEMPLATE.md`**
  (what changed, why, how it was verified, which probes ran).

## The deployed install — deliberately NOT renamed

The desktop install is **portable**: the data lives beside the executable, so renaming the install directory
means moving every save, state, addon and setting. This design does **not** touch it. The new
`EverythingBox.exe` deploys into the existing directory; the folder keeps its old name until the user chooses
to rename it (the app follows its own location, so that is safe to do by hand at any time).

**One hazard this creates, and it must be surfaced rather than assumed away:** the old `MyMediaVault.exe`
remains in that directory, and after migration it would open a *missing* ini and present as a fresh, empty
install. The migration therefore writes a short `RENAMED.txt` beside it explaining what happened and which
executable to run. The old binary is **not** deleted — deleting a user's executable is the user's call.

## Error / edge handling

| Situation | Behaviour |
|---|---|
| Migration interrupted at any step | Per-step flags; the next launch resumes at the first incomplete step |
| `everythingbox.ini` written but unreadable | Legacy ini retained and still used; the step's flag is **not** set, so it retries |
| Drive rename fails (offline, quota, token) | Lookup still accepts the legacy folder name; retried next launch. **No data is moved or deleted** |
| Drive folder already renamed by another device | Lookup finds the new name; the flag is set without a second rename |
| Addon installed under the legacy prefix | Accepted until the rewrite flag is set; the reserved-namespace guard rejects **both** prefixes for third-party packages throughout |
| A remote addon keys on `X-MMV-Config` | It breaks — and one already deployed does. The `aiocatalog-worker` source is renamed in the same change, but a **Cloudflare Worker already published from it keeps reading the old header until it is redeployed**, so the app would send `X-EB-Config` to a worker still expecting `X-MMV-Config` and every per-user credential would arrive empty. The worker therefore accepts **both** headers, preferring the new one — the one compatibility shim in this design, because it spans a boundary a single commit cannot cross |
| User runs the old `MyMediaVault.exe` after migrating | Presents as an empty install. `RENAMED.txt` explains it; the old binary is left in place |
| A new occurrence of the old name is added later | The probe-suite grep gate fails |

## Verification

- **Probe suite grep gate** — zero old-name occurrences outside the two named exemptions. Runs in CI.
- **`probe_brand`** (new, pure, sentinel `BRAND-OK`): `BrandMigration`'s step logic against a temp dir —
  each step idempotent (running twice is a no-op), resumable (a failed step leaves its flag unset and the
  legacy name still resolvable), and **the ini copy never destroys the legacy file before the new one is
  verified**, mutation-tested because that is the step that can lose every setting.
- Full suite green; app builds; all probe targets link.
- **Live:** launch a throwaway copy carrying a legacy `mymediavault.ini` with recognisable settings, confirm
  they survive under the new name; confirm a second launch is a no-op; confirm a bundled addon still loads
  after the id rewrite. The Drive steps are **user-gated** — they need a signed-in account, and will be
  recorded as verified-or-deferred honestly rather than claimed.

## Non-goals

- Renaming the **deployed install directory** or deleting the old executable (see above).
- Renaming the **GitHub repository or its description** — those live on GitHub, not in the tree. They are an
  outward-facing change to the user's account and will be done only on explicit confirmation.
- Any behaviour change beyond the rename and its migration. No feature work rides along.
- Relicensing or vendoring changes to third-party dependencies.
