# PC Games: One Folder, Many Sources — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** One PC Games folder where each game is a single entry and the launcher or download you use is a source you pick.

**Architecture:** A pure identity unit (`PcGameId`) groups the four launcher libraries and the downloaded-games store into one entry per game; a pure `pickAutoSource` decides launch-or-ask; a pure `browse::pcGamesCatalog` emits the merged folder following the existing `SyntheticCatalogs` pattern; and a repeatable, hash-aware remap moves per-item records from the old per-launcher ids to the merged id without ever dropping one.

**Tech Stack:** Qt 6.8.3 (Core), C++17, CMake. Headless console probe. No new dependencies.

## Global Constraints

- **Build config is always Release**, always a named target: `cmake --build build --config Release --target <name>`. A target-less build compiles ~43 probes and stalls.
- **Create a `.cpp` in the SAME step that adds it to CMake** — CMake resolves source lists at configure time.
- **The suite script only RUNS pre-built exes** — build first, then run it.
- **A new probe must be registered in THREE places** or it silently never runs: its `add_executable` in `native/CMakeLists.txt`, the loop list in `native/tools/run-headless-probes.sh` (~line 130), and the `--target` list in `.github/workflows/ci.yml` (~line 52).
- **The suite runs windowed probes with `-platform offscreen`.** A direct run without that flag can report spurious failures.
- **Nav kit only** for the picker — `NavMenu`/`NavConfirm`/`Osk`. Never `QDialog`/`QMessageBox`/`QInputDialog`. `probe_nav` gates this.
- **`normalizeTitle` must NOT strip sequel numerals**, Arabic or Roman. Merging `Hades` with `Hades II` loses a game from the library; seeing it twice merely annoys.
- **The remap must never drop a record it cannot map.** An unmappable id keeps its existing record untouched.
- **Play must never silently start a multi-gigabyte download.** No ready source ⇒ ask.
- **Mutation testing is the standard of proof.** Break the implementation, confirm the probe FAILS, revert, confirm green.
- **Never touch `C:\EverythingBox-app`** or its ini. Throwaway copies only; strip `cloud/*` and `sync/*`. **Never echo an ini wholesale — key names only, never values.** Scope kills to PIDs you launched; zero residue, no stray PNGs.

**Build command (Git Bash):**

```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH"
cmake --build build --config Release --target <targets>
```

## A correction to the spec, discovered while writing this plan

The spec calls the migration a **one-time** remap. **It must be repeatable and idempotent instead**, for a reason that only surfaces from the storage layer:

`ItemMarks` and `ConsumptionStats` store records under **`md5(itemKey)`** (`ItemMarks.cpp:46`,
`ConsumptionStats.cpp:96`), not under the literal id. So the remap cannot enumerate "every `steam:` key
in storage" — storage holds only hashes. It must derive candidate old ids from the **current library**
(the four launcher lists plus `PcGameStore`), hash each one, and look for that record.

The consequence: **a game the user does not currently have installed cannot be migrated**, because its
old id cannot be reconstructed to hash. A one-shot migration would strand those records forever.

Making the remap **repeatable** — run on every library refresh, idempotent, never destructive — solves
it: when a game is reinstalled and reappears in a launcher list, the remap picks it up then. Task 3
builds it that way.

---

## File Structure

**Created:**

| File | Responsibility |
|---|---|
| `native/src/core/PcGameId.h` | `normalizeTitle`, `sameGame`, `mergeKey`, the override store, and `PcGameSource` + `pickAutoSource`. |
| `native/src/core/PcGameId.cpp` | Implementation. QtCore only, so the probe links lean. |
| `native/src/core/PcGameRemap.h` | The pure remap table: old per-launcher ids → merged id. |
| `native/src/core/PcGameRemap.cpp` | Implementation plus the ini-applying pass. |
| `native/tools/probe_pcgames.cpp` | Headless probe. Sentinel `PCGAMES-OK`. |

**Modified:**

