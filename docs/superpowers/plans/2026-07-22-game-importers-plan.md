# Game Library Importers Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Playnite-parity game importers: Steam gap-closure (Recents + owned-games extension + marks sanity) and new Epic/GOG importers mirroring the existing Steam synthetic-console shape.

**Architecture:** Detection modules are QtCore-only with injectable probe roots (the ExternalPlayer precedent); consoles are synthetic catalogs (the Steam-console pattern verbatim); launches ride existing paths (`steam://` fire-and-forget, GOG exe via the monitored launchPcExe, Epic URI). Spec: `docs/superpowers/specs/2026-07-22-game-importers-design.md` (contains the scout's existing-support map).

**Tech Stack:** Qt 6.8.3, QSettings registry reads, headless probes.

## Global Constraints

- Branch: `library/game-importers` off main. Standing autonomy through the merge gate.
- ANCHOR ON FUNCTION NAMES. Key anchors (scout, main@add22ca): `SteamLibrary` (core, the template); `browse::steamGamesCatalog` (SyntheticCatalogs.cpp ~:149) + the console injection (HomeView ~:4139) + `openSteamConsole`/`populateSteamGames` (~:1548); steamgame launch at `resolvePlay` (~:2906) → onOpenItem steam:// branch (MainWindow ~:6222); the pcgame Recent dispatch (MainWindow ~:4819) — the steamgame/epicgame/goggame branches go beside it; `launchPcExe` (~:5454, the monitored process path for GOG); RecentStore kinds doc (RecentStore.h); the user-key settings precedent = SteamGridDB's manifest-driven config.
- Fire-and-forget for Steam/Epic (client-owned processes); GOG = the monitored exe path. NO play-time tracking this track.
- Owned-games: user-supplied Steam Web API key + SteamID (NEVER embedded); no key = installed-only with zero new UI friction; offline/invalid = silent fallback (log only). TTL-cache the owned list (the catalogCache precedent, ~30min).
- Probes: injectable roots, never touch real registries/launchers in probe mode, never launch anything; registered runner + ci.yml; new modules linked into the APP target in their own task.
- Env recipe COMPLETE everywhere (the 4×-disproven hang false-alarm); serial builds; ini snapshot/byte-identical restores; Weekend Picks untouched.

---

### Task 1: Steam gap-closure (Recents + marks sanity + owned-games)

**Files:** Modify `native/src/core/SteamLibrary.{h,cpp}` (owned-games fetch: `ownedGames(apiKey, steamId)` returning the parsed list + a small TTL cache; keep QtCore+Network), `native/src/core/RecentStore.h` (kind doc), `native/src/ui/MainWindow.cpp` (steamgame Recent recording at the steam:// launch branch ~:6222; the Recent-open dispatch branch beside pcgame ~:4819), `native/src/ui/HomeView.cpp` + `native/src/browse/SyntheticCatalogs.cpp` (owned-not-installed entries appended to steamGamesCatalog — badged "Not installed", activation → steam://install/<appid>), settings rows for the key+SteamID (the General or Add-ons panel — follow the SteamGridDB user-key precedent; masked TextField for the key), `native/tools/probe_importers.cpp` (create; owned-JSON parse + TTL + invalid-key fallback with stubbed replies; Recent-kind dispatch table as pure logic where linkable) + CMake/runner/ci.yml.

- [ ] Probe RED → implement → green. Recents: launching a Steam game records {url steam://…, title, kind "steamgame", key "steam:<appid>", capsule thumb}; re-open from Recents re-launches (live). Marks sanity (live): hide/tag a Steam item → filtering + shelves behave (should be free via keyFor="steam:<appid>" — verify).
- [ ] Owned-games: settings rows; when configured, the Steam console appends owned-not-installed (badge in title/subtitle; activation = steam://install/<appid> via the same openUrl handoff); TTL cache; silent fallback. Live with the user's key ONLY if present in the ini already — else stubbed-probe evidence + installed-only live view, recorded honestly.
- [ ] Commit `feat: Steam parity — Recents, marks sanity, owned-games extension (importers T1)`.

---

### Task 2: Epic + GOG importers

**Files:** Create `native/src/core/EpicLibrary.{h,cpp}`, `native/src/core/GogLibrary.{h,cpp}` (both: `isAvailable()`, `installedGames()`, launch helpers; injectable probe roots — Epic: manifests dir override; GOG: a QSettings-ini registry override, the ExternalPlayer regProbeRoot precedent). Modify `SyntheticCatalogs.{h,cpp}` (epicGamesCatalog/gogGamesCatalog builders, mimes epicgame/goggame), `HomeView.cpp` (console injection beside the Steam block ~:4139; open/populate levels; resolvePlay branches — epic: URI launch via openItem like steam; gog: route to the launchPcExe monitored path with the exe), `MainWindow.cpp` (epic URI handoff beside the steam:// branch; Recent kinds epicgame/goggame + dispatch branches; gog Recent rides the pcgame-style exe path semantics), probe_importers (Epic .item JSON fixtures + GOG registry-ini fixtures → parsed lists; hidden-tool/DLC filtering judgments documented).

- [ ] Probe RED (fixtures) → implement → green. Epic entries: DisplayName/AppName/InstallLocation from *.item; launch `com.epicgames.launcher://apps/<AppName>?action=launch&silent=true`. GOG entries: registry gameName/path/exe; launch = the exe via launchPcExe (monitored, Recents as its own goggame kind but the pcgame launch mechanics — document the kind-vs-mechanics split).
- [ ] Live: whichever launchers exist on this machine get a real console walk (scout at runtime — check the Epic manifests dir + GOG registry); absent ones = fixtures + device-unverified recorded (the Android posture). Consoles appear only when non-empty; search works; marks work on both.
- [ ] Commit `feat: Epic + GOG installed-library importers (importers T2)`.

---

### Task 3: close-out — spec status, gates, fable, merge

- [ ] Spec Status → complete-with-honest-verification-posture (which launchers were live-verified vs fixture-verified; owned-games key state). Follow-ups: Xbox/EA/Ubisoft importers (same pattern), play-time stats (roadmap #3), scan persistence (only if perf ever demands). Full suite + perf 3 runs. Fable whole-branch (dimensions: the three-importer console-injection consistency; the goggame kind-vs-pcgame-mechanics split; owned-games creds hygiene — key never logged/committed; Recents dispatch table completeness). Fix rounds; merge+push+redeploy.

## Self-Review (done at write time)

- Spec coverage: Steam Recents/owned/marks ✅T1; Epic+GOG ✅T2; non-goals preserved (no stats, no persistence, no extra launchers); creds = user-supplied only ✅T1+constraints.
- Type consistency: SteamLibrary::ownedGames, Epic/GogLibrary::{isAvailable,installedGames}, mimes steamgame/epicgame/goggame, Recent kinds likewise — consistent across tasks.
- Ambiguities resolved: GOG launches = monitored exe (goggame Recent kind, pcgame mechanics — the split documented); Epic art = name-keyed scrapers (thumbnail may start empty); owned-games UI = badge + install handoff only.
