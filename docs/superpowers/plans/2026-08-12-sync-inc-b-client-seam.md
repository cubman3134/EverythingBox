# Self-hosted sync — Increment B (client SyncBackend seam) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extract a `SyncBackend` interface out of `CloudSync` so the Google Drive transport becomes one backend (`DriveSyncBackend`), with **zero behavior change** — the Drive path is identical and every `probe_*` stays green. This de-risks Increment C (adding a self-hosted `ServerSyncBackend`).

**Architecture (composition, seam-preserving):** `CloudSync` keeps its ENTIRE public API and all orchestration/bundle/carve-out code. It gains an owned `SyncBackend* backend_` (a `DriveSyncBackend` in production). The six Drive primitives + all OAuth move into `DriveSyncBackend`; `CloudSync`'s six methods become **`virtual` forwarders** to `backend_`, and its auth methods forward too. Orchestration bodies are UNCHANGED — they still call the six primitives via `this`, so the `FakeCloud`-overrides-CloudSync test seam is preserved exactly (tests bypass `backend_`; production uses it), matching today's behavior where `FakeCloud` already replaces the real Drive code.

**Tech Stack:** Qt6/C++17, CMake, MSVC. Client repo `C:\Users\cubma\Project Goliath`, sources under `native/src/core/`. Gate = the headless probe suite.

## Global Constraints