| File | Change |
|---|---|
| `native/CMakeLists.txt` | `probe_pcgames`; `PcGameId.*` and `PcGameRemap.*` into the app target and `probe_browse`. |
| `native/tools/run-headless-probes.sh` | `probe_pcgames PCGAMES-OK`. |
| `.github/workflows/ci.yml` | `probe_pcgames` in the `--target` list. |
| `native/src/browse/SyntheticCatalogs.{h,cpp}` | Add `pcGamesCatalog`; retire the four per-launcher builders. |
| `native/src/ui/HomeView.{h,cpp}` | One PC Games folder replaces four; the source picker. |
| `native/src/ui/MainWindow.cpp` | Launch routing by source kind. |

---

### Task 1: PcGameId + pickAutoSource, with probe_pcgames

**Files:**
- Create: `native/src/core/PcGameId.h`, `native/src/core/PcGameId.cpp`, `native/tools/probe_pcgames.cpp`
- Modify: `native/CMakeLists.txt`, `native/tools/run-headless-probes.sh`, `.github/workflows/ci.yml`

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces, for Tasks 2–4:
  - `QString pcgame::normalizeTitle(const QString& raw);`
  - `QString pcgame::mergeKey(const QString& title, const QString& igdbId);` — the grouping key
  - `bool pcgame::sameGame(const QString& titleA, const QString& igdbA, const QString& titleB, const QString& igdbB);`
  - `struct PcGameSource { enum Kind { LauncherInstalled, LauncherOwned, Downloaded, AddonAvailable }; Kind kind; QString launcher, launchId, exePath, launchUrl, addonItemId, label; bool ready; };`
  - `int pcgame::pickAutoSource(const QVector<PcGameSource>& all);` — index, or `-1` meaning ask
  - `bool pcgame::overrideSaysSame(const QString& a, const QString& b);` / `void pcgame::setOverride(const QString& a, const QString& b, bool same);`

- [ ] **Step 1: Write the probe first (RED)**

Create `native/tools/probe_pcgames.cpp`. The table below is the contract; write it verbatim.

