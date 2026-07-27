# Local-Library Resolver Polish Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the same-title-different-year mis-match residual (via the year already in the search row's subtitle), make title matching diacritic-insensitive, and machine-pin the async show-dispatch seam where the TV-resolution C1 bug hid.

**Architecture:** Two pure `CatalogMatch` changes (NFKD-strip in `normalizeTitle`; subtitle-year disambiguation in `bestMatch`) covered by `probe_resolver`; plus a new suite-gated integration probe `probe_showdispatch` that stands up a REAL `AddonManager` + a JsLocal series-fixture addon (zero network) and drives a `CatalogResolver` show job through the typed dispatch to the cache — the exact seam a pure probe can't reach.

**Tech Stack:** Qt 6.8.3, the shipped resolver (`CatalogMatch`/`CatalogResolver`/`LocalResolveCache`), the Duktape JS engine + `AddonManager` (as `probe_addon` links them), headless probes.

## Global Constraints

- **Branch:** `local/resolver-polish` off main. Standing autonomy through the merge gate. The pre-commit hook auto-bumps the patch version — expected; never hand-edit version lines.
- **Scope:** exactly the two `CatalogMatch` changes + the new probe. No getMeta year path (deferred), no `AddonManager` interface extraction, no `bestSeriesMatch`/Seam A/B/cache-format/transport change. `bestMatch`'s change only NARROWS the title-match set (strictly safer) — never widens.
- **ANCHOR ON FUNCTION NAMES.** Current code (main@0b0cee4):

| Concern | Anchor |
|---|---|
| normalizeTitle | `CatalogMatch.cpp:6-15` (pure; `[^a-z0-9]+` currently turns accents into spaces) |
| bestMatch (movies) | `CatalogMatch.cpp:17-43`; the title-match loop `:28-41`, insert subtitle-year skip after the title check `:38` |
| probe_resolver normalizeTitle + Amélie(defanged) | `probe_resolver.cpp:30-33` (drop the `\|\| true`); the `mi(id,title,type)` helper builds a candidate |
| MediaItem.subtitle carries the year | aiocatalog `main.js:86` `subtitle: year(release_date\|\|first_air_date)`; `MediaItem::subtitle` (`AddonModels.h`) |
| probe_addon fixture/spin harness (model for probe_showdispatch) | `spin`/`spinUntil` `probe_addon.cpp:268-281`; `writeText` `:283`; `makeFixture` (manifest+main.js, movies+shows catalogs) `:308-338`; `qputenv("EB_ADDONS_ROOT", …)` + real `AddonManager` + `spinUntil` drive pattern (`probePrefetch`, ~:370-430) |
| Resolver show dispatch (what the probe pins) | `CatalogResolver::enqueue` (episode→show job via `LocalLibrary::showKeyFor`), `startJob` (show job → `requestCatalog(s, seriesCatalogId, showTitle, 1)` over series/tv/mixed catalogs), `finishJob` → `putShowMatched(showKey, ids)`; `LocalResolveCache::seriesIdsByShow()` |
| Settings gate | `Settings::resolveOnline()` default true (resolver enqueue gates on it) |

- **Env recipe:** PATH prepend `/c/Qt/6.8.3/msvc2022_64/bin` + `/c/mpv-dev`; build dir `build` (generated qt.conf, no `QT_PLUGIN_PATH`). **Harness runs RELEASE — build `--config Release`.** **Build hygiene:** build ONLY named targets (never target-less); when you ADD a source/probe, regenerate ONCE with `cmake -S native -B build -DEVERYTHINGBOX_BUILD_APP=ON` (no `-A`); if a build runs >5 min report BLOCKED. Suite: `BUILD_DIR=build bash native/tools/run-headless-probes.sh`.

---

### Task 1: diacritic-insensitive normalizeTitle + subtitle-year disambiguation (pure)

**Files:** Modify `native/src/core/CatalogMatch.cpp`; `native/tools/probe_resolver.cpp`.

**Interfaces:** No signature change — `normalizeTitle` and `bestMatch` keep their signatures; behavior narrows.

