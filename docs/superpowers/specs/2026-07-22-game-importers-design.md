# Game Library Importers (Playnite roadmap #1: Steam parity + Epic/GOG) — Design

**Date:** 2026-07-22
**Status:** Approved roadmap item (user-set order); plan next.
**Origin:** User request — Playnite's killer feature, importers, "Steam first, then Epic/GOG."

## Scope reality (scout, 2026-07-22)

Installed-Steam support ALREADY EXISTS and works: `SteamLibrary` (registry locate,
`libraryfolders.vdf` roots, `appmanifest_*.acf` scan, tool filtering, local capsule +
CDN art, `steam://rungameid` launch), a synthetic "Steam" console injected into the
Games console list, appid-keyed detail metadata (Steam store API). What's MISSING for
Playnite parity, and what's genuinely new:

## 1. Steam gap-closure

- **Recents/resume integration**: `"steamgame"` becomes a recognized Recent kind —
  launching a Steam game records Recents (kind `steamgame`, key `steam:<appid>`,
  thumb = capsule); the Recent-open dispatch (beside the `pcgame` branch) re-launches
  via `SteamLibrary::launchUrl`. Launches stay fire-and-forget (the Steam client owns
  the process — no play-time tracking this track; stats are roadmap #3).
- **Owned-but-not-installed (creds-gated, optional)**: a Steam Web API key + SteamID
  entered in the Add-ons/General settings (user-supplied, the SteamGridDB-key
  precedent — NOT embedded) unlocks owned-library entries appended to the Steam
  console: greyed/badged "Not installed", activation = `steam://install/<appid>`
  (Steam client handles the install UI). No key = installed-only, zero friction,
  exactly today's behavior. API: `IPlayerService/GetOwnedGames` (name + appid +
  logo). Cached with a TTL (the catalogCache precedent); offline/invalid-key =
  silent fallback to installed-only (never an error surface at the console).
- **Marks integration sanity**: Steam items get stable keys (`steam:<appid>` via
  MetaCache::keyFor already) — verify hide/tags/completion + shelves work on the
  Steam console (should be free; verify, don't assume).

## 2. Epic + GOG importers (new, mirroring the Steam shape)

- **`EpicLibrary`** (`native/src/core/EpicLibrary.{h,cpp}`): installed games from
  `%ProgramData%\Epic\EpicGamesLauncher\Data\Manifests\*.item` (JSON: DisplayName,
  AppName, InstallLocation); launch `com.epicgames.launcher://apps/<AppName>?action=launch&silent=true`;
  art: no local capsule convention — title-keyed via the existing scrapers
  (SteamGridDB by name), thumbnail empty initially.
- **`GogLibrary`** (`native/src/core/GogLibrary.{h,cpp}`): installed games from the
  registry `HKLM\SOFTWARE\WOW6432Node\GOG.com\Games\<id>` (gameName, path, exe) —
  works with or without Galaxy; launch = the exe directly (the launchPcExe monitored
  path — GOG games are DRM-free processes) or `goggalaxy://openGameView/<id>` when
  Galaxy is present (prefer the exe: simpler, tracked).
- Each gets a synthetic console (the Steam-console pattern verbatim): appears when
  the library is detected non-empty; per-console search; same activation/detail
  shape. Mimes `epicgame`/`goggame`; Recent kinds likewise.
- Detection modules are QtCore-only with injectable probe roots (the ExternalPlayer
  precedent) — probe-testable without the launchers installed.

## Non-goals (this track)

- Play-time/stats (roadmap #3 owns it). Xbox/EA/Ubisoft/Battle.net importers
  (follow the same pattern later if asked). Persisting scan results to a store
  (live scan is fast and self-healing — revisit only if a scan ever measurably
  lags). Uninstall/install management beyond the steam://install handoff.

## Verification

- Probes: Epic manifest parsing + GOG registry parsing over injected fixtures
  (probe roots); the owned-games JSON parse + TTL/invalid-key fallback logic
  (stubbed reply); Recent-kind dispatch table. Steam's existing scan already works —
  a probe pins the ACF fixture parse if not already covered.
- Live: Steam console Recents round-trip (launch → Recent appears → re-open
  launches); marks on a Steam item (hide/tag + shelf); owned-games with the USER'S
  key IF they provide one (else the stubbed probe stands + the console shows
  installed-only — record honestly); Epic/GOG consoles IF those launchers exist on
  this machine (scout at runtime; else probe fixtures stand + record device-
  unverified, the Android-precedent posture).
- Suite + perf gates unchanged.