```cpp
// Headless check of PC-game identity and source selection. The PC folders were per-launcher; this is
// the unit that lets ONE entry carry Steam, GOG, Epic, Battle.net and a downloaded copy as sources.
//
// Two properties matter more than everything else here and are pinned hardest:
//   * normalizeTitle must NOT strip sequel numerals. Merging "Hades" with "Hades II" LOSES a game from
//     the user's library; failing to merge two editions merely shows it twice. The asymmetry is the
//     whole reason this is a probe and not a hand-wave.
//   * pickAutoSource must never return a NOT-ready source. Play is one keypress; silently starting a
//     multi-gigabyte download from it is the failure this function exists to prevent.
//
// Prints PCGAMES-OK on success; any failure prints PCGAMES-FAIL <cond> and exits non-zero.
#include "PcGameId.h"

#include <QCoreApplication>
#include <QString>
#include <QVector>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "PCGAMES-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

using namespace pcgame;

static PcGameSource src(PcGameSource::Kind k, const QString& launcher, bool ready)
{
    PcGameSource s; s.kind = k; s.launcher = launcher; s.ready = ready; return s;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ---- 1. normalizeTitle: noise that SHOULD collapse -----------------------------------------
    CHECK(normalizeTitle(QStringLiteral("Hades")) == normalizeTitle(QStringLiteral("HADES")));
    CHECK(normalizeTitle(QStringLiteral("Prey")) == normalizeTitle(QStringLiteral("Prey (2017)")));
    CHECK(normalizeTitle(QStringLiteral("Tomb Raider"))
          == normalizeTitle(QStringLiteral("Tomb Raider: Game of the Year Edition")));
    CHECK(normalizeTitle(QStringLiteral("Dishonored"))
          == normalizeTitle(QStringLiteral("Dishonored - Definitive Edition")));
    CHECK(normalizeTitle(QStringLiteral("BioShock"))
          == normalizeTitle(QStringLiteral("BioShock™ Remastered")));
    CHECK(normalizeTitle(QStringLiteral("Deus Ex"))
          == normalizeTitle(QStringLiteral("Deus Ex:  Director's Cut ")));

    // ---- 2. normalizeTitle: SEQUELS MUST NOT COLLAPSE ------------------------------------------
    // Each pair below is a DIFFERENT game. A merge here removes one of them from the library.
    CHECK(normalizeTitle(QStringLiteral("Hades")) != normalizeTitle(QStringLiteral("Hades II")));
    CHECK(normalizeTitle(QStringLiteral("Portal")) != normalizeTitle(QStringLiteral("Portal 2")));
    CHECK(normalizeTitle(QStringLiteral("Diablo II")) != normalizeTitle(QStringLiteral("Diablo III")));
    CHECK(normalizeTitle(QStringLiteral("Fallout 3")) != normalizeTitle(QStringLiteral("Fallout 4")));
    CHECK(normalizeTitle(QStringLiteral("The Witcher 2")) != normalizeTitle(QStringLiteral("The Witcher 3")));
    CHECK(normalizeTitle(QStringLiteral("Civilization V")) != normalizeTitle(QStringLiteral("Civilization VI")));
    // A numeral is NOT edition noise even next to edition noise.
    CHECK(normalizeTitle(QStringLiteral("Diablo II: Resurrected"))
          != normalizeTitle(QStringLiteral("Diablo III: Reaper of Souls")));
    // ...but the SAME sequel across two editions still collapses.
    CHECK(normalizeTitle(QStringLiteral("Portal 2"))
          == normalizeTitle(QStringLiteral("Portal 2 - Game of the Year Edition")));

    // ---- 3. degenerate input ------------------------------------------------------------------
    CHECK(normalizeTitle(QString()).isEmpty());
    CHECK(normalizeTitle(QStringLiteral("   ")).isEmpty());
    CHECK(normalizeTitle(QStringLiteral("!!!")).isEmpty());

    // ---- 4. sameGame: IGDB wins when BOTH sides have one ---------------------------------------
    // Same igdb id, wildly different titles -> same game (a regional rename).
    CHECK(sameGame(QStringLiteral("Rockman"), QStringLiteral("igdb:100"),
                   QStringLiteral("Mega Man"), QStringLiteral("igdb:100")) == true);
    // DIFFERENT igdb ids -> NOT the same game, even when the titles normalise identically. This is the
    // case where trusting the title would merge two genuinely distinct releases.
    CHECK(sameGame(QStringLiteral("Prey"), QStringLiteral("igdb:1"),
                   QStringLiteral("Prey"), QStringLiteral("igdb:2")) == false);
    // Only ONE side has an id -> fall back to the title, do not treat the missing id as a mismatch.
    CHECK(sameGame(QStringLiteral("Hades"), QStringLiteral("igdb:7"),
                   QStringLiteral("Hades"), QString()) == true);
    CHECK(sameGame(QStringLiteral("Hades"), QString(),
                   QStringLiteral("Hades II"), QString()) == false);

    // ---- 5. the override store beats BOTH heuristics -------------------------------------------
    {
        setOverride(QStringLiteral("hades"), QStringLiteral("hades ii"), true);
        CHECK(overrideSaysSame(QStringLiteral("hades"), QStringLiteral("hades ii")) == true);
        // symmetric — the user said these two are the same, in either order
        CHECK(overrideSaysSame(QStringLiteral("hades ii"), QStringLiteral("hades")) == true);
        setOverride(QStringLiteral("hades"), QStringLiteral("hades ii"), false);
        CHECK(overrideSaysSame(QStringLiteral("hades"), QStringLiteral("hades ii")) == false);
    }

    // ---- 6. pickAutoSource: NEVER returns a not-ready source ------------------------------------
    {
        // exactly one ready -> launch it
        QVector<PcGameSource> one{ src(PcGameSource::LauncherOwned, QStringLiteral("steam"), false),
                                   src(PcGameSource::Downloaded, QString(), true),
                                   src(PcGameSource::AddonAvailable, QString(), false) };
        CHECK(pickAutoSource(one) == 1);
        CHECK(one.value(pickAutoSource(one)).ready == true);

        // several ready -> ask
        QVector<PcGameSource> many{ src(PcGameSource::LauncherInstalled, QStringLiteral("steam"), true),
                                    src(PcGameSource::Downloaded, QString(), true) };
        CHECK(pickAutoSource(many) == -1);

        // none ready -> ask, NEVER auto-start a download
        QVector<PcGameSource> none{ src(PcGameSource::LauncherOwned, QStringLiteral("steam"), false),
                                    src(PcGameSource::AddonAvailable, QString(), false) };
        CHECK(pickAutoSource(none) == -1);

        // empty -> ask (and must not index out of range)
        CHECK(pickAutoSource({}) == -1);
    }

    if (failures == 0) { std::puts("PCGAMES-OK"); return 0; }
    std::fprintf(stderr, "PCGAMES: %d check(s) failed\n", failures);
    return 1;
}
```

