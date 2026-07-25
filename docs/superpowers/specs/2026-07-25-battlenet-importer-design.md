# Battle.net Game Importer — Design

**Date:** 2026-07-25
**Status:** Draft — approved through brainstorming; awaiting user spec review before plan.
**Origin:** Roadmap #6 (importers round-out). The game-importers track shipped Steam/Epic/GOG and flagged
Xbox/EA/Ubisoft/Battle.net as "same Steam pattern, later if asked." A runtime scout found none of the four
has installed games on this machine; **Battle.net is the only candidate whose launcher is already installed**,
so it's the one with a live-verify path (the user can install one Blizzard game to confirm against a real
detection source). Chosen: Battle.net first.

## Decisions (user-set, this brainstorm)

- **Scope = Battle.net only** (not all four). EA/Ubisoft aren't installed; Xbox needs elevation + has no games.
- **Detection = registry Uninstall scan, not the protobuf `product.db`.** Blizzard games register standard
  Uninstall entries (`Publisher == "Blizzard Entertainment"`, with `DisplayName` + `InstallLocation`). This
  mirrors GOG's proven `QSettings`/`childGroups` pattern, is fully fixture-testable, and avoids parsing
  Blizzard's undocumented binary `product.db` (which read as empty/inaccessible here). The `product.db`
  protobuf path is recorded as a deferred fallback, not built.
- **Launch = `battlenet://<code>` when the title maps to a known Battle.net game code; else the install-dir
  exe via the monitored `launchPcExe`.**
- **Live-verify confirms the format.** The exact registry fields, the title→code map, and which launch works
  are confirmed/corrected once a real Blizzard game exists. The probe locks the SHAPE now; the live pass locks
  the REALITY. Until a game is installed the importer ships probe-green but the console stays hidden (nothing
  detected) — the intended dormant behavior.

## Scope reality (scout, 2026-07-25)

The importer template is `EpicLibrary`/`GogLibrary`: `namespace`-scoped free functions over a POD struct,
QtCore-only, with an **injectable detection root** (last defaulted arg) as the probe seam, and a **pure parse
function** (`parseManifest`-style) that is the testable discriminator.

- **GOG registry seam** (`GogLibrary.cpp:33-38`): `QSettings("HKEY_LOCAL_MACHINE\\SOFTWARE\\WOW6432Node\\…",
  QSettings::NativeFormat)`; subkeys via `childGroups()`; the injectable root swaps the live hive for
  `QSettings(iniPath, IniFormat)` where INI groups = keys and INI keys mirror the registry values
  (`GogLibrary.cpp:22-28, 47-50`; probe fixture `probe_importers.cpp:282-311`).
- **Six UI touch-points** (all cloned from Epic/GOG): the console-injection block on the Games root
  (`HomeView.cpp:4379-4418`, gated `isAvailable() && !installedGames().isEmpty()`, `mime="<launcher>:console"`);
  the drill dispatch (`HomeView.cpp:2460-2463`); `open<Launcher>Console`/`populate<Launcher>Games`
  (`:1646-1676`); back-nav repopulation (`:3014-3016`); in-console search (`:2930-2932`); the
  `SyntheticCatalogs` builder (`SyntheticCatalogs.cpp:225-266`, `epic/gogGamesCatalog`) + `iconTypeForKind`
  (`:15-17`).
- **Launch dispatch** (`MainWindow::openLibraryItem`, `:6584+`): the Epic URI branch (`:6625-6634`,
  `QDesktopServices::openUrl` + `RecentStore::add(kind)`) and the GOG exe branch (`:6618-6622`,
  `launchPcExe(url, id, title, thumb, "goggame")` which records the Recent itself). Recent kinds in
  `RecentStore.h:39` (enum `Relaunch`) + `relaunchFor` (`RecentStore.cpp:118-128`); re-open in
  `openRecent` (`:4927-4970`).
- **Probe** `probe_importers.cpp`: fixture-root parse assertions per importer + the `relaunchFor` dispatch
  table (`:177-188`) + `iconTypeForKind` (`:191-193`); sentinel `IMPORTERS-OK`. Build wiring: a new
  `BattleNetLibrary.cpp/.h` joins BOTH the app source list (`CMakeLists.txt:~206`) and the `probe_importers`
  target (`:~490`).

## Design

### Component — `BattleNetLibrary` (`native/src/core/BattleNetLibrary.{h,cpp}`, QtCore-only)

```cpp
namespace BattleNetLibrary {
    struct BattleNetGame {
        QString code;        // battlenet:// launch code (e.g. "wow", "d3", "pro"); empty if unknown → exe launch
        QString name;        // DisplayName
        QString installDir;  // InstallLocation
        QString exe;         // best-effort game exe under installDir (launch fallback); may be empty
    };
    // Live: scan the Uninstall hive for Publisher=="Blizzard Entertainment". Fixture: an INI root (groups =
    // uninstall subkeys, keys = DisplayName/Publisher/InstallLocation) read with QSettings(IniFormat).
    bool               isAvailable(const QString& regProbeRoot = QString());
    QVector<BattleNetGame> installedGames(const QString& regProbeRoot = QString());
    QString            launchUri(const QString& code);   // "battlenet://" + code
    // Pure, probe-testable: filter an uninstall entry → a BattleNetGame (empty name ⇒ dropped by callers),
    // deriving `code` from the title→code table. Mirrors EpicLibrary::parseManifest's "empty key = filtered".
    BattleNetGame      parseUninstallEntry(const QString& displayName, const QString& publisher,
                                           const QString& installLocation);
}
```
Detection: enumerate `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*` and its `WOW6432Node`
sibling; for each subkey read `Publisher`/`DisplayName`/`InstallLocation`; keep those whose publisher is
Blizzard; run `parseUninstallEntry`; dedupe by name; sort. `code` comes from a small curated title→code map
(word-normalized `DisplayName` → code: world of warcraft→`wow`, diablo iii→`d3`, diablo iv→`osi`,
overwatch→`pro`, hearthstone→`wtcg`, starcraft ii→`s2`, warcraft iii reforged→`w3`, call of duty→`cod`, …;
unknown → empty). `exe` = best-effort scan of `installLocation` for a top-level `*.exe` (skip
launcher/uninstaller names). The injectable INI root swaps the live Uninstall hive exactly as GOG's does.