- [ ] **Step 1: RED — probe assertions.** In `native/tools/probe_resolver.cpp`, un-defang the diacritic case and add coverage. Replace the existing `Amélie` line (`:33`, the one ending `|| true;`) with:
```cpp
    CHECK(CatalogMatch::normalizeTitle(QStringLiteral("Amélie")) == QStringLiteral("amelie"));
    CHECK(CatalogMatch::normalizeTitle(QStringLiteral("Pokémon")) == QStringLiteral("pokemon"));
    CHECK(CatalogMatch::normalizeTitle(QStringLiteral("WALL·E")) == QStringLiteral("wall e")); // · is punct → space
```
Then add subtitle-year assertions. The `mi(id,title,type)` helper builds a candidate with an empty subtitle; add a subtitle-carrying helper near it:
```cpp
static MediaItem miY(const QString& id, const QString& title, const QString& subtitle)
{ MediaItem it; it.id = id; it.title = title; it.type = QStringLiteral("movie"); it.subtitle = subtitle; return it; }
```
And the table:
```cpp
    // subtitle-year disambiguation (movies): the year is in the aiocatalog search row's subtitle.
    {
        // Local Solaris (2002); catalog offers only the 1972 film → year disagrees → no match.
        QVector<MediaItem> c{ miY("tmdb:movie:1","Solaris","1972") };
        CHECK(CatalogMatch::bestMatch(movie("Solaris", 2002), c) == -1);
        // Both films present → the wrong year is skipped, the right one is the unique hit.
        QVector<MediaItem> c2{ miY("tmdb:movie:1","Solaris","1972"), miY("tmdb:movie:2","Solaris","2002") };
        CHECK(CatalogMatch::bestMatch(movie("Solaris", 2002), c2) == 1);
        // ±1 tolerance accepted.
        QVector<MediaItem> c3{ miY("tmdb:movie:9","Some Film","2001") };
        CHECK(CatalogMatch::bestMatch(movie("Some Film", 2002), c3) == 0);
        // No subtitle year on the candidate → falls back to title match (unchanged behavior).
        QVector<MediaItem> c4{ mi("tmdb:movie:3","Inception","movie") };
        CHECK(CatalogMatch::bestMatch(movie("Inception", 2010), c4) == 0);
        // Local year unknown (0) → year check inert, title match as before.
        QVector<MediaItem> c5{ miY("tmdb:movie:1","Solaris","1972") };
        CHECK(CatalogMatch::bestMatch(movie("Solaris", 0), c5) == 0);
    }
```
(Use the probe's existing `movie(title, year[, imdb])` + `mi(...)` helpers. If `movie(...)` doesn't already exist by that name, use whatever the probe uses to build a `VideoEntry` with a title+year.) Build `probe_resolver` → FAIL (Amélie assertion + the year skips).

- [ ] **Step 2: Implement diacritics.** In `native/src/core/CatalogMatch.cpp`, replace `normalizeTitle` with:
```cpp
QString normalizeTitle(const QString& t)
{
    // Fold diacritics: NFKD decomposes "é" → "e" + combining accent; drop the combining marks so the base
    // ASCII letter survives the [a-z0-9] filter ("Amélie" → "amelie", "Pokémon" → "pokemon").
    QString d = t.normalized(QString::NormalizationForm_KD).toLower();
    QString s; s.reserve(d.size());
    for (const QChar& ch : d)
        if (ch.category() != QChar::Mark_NonSpacing) s += ch;
    static const QRegularExpression nonAlnum(QStringLiteral("[^a-z0-9]+"));
    s.replace(nonAlnum, QStringLiteral(" "));
    s = s.simplified();
    for (const QString& art : { QStringLiteral("the "), QStringLiteral("a "), QStringLiteral("an ") })
        if (s.startsWith(art)) { s = s.mid(art.size()); break; }
    return s;
}
```