- [ ] **Step 2: Create the header in the same step it enters CMake**

Create `native/src/core/PcGameId.h`:

```cpp
// PC-game identity and source selection. The PC library used to be one folder per launcher (Steam,
// GOG, Epic, Battle.net) plus a folder of downloaded games, so the same game appeared several times
// with unrelated ids. This unit is what lets ONE entry carry all of them as sources — the same shape
// the video stream picker already uses for one movie with many streams.
//
// Pure and QtCore-only (the override store is a small ini map), so probe_pcgames links lean.
#pragma once
#include <QString>
#include <QVector>

namespace pcgame
{
    // The matching key. Lowercases, collapses whitespace, strips punctuation, trademark symbols and
    // edition noise (Game of the Year / Definitive / Remastered / Director's Cut / a trailing year).
    //
    // It deliberately does NOT strip SEQUEL NUMERALS, Arabic or Roman. "Hades" and "Hades II" are
    // different games: merging them removes one from the user's library, while failing to merge two
    // editions only shows a game twice. That asymmetry is why the numeral rule is a hard requirement
    // and not a nicety.
    QString normalizeTitle(const QString& raw);

    // The grouping key used by the catalog builder: the igdb id when there is one, else the
    // normalised title. Two entries group together iff their mergeKey matches.
    QString mergeKey(const QString& title, const QString& igdbId);

    // Are these the same game? A user override wins; then, when BOTH sides carry an igdb id, that
    // decides (equal ids -> same, different ids -> NOT same, even if the titles agree); otherwise the
    // normalised titles decide. A missing id on one side is not a mismatch — it just means fall back.
    bool sameGame(const QString& titleA, const QString& igdbA,
                  const QString& titleB, const QString& igdbB);

    // The user's manual "these are/aren't the same" verdicts, keyed on the pair of normalised titles
    // and symmetric in the pair. This is the escape hatch that makes a fuzzy heuristic shippable: a
    // wrong merge is otherwise uncurable.
    bool overrideSaysSame(const QString& normA, const QString& normB);
    void setOverride(const QString& normA, const QString& normB, bool same);

    // One way to launch a game.
    struct PcGameSource
    {
        enum Kind { LauncherInstalled, LauncherOwned, Downloaded, AddonAvailable };
        Kind    kind = LauncherInstalled;
        QString launcher;     // "steam" | "epic" | "gog" | "battlenet"; empty for an addon source
        QString launchId;     // appid / appName / gog id / battle.net code
        QString exePath;      // when the launch is a direct exe
        QString launchUrl;    // when the launch is a protocol URL
        QString addonItemId;  // Downloaded / AddonAvailable
        QString label;        // the picker row
        bool    ready = false; // launches NOW, with no download
    };

    // Which source should Play use? Returns an index, or -1 meaning "ask the user".
    //
    // Exactly one ready source -> that one. Several ready, or none ready -> ask. It must NEVER return
    // a not-ready source: Play is a single keypress, and silently starting a multi-gigabyte download
    // from it is precisely what this function exists to prevent.
    int pickAutoSource(const QVector<PcGameSource>& all);
}
```

- [ ] **Step 3: Create the implementation**

Create `native/src/core/PcGameId.cpp`. Key requirements the code must satisfy — the probe is the
contract, so implement against it:

- `normalizeTitle` strips, in order: trademark/registered symbols; a trailing parenthesised 4-digit
  year; the edition phrases (`game of the year edition`, `goty`, `definitive edition`, `remastered`,
  `director's cut`, `complete edition`, `enhanced edition`); then all remaining punctuation; then
  collapses whitespace and lowercases.