### The six UI touch-points (clone Epic/GOG verbatim)

1. `SyntheticCatalogs.h/.cpp` — `battleNetGamesCatalog(const QVector<BattleNetGame>&, const QString& query)`:
   each game → `MediaItem{ id="bnet:"+code (or "bnet:"+name when code empty), type="game", mime="battlenetgame",
   systemHint="Battle.net", title=name, url = code.isEmpty()? exe : QString() }` (url carries the exe only for
   the code-less exe-launch path, mirroring GOG; a coded game carries no url and launches by URI). Add
   `"battlenetgame"` to `iconTypeForKind`.
2. `HomeView.cpp` injection block (gated `BattleNetLibrary::isAvailable() && !installedGames().isEmpty()`,
   `mime="battlenet:console"`); drill dispatch `if (it.mime=="battlenet:console") openBattleNetConsole(it)`;
   `openBattleNetConsole`/`populateBattleNetGames` (mirror `openGogConsole`); back-nav + in-console search.
3. `RecentStore` — `Relaunch::BattleNetGame` + `relaunchFor("battlenetgame")`.
4. `MainWindow::openLibraryItem` — a `battlenet://` URL-prefix branch (mirror Epic: `openUrl` + record Recent
   kind `battlenetgame`, resume key `"bnet:"+code`); for a code-less game the item's `url` is the exe →
   it falls to the existing exe/mpv-guarded PC path via `launchPcExe(url, id, title, thumb, "battlenetgame")`.
5. `MainWindow::openRecent` — a `BattleNetGame` case: rebuild `battlenet://<code>` from the `bnet:` resume key
   and `openUrl` (mirror the Epic re-open); code-less → `launchPcExe` the recorded path (mirror GOG).
6. CMake: `BattleNetLibrary.cpp/.h` into the app target + `probe_importers`.

### Data flow

Games root render → `isAvailable() && !installedGames().isEmpty()` → inject the `battlenet:console` tile →
drill → `battleNetGamesCatalog(installedGames())` lists the games → activate → `openLibraryItem` launches
via `battlenet://<code>` (or the exe) and records a Recent → re-open from Recents relaunches. All detection
is live-scan (no persistence — the game-importers precedent: fast + self-healing).

## Error / edge handling

| Situation | Behavior |
|---|---|
| Battle.net installed, no games | `installedGames()` empty → no console injected (dormant; today's state on this machine). |
| Blizzard game with no known code | `code` empty → the exe-launch fallback (`launchPcExe`); still lists + badges + Recents. |
| No exe found + no code | Game still LISTS (name/installDir); launch no-ops gracefully (like GOG's missing-exe weak-degrade). |
| Uninstall entry not Blizzard | Filtered by the `Publisher` check in `parseUninstallEntry`. |
| product.db present but binary | Ignored (registry is the source); the protobuf path is a deferred fallback. |
| Registry read fails / absent | `isAvailable` false → dormant, no crash. |

## Verification

- **`probe_importers`** (extend, fixture-first): `parseUninstallEntry` table (Blizzard entry kept with the
  right code; non-Blizzard publisher filtered; unknown-title → empty code; missing InstallLocation still
  parses name); `installedGames(iniRoot)` over an INI fixture (groups = uninstall subkeys) → parse/sort/dedupe;
  `launchUri("wow") == "battlenet://wow"`; `battleNetGamesCatalog` mapping (coded → no url + `bnet:wow` id;
  code-less → exe rides url); `relaunchFor("battlenetgame") == BattleNetGame`; `iconTypeForKind("battlenetgame")`.
  Sentinel `IMPORTERS-OK`.
- **Live (USER-gated):** the user installs ONE Blizzard game via the already-installed Battle.net client, then
  a scan → the **Battle.net console appears on the Games root**, drilling lists the game, activating launches
  it (URI or exe), and it records + re-opens from Recents. This pass **confirms/corrects** the real registry
  field names, the title→code entry, and the working launch mechanism — the parse seam is adjusted to match if
  reality differs. Recorded honestly as the game-importers posture (fixture-verified now; live when a game
  exists).
- Full headless suite + app compile; no perf run (importer detection is a Games-root live scan, off the hot
  path — the Steam/Epic/GOG precedent).

## Non-goals

- EA / Ubisoft / Xbox importers (not installed; deferred — same pattern when asked).
- Parsing the `product.db` protobuf (registry is simpler + testable; deferred fallback).
- Owned-but-not-installed Blizzard games (no public per-account API akin to Steam's; installed-only).
- Play-time/stats for Battle.net games (roadmap #3 owns launcher stats; URI launches are fire-and-forget).
- gamelist.xml import (a separate #6 sub-item, not this track).
- Any change to Steam/Epic/GOG, Seam A/B, or the addon/sync transports.