- [ ] **Step 3: Implement subtitle-year.** Add a file-static year parser above `bestMatch`:
```cpp
static int yearFromSubtitle(const QString& s)
{
    static const QRegularExpression re(QStringLiteral("\\b(19|20)\\d{2}\\b"));
    const QRegularExpressionMatch m = re.match(s);
    return m.hasMatch() ? m.captured(0).toInt() : 0;
}
```
In `bestMatch`, in the title-match loop, AFTER the `if (normalizeTitle(c.title) != wt) continue;` line (`:38`) and BEFORE the ambiguity `if (hit != -1)` check, insert:
```cpp
        // Subtitle-year disambiguation: when we know the local year and the candidate advertises a year
        // (aiocatalog puts it in the search row's subtitle), they must agree within ±1 — else this is a
        // same-title film of a different year (e.g. Solaris 1972 vs 2002). Skip it. Candidates with no
        // parseable subtitle year are left as title matches (unchanged fallback).
        if (want.year > 0)
        {
            const int cy = yearFromSubtitle(c.subtitle);
            if (cy > 0 && qAbs(cy - want.year) > 1) continue;
        }
```
Ensure `#include <QtGlobal>` (for `qAbs`) — `<QRegularExpression>` is already included.

- [ ] **Step 4: Build GREEN + suite.**
```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH"
cmake --build build --target probe_resolver --config Release --parallel
BUILD_DIR=build bash native/tools/run-headless-probes.sh   # RESOLVER-OK + ALL HEADLESS PROBES PASSED
```

- [ ] **Step 5: Commit.**
```bash
git add native/src/core/CatalogMatch.cpp native/tools/probe_resolver.cpp
git commit -m "feat: diacritic-insensitive title match + subtitle-year disambiguation (resolver-polish T1)"
```

---

### Task 2: hermetic show-dispatch probe (real AddonManager + JsLocal series fixture)