- **Numerals survive.** Do the edition-phrase strip with explicit phrase matching, never a generic
  "drop trailing tokens" rule — the latter is exactly what eats `2` and `III`.
- `mergeKey` returns the igdb id when non-empty, else `normalizeTitle(title)`.
- `sameGame` order: override → both-ids → titles.
- The override store is an ini map under `pcgames/alias/`; key the pair canonically (sort the two
  normalised titles) so it is symmetric by construction rather than by a second lookup.
- `pickAutoSource` counts ready sources; returns the single ready index, else `-1`.

- [ ] **Step 4: Register the probe target in CMake**

In `native/CMakeLists.txt`, after the `probe_trakt` block, add:

```cmake
    # Headless test for PC-game identity + source selection (the merged PC Games folder). PcGameId.cpp
    # is QtCore-only, so this links against Qt6::Core alone.
    add_executable(probe_pcgames tools/probe_pcgames.cpp
        src/core/PcGameId.cpp src/core/PcGameId.h)
    target_include_directories(probe_pcgames PRIVATE src src/core)
    target_link_libraries(probe_pcgames PRIVATE Qt6::Core)
```

Also add `src/core/PcGameId.cpp` / `.h` to the main app target's source list.

- [ ] **Step 5: Register in the suite script and CI**

Add `"probe_pcgames PCGAMES-OK"` to the `for p in ...` list in `native/tools/run-headless-probes.sh`
(~line 130), and `probe_pcgames` to the `--target` list in `.github/workflows/ci.yml` (~line 52).

- [ ] **Step 6: Configure, build, run**

```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH"
cmake -S native -B build -DEVERYTHINGBOX_BUILD_APP=ON \
  -DCMAKE_PREFIX_PATH="C:/Qt/6.8.3/msvc2022_64" \
  -DMPV_INCLUDE_DIR="C:/mpv-dev/include" -DMPV_LIBRARY="C:/mpv-dev/libmpv.lib" \
  -DSDL2_INCLUDE_DIR="C:/SDL2/include" -DSDL2_LIBRARY="C:/SDL2/lib/x64/SDL2.lib"
cmake --build build --config Release --target probe_pcgames
./build/Release/probe_pcgames.exe
```

Expected: `PCGAMES-OK`, exit 0.

- [ ] **Step 7: Mutation-test every assertion group**

Apply each, rebuild, run, confirm FAILURE at the named check, revert, confirm `PCGAMES-OK`. **Record
the observed failing line for each.**

| # | Mutation in `PcGameId.cpp` | Must fail |
|---|---|---|
| 1 | `normalizeTitle`: also strip a trailing numeric/Roman token | every §2 sequel check |
| 2 | `normalizeTitle`: skip the edition-phrase strip | the §1 GOTY / Definitive / Remastered checks |
| 3 | `normalizeTitle`: skip the year strip | the §1 `Prey (2017)` check |
| 4 | `sameGame`: treat two different igdb ids as "fall back to titles" | the §4 `igdb:1` vs `igdb:2` check |
| 5 | `sameGame`: require BOTH ids present or return false | the §4 one-sided-id check |
| 6 | `sameGame`: consult the override last instead of first | the §5 override checks |
| 7 | `overrideSaysSame`: key the pair unsorted | the §5 symmetry check |
| 8 | `pickAutoSource`: return the first source regardless of `ready` | the §6 one-ready and none-ready checks |
| 9 | `pickAutoSource`: return the first ready even when several are ready | the §6 several-ready check |

If any mutation does NOT fail, that assertion is not real coverage — strengthen it before proceeding.

- [ ] **Step 8: Full suite, then commit**

```bash
bash native/tools/run-headless-probes.sh
git add native/src/core/PcGameId.h native/src/core/PcGameId.cpp native/tools/probe_pcgames.cpp \
        native/CMakeLists.txt native/tools/run-headless-probes.sh .github/workflows/ci.yml
git commit -m "feat: PcGameId — PC-game identity and source auto-pick"
```

Expected: `ALL HEADLESS PROBES PASSED`.

---

### Task 2: The merged catalog builder

**Files:**
- Modify: `native/src/browse/SyntheticCatalogs.{h,cpp}`, `native/tools/probe_browse.cpp`, `native/CMakeLists.txt`

