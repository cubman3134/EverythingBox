# PS3 Game Auto-Update — Design

**Status:** Design (approved decisions locked; pending user review of this doc)
**Date:** 2026-08-18
**Repo:** EverythingBox client (Project Goliath) — `native/src`

## Problem

When EverythingBox has a PS3 game, it runs it through the bundled **RPCS3**
emulator against whatever the base game shipped as (ISO / PKG / JB folder /
`EBOOT.BIN`). That base is unpatched. Sony hosts the official update PKG chain
for every retail and PSN title, indexed exactly the way
`ps3.aldostools.org/updates.html` reads it. We want EverythingBox to fetch a
game's official Sony updates and install them into RPCS3 before the game boots,
so the player runs the patched game with no manual step.

## Locked decisions

- **Architecture: client-side.** Everything lives in the EverythingBox client.
  The server/Allarr side is *not* touched — it keeps delivering base games as it
  already does. The client reads the game's Title ID, fetches Sony's update
  feed, and installs the PKGs into RPCS3.
- **Trigger: auto on launch.** The update check runs on the RPCS3 launch path,
  before the game process starts, cached so it only does real work once per
  game/version.
- **Scope: install all packages in the feed, in version order.** Sony's
  `ver.xml` is the authoritative update chain; some titles (e.g. GTA V
  `BLUS31156`) list several packages. Installing the full listed set in order is
  exactly what RPCS3's own updater does and is always safe (a later cumulative
  package simply supersedes earlier ones). "Latest only" is rejected because a
  few titles ship genuinely incremental chains.

## Feasibility — verified

The one open technical risk was the Sony fetch. Verified live on 2026-08-18:

- Endpoint: `https://a0.ww.np.dl.playstation.net/tpl/np/{TITLEID}/{TITLEID}-ver.xml`
- Requires TLS **certificate verification disabled** (`curl -k`) — Sony's cert
  CN does not match the host; RPCS3 and aldostools both connect this way.
- **Has updates** → `<titlepatch>` with one or more
  `<package version="…" size="…" sha1sum="…" url="…" ps3_system_ver="…">`
  children. The package `url` is plain `http://` (Akamai NetStorage), no auth.
- **No updates** → HTTP 200 with an **empty body** (0 bytes). This is the common
  "nothing to do" signal and must be treated as "no updates", not an error.

Confirmed shapes:
`BCUS98174` (The Last of Us) → one package `01.11`; `BLUS31156` (GTA V) → a
multi-package chain; `BLUS30443` / `NPUB30910` → empty body (no updates).

## Where it hooks

`EmulatorManager::launch()` (`native/src/core/EmulatorManager.cpp`) is the single
launch path for every standalone emulator. RPCS3 is portable on Windows, so
`dev_hdd0/` sits under the emulator's `binDir` (the directory of the RPCS3 exe),
and update PKGs install into `dev_hdd0/game/{TITLEID}` — which RPCS3 applies
automatically when it boots the matching Title ID (disc or PSN).

The update step runs inside `launch()` only when `em_.id == "rpcs3"`, the game is
a bootable target, and the "Auto-install PS3 game updates" setting is on. It
runs *before* `startGameProcess`, blocking the boot with an on-screen
"Updating game…" note while it works. **Any failure at any stage logs and falls
through to a normal boot** — the update path can never prevent a game from
launching.

## Components

Each unit is small, single-purpose, and independently testable. The two units
that touch the outside world (network, `rpcs3.exe`) sit behind injected seams so
tests never hit Sony or spawn a process.

### 1. `Ps3TitleId` — read the Title ID from the game

**Interface:** `std::optional<QString> readPs3TitleId(const QString& romPath)`
returning a canonical Title ID like `BLUS31156`, or `std::nullopt` if it can't be
determined.

The Title ID lives in the game's `PARAM.SFO` (a small, documented key/value
binary: a fixed header, an index table, and key/data tables; the `TITLE_ID` key
holds the ID). `SystemCatalog` declares PS3 games as **folders or `.pkg`**, so
those are the two formats the reader handles:

- **JB folder / extracted game** (`rom` points at a game dir or at
  `…/PS3_GAME/USRDIR/EBOOT.BIN`): read `PARAM.SFO` from the game root
  (`PS3_GAME/PARAM.SFO`, or `PARAM.SFO` beside the dir), walking up from an
  `EBOOT.BIN` target to the game root.
- **PKG** (PSN): the Title ID is in the PKG header `content_id`
  (`XXYYYY-{TITLEID}_00-…` at a fixed offset), read directly without unpacking.