**Files:** Create `native/tools/probe_showdispatch.cpp`; Modify `native/CMakeLists.txt` (new `add_executable(probe_showdispatch …)`, modeled on the `probe_addon` target's link set), `native/tools/run-headless-probes.sh` (add `"probe_showdispatch SHOWDISPATCH-OK"`), `.github/workflows/ci.yml` (append `probe_showdispatch` to the build list).

**Interfaces:** Consumes the shipped `CatalogResolver`/`LocalResolveCache`/`AddonManager`; asserts existing post-C1 behavior (a regression net).

- [ ] **Step 1: Find the `probe_addon` CMake target** in `native/CMakeLists.txt` (its `add_executable(probe_addon …)` + `target_link_libraries`). Note its full source list + linked Qt/Duktape/miniz libs — `probe_showdispatch` needs the SAME (real `AddonManager` + JS engine) PLUS the resolver/core sources.

- [ ] **Step 2: Write the probe.** Create `native/tools/probe_showdispatch.cpp`:
```cpp
// Hermetic integration probe for the CatalogResolver SHOW-dispatch seam (where the TV-resolution C1 bug hid:
// an untyped search that a pure probe can't catch). Stands up a REAL AddonManager pointed at a temp dir with a
// JsLocal series fixture (zero network), drives a CatalogResolver show job through the typed requestCatalog
// dispatch, and asserts the cache recorded the series tile id. Prints SHOWDISPATCH-OK on success.
#include "AddonManager.h"
#include "CatalogResolver.h"
#include "LocalResolveCache.h"
#include "LocalLibrary.h"

#include <QCoreApplication>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QElapsedTimer>
#include <QEventLoop>
#include <functional>
#include <cstdio>

static int failures = 0;
#define CHECK(c) do { if(!(c)){ std::fprintf(stderr,"SHOWDISPATCH-FAIL %s (line %d)\n", #c, __LINE__); ++failures; } } while(0)

static bool spinUntil(const std::function<bool()>& pred, int timeoutMs)
{
    QElapsedTimer t; t.start();
    while (!pred() && t.elapsed() < timeoutMs)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return pred();
}
static bool writeText(const QString& p, const QByteArray& b)
{ QFile f(p); if(!f.open(QIODevice::WriteOnly|QIODevice::Truncate)) return false; return f.write(b)==b.size(); }

// A JsLocal media-source whose "shows" (series) catalog returns a canned series row matching any query — the
// exact shape aiocatalog's TMDB /search/tv yields. `serieslike`: if false, returns MOVIE-typed rows instead
// (the negative control — a source that can't serve series).
static bool makeSeriesFixture(const QString& root, const QString& id, bool serieslike)
{
    const QString dir = root + "/" + id;
    if (!QDir().mkpath(dir)) return false;
    const QByteArray manifest =
        "{\n  \"id\": \"" + id.toUtf8() + "\",\n  \"name\": \"Series Fixture\",\n  \"version\": \"1.0.0\",\n"
        "  \"type\": \"media-source\",\n  \"entry\": \"main.js\",\n  \"permissions\": [],\n"
        "  \"catalogs\": [ { \"id\": \"shows\", \"name\": \"Shows\", \"type\": \"series\" } ]\n}\n";
    const QByteArray js = QByteArray(
        "function J(s){try{return JSON.parse(s);}catch(e){return null;}}\n"
        "function getCatalog(argJson){var a=J(argJson)||{};\n"
        "  var t=") + (serieslike ? "'series'" : "'movie'") + ";\n"
        "  var items=[{id:") + (serieslike ? "'tmdb:tv:1396'" : "'tmdb:movie:1'") + ",\n"
        "    title:'Breaking Bad', type:t, subtitle:'2008', thumbnailUrl:'', url:''}];\n"
        "  return JSON.stringify({title:'r', items:items, hasMore:false});\n"
        "}\n";
    return writeText(dir + "/manifest.json", manifest) && writeText(dir + "/main.js", js);
}

static LocalLibrary::VideoEntry ep(const QString& show, int s, int e, const QString& path)
{ LocalLibrary::VideoEntry v; v.kind = LocalLibrary::Kind::Episode; v.show = show; v.season = s; v.episode = e; v.path = path; return v; }

static bool runCase(bool serieslike, QStringList& outIds)
{
    QTemporaryDir root; QTemporaryDir data;
    makeSeriesFixture(root.path(), "fixture.series", serieslike);
    qputenv("EB_ADDONS_ROOT", root.path().toUtf8());
    AddonManager mgr;                                   // real manager, loads the JsLocal fixture, no network
    LocalResolveCache cache(data.path() + "/localresolve.json"); cache.load();
    CatalogResolver resolver(&mgr, &cache);
    resolver.enqueue({ ep("Breaking Bad", 1, 1, data.path() + "/BB.S01E01.mkv") });
    const QString sk = LocalLibrary::showKeyFor(ep("Breaking Bad", 1, 1, QString()));
    // In-memory cache is updated in finishJob (before the resolved() debounce); wait for either a matched
    // entry OR the nomatch path (isShowFresh true) so the negative case terminates too.
    spinUntil([&]{ return !cache.seriesIdsByShow().value(sk).isEmpty()
                        || cache.isShowFresh(sk, 4102444800LL /*far future*/); }, 8000);
    outIds = cache.seriesIdsByShow().value(sk);
    return true;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    QStringList pos; runCase(/*serieslike=*/true, pos);
    CHECK(pos.contains(QStringLiteral("tmdb:tv:1396")));   // typed dispatch → series result → matched + cached

    QStringList neg; runCase(/*serieslike=*/false, neg);
    CHECK(neg.isEmpty());                                  // movie-only source → no series match → not cached

    if (failures == 0) { std::puts("SHOWDISPATCH-OK"); return 0; }
    std::fprintf(stderr, "SHOWDISPATCH: %d check(s) failed\n", failures);
    return 1;
}
```
(Adapt field names to `LocalLibrary::VideoEntry`'s actual members and `LocalResolveCache`'s actual API if they differ from the above. The `4102444800` future-time makes `isShowFresh` true for a cached nomatch so the negative case's `spinUntil` doesn't burn the full timeout; if `isShowFresh`'s signature differs, use the real one. `Settings::resolveOnline()` defaults true, so `enqueue` proceeds; if the probe's data dir makes it read false, set it true via `Settings::setResolveOnline(true)` before enqueue.)

- [ ] **Step 3: CMake.** Add `probe_showdispatch` modeled on `probe_addon`'s block — SAME Qt/Duktape/miniz/AddonManager source+lib set, PLUS `src/core/CatalogResolver.cpp src/core/CatalogMatch.cpp src/core/LocalResolveCache.cpp src/core/LocalLibrary.cpp src/core/Settings.cpp src/theme2/FormFactor.cpp` (add sources until it links — mirror the app's resolver link set). `target_include_directories` `src src/core src/addons src/theme2`.