**Interfaces:**
- Consumes: everything from Task 1.
- Produces, for Task 4:
  - `MediaCatalog browse::pcGamesCatalog(const QList<SteamGame>& steam, const QList<EpicGame>& epic, const QList<GogGame>& gog, const QList<BattleNetGame>& bnet, const QVector<pcgame::PcGameSource>& downloaded, const QString& query, const QString& launcherFilter);`

- [ ] **Step 1: Add the builder**

Declare it in `native/src/browse/SyntheticCatalogs.h` inside `namespace browse`, following the shape
of the existing builders (plain lists in, `MediaCatalog` out, no UI or store-singleton dependency).

Per game it emits ONE `MediaItem`:
- `it.id` = `"pcgame:" + pcgame::mergeKey(title, igdbId)` — the merged identity, and what every
  per-item store will key on after Task 3.
- `it.mime` = `"pcgame"` — the single routing kind, replacing `steamgame`/`epicgame`/`goggame`/
  `battlenetgame`.
- `it.title` = the best display title among the sources (prefer a launcher's own name over a
  file-provider release name, which carries scene tokens).
- `it.url` left empty — the source picker resolves the launch at activation.

`launcherFilter` narrows to one launcher when non-empty, so "only what I own on Steam" survives
without a separate folder. `query` filters on the normalised title.

**Battle.net titles with an empty `code`** get a source whose `label` marks it as a best-effort exe
launch. Do not present it at parity with a protocol launch.

Sort the emitted sources deterministically — ready before not-ready, then by launcher name — so the
picker's row order is stable between runs and the probe can assert it.

- [ ] **Step 2: Add probe coverage to probe_browse**

`probe_browse` already links `SyntheticCatalogs.cpp`; add `src/core/PcGameId.cpp` to that target in
`native/CMakeLists.txt` (the builder now calls into it). Then assert:

- one game present in Steam AND GOG AND downloaded yields exactly **one** item with **three** sources;
- a game present in only one launcher yields exactly one item with one source;
- `Hades` and `Hades II` across two launchers yield **two** items, not one — the regression that would
  lose a game;
- `launcherFilter = "steam"` returns only games with a Steam source;
- source order is ready-first and stable;
- an empty input yields an empty catalog with a valid title.

Mutation-test at least: dropping the grouping (one item per source), and sorting not-ready first.

- [ ] **Step 3: Build, run, commit**

```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH"
cmake --build build --config Release --target probe_browse probe_pcgames
./build/Release/probe_browse.exe
bash native/tools/run-headless-probes.sh
git add native/src/browse native/tools/probe_browse.cpp native/CMakeLists.txt
git commit -m "feat: pcGamesCatalog — one entry per game, launchers as sources"
```

Expected: `BROWSE-OK` and `ALL HEADLESS PROBES PASSED`.

---

### Task 3: The remap — repeatable, hash-aware, never destructive

**Files:**
- Create: `native/src/core/PcGameRemap.{h,cpp}`
- Modify: `native/tools/probe_pcgames.cpp`, `native/CMakeLists.txt`

**Interfaces:**
- Consumes: `pcgame::mergeKey` (Task 1).
- Produces, for Task 4:
  - `QHash<QString,QString> pcgame::remapTable(const QVector<QPair<QString,QString>>& oldIdToTitle, const QHash<QString,QString>& titleToIgdb);` — old id → merged id
  - `void pcgame::applyRemap(const QHash<QString,QString>& table);`

**Read this before writing code.** `ItemMarks` and `ConsumptionStats` store records under
**`md5(itemKey)`** (`ItemMarks.cpp:46`, `ConsumptionStats.cpp:96`), not the literal id. So the remap
**cannot enumerate stored keys** — storage holds only hashes. It must:

1. derive candidate old ids from the **current library** (the four launcher lists + `PcGameStore`),
2. compute `md5(oldId)` to locate each record,
3. rewrite it under `md5(newMergedId)`.

Therefore the remap is **repeatable, not one-shot**: a game the user does not currently have installed
cannot be reconstructed, so a one-shot pass would strand its records forever. Running it on every
library refresh means a reinstalled game gets migrated when it reappears. It must be idempotent.