- **Behavior-preserving.** No orchestration/bundle/carve-out logic changes; OAuth and the six primitives move VERBATIM (only re-homed into `DriveSyncBackend`). The gate is the probes — run the FULL headless suite, not just the two sync probes.
- **CloudSync's public API is unchanged.** MainWindow, `SaveSync`, and the probes call `cloud->ensureFolder/findFile/uploadFile/downloadFile/findFolderNamed/renameFile`, `cloud->signIn/signOut/isSignedIn/accountEmail/lastAuth`, and `CloudSync::isConfigured/signInAvailable/driveQueryQuote/isDeviceLocalKey/…` — all must still exist with identical signatures (as forwarders where the impl moved).
- **The seam stays at the six primitives** (the header's own rule, lines 71–76). Orchestration keeps calling them via `this` (virtual dispatch), so `FakeCloud` overrides still take effect.
- Build **named targets only** (never a target-less build — 52 probe harnesses). No AI attribution; the repo's version-bump hook handles `CMakeLists.txt` version lines.

## Exact boundary (from the CloudSync.cpp map)

**MOVE to `DriveSyncBackend`** (verbatim): constants/helpers `:40-73` (`kClientId/kClientSecret/kAuthUrl/kTokenUrl/kUserInfo/kDrive/kDriveUp/kScopes/kFolder`, `clientId()/clientSecret()/randomToken()`); the `nam_` creation + 60s timeout from the ctor `:75-86`; `driveQueryQuote` `:88-94` (keep **public static**); OAuth `:96-308` (`isConfigured`, `isSignedIn`, `accountEmail`, `signOut`, `signInAvailable`, `signIn`, `exchangeCode`, `fetchAccountEmail`, `withAccessToken`); the six primitives `:312-496` (`findFolderNamed`, `renameFile`, `ensureFolder`, `findFile`, `uploadFile`, `downloadFile`); private state `nam_/loopback_/accessToken_/accessExpiryMs_/lastAuth_/pendingVerifier_/pendingState_/redirectUri_`; the three signals.

**STAY in `CloudSync`**: the bundle/carve-out/hash block `:498-923` (incl. `buildBundle/applyBundle/buildSettingsJson/applySettingsJson/stateFingerprint/stateHash/isDeviceLocalKey/isPerItemStoreKey/adoptSyncedBaseline/localChangedSinceSync` + file-statics `firstPartyAddonDirs/topSegment/zipAddDir/kBundleName/kProgressName`); `findBrandedFile` `:398-408`; `checkStatus` `:925`; `applyRemote` `:953`; `pushLocal` `:964`; `pullProgress` `:991`; `pushProgress` `:1002`. All reach Drive ONLY through the six primitives + `store()`.

**SHARED**: `store()` `:51-56` (a trivial `QSettings(AppBrand::kIniFile,…)` accessor) — both TUs need it; give `DriveSyncBackend.cpp` its own file-static `store()` copy (same ini, so consistent) and leave CloudSync's in place.

---

### Task 1: Extract `SyncBackend` + `DriveSyncBackend`; make `CloudSync` compose

**Files:**
- Create: `native/src/core/SyncBackend.h` (abstract interface)
- Create: `native/src/core/DriveSyncBackend.h`, `native/src/core/DriveSyncBackend.cpp` (the moved Drive/OAuth code)
- Modify: `native/src/core/CloudSync.h`, `native/src/core/CloudSync.cpp` (remove moved code; add forwarders + `backend_`)
- Modify: `native/CMakeLists.txt` (add the two new source files to the app AND every probe target that links `CloudSync.cpp`)

**Interfaces:**
- Produces: `class SyncBackend : public QObject` (pure-virtual six primitives + `signIn/signOut/isSignedIn/accountEmail/lastAuth`; signals `signedIn/signInFailed/signedOut`); `class DriveSyncBackend : public SyncBackend`; `CloudSync` gains `SyncBackend* backend_`.

- [ ] **Step 1: `SyncBackend.h`** — the abstract interface (QObject for signals). Declare the six primitives (copy the exact signatures from `CloudSync.h:77-97`) as pure virtual, plus the instance auth surface, plus the signals:
```cpp
#pragma once
#include "PendingPush.h"
#include <QObject>
#include <QString>
#include <QByteArray>
#include <functional>

// The transport seam for cloud sync. Google Drive is one implementation (DriveSyncBackend); a
// self-hosted server backend is another (Increment C). CloudSync orchestrates ABOVE this — bundle,
// merge, carve-outs — and reaches a backend only through these primitives.
class SyncBackend : public QObject
{
    Q_OBJECT
public:
    explicit SyncBackend(QObject* parent = nullptr) : QObject(parent) {}
    ~SyncBackend() override = default;

    // ---- auth / account (Drive uses OAuth; a token-URL backend reports differently) ----
    virtual bool isSignedIn() const = 0;
    virtual QString accountEmail() const = 0;
    virtual PendingPush::Auth lastAuth() const = 0;
    virtual void signIn() = 0;
    virtual void signOut() = 0;

    // ---- the six primitives (callbacks fire on the GUI thread) ----
    virtual void ensureFolder(std::function<void(const QString& folderId)> cb) = 0;
    virtual void findFile(const QString& folderId, const QString& name,
        std::function<void(bool listOk, const QString& id, const QString& modifiedIso, const QString& stateHash)> cb) = 0;
    virtual void uploadFile(const QString& folderId, const QString& existingId, const QString& name,
        const QString& mimeType, const QByteArray& data, const QString& stateHash,
        std::function<void(const QString& id)> cb) = 0;
    virtual void downloadFile(const QString& fileId, std::function<void(bool ok, const QByteArray& data)> cb) = 0;
    virtual void findFolderNamed(const QString& name, std::function<void(bool queryOk, const QString& id)> cb) = 0;
    virtual void renameFile(const QString& fileId, const QString& newName, std::function<void(bool ok)> cb) = 0;

signals:
    void signedIn(const QString& email);
    void signInFailed(const QString& error);
    void signedOut();
};
```

- [ ] **Step 2: `DriveSyncBackend.{h,cpp}`** — MOVE the Drive/OAuth code here, verbatim, only re-homed. `DriveSyncBackend.h` declares `class DriveSyncBackend : public SyncBackend`, `override`s the six primitives + the auth methods, keeps `public: static bool isConfigured(); static bool signInAvailable(); static QString driveQueryQuote(const QString&);`, and holds the private state (`nam_/loopback_/accessToken_/accessExpiryMs_/lastAuth_/pendingVerifier_/pendingState_/redirectUri_`) + private `exchangeCode/fetchAccountEmail/withAccessToken`. `DriveSyncBackend.cpp` receives the moved bodies from `CloudSync.cpp:40-308` (constants/helpers/OAuth) and `:312-496` (the six primitives), plus its own file-static `store()` and the `nam_` setup in its ctor. Change `CloudSync::` qualifiers to `DriveSyncBackend::`. The emit sites move as-is (they now emit `DriveSyncBackend`'s signals). Include what the moved code used (`PendingPush.h`, `BrandMigration.h`, `AppBrand`, `QNetworkAccessManager`, `QTcpServer`, `QDesktopServices`, crypto, etc.).

- [ ] **Step 3: Rewrite `CloudSync` to compose** — in `CloudSync.h`:
  - Keep the six primitive declarations (still `virtual`, same signatures) and the auth methods (`isSignedIn/accountEmail/signIn/signOut/lastAuth`, the static `isConfigured/signInAvailable/driveQueryQuote`) and all the static bundle/carve-out declarations and the `Status` struct + orchestration methods and the three signals — the PUBLIC API is unchanged.
  - Remove the moved private members/methods (`exchangeCode/fetchAccountEmail/withAccessToken`, `nam_/loopback_/accessToken_/accessExpiryMs_/lastAuth_/pending*/redirectUri_`).
  - Add `#include "SyncBackend.h"` (fwd-declare `DriveSyncBackend`) and a private member `SyncBackend* backend_ = nullptr;`.

  In `CloudSync.cpp`:
  - ctor: create `backend_ = new DriveSyncBackend(this);` and `connect` its three signals to re-emit CloudSync's (`connect(backend_, &SyncBackend::signedIn, this, &CloudSync::signedIn);` etc.).
  - Replace the six primitive bodies with one-line forwarders, e.g.:
    ```cpp
    void CloudSync::ensureFolder(std::function<void(const QString&)> cb) { backend_->ensureFolder(std::move(cb)); }
    void CloudSync::findFile(const QString& folderId, const QString& name, std::function<void(bool,const QString&,const QString&,const QString&)> cb) { backend_->findFile(folderId, name, std::move(cb)); }
    // ... uploadFile / downloadFile / findFolderNamed / renameFile likewise ...
    ```
  - Replace the auth method bodies with forwarders: `bool CloudSync::isSignedIn() const { return backend_->isSignedIn(); }`, `QString CloudSync::accountEmail() const { return backend_->accountEmail(); }`, `void CloudSync::signIn() { backend_->signIn(); }`, `void CloudSync::signOut() { backend_->signOut(); }`. Make `lastAuth()` (currently inline in the header returning `lastAuth_`) forward: change the header inline to `PendingPush::Auth lastAuth() const;` and add `PendingPush::Auth CloudSync::lastAuth() const { return backend_->lastAuth(); }`.
  - Static forwarders: `bool CloudSync::isConfigured() { return DriveSyncBackend::isConfigured(); }`, `bool CloudSync::signInAvailable() { return DriveSyncBackend::signInAvailable(); }`, `QString CloudSync::driveQueryQuote(const QString& v) { return DriveSyncBackend::driveQueryQuote(v); }` (keeps the probe's `CloudSync::driveQueryQuote` call working).
  - **Leave every orchestration/bundle/carve-out method body byte-identical** (`checkStatus/applyRemote/pushLocal/pullProgress/pushProgress/findBrandedFile/buildBundle/applyBundle/…`). They call the six via `this` (virtual dispatch), which now forwards to `backend_` in production and is overridden by `FakeCloud` in tests — unchanged behavior. Keep the file-static `store()` and the bundle statics.
  - Remove the now-moved constants (`kClientId…kScopes`, `clientId/clientSecret/randomToken`) and the Drive REST includes that are no longer used by `CloudSync.cpp` (keep those still used by orchestration/bundle — e.g. `QSettings`, `AppBrand`, miniz, crypto).

- [ ] **Step 4: CMake wiring** — in `native/CMakeLists.txt`, add `native/src/core/DriveSyncBackend.cpp` (and the two new headers, so AUTOMOC processes the `Q_OBJECT` types) to the app target's source list (next to `CloudSync.cpp`, ~:373) AND to EVERY probe target that links `CloudSync.cpp`: `probe_cloudmerge` (~:948), `probe_savesync` (~:1391-1407), `probe_sync` (~:547), `probe_onboarding` (~:961), and any other target whose source list includes `CloudSync.cpp` (grep `CloudSync.cpp` in `CMakeLists.txt` and add `DriveSyncBackend.cpp` beside each). `SyncBackend.h`/`DriveSyncBackend.h` carry `Q_OBJECT`, so they must be visible to AUTOMOC in each target (adding the .cpp + headers to the target sources handles this).

- [ ] **Step 5: Build the sync-linked probes + the app**
Run (named targets only):
```bash
cmake --build build --config Release --target probe_cloudmerge probe_savesync probe_sync probe_onboarding
```
Expected: all compile and link (0 errors). If a probe target fails to link with an undefined `DriveSyncBackend::…` or a missing MOC, its `CMakeLists.txt` source list is missing `DriveSyncBackend.cpp` — add it. Then also build the app to prove the full graph:
```bash
cmake --build build --config Release --target everythingbox
```

- [ ] **Step 6: Run the FULL headless probe suite (the behavior gate)**
Run:
```bash
BUILD_DIR=build bash native/tools/run-headless-probes.sh
```
Expected: `ALL HEADLESS PROBES PASSED` — in particular `CLOUDMERGE-OK` and `SAVESYNC-OK` (the sync gate), plus every other probe unaffected. If any sync probe regresses, the extraction drifted (a primitive/OAuth body was altered rather than moved verbatim, or a forwarder is wrong) — fix the moved code to match the original; do not change the probe.

- [ ] **Step 7: Commit**
```bash
git add native/src/core/SyncBackend.h native/src/core/DriveSyncBackend.h native/src/core/DriveSyncBackend.cpp native/src/core/CloudSync.h native/src/core/CloudSync.cpp native/CMakeLists.txt
git commit -m "refactor: extract a SyncBackend seam; Google Drive becomes DriveSyncBackend behind it"
```
(Stage by explicit path; `CMakeLists.txt` version lines are auto-resolved by the commit hook — do not hand-edit them.)

---

## Self-review

**Spec coverage:** `SyncBackend` interface at the six-primitive seam (spec Increment B) → Step 1. Drive HTTP + OAuth → `DriveSyncBackend`, behavior-preserving (spec) → Step 2. `CloudSync` composes + forwards, orchestration/bundle/carve-outs stay above the backend (spec) → Step 3. Probes green as the gate (spec: `probe_cloudmerge`/`probe_savesync`/…) → Step 6. No new user feature (spec: pure refactor) → nothing else added. ✅

**Placeholder scan:** none — the move-list is the exact boundary map (lines cited), the forwarder pattern is spelled out, the seam-preservation rationale (orchestration calls the six via `this`, `FakeCloud` still overrides) is explicit, and the CMake step enumerates every probe target that links `CloudSync.cpp`.

**Type consistency:** the six primitive signatures in `SyncBackend.h` are copied verbatim from `CloudSync.h:77-97`; `DriveSyncBackend` overrides them; `CloudSync` keeps identical public signatures as forwarders. `lastAuth()` changes from header-inline to a forwarding definition (only site that moves). `isConfigured/signInAvailable/driveQueryQuote` stay `static` on both `CloudSync` (forwarder) and `DriveSyncBackend` (impl). `store()` duplicated (trivial, same ini). No caller (MainWindow/SaveSync/probes) changes. ✅