- [ ] **Step 4: Reconfigure + build + verify.**
```bash
cmake -S native -B build -DEVERYTHINGBOX_BUILD_APP=ON   # new target
cmake --build build --target probe_showdispatch --config Release --parallel
./build/Release/probe_showdispatch.exe    # expect SHOWDISPATCH-OK
```
If the positive case fails (no `tmdb:tv:1396`), the fixture's series catalog isn't being queried as a series search — verify the resolver's `startJob` iterates the `shows` (type series) catalog and that `requestCatalog` reaches the fixture's `getCatalog`; the probe is the exact reproduction, so a failure here is a real finding.

- [ ] **Step 5: Wire runner + CI, full suite.** Append `"probe_showdispatch SHOWDISPATCH-OK"` to the `run-headless-probes.sh` loop; append `probe_showdispatch` to `ci.yml`'s build-target list. Then `BUILD_DIR=build bash native/tools/run-headless-probes.sh` → `ALL HEADLESS PROBES PASSED`.

- [ ] **Step 6: Commit.**
```bash
git add native/tools/probe_showdispatch.cpp native/CMakeLists.txt native/tools/run-headless-probes.sh .github/workflows/ci.yml
git commit -m "test: hermetic show-dispatch probe pins the CatalogResolver typed-search seam (resolver-polish T2)"
```

---

### Task 3: close-out — gates + fable + merge

- [ ] **Step 1: Full suite green** (`RESOLVER-OK` with the new year/diacritic asserts + `SHOWDISPATCH-OK` + `ALL HEADLESS PROBES PASSED`). No perf run needed — `bestMatch` is off the render/hot path (resolve-time only) and the change is a narrower comparison; note that inline.
- [ ] **Step 2: Fable review.** `scripts/review-package $(git merge-base main HEAD) HEAD`, most-capable model. Dimensions: `normalizeTitle` NFKD-strip correctness (Amélie/Pokémon fold; `·`/punct still → space; ASCII unaffected; the article-strip + simplified still run after); `bestMatch` subtitle-year only NARROWS (a candidate with no subtitle year, or `want.year==0`, behaves exactly as before — no new accept path; the ±1 tolerance; disambiguation picks the right film when both present); `bestSeriesMatch` untouched; the hermetic probe genuinely reproduces the C1 seam (real AddonManager + JsLocal series fixture, positive asserts `tmdb:tv:1396` cached, negative asserts movie-only → not cached), is deterministic/no-network, and terminates (spinUntil bounded). Fix rounds → merge.
- [ ] **Step 3: Merge + push + redeploy.** Spec Status → complete. Merge `local/resolver-polish` → main (resolve any version-line conflict by taking the higher patch), rebuild the combined tree, full suite green (**build all probe targets incl. probe_browse/probe_perf/probe_resolver/probe_importers/probe_locallib/probe_showdispatch** to catch any latent link break), push, delete the branch, redeploy Release to `C:\EverythingBox-app` (md5-verify), update `.superpowers/sdd/progress.md`, mark the chapter.

## Self-Review (done at write time)

- **Spec coverage:** diacritics ✅T1; subtitle-year disambiguation ✅T1; hermetic show-dispatch probe ✅T2; suite+fable+merge ✅T3. Non-goals (getMeta year path, AddonManager interface extraction, bestSeriesMatch/Seam A-B/cache/transport changes) not built ✅. `bestMatch` only narrows ✅ (year check adds a `continue`, never a new accept).
- **Placeholder scan:** every code step carries full code; the probe notes where to adapt to the real `VideoEntry`/`LocalResolveCache`/`Settings` API (concrete instruction, not a placeholder), and T2 Step 1 has the implementer read `probe_addon`'s real link set to copy.
- **Type consistency:** `normalizeTitle`/`bestMatch`/`yearFromSubtitle`/`miY`, `SHOWDISPATCH-OK`, `makeSeriesFixture`/`runCase`, `LocalLibrary::showKeyFor`, `LocalResolveCache::{seriesIdsByShow,isShowFresh}`, `CatalogResolver::enqueue` — consistent with the shipped code the scout quoted.
- **Ambiguity resolved:** the year lives in the candidate's `subtitle` (no getMeta); the hermetic test uses a real AddonManager + fixture (AddonManager is non-virtual, unmockable); a new suite-gated probe (probe_addon isn't in the suite loop) so the C1-class regression is actually CI-gated.