- [ ] **Step 1: Add probe cases for the table (RED)**

Append to `native/tools/probe_pcgames.cpp`:

```cpp
    // ---- 7. the remap table: old per-launcher id -> merged id ----------------------------------
    {
        QVector<QPair<QString, QString>> lib;      // (old id, title)
        lib << qMakePair(QStringLiteral("steam:1145360"), QStringLiteral("Hades"))
            << qMakePair(QStringLiteral("gog:1207658930"), QStringLiteral("Hades"))
            << qMakePair(QStringLiteral("steam:2074920"), QStringLiteral("Hades II"));
        QHash<QString, QString> igdb;              // title -> igdb id (empty here: title fallback)
        const QHash<QString, QString> t = remapTable(lib, igdb);

        // Both Hades entries land on the SAME merged id...
        CHECK(t.value(QStringLiteral("steam:1145360")) == t.value(QStringLiteral("gog:1207658930")));
        CHECK(!t.value(QStringLiteral("steam:1145360")).isEmpty());
        // ...and Hades II lands on a DIFFERENT one. A table that merges these loses a game.
        CHECK(t.value(QStringLiteral("steam:2074920")) != t.value(QStringLiteral("steam:1145360")));

        // IDEMPOTENT: feeding the already-merged ids back yields ids that map to themselves.
        QVector<QPair<QString, QString>> again;
        again << qMakePair(t.value(QStringLiteral("steam:1145360")), QStringLiteral("Hades"));
        const QHash<QString, QString> t2 = remapTable(again, igdb);
        CHECK(t2.value(t.value(QStringLiteral("steam:1145360")))
              == t.value(QStringLiteral("steam:1145360")));

        // An id the table cannot map is ABSENT from the table — never mapped to empty, which a caller
        // would happily write as a key and thereby destroy the record.
        CHECK(!t.contains(QStringLiteral("steam:999999")));
        CHECK(t.value(QStringLiteral("steam:999999")).isEmpty());   // value() default, not an entry
    }
```

- [ ] **Step 2: Implement `PcGameRemap`**

`remapTable` maps each old id to `"pcgame:" + mergeKey(title, igdb.value(title))`. An entry with an
empty title is **omitted from the table entirely** — never mapped to an empty string.

`applyRemap` walks each per-item store and moves records:

| Store | Key shape | Hashed? |
|---|---|---|
| `ItemMarks` | `marks/<profile>/items/<md5(id)>` | **yes** |
| `ConsumptionStats` | `stats/<profile>/<device>/items/<md5(id)>` | **yes** |
| `PlayStats` | `playstats/<profile>/<device>/<id>/…` | check at implementation time |
| `FavoritesStore` | `favorites/<profile>` — a list holding `itemId` | no |
| resume | `resume/<hash>` | check at implementation time |

For each old id in the table: locate the record, and if one exists, write it under the new id and
remove the old. **If the destination already holds a record, merge rather than overwrite** — two
launcher entries collapsing into one game means two records collapsing into one, and silently
discarding one loses play time or a favourite.

**Never remove a record without having written its replacement.** Verify each write before the delete.

- [ ] **Step 3: Mutation-test the table, then commit**

| # | Mutation | Must fail |
|---|---|---|
| 10 | `remapTable`: include empty-title entries mapped to `""` | the §7 unmappable-id check |
| 11 | `remapTable`: group on the raw title instead of `mergeKey` | the §7 Hades-II separation check (raw titles already differ, so verify this one actually fails; if it does not, strengthen the case with two editions of the same game) |

```bash
git add native/src/core/PcGameRemap.h native/src/core/PcGameRemap.cpp \
        native/tools/probe_pcgames.cpp native/CMakeLists.txt
git commit -m "feat: repeatable, hash-aware remap from per-launcher ids to the merged id"
```

---

### Task 4: The folder and the source picker

**Files:**
- Modify: `native/src/ui/HomeView.{h,cpp}`, `native/src/ui/MainWindow.cpp`, `native/src/browse/SyntheticCatalogs.{h,cpp}`

- [ ] **Step 1: Replace the four folders with one**

