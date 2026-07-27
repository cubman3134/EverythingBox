# Battle.net Importer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Detect installed Blizzard games from the Windows Uninstall registry and surface them as a "Battle.net" console in the Games catalogue, launching by `battlenet://<code>` (or the install-dir exe when the title has no known code), with Recents round-tripping — following the shipped Steam/Epic/GOG importer pattern.

**Architecture:** A new QtCore-only `BattleNetLibrary` namespace mirroring `GogLibrary` verbatim (free functions over a POD struct, an injectable INI fixture root as the probe seam) plus a pure `parseUninstallEntry` discriminator (the `EpicLibrary::parseManifest` seam). Eight UI touch-points cloned from the GOG blocks make it a synthetic console with its own mime + Recent kind. Detection is a live scan (no persistence — the game-importers precedent).

**Tech Stack:** Qt 6.8.3 (QSettings NativeFormat/IniFormat, QDir), the shipped `SyntheticCatalogs`/`RecentStore`/`launchPcExe` plumbing, headless probes.

## Global Constraints

- **Branch:** `local/battlenet-importer` off main. Standing autonomy through the merge gate. The pre-commit hook auto-bumps the patch version — expected; never hand-edit version lines.
- **Scope:** Battle.net only. NO EA/Ubisoft/Xbox importer, NO `product.db` protobuf parsing, NO owned-but-not-installed, NO play-time stats, NO gamelist.xml. Explicit non-goals — do not build them.
- **Detection = registry Uninstall scan** (`Publisher == "Blizzard Entertainment"`), NOT `product.db`. Live-scan only (no store/persistence).
- **Dormant when nothing detected:** with no Blizzard games installed (this machine's current state), `installedGames()` is empty → NO console injected → zero UI change. Every task must preserve this.
- **ANCHOR ON FUNCTION NAMES.** Current code (main@52eaa9a) — clone the GOG blocks:

| Concern | Anchor |
|---|---|
| Library template (registry + INI fixture seam) | `native/src/core/GogLibrary.{h,cpp}` — `winPathToSlash` `:11-15`, `readGame(regProbeRoot,id)` `:19-42` (INI branch `:22-28` / live `QSettings NativeFormat` `:31-39`), `gameIds(regProbeRoot)` `:45-59`, `isAvailable` `:63-66`, `installedGames` (skip-incomplete + sort-by-name) `:68-81` |
| Pure parse seam (empty key ⇒ filtered) | `EpicLibrary::parseManifest` (`EpicLibrary.h:41-44`, `.cpp:41-67`) |
| Catalog builder + icon kind | `browse::gogGamesCatalog` `SyntheticCatalogs.cpp:246-266` (`it.mime="goggame"` `:259`); `iconTypeForKind` `:10-17` |
| Console injection (Games root) | `HomeView.cpp:4408-4418` (`GogLibrary::isAvailable() && !installedGames().isEmpty()`; `gog.id`/`gog.mime = "gog:console"`) |
| Drill dispatch | `HomeView.cpp:2463` (`if (it.mime=="gog:console") { openGogConsole(it); return; }`) |
| Console open/populate | `HomeView.cpp:1663-1676` (`openGogConsole` / `populateGogGames` → `showSyntheticCatalog(browse::gogGamesCatalog(...))`) |
| Back-nav repopulate | `HomeView.cpp:3016` |
| In-console search + not-a-ROM guard | `HomeView.cpp:2932` (search scope) and `:2238` (store games aren't ROM files) |
| Recent kind | `RecentStore.h:12,17-19,39` (`enum class Relaunch{…GogGame…}`); `RecentStore.cpp:121-122` (`relaunchFor`) |
| Launch dispatch | `MainWindow.cpp:6616-6621` (goggame → `launchPcExe(url,id,title,thumb,"goggame")`); Epic URI branch (`openUrl` + `RecentStore::add`) just below it |
| Recent re-open | `MainWindow.cpp:4961-4964` + `relaunchGogGame` `:6125-6136` (registry-first exe re-resolve, then recorded path) |
| Probe | `native/tools/probe_importers.cpp` — GOG INI fixture `:282-311`, catalog assertions `:313-325`, `relaunchFor` table `:177-188`, `iconTypeForKind` `:191-193`; sentinel `IMPORTERS-OK` |
| CMake | app source list `native/CMakeLists.txt:~206`; `probe_importers` target `:~488-499` |

- **Env recipe:** PATH prepend `/c/Qt/6.8.3/msvc2022_64/bin` + `/c/mpv-dev`; build dir `build` (generated qt.conf, no `QT_PLUGIN_PATH`). **Harness runs RELEASE — build `--config Release`.** App target `everythingbox`. **Build hygiene:** build ONLY named targets (never target-less); adding a source file requires ONE `cmake -S native -B build -DEVERYTHINGBOX_BUILD_APP=ON` (no `-A`) regenerate; >5 min → report BLOCKED. Suite: `BUILD_DIR=build bash native/tools/run-headless-probes.sh`.

---

### Task 1: BattleNetLibrary + probe fixtures

**Files:**
- Create: `native/src/core/BattleNetLibrary.h`, `native/src/core/BattleNetLibrary.cpp`
- Modify: `native/tools/probe_importers.cpp` (fixture assertions), `native/CMakeLists.txt` (app source list + `probe_importers` sources)

**Interfaces (Produces):**
```cpp
struct BattleNetGame {
    QString code;        // battlenet:// launch code ("wow","d3",…); EMPTY when the title has no known code
    QString name;        // DisplayName
    QString installDir;  // InstallLocation (forward slashes)
    QString exe;         // best-effort game exe under installDir (launch fallback); may be empty
};
namespace BattleNetLibrary {
    bool                   isAvailable(const QString& regProbeRoot = QString());
    QVector<BattleNetGame> installedGames(const QString& regProbeRoot = QString());
    QString                launchUri(const QString& code);      // "battlenet://" + code
    QString                codeForTitle(const QString& displayName);  // curated map; "" when unknown
    BattleNetGame          parseUninstallEntry(const QString& displayName, const QString& publisher,
                                               const QString& installLocation);  // empty name ⇒ filtered
}
```

- [ ] **Step 1: RED — probe assertions.** In `native/tools/probe_importers.cpp`, after the GOG section (~`:325`), add (`#include "BattleNetLibrary.h"` at the top):
```cpp
    // ---- Battle.net: pure entry parse + INI-fixture registry scan --------------------------------
    {
        using BattleNetLibrary::parseUninstallEntry;
        // A Blizzard entry is kept, its code resolved from the title.
        const BattleNetGame wow = parseUninstallEntry(QStringLiteral("World of Warcraft"),
            QStringLiteral("Blizzard Entertainment"), QStringLiteral("C:\\Games\\World of Warcraft"));
        CHECK(wow.name == QStringLiteral("World of Warcraft"));
        CHECK(wow.code == QStringLiteral("wow"));
        CHECK(wow.installDir == QStringLiteral("C:/Games/World of Warcraft"));   // separators normalized
        // A non-Blizzard publisher is filtered (empty name ⇒ callers drop it).
        CHECK(parseUninstallEntry(QStringLiteral("Some App"), QStringLiteral("Acme Inc"),
                                  QStringLiteral("C:\\Acme")).name.isEmpty());
        // A Blizzard title with no known code still parses — code empty ⇒ exe-launch fallback.
        const BattleNetGame unk = parseUninstallEntry(QStringLiteral("Blizzard Arcade Collection"),
            QStringLiteral("Blizzard Entertainment"), QStringLiteral("C:\\Games\\Arcade"));
        CHECK(!unk.name.isEmpty());
        CHECK(unk.code.isEmpty());
        // Case/spacing-insensitive title→code.
        CHECK(BattleNetLibrary::codeForTitle(QStringLiteral("  diablo   III  ")) == QStringLiteral("d3"));
        CHECK(BattleNetLibrary::codeForTitle(QStringLiteral("Totally Not A Blizzard Game")).isEmpty());
        CHECK(BattleNetLibrary::launchUri(QStringLiteral("wow")) == QStringLiteral("battlenet://wow"));

        // Fake-registry INI: groups = Uninstall subkeys, keys mirror the registry value names.
        QTemporaryDir tmp; CHECK(tmp.isValid());
        const QString ini = tmp.path() + QStringLiteral("/bnet.ini");
        {
            QSettings s(ini, QSettings::IniFormat);
            s.setValue(QStringLiteral("WorldOfWarcraft/DisplayName"), QStringLiteral("World of Warcraft"));
            s.setValue(QStringLiteral("WorldOfWarcraft/Publisher"), QStringLiteral("Blizzard Entertainment"));
            s.setValue(QStringLiteral("WorldOfWarcraft/InstallLocation"), QStringLiteral("C:\\Games\\WoW"));
            s.setValue(QStringLiteral("Overwatch/DisplayName"), QStringLiteral("Overwatch"));
            s.setValue(QStringLiteral("Overwatch/Publisher"), QStringLiteral("Blizzard Entertainment"));
            s.setValue(QStringLiteral("Overwatch/InstallLocation"), QStringLiteral("C:\\Games\\OW"));
            s.setValue(QStringLiteral("Notepadpp/DisplayName"), QStringLiteral("Notepad++"));
            s.setValue(QStringLiteral("Notepadpp/Publisher"), QStringLiteral("Don Ho"));
            s.setValue(QStringLiteral("Notepadpp/InstallLocation"), QStringLiteral("C:\\npp"));
            s.sync();
        }
        const QVector<BattleNetGame> games = BattleNetLibrary::installedGames(ini);
        CHECK(games.size() == 2);                                    // the non-Blizzard entry is filtered
        CHECK(games[0].name == QStringLiteral("Overwatch"));         // sorted by name
        CHECK(games[1].name == QStringLiteral("World of Warcraft"));
        CHECK(games[1].code == QStringLiteral("wow"));
        CHECK(BattleNetLibrary::isAvailable(ini));
        CHECK(!BattleNetLibrary::isAvailable(tmp.path() + QStringLiteral("/missing.ini")));  // dormant
    }
```
Also extend the existing `relaunchFor`/`iconTypeForKind` tables (`:177-193`):
```cpp
    CHECK(RecentStore::relaunchFor(QStringLiteral("battlenetgame")) == RecentStore::Relaunch::BattleNetGame);
    CHECK(browse::iconTypeForKind(QStringLiteral("battlenetgame")) == QStringLiteral("game"));
```
(Match the probe's actual assertion style for those two tables — it may use a local alias like `RL::`.)

- [ ] **Step 2: Verify RED.** Add `BattleNetLibrary.cpp/.h` to the `probe_importers` target sources in `native/CMakeLists.txt` (`:~488-499`, beside `GogLibrary.cpp`) AND to the app source list (`:~206`), then regenerate + build:
```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH"
cmake -S native -B build -DEVERYTHINGBOX_BUILD_APP=ON
cmake --build build --target probe_importers --config Release --parallel
```
Expected: compile/link failure — `BattleNetLibrary.h` doesn't exist yet (create the two files empty/stubbed if CMake needs them to exist to configure, so the failure is an *unresolved symbol / missing declaration*, i.e. genuine RED).

- [ ] **Step 3: Write the header.** `native/src/core/BattleNetLibrary.h`:
```cpp
// Battle.net games installed by the Blizzard client. Detection reads the standard Windows Uninstall hive
// (Publisher == "Blizzard Entertainment") rather than Blizzard's undocumented binary product.db: the registry
// is stable, documented-by-convention, and — like GogLibrary — swappable for an INI fixture so the parse is
// probe-testable with no launcher installed. Launch prefers battlenet://<code> for titles in the curated
// code map, else the install-dir exe through the monitored launchPcExe path (the GOG mechanic).
#pragma once
#include <QString>
#include <QVector>

struct BattleNetGame
{
    QString code;        // battlenet:// launch code ("wow", "d3", …). EMPTY when the title has no known code.
    QString name;        // DisplayName
    QString installDir;  // InstallLocation, forward slashes
    QString exe;         // best-effort game exe under installDir (launch fallback); may be empty
};

namespace BattleNetLibrary
{
    // regProbeRoot empty => the live Uninstall hive; non-empty => a fake-registry INI (groups = uninstall
    // subkeys, keys = DisplayName/Publisher/InstallLocation), exactly the GogLibrary probe seam.
    bool                   isAvailable(const QString& regProbeRoot = QString());
    QVector<BattleNetGame> installedGames(const QString& regProbeRoot = QString());

    QString launchUri(const QString& code);              // "battlenet://" + code
    QString codeForTitle(const QString& displayName);    // curated title -> code; empty when unknown

    // Pure discriminator (the EpicLibrary::parseManifest seam): a non-Blizzard publisher yields an entry with
    // an EMPTY name, which callers drop. Exposed so the filter is probe-testable without any registry.
    BattleNetGame parseUninstallEntry(const QString& displayName, const QString& publisher,
                                      const QString& installLocation);
}
```

- [ ] **Step 4: Write the implementation.** `native/src/core/BattleNetLibrary.cpp`:
```cpp
#include "BattleNetLibrary.h"

#include <QSettings>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <algorithm>

namespace {

// Registry values are Windows-style paths regardless of the host OS (QDir::fromNativeSeparators is a no-op
// off-Windows), so convert backslashes explicitly — same helper shape as GogLibrary.
QString winPathToSlash(QString p)
{
    p.replace(QLatin1Char('\\'), QLatin1Char('/'));
    return p;
}

// Lowercased, punctuation-stripped, whitespace-collapsed title — the code-map lookup key.
QString titleKey(const QString& t)
{
    QString s = t.toLower();
    static const QRegularExpression nonAlnum(QStringLiteral("[^a-z0-9]+"));
    s.replace(nonAlnum, QStringLiteral(" "));
    return s.simplified();
}

// The curated Battle.net product codes. Keys are titleKey()-normalized DisplayName prefixes; a title that
// starts with a key takes its code (so "World of Warcraft Classic" resolves to wow). Unknown => empty code,
// which routes the launch to the install-dir exe instead of a battlenet:// URI.
const QVector<QPair<QString, QString>>& codeTable()
{
    static const QVector<QPair<QString, QString>> t = {
        { QStringLiteral("world of warcraft"),   QStringLiteral("wow")  },
        { QStringLiteral("diablo iv"),           QStringLiteral("osi")  },
        { QStringLiteral("diablo iii"),          QStringLiteral("d3")   },
        { QStringLiteral("diablo ii resurrected"), QStringLiteral("osi") },
        { QStringLiteral("overwatch"),           QStringLiteral("pro")  },
        { QStringLiteral("hearthstone"),         QStringLiteral("wtcg") },
        { QStringLiteral("starcraft ii"),        QStringLiteral("s2")   },
        { QStringLiteral("starcraft"),           QStringLiteral("s1")   },
        { QStringLiteral("warcraft iii"),        QStringLiteral("w3")   },
        { QStringLiteral("heroes of the storm"), QStringLiteral("hero") },
        { QStringLiteral("call of duty"),        QStringLiteral("cod")  },
    };
    return t;
}

// Best-effort: the largest top-level .exe under installDir that isn't an updater/uninstaller/launcher.
QString findGameExe(const QString& installDir)
{
    if (installDir.isEmpty()) return QString();
    QDir d(installDir);
    if (!d.exists()) return QString();
    QString best; qint64 bestSize = -1;
    for (const QFileInfo& fi : d.entryInfoList(QStringList{ QStringLiteral("*.exe") }, QDir::Files))
    {
        const QString n = fi.fileName().toLower();
        if (n.contains(QStringLiteral("uninstall")) || n.contains(QStringLiteral("unins"))
            || n.contains(QStringLiteral("launcher")) || n.contains(QStringLiteral("updater"))
            || n.contains(QStringLiteral("battle.net"))) continue;
        if (fi.size() > bestSize) { bestSize = fi.size(); best = fi.absoluteFilePath(); }
    }
    return best;
}

// One uninstall subkey's fields, from the fake-registry INI or the live hive (both 64-bit and WOW6432Node
// views — Blizzard's installers write to either depending on the title's bitness).
struct RawEntry { QString displayName, publisher, installLocation; };

RawEntry readEntry(const QString& regProbeRoot, const QString& hive, const QString& sub)
{
    RawEntry e;
    if (!regProbeRoot.isEmpty())
    {
        QSettings ini(regProbeRoot, QSettings::IniFormat);
        e.displayName     = ini.value(sub + QStringLiteral("/DisplayName")).toString();
        e.publisher       = ini.value(sub + QStringLiteral("/Publisher")).toString();
        e.installLocation = ini.value(sub + QStringLiteral("/InstallLocation")).toString();
        return e;
    }
#ifdef Q_OS_WIN
    QSettings reg(hive + QLatin1Char('\\') + sub, QSettings::NativeFormat);
    e.displayName     = reg.value(QStringLiteral("DisplayName")).toString();
    e.publisher       = reg.value(QStringLiteral("Publisher")).toString();
    e.installLocation = reg.value(QStringLiteral("InstallLocation")).toString();
#else
    Q_UNUSED(hive); Q_UNUSED(sub);
#endif
    return e;
}

// (hive, subkey) pairs to inspect: the INI's groups under a fixture, else both live Uninstall views.
QVector<QPair<QString, QString>> entryKeys(const QString& regProbeRoot)
{
    QVector<QPair<QString, QString>> out;
    if (!regProbeRoot.isEmpty())
    {
        QSettings ini(regProbeRoot, QSettings::IniFormat);
        for (const QString& g : ini.childGroups()) out.push_back({ QString(), g });
        return out;
    }
#ifdef Q_OS_WIN
    static const QStringList hives = {
        QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall"),
        QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall"),
    };
    for (const QString& h : hives)
    {
        QSettings reg(h, QSettings::NativeFormat);
        for (const QString& g : reg.childGroups()) out.push_back({ h, g });
    }
#endif
    return out;
}

} // namespace

QString BattleNetLibrary::codeForTitle(const QString& displayName)
{
    const QString k = titleKey(displayName);
    if (k.isEmpty()) return QString();
    for (const auto& p : codeTable())
        if (k.startsWith(p.first)) return p.second;
    return QString();
}

QString BattleNetLibrary::launchUri(const QString& code)
{
    return code.isEmpty() ? QString() : (QStringLiteral("battlenet://") + code);
}

BattleNetGame BattleNetLibrary::parseUninstallEntry(const QString& displayName, const QString& publisher,
                                                    const QString& installLocation)
{
    BattleNetGame g;
    if (!publisher.contains(QStringLiteral("Blizzard"), Qt::CaseInsensitive)) return g; // empty name ⇒ filtered
    if (displayName.trimmed().isEmpty()) return g;
    g.name = displayName.trimmed();
    g.code = codeForTitle(g.name);
    g.installDir = winPathToSlash(installLocation);
    return g;
}

QVector<BattleNetGame> BattleNetLibrary::installedGames(const QString& regProbeRoot)
{
    QVector<BattleNetGame> out;
    for (const auto& hk : entryKeys(regProbeRoot))
    {
        const RawEntry r = readEntry(regProbeRoot, hk.first, hk.second);
        BattleNetGame g = parseUninstallEntry(r.displayName, r.publisher, r.installLocation);
        if (g.name.isEmpty()) continue;                                   // not Blizzard / incomplete
        bool dup = false;
        for (const BattleNetGame& e : out) if (e.name.compare(g.name, Qt::CaseInsensitive) == 0) dup = true;
        if (dup) continue;                                                // same title in both hive views
        g.exe = findGameExe(g.installDir);
        out.push_back(g);
    }
    std::sort(out.begin(), out.end(), [](const BattleNetGame& a, const BattleNetGame& b) {
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });
    return out;
}

bool BattleNetLibrary::isAvailable(const QString& regProbeRoot)
{
    return !installedGames(regProbeRoot).isEmpty();
}
```

- [ ] **Step 5: Build GREEN.** `cmake --build build --target probe_importers --config Release --parallel` then `./build/Release/probe_importers.exe` → `IMPORTERS-OK`. (The `relaunchFor`/`iconTypeForKind` assertions added in Step 1 will still FAIL — those land in Task 2. If you prefer a fully-green Task 1, comment them with a `// (Task 2)` marker and uncomment there; state which you chose.)

- [ ] **Step 6: Commit.**
```bash
git add native/src/core/BattleNetLibrary.h native/src/core/BattleNetLibrary.cpp native/tools/probe_importers.cpp native/CMakeLists.txt
git commit -m "feat: BattleNetLibrary registry-Uninstall detection + probe fixtures (bnet T1)"
```

---

### Task 2: the UI touch-points — console, catalog, mime, Recent kind, launch

**Files:** Modify `native/src/browse/SyntheticCatalogs.h`/`.cpp`; `native/src/ui/HomeView.h`/`.cpp`; `native/src/core/RecentStore.h`/`.cpp`; `native/src/ui/MainWindow.h`/`.cpp`; `native/tools/probe_importers.cpp` (the catalog-builder assertions + un-comment the Task-1 markers if used).

**Interfaces:** Consumes T1's `BattleNetLibrary`. Produces `browse::battleNetGamesCatalog`, mime `battlenetgame`, console mime `battlenet:console`, `RecentStore::Relaunch::BattleNetGame`.

- [ ] **Step 1: Catalog builder + icon kind.** In `native/src/browse/SyntheticCatalogs.h`, beside the `gogGamesCatalog` declaration (~`:71-84`), add (and `#include "../core/BattleNetLibrary.h"` beside the GogLibrary include ~`:14-16`):
```cpp
    // Battle.net games as a synthetic console page. A game with a known code launches by battlenet:// URI
    // (no url on the tile); a code-less game carries its exe in `url` for the monitored launchPcExe path.
    MediaCatalog battleNetGamesCatalog(const QList<BattleNetGame>& installed, const QString& query,
                                       const QString& marker = QString());
```
(Match `gogGamesCatalog`'s exact parameter list/defaults at `SyntheticCatalogs.cpp:246` — mirror it.) In `SyntheticCatalogs.cpp`, add `"battlenetgame"` to `iconTypeForKind`'s game branch (`:15-17`) and implement after `gogGamesCatalog` (`:266`):
```cpp
MediaCatalog battleNetGamesCatalog(const QList<BattleNetGame>& installed, const QString& query,
                                   const QString& marker)
{
    Q_UNUSED(marker);
    MediaCatalog cat; cat.title = QObject::tr("Battle.net");
    for (const BattleNetGame& g : installed)
    {
        if (!query.isEmpty() && !g.name.contains(query, Qt::CaseInsensitive)) continue;
        MediaItem it;
        it.id = QStringLiteral("bnet:") + (g.code.isEmpty() ? g.name : g.code);
        it.title = g.name;
        it.type = QStringLiteral("game");
        it.mime = QStringLiteral("battlenetgame");
        it.systemHint = QStringLiteral("Battle.net");
        // A coded game launches by URI (no url); a code-less one rides its exe like a GOG game.
        if (g.code.isEmpty()) it.url = g.exe;
        it.thumbnailUrl = MetaCache::displayImage(it.id, QString());
        cat.items.push_back(it);
    }
    cat.hasMore = false;
    return cat;
}
```
(Mirror `gogGamesCatalog`'s query-filter + thumbnail idiom exactly as written there; adapt if it differs.)

- [ ] **Step 2: Recent kind.** In `native/src/core/RecentStore.h`, add `BattleNetGame` to the `Relaunch` enum (`:39`) and extend the kind comment (`:12`). In `RecentStore.cpp` `relaunchFor` (`:121-122`), add:
```cpp
    if (kind == QStringLiteral("battlenetgame")) return Relaunch::BattleNetGame;
```

- [ ] **Step 3: Console injection + drill + open/populate + back-nav + search.** In `native/src/ui/HomeView.cpp`:
  (a) after the GOG injection block (`:4408-4418`) add the same shape:
```cpp
            if (BattleNetLibrary::isAvailable() && !BattleNetLibrary::installedGames().isEmpty())
            {
                MediaItem bnet;
                bnet.id = QStringLiteral("battlenet:console");
                bnet.title = tr("Battle.net");
                bnet.type = QStringLiteral("platform");
                bnet.expandable = true;
                bnet.mime = QStringLiteral("battlenet:console");
                items_.push_back(bnet);
            }
```
  (mirror the GOG block's exact field set/order — copy it and swap the strings.)
  (b) drill dispatch beside `:2463`:
```cpp
    if (it.mime == QStringLiteral("battlenet:console")) { openBattleNetConsole(it); return; }
```
  (c) after `populateGogGames` (`:1672-1676`) add the pair (mirroring it verbatim):
```cpp
void HomeView::openBattleNetConsole(const MediaItem& consoleItem)
{
    // (copy openGogConsole's body verbatim, swapping the console item + populate call)
}

void HomeView::populateBattleNetGames()
{
    const QString query = /* same query source populateGogGames uses */ QString();
    showSyntheticCatalog(browse::battleNetGamesCatalog(BattleNetLibrary::installedGames(), query));
}
```
  (Read `openGogConsole`/`populateGogGames` at `:1663-1676` and clone them exactly — including how `query` is derived.)
  (d) back-nav beside `:3016`:
```cpp
    if (top.detail && top.item.mime == QStringLiteral("battlenet:console")) { populateBattleNetGames(); return; }
```
  (e) in-console search guard beside `:2932` and the not-a-ROM guard at `:2238` — add `|| …mime == "battlenet:console"` to both conditions.
  Declare `openBattleNetConsole(const MediaItem&)` + `populateBattleNetGames()` in `native/src/ui/HomeView.h` beside the GOG pair. Add `#include "../core/BattleNetLibrary.h"`.

- [ ] **Step 4: Launch + Recent re-open.** In `native/src/ui/MainWindow.cpp`, after the goggame branch (`:6616-6621`) add:
```cpp
    // A Battle.net game: a known product code launches the client by URI (fire-and-forget, like Epic/Steam);
    // a code-less title falls back to its install-dir exe through the monitored path (the GOG mechanic).
    if (item.mime == QStringLiteral("battlenetgame"))
    {
        if (item.url.isEmpty())   // coded game: id is "bnet:<code>"
        {
            const QString code = item.id.mid(QStringLiteral("bnet:").size());
            const QString uri = BattleNetLibrary::launchUri(code);
            if (!uri.isEmpty())
            {
                QDesktopServices::openUrl(QUrl(uri));
                RecentStore::add({ uri, item.title, QStringLiteral("battlenetgame"), item.thumbnailUrl, item.id });
            }
            return;
        }
        launchPcExe(item.url, item.id, item.title, item.thumbnailUrl, QStringLiteral("battlenetgame"));
        return;
    }
```
In `openRecent` beside the GOG case (`:4961-4964`) add a `BattleNetGame` case: if the recorded `path` starts with `battlenet://` → `QDesktopServices::openUrl` + re-record to front (mirror the Epic case at `:4934-4958`); else → `launchPcExe(path, resumeKey, title, thumb, "battlenetgame")` (mirror the GOG fallback). Add `#include "../core/BattleNetLibrary.h"`.

- [ ] **Step 5: Probe the builder + tables.** In `native/tools/probe_importers.cpp`, un-comment/add the `relaunchFor`/`iconTypeForKind` assertions from Task 1 Step 1 and add the catalog mapping:
```cpp
    {
        QVector<BattleNetGame> gs;
        BattleNetGame a; a.name = QStringLiteral("World of Warcraft"); a.code = QStringLiteral("wow");
        BattleNetGame b; b.name = QStringLiteral("Arcade"); b.exe = QStringLiteral("C:/Games/Arcade/arcade.exe");
        gs << a << b;
        const MediaCatalog c = browse::battleNetGamesCatalog(QList<BattleNetGame>(gs.begin(), gs.end()), QString());
        CHECK(c.items.size() == 2);
        const MediaItem& wow = c.items[0];
        CHECK(wow.id == QStringLiteral("bnet:wow"));
        CHECK(wow.mime == QStringLiteral("battlenetgame"));
        CHECK(wow.url.isEmpty());                                   // coded ⇒ URI launch, no url
        const MediaItem& arc = c.items[1];
        CHECK(arc.url == QStringLiteral("C:/Games/Arcade/arcade.exe")); // code-less ⇒ exe rides the tile
    }
```
(Adapt the container type to `battleNetGamesCatalog`'s actual parameter — `QList` vs `QVector` — matching `gogGamesCatalog`.)

- [ ] **Step 6: Build + suite.**
```bash
cmake --build build --target everythingbox probe_importers --config Release --parallel
BUILD_DIR=build bash native/tools/run-headless-probes.sh   # IMPORTERS-OK + ALL HEADLESS PROBES PASSED
```
App must compile clean. **Dormancy check:** with no Blizzard games installed (this machine), launching the app must show NO Battle.net console — confirm by code-read of the injection guard (and in the T3 live pass).

- [ ] **Step 7: Commit.**
```bash
git add native/src/browse/SyntheticCatalogs.h native/src/browse/SyntheticCatalogs.cpp native/src/ui/HomeView.h native/src/ui/HomeView.cpp native/src/core/RecentStore.h native/src/core/RecentStore.cpp native/src/ui/MainWindow.cpp native/tools/probe_importers.cpp
git commit -m "feat: Battle.net console + catalog + mime + Recent kind + launch (bnet T2)"
```

---

### Task 3: close-out — dormancy check, review, merge (live verify is USER-gated)

- [ ] **Step 1: Gates.** Full suite green (`IMPORTERS-OK` + `ALL HEADLESS PROBES PASSED`); app compiles Release. No perf run (detection is a Games-root live scan, off the render hot path — the Steam/Epic/GOG precedent); note that inline.
- [ ] **Step 2: Dormancy live check (no Blizzard game needed).** Portable throwaway (copy the deployed data dir, cloud-stripped; NEVER touch `C:\EverythingBox-app`), `EB_UITEST=1` + `native/tools/uitest.py`: open the Games catalogue root and confirm **NO Battle.net console appears** (nothing installed ⇒ dormant) and that Steam/Epic/GOG consoles are unchanged. Screenshot `bnet-dormant.png`. This is the verification that IS possible today.
- [ ] **Step 3: Review.** `scripts/review-package $(git merge-base main HEAD) HEAD`, most-capable model. Dimensions: the dormant contract (no games ⇒ no console, zero UI change); `parseUninstallEntry`'s publisher filter (non-Blizzard entries can never reach the console) and that an empty name is the only "filtered" signal; the INI fixture seam mirrors GogLibrary (probe-testable with no launcher); dedupe across the two live hive views; the coded-vs-code-less launch split (id `bnet:<code>` ⇒ URI + `RecentStore::add`; exe ⇒ `launchPcExe` records itself — no double-record); the Recent re-open round-trip for both; all six UI touch-points present and mirroring GOG (injection/drill/open/back/search/not-a-ROM); no Steam/Epic/GOG regression; `findGameExe`'s filters can't pick an uninstaller/launcher. Fix rounds → merge.
- [ ] **Step 4: Merge + push + redeploy.** Spec Status → complete, recording honestly: fixture-verified + dormancy-verified; the **live console/launch pass is USER-gated** (requires installing one Blizzard game via the already-installed Battle.net client) and will confirm/correct the real registry fields, the title→code entry, and the working launch mechanism. Merge `local/battlenet-importer` → main (version-line conflict → take the higher patch), rebuild the combined tree, full suite green (**build all probe targets** to catch a latent link break), push, delete the branch, redeploy Release to `C:\EverythingBox-app` (md5-verify), update `.superpowers/sdd/progress.md`, mark the chapter.

## Self-Review (done at write time)

- **Spec coverage:** `BattleNetLibrary` (struct + isAvailable/installedGames/launchUri/codeForTitle/parseUninstallEntry, INI fixture seam) ✅T1; registry-Uninstall detection w/ Blizzard publisher filter ✅T1; title→code map + exe fallback ✅T1; the six UI touch-points ✅T2 (builder+iconTypeForKind, injection, drill, open/populate, back-nav, search/not-a-ROM); Recent kind + launch + re-open ✅T2; probe fixtures ✅T1/T2; dormant contract ✅T1 guard + ✅T3 live check; user-gated live verify recorded ✅T3. Non-goals (EA/Ubisoft/Xbox, product.db, owned-not-installed, stats, gamelist.xml) not built ✅.
- **Placeholder scan:** every code step carries real code. Three steps deliberately say "clone the GOG block verbatim, swapping strings" (`openGogConsole`/`populateGogGames` body, `gogGamesCatalog`'s query/thumbnail idiom, the injection field order) with the exact anchor line to copy from — that is a precise instruction to read one cited function, not a TBD.
- **Type consistency:** `BattleNetGame{code,name,installDir,exe}`, `isAvailable/installedGames/launchUri/codeForTitle/parseUninstallEntry`, `battleNetGamesCatalog`, mimes `battlenetgame`/`battlenet:console`, id prefix `bnet:`, `Relaunch::BattleNetGame`, `openBattleNetConsole`/`populateBattleNetGames` — consistent across T1–T3 and with the probe assertions.
- **Ambiguity resolved:** coded games carry NO url (URI launch), code-less carry the exe (launchPcExe) — the id/url mapping and the launch branch agree; the publisher filter (not the code) decides membership, so an unknown Blizzard title still lists; Task 1's two cross-task probe assertions are explicitly flagged as Task-2-landing.