Any other format (including a raw `.iso`) → `std::nullopt` → the update step
falls through to a normal unpatched boot (today's behavior). ISO9660 support is a
**deferred follow-up**, not part of this build: PS3 ISOs are the uncommon case
here, and a full ISO9660 walk plus fixture is disproportionate to that.

The SFO parser (raw SFO bytes → `TITLE_ID`) is pure and unit-tested against a
fixture; the folder locator and the PKG `content_id` reader are separately
testable.

### 2. `Ps3UpdateFeed` — fetch and parse Sony's ver.xml

**Interface:**
`QVector<Ps3UpdatePackage> parsePs3VerXml(const QByteArray& xml)` (pure), plus a
fetch seam `std::function<std::optional<QByteArray>(const QString& titleId)>`
whose production implementation does the cert-disabled HTTPS GET.

`Ps3UpdatePackage` = `{ QString version; qint64 size; QString sha1; QString url; QString ps3SystemVer; }`.

- Empty body → empty vector (no updates).
- Non-empty → one `Ps3UpdatePackage` per `<package>`, returned **sorted ascending
  by version** so install order is the update chain order.
- Malformed XML → empty vector (logged) — treated as "no updates", never fatal.

### 3. `Ps3UpdateInstaller` — download, verify, install

**Interface:** given a Title ID and the sorted package list, for each package in
order: download the PKG to a temp file, verify its SHA-1 against `sha1sum`, then
install it via the RPCS3 CLI. Reports progress per package.

- Download and the `rpcs3.exe --installpkg <pkg>` invocation are injected seams
  (a downloader delegate and a process-runner delegate) so tests drive them
  without network or process spawns.
- SHA-1 mismatch on any package → **abort the whole update** (a chain must be
  applied intact), log it, and fall through to a normal boot. State is not
  advanced, so the next launch retries the update cleanly.
- Temp PKGs are cleaned up after install (and after an abort).

### 4. `Ps3UpdateState` — idempotency

**Interface:** per Title ID, record the highest update version already installed;
`bool needsUpdate(titleId, latestVersion)` and `markInstalled(titleId, version)`.

Persisted as a small JSON file under the app data dir (e.g.
`data/ps3-updates.json`). On launch, after fetching the feed, if the recorded
version already covers the feed's latest, the whole step is skipped instantly and
no PKG is downloaded. This makes the second and later launches of a patched game
pay only the cost of one cheap ver.xml fetch (or zero, see the setting).

### 5. Orchestration in `EmulatorManager::launch()`

The RPCS3 branch calls a single coordinator: read Title ID → fetch feed →
compare against state → if behind, show "Updating game…", run the installer,
update state → proceed to `startGameProcess`. Each arrow short-circuits to a
plain boot on absence/failure.

### 6. Setting — "Auto-install PS3 game updates"

A boolean, **default on**, added to **both** settings surfaces per the nav-kit
rule (the themed builder and the QWidget builder in `MainWindow.cpp` — see the
"two settings builders" project rule). When off, the update step is skipped
entirely (not even a feed fetch).

## Data flow

```
launch(rpcs3, rom)
  └─ setting on? ── no ─▶ boot
        │ yes
        ▼
   readPs3TitleId(rom) ── nullopt ─▶ boot
        │ TITLEID
        ▼
   fetch ver.xml (cert off) ── empty/err ─▶ boot
        │ packages[]  (sorted by version)
        ▼
   Ps3UpdateState.needsUpdate? ── no ─▶ boot
        │ yes
        ▼
   show "Updating game…"
   for pkg in packages: download ▶ verify sha1 ▶ rpcs3 --installpkg
        │ ok
        ▼
   markInstalled(TITLEID, latest) ─▶ boot (patched)
```

## Error handling

The governing rule: **the update path never blocks a launch.** Every stage —
unreadable Title ID, network failure, empty feed, malformed XML, download
failure, SHA mismatch, install non-zero exit — is caught, logged, and falls
through to a normal boot. The worst case degrades to today's behavior (unpatched
game), never to "game won't start."

## Testing

- **SFO parser:** fixture `PARAM.SFO` bytes → `TITLE_ID` extracted; a malformed
  blob → `nullopt`.
- **Format locators:** a temp folder layout (`PS3_GAME/PARAM.SFO`) and a PKG
  header fixture → correct SFO located / content-id parsed; an unknown format →
  `nullopt`.
- **ver.xml parser:** the three verified fixtures (single package, multi-package
  chain, empty body) → correct sorted `Ps3UpdatePackage` vectors; malformed XML →
  empty.
- **Installer:** stubbed downloader + process-runner assert install order,
  SHA-verify gating, temp cleanup, and abort-on-corrupt.
- **State:** round-trip persistence; `needsUpdate` version comparison
  (`01.06` vs `01.11`, equal, older).
- **Orchestration:** every failure branch falls through to boot; happy path
  installs then boots.

No test performs a real network request or spawns `rpcs3.exe`; the two external
seams are injected in every test.

## Out of scope

- **DLC / add-on content** (separate licensed PKGs) — not a game patch.
- **PS3 firmware** — a genuine one-time user requirement, already handled
  separately.
- **Non-PS3 consoles** — this is RPCS3-only.
- **Server / Allarr changes** — none; base-game delivery is unchanged.
- **A UI to browse/pick specific update versions** — auto, latest-chain only.
- **Raw `.iso` Title-ID reading** (ISO9660 walk) — deferred follow-up; unknown
  formats fall through to an unpatched boot.

## Global constraints

- No AI attribution anywhere in commits/PRs/issue bodies (repo `CLAUDE.md`).
- Stage by explicit path; never `git add -A` / `git add .` (shared working tree).
- New user-facing setting goes in **both** settings builders (nav-kit rule).
- All UI through the nav kit — no `QDialog`/`QMessageBox`/top-level windows.
- Follow existing `EmulatorManager` patterns (seed-if-absent, portable binDir,
  `{rom}` handling); do not restructure the launch path beyond the added hook.