`HomeView.cpp` builds the four PC folders around lines 1694/1733/1749/1765 (`steamGamesCatalog`,
`epicGamesCatalog`, `gogGamesCatalog`, `battleNetGamesCatalog`). Replace them with a single **PC
Games** folder rendered from `browse::pcGamesCatalog`, and delete the four builders and their
declarations once nothing references them.

Run the remap (Task 3) as part of the library refresh that populates this folder, so a reinstalled
game is migrated when it reappears.

- [ ] **Step 2: Wire the source picker**

On activating a `pcgame` item, build its sources, call `pcgame::pickAutoSource`:
- index ≥ 0 → launch that source directly;
- `-1` → show a `NavMenu` of the sources using the same row-budget idea as the stream picker's
  `describe`, then launch the chosen one.

Launch routing by `kind`: `launchUrl` when set (Steam/Epic/Battle.net protocol), else `exePath`
(GOG/Battle.net exe/downloaded). A `LauncherOwned` or `AddonAvailable` row, when chosen explicitly,
starts its download/install handoff — that is the user asking for it, which is different from Play
doing it silently.

- [ ] **Step 3: Build, suite, commit**

```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH"
cmake --build build --config Release --target everythingbox probe_browse probe_pcgames probe_nav probe_navqml
bash native/tools/run-headless-probes.sh
git add native/src/ui native/src/browse
git commit -m "feat: one PC Games folder with a source picker, replacing the four launcher folders"
```

---

### Task 5: Close-out

- [ ] **Step 1: Live verification on a throwaway**

Copy the deployed tree to scratch, strip `cloud/*` and `sync/*` from every ini (key-name-only output
when checking), and drive it under `EB_UITEST=1` with a unique `EB_UITEST_PIPE`. Confirm:

1. one PC Games folder, and the four per-launcher folders are gone;
2. a game present in two launchers appears **once**;
3. a game with exactly one ready source launches directly with no menu;
4. a game with several ready sources shows the picker;
5. a game with **no** ready source shows the picker and does **not** start a download;
6. **a favourite made before the change still opens after it** — the migration's whole point.

- [ ] **Step 2: Full suite, whole-branch review, merge**

```bash
cmake --build build --config Release --target everythingbox probe_pcgames probe_browse probe_nav probe_navqml
bash native/tools/run-headless-probes.sh
```

Generate the review package for `$(git merge-base main HEAD)..HEAD` and dispatch the final reviewer on
the most capable model, pointing it at the remap (data loss), the sequel-numeral rule, and whether any
launch path can be reached without passing `pickAutoSource`.

Then merge, push, and redeploy — **confirm the app is closed first** rather than killing the user's
session; the copy fails while it runs.

- [ ] **Step 3: Update the ledger**

Append one line per task to `.superpowers/sdd/progress.md`, plus a close-out line with the merge
commit, the deploy md5, and any follow-ups.

---

## Self-Review

**Spec coverage:** §1 identity → Task 1. §2 `PcGameSource` → Task 1. §3 `pickAutoSource` → Task 1
(§6 assertions). §4 merged folder → Task 2, surfaced in Task 4. §5 migration → Task 3, verified in
Task 5 Step 1.6. §6 testing → Tasks 1–3 probes plus Task 5. Battle.net empty-`code` labelling → Task 2
Step 1. Launcher filter → Task 2 Step 1. No gaps.

**Type consistency:** `normalizeTitle`, `mergeKey`, `sameGame`, `overrideSaysSame`, `setOverride`,
`PcGameSource`, `pickAutoSource`, `remapTable`, `applyRemap` are spelled identically everywhere they
appear.

**Deviation from the spec, flagged for the reviewer:** the spec says the migration is **one-time**;
this plan makes it **repeatable and idempotent**, because per-item records are stored under
`md5(itemKey)` and cannot be enumerated — a one-shot pass would permanently strand the records of any
game not installed at that moment. The reasoning is written at the top of this plan so a reviewer can
reject it if they disagree.

**Known judgement call:** Task 3's mutation #11 may not fail as written, because the raw titles in that
fixture already differ. The step says so and requires strengthening the case rather than accepting a
green mutation — an inert mutation is not coverage.
