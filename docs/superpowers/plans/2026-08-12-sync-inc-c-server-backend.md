# Self-hosted sync — Increment C (ServerSyncBackend + selector + pairing) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a self-hosted `ServerSyncBackend` behind the Increment-B `SyncBackend` seam, let `CloudSync` pick it from config, and give the user a backend chooser + URL/token pairing UI with switch-as-migration — so save/state sync works over the self-hosted server (Increment A's object store) instead of Google Drive.

**Architecture:** `ServerSyncBackend : SyncBackend` maps the six primitives onto Increment A's HTTP object store (mirroring `DriveSyncBackend`'s `QNetworkAccessManager` async pattern; the token is a URL path prefix, not a header). `CloudSync`'s ctor reads `cloud/backend` and constructs the right backend (Drive default). The Cloud Sync panel gains a chooser + URL/token fields (masked token); switching backends rewrites config, rebuilds `cloud_`, and force-pushes local state to adopt the new baseline. Merge rules, carve-outs, and `SaveSync` are unchanged (they sit above the seam).

**Tech Stack:** Qt6/C++17 (Core/Network/Gui), CMake, MSVC. Client repo `C:\Users\cubma\Project Goliath`, `native/src/core/` + `native/src/ui/`. Gate = the headless probe suite + a new `probe_serversync`.

## Global Constraints

- **Drive path unchanged.** Default `cloud/backend` (unset/`"drive"`) → `DriveSyncBackend`, byte-identical behavior. The full headless suite must stay green (`CLOUDMERGE-OK`, `SAVESYNC-OK`, …).
- **Config lives under `cloud/`** (already device-local via `isDeviceLocalKey`, so `cloud/backend`/`cloud/server/{url,token,namespace}` never enter the synced bundle or `stateHash` — no new carve-out needed). The token is a device-local secret; never logged, never synced.
- **The server token is a URL PATH PREFIX**, matching the server contract: an endpoint is `{url}` + (token ? `/{token}` : "") + `/sync/{ns}[/{key}]`. No `Authorization` header.
- **`ServerSyncBackend` maps onto the six primitives faithfully to Drive's semantics** (client-optimistic CAS, as today — the server's `If-Match` is available but the client's existing `stateHash` guard is what's relied on; use `If-None-Match: *` on create only).
- All UI via the themed panel / nav kit (never QDialog/QInputDialog). Build named targets only. No AI attribution; the version-bump hook owns `CMakeLists.txt` version lines. Stage by explicit path (the tree is shared).

---

### Task 1: `ServerSyncBackend` + config-driven backend selection + `probe_serversync`

**Files:**
- Create: `native/src/core/ServerSyncBackend.h`, `native/src/core/ServerSyncBackend.cpp`
- Modify: `native/src/core/CloudSync.cpp` (ctor picks backend from config; add an injection ctor), `native/src/core/CloudSync.h` (declare the injection ctor)
- Modify: `native/src/core/BrandMigrationDrive.cpp:117` (force a Drive backend — brand migration is Drive-only)
- Create: `native/tools/probe_serversync.cpp`
- Modify: `native/CMakeLists.txt` (add `ServerSyncBackend.cpp` to the six CloudSync-linked targets + the app; add a `probe_serversync` target; register it in the headless-probe list)

**Interfaces:**
- Produces: `class ServerSyncBackend : public SyncBackend` (implements the six primitives + auth against the object store); `CloudSync(SyncBackend* backend, QObject* parent)` injection ctor.

- [ ] **Step 1: `ServerSyncBackend.{h,cpp}`** — mirror `DriveSyncBackend`'s structure (own a `QNetworkAccessManager* nam_` with a 60s transfer timeout; its own file-static `store()` over `AppPaths::dataDir()/AppBrand::kIniFile`; callbacks fire on the GUI thread via `QNetworkReply::finished`). Read config lazily from `store()`:
  - `serverBase()` = `store().value("cloud/server/url").toString().trimmed()`; `token()` = `store().value("cloud/server/token").toString().trimmed()`; `ns()` = a non-empty `store().value("cloud/server/namespace").toString()` else `ProfileStore::currentId()` (seed it if you like, but reading with the fallback is enough).
  - `endpoint(key)` helper: `QString base = serverBase(); if (!token().isEmpty()) base += "/" + token(); QString u = base + "/sync/" + ns(); if (!key.isEmpty()) u += "/" + QString::fromUtf8(QUrl::toPercentEncoding(key)); return u;`
  Implement the six primitives (each mirrors a `DriveSyncBackend` primitive's async shape — `nam_->get/sendCustomRequest`, parse in the `finished` lambda, `cb(...)` on the GUI thread):
  - **`ensureFolder(cb)`** → the namespace is implicit; `cb(ns())` synchronously (a non-empty "folder id" means ready; reachability flows through `findFile`'s `listOk`, per the contract). If `serverBase()` is empty, `cb("")`.
  - **`findFile(folderId, name, cb)`** → `GET endpoint("")` (the list). On network error → `cb(false /*listOk*/, "", "", "")`. On HTTP 200 → parse JSON `{objects:[{key,version,meta,size,deleted,modifiedUtc}]}`; find `key==name` with `deleted==false` → `cb(true, name, modifiedUtc, meta)`; not found → `cb(true, "", "", "")` (genuinely absent). (`meta` maps to Drive's `stateHash`; `version` is the ETag, unused by the client's optimistic CAS but available.)
  - **`uploadFile(folderId, existingId, name, mime, data, stateHash, cb)`** → `PUT endpoint(name)` via `nam_->sendCustomRequest(req, "PUT", data)`; set `req.setRawHeader("X-Sync-Meta", stateHash.toUtf8())`; if `existingId.isEmpty()` set `req.setRawHeader("If-None-Match", "*")` (create-only; a 412 → treat as failure `cb("")`, the next cycle retries as update). On HTTP 204 → `cb(name)`; else `cb("")`.
  - **`downloadFile(fileId, cb)`** → `GET endpoint(fileId)`. HTTP 200 → `cb(true, reply->readAll())`; else `cb(false, {})`.
  - **`findFolderNamed(name, cb)`** → server has no folders; `cb(true, name)` (only Drive/brand-migration calls this; unused on the server path).
  - **`renameFile(fileId, newName, cb)`** → `cb(true)` no-op (brand migration is Drive-only; unused on the server path). (A faithful PUT-new+DELETE-old is possible later; not needed now.)
  - **Auth surface:** `isSignedIn()` → `!serverBase().isEmpty()`; `accountEmail()` → `serverBase()` (shown as the "account"); `lastAuth()` → `PendingPush::Auth::Ok`; `signIn()` → emit `signedIn(serverBase())` immediately (config, not a flow) if configured, else `emit signInFailed(tr("Set the server URL first"))`; `signOut()` → `store().remove("cloud/server/url"); store().remove("cloud/server/token"); store().sync(); emit signedOut();`. (These are instance methods overriding `SyncBackend`.)
  Include the response-status idiom `reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()` and `reply->error() != QNetworkReply::NoError` (network vs HTTP). Emit the three signals as needed.

- [ ] **Step 2: Backend selection in `CloudSync`** — in `CloudSync.h` add a second ctor `explicit CloudSync(SyncBackend* backend, QObject* parent = nullptr);` (adopts `backend`, reparents it to `this`). In `CloudSync.cpp`:
  - Extract a small file-static `SyncBackend* makeConfiguredBackend(QObject* owner)` that reads `store().value("cloud/backend").toString()` and returns `new ServerSyncBackend(owner)` when it equals `"server"`, else `new DriveSyncBackend(owner)`.
  - The default ctor `CloudSync(QObject* parent)` sets `backend_ = makeConfiguredBackend(this);` then the three `connect(backend_, &SyncBackend::…, this, &CloudSync::…)` (unchanged).
  - The new injection ctor sets `backend_ = backend; backend_->setParent(this);` then the same three connects. (Refactor the connects into a private `wireBackend()` helper to avoid duplication.)

- [ ] **Step 3: Force Drive for brand migration** — `BrandMigrationDrive.cpp:117` `new CloudSync()` → `new CloudSync(new DriveSyncBackend(), parent)` (include `DriveSyncBackend.h`). Brand migration renames the OLD brand's Drive folder and is meaningless on the server backend; it must always run against Drive.

- [ ] **Step 4: `probe_serversync.cpp`** — a network-free end-to-end test using an in-process stub HTTP server (`QTcpServer` on `127.0.0.1:0`) that implements the object store: `GET /…/sync/{ns}` → the list JSON; `GET /…/sync/{ns}/{key}` → bytes + `ETag` + `X-Sync-Meta`; `PUT` → store bytes+meta, honor `If-None-Match: *` (412 if present), 204 + new ETag; `DELETE` → tombstone. Point `ServerSyncBackend` at the stub by writing `cloud/server/url = http://127.0.0.1:<port>`, `cloud/server/token=""`, `cloud/server/namespace="p1"` into a temp ini (set `AppPaths`/`EB_*` so `store()` uses it — mirror how `probe_savesync` isolates its data dir). Then drive the primitives directly and assert: `ensureFolder`→ns; `uploadFile` new→get id, `findFile`→listOk+id+meta(stateHash echoed), `downloadFile`→bytes; a second `uploadFile` with a non-empty `existingId` overwrites; a `findFile` for an absent key→listOk=true,id=""; a stub returning a connection error→`findFile` listOk=false. Print `SERVERSYNC-OK` and exit nonzero on any failure (match the probe idiom). Keep it deterministic; no real sockets beyond loopback.

- [ ] **Step 5: CMake** — add `ServerSyncBackend.cpp/.h` beside `DriveSyncBackend.cpp` in the app target and the CloudSync-linked probe targets (a `ServerSyncBackend` reference now lives in `CloudSync.cpp`, so every target linking `CloudSync.cpp` needs it: app, probe_audioout, probe_cloudmerge, probe_onboarding, probe_brand, probe_savesync). Add a `probe_serversync` target (copy `probe_savesync`'s target block; sources `tools/probe_serversync.cpp` + `src/core/ServerSyncBackend.cpp/.h` + `src/core/SyncBackend.h` + `ProfileStore.cpp` + whatever `store()`/AppPaths deps it needs; link `Qt6::Core Qt6::Network`). Register `probe_serversync` in `native/tools/run-headless-probes.sh`'s probe list.

- [ ] **Step 6: Build + probes**
```bash
cmake --build build --config Release --target probe_cloudmerge probe_savesync probe_serversync everythingbox
BUILD_DIR=build bash native/tools/run-headless-probes.sh
```
Expected: `ALL HEADLESS PROBES PASSED` incl. the new `SERVERSYNC-OK`, and `CLOUDMERGE-OK`/`SAVESYNC-OK` unchanged (Drive path unaffected — default backend is still Drive). (If `Qt6Test.dll` is missing for `probe_navqml/formfactor/uitest`, copy it into `build/Release/` — a known pre-existing deployment gap, not part of this change.)

- [ ] **Step 7: Commit**
```bash
git add native/src/core/ServerSyncBackend.h native/src/core/ServerSyncBackend.cpp native/src/core/CloudSync.h native/src/core/CloudSync.cpp native/src/core/BrandMigrationDrive.cpp native/tools/probe_serversync.cpp native/CMakeLists.txt native/tools/run-headless-probes.sh
git commit -m "feat: ServerSyncBackend over the self-hosted object store; CloudSync picks its backend from config"
```

---

### Task 2: Backend chooser + URL/token pairing UI + switch-as-migration

**Files:**
- Modify: `native/src/ui/MainWindow.cpp` (`openCloudSync()` — the themed panel branch ~:14609 and the classic branch ~:14692; add the chooser + fields + the migration action)

**Interfaces:**
- Consumes: `ServerSyncBackend`/the `cloud/backend`+`cloud/server/*` keys (Task 1); `CloudSync` rebuild on switch.

- [ ] **Step 1: Read the current backend + render the chooser** — at the top of `openCloudSync()`, read `const bool serverBackend = store value "cloud/backend" == "server"` (use a local `QSettings` on the ini path, as `openCloudClientSetup` does). In the themed-panel row build (before the `cloud.status` row), add a `PanelRow::Choice` row `id="cloud.backend"`, label `tr("Backend")`, `value` = `serverBackend ? tr("My server") : tr("Google Drive")`, options `{tr("Google Drive"), tr("My server")}`. In `onAct`, when `id=="cloud.backend"` with the chosen `val`, map it to `"drive"`/`"server"`, and if it changed call a new `switchSyncBackend(newBackend)` (Step 3).

- [ ] **Step 2: Server fields + connect (mirror `openCloudClientSetup`)** — when `serverBackend`, replace the Drive-specific rows (`cloud.signin` "Sign in with Google", `cloud.setup` "Change sign-in client…") with:
  - a `PanelRow::TextField` `id="cloud.srv.url"`, label `tr("Server URL")`, `value` = current `cloud/server/url`;
  - a `PanelRow::TextField` `id="cloud.srv.token"`, label `tr("Access token")`, `value` = current `cloud/server/token`, **`masked = true`**;
  - a `PanelRow::Action` `id="cloud.srv.save"` label `tr("Connect")`.
  Seed a `std::make_shared<QPair<QString,QString>>` (url, token) from the ini; in `onAct`, stash `cloud.srv.url`/`cloud.srv.token` into the pair, and on `cloud.srv.save` write `cloud/server/url`/`cloud/server/token` (trimmed url; token not trimmed, never logged) + `cloud/server/namespace` (seed from `ProfileStore::currentId()` if empty) via `QSettings::setValue`+`sync()`, then rebuild `cloud_` (Step 3, without a backend change — just re-read config) and refresh the panel. Keep the `in = cloud_->isSignedIn()` row logic (for the server backend, `isSignedIn()` == URL present, so "Sync now"/"Sign out" appear once configured). Mirror this in the classic Qt branch with `Osk::getText` prompts (URL `Normal`, token `Password`) like the OPDS add flow, gated on `serverBackend`.

- [ ] **Step 3: `switchSyncBackend` + migration** — add a `MainWindow` helper (or inline in `onAct`) that, on a backend change: (a) writes `cloud/backend` = the new value + any `cloud/server/*` already entered; (b) **rebuilds `cloud_`** — `cloud_ = std::make_unique<CloudSync>(this);` (the ctor now picks the new backend) and re-wire the window-scoped listeners exactly as the original construction at `MainWindow.cpp:519-532` does (extract that wiring into a `wireCloudSignals()` helper and call it from both sites), and recreate `saveSync_` bound to the new `cloud_.get()` (`MainWindow.cpp:537`); (c) **force-push local state to the new target** by calling `cloudSyncNow()` (which does `pushLocal` + `startSaveSync()`), so the new backend adopts the local state as its baseline (`pushLocal`→`adoptSyncedBaseline`). Guard: only migrate when the new backend is actually configured (server needs a URL) — otherwise just switch and let the user enter the URL, then Connect triggers the push. Re-present the panel to reflect the new backend's rows.

- [ ] **Step 4: Build the app + smoke the panel via the UI harness (best-effort)**
```bash
cmake --build build --config Release --target everythingbox
```
Expected: compiles. The UI itself is not headless-probe-covered; a live check is the `EB_UITEST` harness (`native/tools/uitest.py`) driving `openCloudSync` — note this as a manual/hardware verification step (do NOT block the task on it, but state it clearly in the report). Re-run `BUILD_DIR=build bash native/tools/run-headless-probes.sh` to confirm nothing regressed.

- [ ] **Step 5: Commit**
```bash
git add native/src/ui/MainWindow.cpp
git commit -m "feat: Cloud Sync backend chooser + My-server URL/token pairing, with switch-as-migration"
```

---

## Self-review

**Spec coverage (Increment C):** `ServerSyncBackend` over the object-store contract, `stateHash`↔`X-Sync-Meta`, client-optimistic CAS (spec) → Task 1 Step 1. Backend selector from `cloud/backend` (spec) → Task 1 Step 2. Config under device-local `cloud/` (spec) → Task 1 Step 1 + Task 2 Step 2. Pairing UI URL+token, token masked (spec) → Task 2 Steps 1-2. Switch-as-migration, one master (spec) → Task 2 Step 3. Categories/carve-outs reused verbatim (spec) → nothing above the seam changes. Network-free probe (spec) → Task 1 Step 4. Drive path unchanged (spec) → default backend Drive, full suite green. ✅

**Placeholder scan:** the six primitives carry exact per-primitive contracts (URL, verb, headers, response→cb); the UI mirrors the enumerated `openCloudClientSetup` template; the migration reuses `cloudSyncNow`/`pushLocal`/`adoptSyncedBaseline`. The async Qt lambda bodies are left to mirror `DriveSyncBackend`'s established pattern (cited), which is transcription, not invention.

**Type consistency:** `ServerSyncBackend : SyncBackend` overrides the exact six-primitive + auth signatures from `SyncBackend.h`. `CloudSync(SyncBackend*, QObject*)` injection ctor + `makeConfiguredBackend` defined Task 1, used by `BrandMigrationDrive` (Drive-forced) and the default ctor. `cloud/backend`/`cloud/server/{url,token,namespace}` keys consistent across backend, ctor, and UI. `PanelRow::Choice`/`TextField`(+`masked`) match `PanelModel.h`. `cloudSyncNow`/`pushLocal`/`adoptSyncedBaseline`/`ProfileStore::currentId` are existing symbols. ✅
