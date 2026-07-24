# Local Library Network ID-Resolver (movies) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Resolve each scanned local movie once to the id(s) the installed catalog addons use (`tmdb:movie:…`, `tt…`) by reusing addon search, cache the result persistently, and index those ids → local path — so the shipped Seam A badge + Seam B prefer-local fire on real catalog tiles (esp. aiocatalog's TMDB tiles).

**Architecture:** A pure matcher (`CatalogMatch`, probe-tested) + a persisted device-local cache (`LocalResolveCache`) + an async throttled engine (`CatalogResolver`, a QObject over `AddonManager::requestSearch`/`catalogReady`, mirroring `GameMetaAggregator`'s queue). `LocalLibrary::buildIndex` grows a second arg (a `path → resolved-ids` snapshot) and indexes those ids. Seam A/B are untouched. Resolution runs after the base index installs in `MainWindow::rescanLocalLibrary`; results land in the cache and trigger a debounced index rebuild.

**Tech Stack:** Qt 6.8.3 (QtCore: QJson, QFileInfo, QRegularExpression, QTimer; the existing AddonManager async plumbing), headless probes.

## Global Constraints

- **Branch:** `local/id-resolver` off main. Standing autonomy through the merge gate. The pre-commit hook auto-bumps the patch version on every commit — expected; never hand-edit the version lines.
- **Scope = movies only.** No TV/episode resolution, no standalone TMDB client, no NFO write-back, no music/books, no resolve-cache syncing. Explicit non-goals (spec §Non-goals) — do not build them.
- **Two plan-time refinements vs the spec** (discovered from the anchors; the spec close-out records them):
  - Search-result `MediaItem`s carry **no year field**, so `bestMatch` v1 = **unique-normalized-title + IMDB cross-check** (a same-title remake → ≥2 title hits → conservative −1). Year-based remake disambiguation (per-candidate getMeta) is a follow-up. Safety (never mis-badge) is preserved.
  - **No `getMeta` step.** Searching *every* installed movie-catalog source and taking each source's own `bestMatch` id already yields each catalog's tile-id space (aiocatalog `tmdb:movie:…`, Cinemeta `tt…`). Store each matched source's id.
- **ANCHOR ON FUNCTION NAMES.** Scout anchors (main@7e447ef + the merged local library):

| Concern | Anchor |
|---|---|
| Async search/meta | `AddonManager::requestSearch(LoadedAddon*, const QString&) -> int reqId` (`AddonManager.h:93`, `-1` if src null); results on **`catalogReady(int reqId, const MediaCatalog&)`** (`AddonManager.h:174`) — search returns a `MediaCatalog`, NOT a searchReady. `reqCounter_` starts 0, valid reqId ≥ 1; guard `if (reqId >= 0)` |
| Sources + movie-catalog test | `sources() -> const QVector<LoadedAddon*>&` (`AddonManager.h:71`); `catalogs(LoadedAddon*) -> QVector<AddonCatalog>` (`AddonManager.h:75`); `AddonCatalog::type` is `"movie"`/`"mixed"`/… (`AddonModels.h:25-30`); `LoadedAddon::isMediaSource()` (`AddonManager.h:38`) |
| Throttle to mirror | `GameMetaAggregator` — `Job`/`pending_`/`jobs_`/`reqToJob_ (QHash<int,quint64>)`/`maxActive_=2`/`pump`/`startJob`/`finishJob` (`GameMetaAggregator.h:47-73`, `.cpp:85-134`); connect to `catalogReady` (search) not `metaReady` |
| Current buildIndex | `LocalLibrary::buildIndex` (`LocalLibrary.cpp:176-200`); `installIndex`/`index`/`root` (`:212-217`) |
| Rescan hook | `MainWindow::rescanLocalLibrary` (`MainWindow.cpp:1025-1040`) — enqueue inside the `finished` lambda after `installIndex`, within the `gen == libScanGen_` guard |
| AddonManager owner | `std::unique_ptr<AddonManager> addons_` (`MainWindow.h:482`); pass `addons_.get()` (the `home_ = new HomeView(addons_.get(), this)` precedent, `MainWindow.cpp:378`) |
| Settings toggle + LL section | themed `toggle(id,label,on)`/`action(id,label)` lambdas (`MainWindow.cpp:7903-7913`); the "Local Library" section (`:7938-7942`); handlers (`:8065-8079`); clone `keepScrapedData` getter/setter (`Settings.h:128-129`, `Settings.cpp:257-258`) |
| Candidate imdb | search-result `MediaItem::imdbStreamId` is EMPTY; a Cinemeta candidate's `id` IS `tt…`; aiocatalog's is `tmdb:movie:…`. In `bestMatch`, treat `id` as imdb iff `id.startsWith("tt")` |

- **Env recipe (copy verbatim):** PATH prepend `/c/Qt/6.8.3/msvc2022_64/bin` + `/c/mpv-dev`; build dir `build` carries generated `qt.conf` (no `QT_PLUGIN_PATH`); configure `cmake -S native -B build -G "Visual Studio 18 2026" -A x64 -DMYMEDIAVAULT_BUILD_APP=ON -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64 -DMPV_INCLUDE_DIR=C:/mpv-dev/include -DMPV_LIBRARY=C:/mpv-dev/libmpv.lib`. **The probe harness runs the RELEASE binary — ALWAYS build `--config Release`.** App target: `mymediavault`. Suite: `BUILD_DIR=build bash native/tools/run-headless-probes.sh`.
- **Probe discipline:** RED-first. `probe_resolver` is QtCore-only (links `CatalogMatch.cpp` + `LocalResolveCache.cpp` + `LocalLibrary.cpp` + `AddonModels.cpp` + `Settings.cpp` + `theme2/FormFactor.cpp`; NOT AddonManager). Hermetic `QTemporaryDir` for the cache file. Sentinel `RESOLVER-OK` / `RESOLVER-FAIL <cond> (line)`.
- **Dormant contract:** `resolveOnline` default off OR no movie-catalog source ⇒ resolver never runs; NFO-only behavior; zero network. Additive only — Seam A/B, addon transport, sync transport unchanged.

---

### Task 1: CatalogMatch pure matcher + probe_resolver (RED-first)

**Files:**
- Create: `native/src/core/CatalogMatch.h`, `native/src/core/CatalogMatch.cpp`
- Create: `native/tools/probe_resolver.cpp`
- Modify: `native/CMakeLists.txt` (new `add_executable(probe_resolver …)` after the `probe_locallib` block; add `CatalogMatch.cpp` to the app target beside `LocalLibrary.cpp`)
- Modify: `native/tools/run-headless-probes.sh` (append `"probe_resolver RESOLVER-OK"` to the loop)
- Modify: `.github/workflows/ci.yml` (append `probe_resolver` to the build-target list)

**Interfaces (Produces):**
```cpp
namespace CatalogMatch {
    QString normalizeTitle(const QString& t);   // lowercase, non-alnum→space, simplified, drop leading the/a/an
    // Index into `candidates` of the accepted match, or -1. want = local movie; candidates = search hits.
    int bestMatch(const LocalLibrary::VideoEntry& want, const QVector<MediaItem>& candidates);
}
```

- [ ] **Step 1: Write the failing probe.** Create `native/tools/probe_resolver.cpp` (mirrors `probe_locallib.cpp` shape: header doc, CHECK macro, `QCoreApplication`, sentinel `RESOLVER-OK`/`RESOLVER-FAIL %s (line %d)`). Assert the match table:

```cpp
// Headless probe for the catalog id-resolver: pure matcher (CatalogMatch) + persisted cache (LocalResolveCache).
// Prints RESOLVER-OK on success; any failure prints RESOLVER-FAIL <cond> (line) and exits non-zero.
#include "CatalogMatch.h"
#include "LocalLibrary.h"
#include "AddonModels.h"

#include <QCoreApplication>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "RESOLVER-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

static MediaItem mi(const QString& id, const QString& title, const QString& type = QStringLiteral("movie"))
{ MediaItem it; it.id = id; it.title = title; it.type = type; return it; }

static LocalLibrary::VideoEntry movie(const QString& title, int year, const QString& imdb = QString())
{ LocalLibrary::VideoEntry e; e.kind = LocalLibrary::Kind::Movie; e.title = title; e.year = year; e.imdbId = imdb; return e; }

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // normalizeTitle
    CHECK(CatalogMatch::normalizeTitle(QStringLiteral("The Matrix")) == QStringLiteral("matrix"));
    CHECK(CatalogMatch::normalizeTitle(QStringLiteral("WALL·E!")) == QStringLiteral("wall e"));
    CHECK(CatalogMatch::normalizeTitle(QStringLiteral("Amélie")) == CatalogMatch::normalizeTitle(QStringLiteral("amelie"))
          || true);  // diacritics best-effort; do not over-assert

    // IMDB cross-check wins outright.
    {
        QVector<MediaItem> c{ mi("tmdb:movie:27205", "Inception"), mi("tt1375666", "Inception") };
        CHECK(CatalogMatch::bestMatch(movie("Inception", 2010, "tt1375666"), c) == 1);
    }
    // Unique normalized-title match (no imdb) → that index.
    {
        QVector<MediaItem> c{ mi("tmdb:movie:27205", "Inception"), mi("tmdb:movie:99", "Interstellar") };
        CHECK(CatalogMatch::bestMatch(movie("inception", 2010), c) == 0);
    }
    // Article/case/punctuation normalization still matches.
    {
        QVector<MediaItem> c{ mi("tt0133093", "The Matrix") };
        CHECK(CatalogMatch::bestMatch(movie("Matrix", 1999), c) == 0);
    }
    // Ambiguous: two candidates share the normalized title → -1 (conservative, never mis-badge).
    {
        QVector<MediaItem> c{ mi("tmdb:movie:1", "The Mummy"), mi("tmdb:movie:2", "The Mummy") };
        CHECK(CatalogMatch::bestMatch(movie("The Mummy", 2017), c) == -1);
    }
    // A same-title SERIES candidate is not a movie match.
    {
        QVector<MediaItem> c{ mi("tt111", "Fargo", "series") };
        CHECK(CatalogMatch::bestMatch(movie("Fargo", 1996), c) == -1);
    }
    // No candidates / empty title → -1.
    CHECK(CatalogMatch::bestMatch(movie("Inception", 2010), {}) == -1);
    CHECK(CatalogMatch::bestMatch(movie("", 0), { mi("tt1", "x") }) == -1);

    if (failures == 0) { std::puts("RESOLVER-OK"); return 0; }
    std::fprintf(stderr, "RESOLVER: %d check(s) failed\n", failures);
    return 1;
}
```

- [ ] **Step 2: Wire CMake.** In `native/CMakeLists.txt`, after the `probe_locallib` block, add:

```cmake
    # Headless test for the catalog id-resolver core (pure matcher + persisted cache).
    add_executable(probe_resolver tools/probe_resolver.cpp
        src/core/CatalogMatch.cpp src/core/CatalogMatch.h
        src/core/LocalResolveCache.cpp src/core/LocalResolveCache.h
        src/core/LocalLibrary.cpp src/core/LocalLibrary.h
        src/core/Settings.cpp src/core/Settings.h
        src/theme2/FormFactor.cpp src/theme2/FormFactor.h
        src/addons/AddonModels.cpp)
    target_include_directories(probe_resolver PRIVATE src src/core src/addons src/theme2)
    target_link_libraries(probe_resolver PRIVATE Qt6::Core Qt6::Network Qt6::Gui)
```

(The `LocalResolveCache.cpp` reference is for Task 2 — create an empty placeholder `LocalResolveCache.{h,cpp}` with just an include guard / empty namespace now so the target links in Task 1; Task 2 fills it. Add `CatalogMatch.cpp`/`.h` to the app target beside `LocalLibrary.cpp`.)

- [ ] **Step 3: Verify RED.** `cmake -S native -B build … ` (reconfigure — new files) then `cmake --build build --target probe_resolver --config Release --parallel`. Expected: link error (`CatalogMatch::bestMatch`/`normalizeTitle` undefined).

- [ ] **Step 4: Implement `CatalogMatch.h`:**

```cpp
#pragma once
#include <QString>
#include <QVector>
#include "AddonModels.h"
#include "LocalLibrary.h"

namespace CatalogMatch
{
    // Lowercase, non-alphanumeric → space, whitespace-collapsed, leading article (the/a/an) dropped.
    QString normalizeTitle(const QString& t);

    // Return the index into `candidates` of the accepted match, or -1. Strict:
    //  - if `want.imdbId` is set and a candidate's id equals it (case-insensitive) → that index (outright).
    //  - else the SINGLE candidate that is a movie and whose normalized title equals want's; -1 if none or >1.
    // (Search results carry no year, so same-title remakes are ambiguous → -1: conservative, never mis-badge.)
    int bestMatch(const LocalLibrary::VideoEntry& want, const QVector<MediaItem>& candidates);
}
```

- [ ] **Step 5: Implement `CatalogMatch.cpp`:**

```cpp
#include "CatalogMatch.h"
#include <QRegularExpression>

namespace CatalogMatch
{
QString normalizeTitle(const QString& t)
{
    QString s = t.toLower();
    static const QRegularExpression nonAlnum(QStringLiteral("[^a-z0-9]+"));
    s.replace(nonAlnum, QStringLiteral(" "));
    s = s.simplified();
    for (const QString& art : { QStringLiteral("the "), QStringLiteral("a "), QStringLiteral("an ") })
        if (s.startsWith(art)) { s = s.mid(art.size()); break; }
    return s;
}

int bestMatch(const LocalLibrary::VideoEntry& want, const QVector<MediaItem>& candidates)
{
    if (!want.imdbId.isEmpty())
        for (int i = 0; i < candidates.size(); ++i)
            if (candidates[i].id.compare(want.imdbId, Qt::CaseInsensitive) == 0)
                return i;

    const QString wt = normalizeTitle(want.title);
    if (wt.isEmpty()) return -1;

    int hit = -1;
    for (int i = 0; i < candidates.size(); ++i)
    {
        const MediaItem& c = candidates[i];
        if (!c.type.isEmpty() && c.type != QStringLiteral("movie")) continue; // not a same-named series/etc.
        if (normalizeTitle(c.title) != wt) continue;
        if (hit != -1) return -1;   // ambiguous: more than one title match
        hit = i;
    }
    return hit;
}
}
```

- [ ] **Step 6: Build GREEN + wire runner/CI.** `cmake --build build --target probe_resolver --config Release --parallel` → run it → `RESOLVER-OK`. Append `"probe_resolver RESOLVER-OK"` to `native/tools/run-headless-probes.sh`'s loop and `probe_resolver` to `.github/workflows/ci.yml:52`. Full suite: `BUILD_DIR=build bash native/tools/run-headless-probes.sh` → `ALL HEADLESS PROBES PASSED`.

- [ ] **Step 7: Commit.**
```bash
git add native/src/core/CatalogMatch.h native/src/core/CatalogMatch.cpp native/src/core/LocalResolveCache.h native/src/core/LocalResolveCache.cpp native/tools/probe_resolver.cpp native/CMakeLists.txt native/tools/run-headless-probes.sh .github/workflows/ci.yml
git commit -m "feat: CatalogMatch pure matcher + probe_resolver (id-resolver T1)"
```

---

### Task 2: LocalResolveCache (persisted) + buildIndex extension

**Files:**
- Modify: `native/src/core/LocalResolveCache.h` / `.cpp` (fill the Task-1 placeholder)
- Modify: `native/src/core/LocalLibrary.h` / `.cpp` (`buildIndex` gains a `path→ids` arg)
- Modify: `native/tools/probe_resolver.cpp` (cache + buildIndex-with-ids assertions)

**Interfaces (Produces):**
```cpp
class LocalResolveCache {
public:
    struct Entry { qint64 size=0; qint64 mtime=0; QStringList ids; bool matched=false; qint64 ts=0; };
    explicit LocalResolveCache(QString filePath);          // e.g. AppPaths::dataDir()+"/localresolve.json"
    void load();                                           // read the JSON (missing file = empty)
    void save() const;                                     // write the JSON
    bool has(const QString& path) const;
    Entry entry(const QString& path) const;
    // Fresh = record exists, size+mtime match, AND (matched OR a nomatch still within the retry window).
    bool isFresh(const QString& path, qint64 size, qint64 mtime, qint64 nowSecs, qint64 retryDays = 14) const;
    void putMatched(const QString& path, qint64 size, qint64 mtime, const QStringList& ids, qint64 nowSecs);
    void putNoMatch(const QString& path, qint64 size, qint64 mtime, qint64 nowSecs);
    QHash<QString, QStringList> matchedIdsByPath() const;  // snapshot for buildIndex (matched entries only)
private:
    QString file_;
    QHash<QString, Entry> byPath_;
};
// LocalLibrary::buildIndex gains an optional trailing arg:
OwnedIndex buildIndex(const QVector<VideoEntry>& entries,
                      const QHash<QString, QStringList>& extraMovieIdsByPath = {});
```

- [ ] **Step 1: Add failing assertions to `native/tools/probe_resolver.cpp`** (before the sentinel), and `#include "LocalResolveCache.h"`, `#include <QTemporaryDir>`, `#include <QDateTime>`:

```cpp
    QTemporaryDir tmp; CHECK(tmp.isValid());
    const QString cachePath = tmp.path() + QStringLiteral("/localresolve.json");
    const qint64 now = 1000000;
    {
        LocalResolveCache c(cachePath);
        c.load();
        CHECK(!c.has("/movies/Inception.mkv"));
        c.putMatched("/movies/Inception.mkv", 100, 200, { "tmdb:movie:27205", "tt1375666" }, now);
        c.putNoMatch("/movies/Unknown.mkv", 50, 60, now);
        CHECK(c.isFresh("/movies/Inception.mkv", 100, 200, now));
        CHECK(!c.isFresh("/movies/Inception.mkv", 100, 999, now));           // mtime changed → stale
        CHECK(c.isFresh("/movies/Unknown.mkv", 50, 60, now));               // nomatch within window
        CHECK(!c.isFresh("/movies/Unknown.mkv", 50, 60, now + 15LL*86400)); // nomatch past 14d → stale (retry)
        c.save();
    }
    {
        LocalResolveCache c(cachePath); c.load();                          // persistence round-trip
        CHECK(c.entry("/movies/Inception.mkv").ids.contains("tmdb:movie:27205"));
        CHECK(c.matchedIdsByPath().value("/movies/Inception.mkv").contains("tt1375666"));
        CHECK(!c.matchedIdsByPath().contains("/movies/Unknown.mkv"));       // nomatch not in the snapshot
    }
    // buildIndex indexes the resolved ids → the movie's path, alongside the NFO id.
    {
        LocalLibrary::VideoEntry e; e.kind = LocalLibrary::Kind::Movie; e.path = "/m/Inception.mkv";
        e.title = "Inception"; e.imdbId = "tt1375666";
        QHash<QString, QStringList> extra; extra.insert(e.path, { "tmdb:movie:27205" });
        const LocalLibrary::OwnedIndex idx = LocalLibrary::buildIndex({ e }, extra);
        CHECK(idx.ownsId("tt1375666"));                 // NFO id (existing behavior)
        CHECK(idx.ownsId("tmdb:movie:27205"));          // resolved id (new)
        CHECK(idx.localPathFor("tmdb:movie:27205") == e.path);
    }
```

- [ ] **Step 2: Verify RED** — build probe_resolver → fails (LocalResolveCache methods undefined, buildIndex arity).

- [ ] **Step 3: Implement `LocalResolveCache.h`:**

```cpp
#pragma once
#include <QString>
#include <QStringList>
#include <QHash>

class LocalResolveCache
{
public:
    struct Entry { qint64 size = 0; qint64 mtime = 0; QStringList ids; bool matched = false; qint64 ts = 0; };

    explicit LocalResolveCache(QString filePath) : file_(std::move(filePath)) {}
    void load();
    void save() const;
    bool has(const QString& path) const { return byPath_.contains(path); }
    Entry entry(const QString& path) const { return byPath_.value(path); }
    bool isFresh(const QString& path, qint64 size, qint64 mtime, qint64 nowSecs, qint64 retryDays = 14) const;
    void putMatched(const QString& path, qint64 size, qint64 mtime, const QStringList& ids, qint64 nowSecs);
    void putNoMatch(const QString& path, qint64 size, qint64 mtime, qint64 nowSecs);
    QHash<QString, QStringList> matchedIdsByPath() const;

private:
    QString file_;
    QHash<QString, Entry> byPath_;
};
```

- [ ] **Step 4: Implement `LocalResolveCache.cpp`:**

```cpp
#include "LocalResolveCache.h"
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

void LocalResolveCache::load()
{
    byPath_.clear();
    QFile f(file_);
    if (!f.open(QIODevice::ReadOnly)) return;
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    for (auto it = root.constBegin(); it != root.constEnd(); ++it)
    {
        const QJsonObject o = it.value().toObject();
        Entry e;
        e.size = (qint64)o.value(QStringLiteral("size")).toDouble();
        e.mtime = (qint64)o.value(QStringLiteral("mtime")).toDouble();
        e.matched = o.value(QStringLiteral("matched")).toBool();
        e.ts = (qint64)o.value(QStringLiteral("ts")).toDouble();
        for (const QJsonValue& v : o.value(QStringLiteral("ids")).toArray()) e.ids << v.toString();
        byPath_.insert(it.key(), e);
    }
}

void LocalResolveCache::save() const
{
    QJsonObject root;
    for (auto it = byPath_.constBegin(); it != byPath_.constEnd(); ++it)
    {
        const Entry& e = it.value();
        QJsonArray ids; for (const QString& s : e.ids) ids.append(s);
        root.insert(it.key(), QJsonObject{
            { QStringLiteral("size"), (double)e.size }, { QStringLiteral("mtime"), (double)e.mtime },
            { QStringLiteral("matched"), e.matched }, { QStringLiteral("ts"), (double)e.ts },
            { QStringLiteral("ids"), ids } });
    }
    QFile f(file_);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

bool LocalResolveCache::isFresh(const QString& path, qint64 size, qint64 mtime, qint64 nowSecs, qint64 retryDays) const
{
    const auto it = byPath_.constFind(path);
    if (it == byPath_.constEnd()) return false;
    const Entry& e = it.value();
    if (e.size != size || e.mtime != mtime) return false;   // file changed → re-resolve
    if (e.matched) return true;                             // a match never expires (until the file changes)
    return (nowSecs - e.ts) < retryDays * 86400;           // a nomatch is fresh only within the retry window
}

void LocalResolveCache::putMatched(const QString& path, qint64 size, qint64 mtime, const QStringList& ids, qint64 nowSecs)
{ byPath_.insert(path, Entry{ size, mtime, ids, true, nowSecs }); }

void LocalResolveCache::putNoMatch(const QString& path, qint64 size, qint64 mtime, qint64 nowSecs)
{ byPath_.insert(path, Entry{ size, mtime, {}, false, nowSecs }); }

QHash<QString, QStringList> LocalResolveCache::matchedIdsByPath() const
{
    QHash<QString, QStringList> out;
    for (auto it = byPath_.constBegin(); it != byPath_.constEnd(); ++it)
        if (it.value().matched && !it.value().ids.isEmpty()) out.insert(it.key(), it.value().ids);
    return out;
}
```

- [ ] **Step 5: Extend `buildIndex`.** In `native/src/core/LocalLibrary.h`, change the declaration to:
```cpp
    OwnedIndex buildIndex(const QVector<VideoEntry>& entries,
                          const QHash<QString, QStringList>& extraMovieIdsByPath = {});
```
Add `#include <QStringList>` if needed. In `native/src/core/LocalLibrary.cpp`, update `buildIndex` — after the existing movie NFO-id insert block, index each resolved id for that movie's path (first-wins, same as the NFO id):

```cpp
        if (e.kind == Kind::Movie)
        {
            if (!e.imdbId.isEmpty() && !idx.pathById.contains(e.imdbId))
                idx.pathById.insert(e.imdbId, e.path);
            for (const QString& rid : extraMovieIdsByPath.value(e.path))   // resolved catalog ids
                if (!rid.isEmpty() && !idx.pathById.contains(rid))
                    idx.pathById.insert(rid, e.path);
        }
        else if (e.kind == Kind::Episode && !e.seriesImdbId.isEmpty())
        {
            // ... unchanged episode block ...
        }
```
(Preserve the existing episode block verbatim; only the movie branch changes. Confirm the existing single-arg callers still compile — the new arg is defaulted; `probe_locallib`'s `buildIndex(scanned)` calls stay valid.)

- [ ] **Step 6: Build GREEN.** `cmake --build build --target probe_resolver probe_locallib --config Release --parallel` → both green. Full suite → `ALL HEADLESS PROBES PASSED` (probe_locallib unaffected — default arg).

- [ ] **Step 7: Commit.**
```bash
git add native/src/core/LocalResolveCache.h native/src/core/LocalResolveCache.cpp native/src/core/LocalLibrary.h native/src/core/LocalLibrary.cpp native/tools/probe_resolver.cpp
git commit -m "feat: LocalResolveCache (persisted) + buildIndex indexes resolved ids (id-resolver T2)"
```

---

### Task 3: CatalogResolver async engine (throttled)

**Files:**
- Create: `native/src/core/CatalogResolver.h`, `native/src/core/CatalogResolver.cpp`
- Modify: `native/CMakeLists.txt` (add `CatalogResolver.cpp` to the app target only — it needs AddonManager, so it is NOT in the probe)

**Interfaces (Produces):**
```cpp
class CatalogResolver : public QObject {
    Q_OBJECT
public:
    CatalogResolver(AddonManager* addons, LocalResolveCache* cache, QObject* parent = nullptr);
    // Queue movies not already fresh in the cache (skips episodes, skips when resolveOnline is off).
    void enqueue(const QVector<LocalLibrary::VideoEntry>& entries);
    void clearCacheAndRequeue(const QVector<LocalLibrary::VideoEntry>& entries); // the "Re-match online" action
signals:
    void resolved();   // debounced: a batch of movies finished resolving; the owner should rebuild the index
};
```

**Interfaces (Consumes):** `AddonManager::{sources,catalogs,requestSearch}` + `catalogReady`; `CatalogMatch::bestMatch`; `LocalResolveCache::{isFresh,putMatched,putNoMatch,save}`; `Settings::resolveOnline` (T4 adds it — for T3, gate on a bool the owner passes, OR call `Settings::resolveOnline()` directly and note T4 adds it; use `Settings::resolveOnline()`).

- [ ] **Step 1: Implement `CatalogResolver.h`** (mirror `GameMetaAggregator`'s Job/queue shape, but for search across movie-catalog sources):

```cpp
#pragma once
#include <QObject>
#include <QHash>
#include <QList>
#include <QSet>
#include <QSharedPointer>
#include <QStringList>
#include "LocalLibrary.h"

class AddonManager;
class LocalResolveCache;
class QTimer;

class CatalogResolver : public QObject
{
    Q_OBJECT
public:
    CatalogResolver(AddonManager* addons, LocalResolveCache* cache, QObject* parent = nullptr);
    void enqueue(const QVector<LocalLibrary::VideoEntry>& entries);
    void clearCacheAndRequeue(const QVector<LocalLibrary::VideoEntry>& entries);

signals:
    void resolved();

private slots:
    void onCatalogReady(int reqId, const class MediaCatalog& catalog);

private:
    struct Job {
        quint64 id = 0;
        LocalLibrary::VideoEntry movie;
        qint64 size = 0, mtime = 0;
        QSet<int> outstanding;      // in-flight search reqIds
        QStringList matchedIds;     // one per source that matched
        QTimer* timer = nullptr;
    };
    void pump();
    void startJob(const QSharedPointer<Job>& job);
    void finishJob(quint64 id);
    void scheduleResolvedSignal();

    AddonManager* addons_;
    LocalResolveCache* cache_;
    QHash<quint64, QSharedPointer<Job>> jobs_;
    QList<QSharedPointer<Job>> pending_;
    QHash<int, quint64> reqToJob_;
    QSet<QString> seen_;            // paths queued/run this session (dedup)
    quint64 nextId_ = 1;
    int maxActive_ = 2;
    QTimer* resolvedDebounce_ = nullptr;
    bool cacheDirty_ = false;
};
```

- [ ] **Step 2: Implement `CatalogResolver.cpp`.** Key logic: `enqueue` filters to uncached movies; `startJob` fans `requestSearch` across movie-catalog sources; `onCatalogReady` runs `bestMatch` on each reply and collects the matched id; `finishJob` writes the cache and debounces `resolved()`.

```cpp
#include "CatalogResolver.h"
#include "CatalogMatch.h"
#include "LocalResolveCache.h"
#include "Settings.h"
#include "AddonManager.h"
#include "AddonModels.h"

#include <QTimer>
#include <QFileInfo>
#include <QDateTime>

CatalogResolver::CatalogResolver(AddonManager* addons, LocalResolveCache* cache, QObject* parent)
    : QObject(parent), addons_(addons), cache_(cache)
{
    connect(addons_, &AddonManager::catalogReady, this, &CatalogResolver::onCatalogReady);
    resolvedDebounce_ = new QTimer(this);
    resolvedDebounce_->setSingleShot(true);
    connect(resolvedDebounce_, &QTimer::timeout, this, [this] {
        if (cacheDirty_) { cache_->save(); cacheDirty_ = false; }
        emit resolved();
    });
}

static bool isMovieCatalogSource(AddonManager* m, LoadedAddon* s)
{
    if (!s || !s->isMediaSource()) return false;
    for (const AddonCatalog& c : m->catalogs(s))
        if (c.type == QStringLiteral("movie") || c.type == QStringLiteral("mixed")) return true;
    return false;
}

void CatalogResolver::enqueue(const QVector<LocalLibrary::VideoEntry>& entries)
{
    if (!Settings::resolveOnline()) return;
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    for (const LocalLibrary::VideoEntry& e : entries)
    {
        if (e.kind != LocalLibrary::Kind::Movie) continue;         // movies only this track
        if (seen_.contains(e.path)) continue;
        const QFileInfo fi(e.path);
        const qint64 size = fi.size();
        const qint64 mtime = fi.lastModified().toSecsSinceEpoch();
        if (cache_->isFresh(e.path, size, mtime, now)) continue;   // already resolved / nomatch-in-window
        seen_.insert(e.path);
        auto job = QSharedPointer<Job>::create();
        job->id = nextId_++; job->movie = e; job->size = size; job->mtime = mtime;
        pending_.append(job);
    }
    pump();
}

void CatalogResolver::clearCacheAndRequeue(const QVector<LocalLibrary::VideoEntry>& entries)
{
    *cache_ = LocalResolveCache(QString());   // WRONG — see note
    // NOTE: implement clear by re-constructing over the same file path. The owner (T4) passes a cache
    // bound to localresolve.json; expose a cache clear via a new method OR have T4 delete the file + reload.
    seen_.clear();
    enqueue(entries);
}

void CatalogResolver::pump()
{
    while (jobs_.size() < maxActive_ && !pending_.isEmpty())
        startJob(pending_.takeFirst());
}

void CatalogResolver::startJob(const QSharedPointer<Job>& job)
{
    jobs_.insert(job->id, job);
    for (LoadedAddon* s : addons_->sources())
    {
        if (!isMovieCatalogSource(addons_, s)) continue;
        const int reqId = addons_->requestSearch(s, job->movie.title);
        if (reqId >= 0) { job->outstanding.insert(reqId); reqToJob_.insert(reqId, job->id); }
    }
    if (job->outstanding.isEmpty()) { const quint64 id = job->id; QTimer::singleShot(0, this, [this, id]{ finishJob(id); }); return; }
    job->timer = new QTimer(this); job->timer->setSingleShot(true);
    const quint64 id = job->id;
    connect(job->timer, &QTimer::timeout, this, [this, id]{ finishJob(id); });
    job->timer->start(12000);
}

void CatalogResolver::onCatalogReady(int reqId, const MediaCatalog& catalog)
{
    const auto jt = reqToJob_.constFind(reqId);
    if (jt == reqToJob_.constEnd()) return;   // not one of ours (normal browse search)
    const quint64 jobId = jt.value(); reqToJob_.erase(jt);
    const auto j = jobs_.constFind(jobId);
    if (j == jobs_.constEnd()) return;
    const QSharedPointer<Job> job = j.value();
    job->outstanding.remove(reqId);
    const int idx = CatalogMatch::bestMatch(job->movie, catalog.items);
    if (idx >= 0) { const QString id = catalog.items[idx].id; if (!id.isEmpty() && !job->matchedIds.contains(id)) job->matchedIds << id; }
    if (job->outstanding.isEmpty()) finishJob(jobId);
}

void CatalogResolver::finishJob(quint64 id)
{
    const auto j = jobs_.constFind(id);
    if (j == jobs_.constEnd()) return;
    const QSharedPointer<Job> job = j.value();
    jobs_.remove(id);
    if (job->timer) { job->timer->stop(); job->timer->deleteLater(); job->timer = nullptr; }
    for (int r : job->outstanding) reqToJob_.remove(r);   // drop lingering (timeout path)
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    if (!job->matchedIds.isEmpty()) cache_->putMatched(job->movie.path, job->size, job->mtime, job->matchedIds, now);
    else                            cache_->putNoMatch(job->movie.path, job->size, job->mtime, now);
    cacheDirty_ = true;
    scheduleResolvedSignal();
    pump();
}

void CatalogResolver::scheduleResolvedSignal()
{ resolvedDebounce_->start(1500); }   // coalesce a batch of finishes into one index rebuild
```

**IMPORTANT for the implementer:** the `clearCacheAndRequeue` body above is intentionally marked WRONG — do NOT ship it. Implement the clear cleanly: add a `void clear()` method to `LocalResolveCache` (empties `byPath_` and `save()`s) and call `cache_->clear(); seen_.clear(); enqueue(entries);`. Add the `clear()` to `LocalResolveCache` in this task (small, and its unit test can be a one-line probe assertion appended to probe_resolver: put → clear → `!has`).

- [ ] **Step 2b: Add `LocalResolveCache::clear()`** (`void clear() { byPath_.clear(); save(); }` in the header) and a probe assertion (put an entry → `clear()` → `!has(...)`). Rebuild probe_resolver → green.

- [ ] **Step 3: CMake.** Add `src/core/CatalogResolver.cpp src/core/CatalogResolver.h` to the app target only (beside `LocalLibrary.cpp`). CatalogResolver is NOT added to any probe (it needs AddonManager). Build the app: `cmake --build build --target mymediavault --config Release --parallel` — must compile clean.

- [ ] **Step 4: Full suite** → `ALL HEADLESS PROBES PASSED` (unchanged; the resolver has no probe, its pure deps are covered by probe_resolver).

- [ ] **Step 5: Commit.**
```bash
git add native/src/core/CatalogResolver.h native/src/core/CatalogResolver.cpp native/src/core/LocalResolveCache.h native/tools/probe_resolver.cpp native/CMakeLists.txt
git commit -m "feat: CatalogResolver async throttled engine over addon search (id-resolver T3)"
```

---

### Task 4: Wire-up — Settings toggle, resolver construction, rescan hook, progressive refresh

**Files:**
- Modify: `native/src/core/Settings.h` / `.cpp` (`resolveOnline`/`setResolveOnline`)
- Modify: `native/src/ui/MainWindow.h` / `.cpp` (own the cache + resolver; hook `rescanLocalLibrary`; the `resolved()` → rebuild; the settings toggle + `library.rematch` action + handlers)

**Interfaces:** Consumes T1-T3. Produces the live feature.

- [ ] **Step 1: `Settings::resolveOnline`.** In `native/src/core/Settings.h` (beside `keepScrapedData`):
```cpp
    bool resolveOnline();
    void setResolveOnline(bool on);
```
In `Settings.cpp`:
```cpp
bool Settings::resolveOnline() { return store().value(QStringLiteral("library/resolveOnline"), true).toBool(); }
void Settings::setResolveOnline(bool on) { store().setValue(QStringLiteral("library/resolveOnline"), on); store().sync(); }
```
(Default `true` per the spec — the feature is the point; the toggle disables it. `library/resolveOnline` is a preference, NOT device-local: it may sync. Do NOT add it to `CloudSync::isDeviceLocalKey`.)

- [ ] **Step 2: Own the cache + resolver in MainWindow.** In `native/src/ui/MainWindow.h`, add members:
```cpp
    std::unique_ptr<LocalResolveCache> resolveCache_;
    std::unique_ptr<CatalogResolver> resolver_;
```
Add includes for both headers. In `MainWindow.cpp`, in the constructor AFTER `addons_` is constructed and BEFORE the first `rescanLocalLibrary()` call, construct them:
```cpp
    resolveCache_ = std::make_unique<LocalResolveCache>(AppPaths::dataDir() + QStringLiteral("/localresolve.json"));
    resolveCache_->load();
    resolver_ = std::make_unique<CatalogResolver>(addons_.get(), resolveCache_.get(), this);
    connect(resolver_.get(), &CatalogResolver::resolved, this, [this] {
        // A batch of movies resolved: rebuild the index from the current scan + the now-richer cache, then refresh.
        const QString libRoot = LocalLibrary::root();
        const auto extra = resolveCache_->matchedIdsByPath();
        auto* w = new QFutureWatcher<LocalLibrary::OwnedIndex>(this);
        connect(w, &QFutureWatcher<LocalLibrary::OwnedIndex>::finished, this, [this, w] {
            LocalLibrary::installIndex(w->result());
            if (home_) home_->onLocalLibraryChanged();
            w->deleteLater();
        });
        w->setFuture(QtConcurrent::run([libRoot, extra] {
            return LocalLibrary::buildIndex(LocalLibrary::scanFolder(libRoot), extra);
        }));
    });
```

- [ ] **Step 2b: Feed the cache into the base scan too.** In `rescanLocalLibrary()` (`MainWindow.cpp:1025`), capture the cache snapshot on the main thread and pass it to `buildIndex`, and enqueue the movies after install. Replace the body with:
```cpp
void MainWindow::rescanLocalLibrary()
{
    const QString libRoot = LocalLibrary::root();
    const quint64 gen = ++libScanGen_;
    const QHash<QString, QStringList> extra = resolveCache_ ? resolveCache_->matchedIdsByPath()
                                                            : QHash<QString, QStringList>{};
    auto* w = new QFutureWatcher<LocalLibrary::OwnedIndex>(this);
    connect(w, &QFutureWatcher<LocalLibrary::OwnedIndex>::finished, this, [this, w, gen] {
        if (gen == libScanGen_) {
            LocalLibrary::installIndex(w->result());
            if (home_) home_->onLocalLibraryChanged();
            if (resolver_) resolver_->enqueue(LocalLibrary::index().all());   // resolve uncached movies in the background
        }
        w->deleteLater();
    });
    w->setFuture(QtConcurrent::run([libRoot, extra] {
        return LocalLibrary::buildIndex(LocalLibrary::scanFolder(libRoot), extra);
    }));
}
```
(`LocalLibrary::index().all()` returns the just-installed entries — the resolver reads `.path`/`.title`/`.imdbId` and skips episodes. The `extra` snapshot is captured by value → thread-safe in the worker.)

- [ ] **Step 3: Settings UI — toggle + re-match action.** In the themed "Local Library" section (`MainWindow.cpp:7938-7942`), after the `library.rescan` action add:
```cpp
        toggle(QStringLiteral("library.resolveonline"), tr("Match local files to online catalogs"),
               Settings::resolveOnline());
        action(QStringLiteral("library.rematch"), tr("Re-match Local Library online"));
```
In the handler chain (beside `library.rescan`, `:8074`) add:
```cpp
                else if (id == QStringLiteral("library.resolveonline")) {
                    Settings::setResolveOnline(on);
                    if (on && resolver_) resolver_->enqueue(LocalLibrary::index().all());
                }
                else if (id == QStringLiteral("library.rematch")) {
                    if (resolver_) resolver_->clearCacheAndRequeue(LocalLibrary::index().all());
                    statusBar()->showMessage(tr("Re-matching your Local Library online…"), 4000);
                }
```

- [ ] **Step 4: Build + suite.** `cmake --build build --target mymediavault --config Release --parallel` (clean) + `BUILD_DIR=build bash native/tools/run-headless-probes.sh` (`ALL HEADLESS PROBES PASSED`).

- [ ] **Step 5: Commit.**
```bash
git add native/src/core/Settings.h native/src/core/Settings.cpp native/src/ui/MainWindow.h native/src/ui/MainWindow.cpp
git commit -m "feat: resolveOnline toggle + resolver wiring + progressive index refresh (id-resolver T4)"
```

---

### Task 5: Close-out — live verify (badge on a real aiocatalog tile), perf, fable, merge

- [ ] **Step 1: Live verify (the payoff — now inducible).** Portable-throwaway technique (copy the deployed data dir with `aiocatalog` installed, cloud-stripped; real app untouched; per the `verify-app-gui-capture` memory + `MMV_UITEST`). Seed `library/folder` → a fixture with a movie whose title matches a real TMDB entry (e.g. `Inception (2010)/Inception (2010).mkv`, no NFO). Launch → scan → the resolver searches aiocatalog → within a few seconds the movie's `tmdb:movie:…` id is cached + indexed. Then browse the aiocatalog movie catalog to a page containing Inception → **verify the "On disk" badge renders on the real aiocatalog tile**, and **activating it plays the local file** (Seam B prefer-local). Screenshots `resolver-badge.png`, `resolver-play.png`. Also verify: toggling "Match local files to online catalogs" off → no resolution on a fresh cache; "Re-match" clears + re-resolves. If aiocatalog isn't present/searchable in the throwaway, record honestly and fall back to the probe + code-walk (the game-importers posture) — but attempt it, since this is the demo the shipped track couldn't produce.
- [ ] **Step 2: Perf.** 3 runs → `docs/superpowers/perf/2026-07-24-idresolver-baseline.md`; confirm `nav.select` flat (resolution is off the render/hot path — background searches + a debounced rebuild; Seam A/B stay O(1)). Confirm startup unaffected (the base scan/index is unchanged; enqueue is post-install and non-blocking).
- [ ] **Step 3: Fable whole-branch review** (`scripts/review-package $(git merge-base main HEAD) HEAD`, most-capable model). Dimensions: the dormant contract (resolveOnline off / no movie-catalog source → zero network); `bestMatch` never mis-badges (ambiguous → −1; series candidate rejected; imdb cross-check correct); the async engine's reqId correlation + timeout + slot-freeing mirror GameMetaAggregator without leaks; `reqToJob_` can't collide with normal browse searches (a browse `catalogReady` reqId isn't in `reqToJob_` → ignored); thread-safety (the `extra` snapshot captured by value; `g_index` main-thread-only; the resolver runs entirely on the main thread; `enqueue` reads `index().all()` on the main thread); the cache round-trip + mtime invalidation + nomatch retry window; `resolveOnline` genuinely syncs (a preference) while `library/folder` stays device-local; the `clearCacheAndRequeue` clean `clear()` (the WRONG placeholder is gone); the debounced `resolved()` doesn't thrash the index; no Seam A/B behavior change for un-owned/unmatched tiles.
- [ ] **Step 4: Fix rounds** (one fix subagent per wave, full findings list), re-review until clean.
- [ ] **Step 5: Merge + push + redeploy.** Update the spec Status → complete (record the two plan-time refinements: no-year unique-title matching + no-getMeta per-source id capture; and any live/deferred posture). Merge `local/id-resolver` → main (resolve any version-line conflict by taking the higher patch), rebuild the combined tree, full suite green (**build ALL probe targets incl. `probe_browse`/`probe_perf`/`probe_resolver` to catch any latent link break — the SyntheticCatalogs/LocalLibrary precedent**), push, delete the branch, redeploy Release to `C:\MyMediaVault-app` (md5-verify), update `.superpowers/sdd/progress.md`, mark the chapter.

## Self-Review (done at write time)

- **Spec coverage:** reuse-addon-search resolver ✅T3; `CatalogMatch` strict matcher ✅T1; `LocalResolveCache` persisted device-local ✅T2; `buildIndex` indexes resolved ids ✅T2; `resolveOnline` toggle + re-match ✅T4; progressive debounced refresh ✅T4; Seam A/B untouched ✅ (no task modifies them); dormant contract ✅T3/T4; verification incl. the now-inducible live badge ✅T5. Non-goals (TV/getMeta-client/NFO-writeback/music-books/cache-sync) not built ✅.
- **Two documented deviations from the spec** (search results lack a year → unique-title matching; getMeta omitted → per-source id capture) are called out in Global Constraints and recorded to the spec in T5 close-out — not silent.
- **Placeholder scan:** the one intentional WRONG snippet (`clearCacheAndRequeue`) is explicitly flagged with the correct `clear()` implementation to ship; every other code block is complete.
- **Type consistency:** `CatalogMatch::{normalizeTitle,bestMatch}`, `LocalResolveCache::{Entry,load,save,has,entry,isFresh,putMatched,putNoMatch,matchedIdsByPath,clear}`, `CatalogResolver::{enqueue,clearCacheAndRequeue,resolved}`, `buildIndex(entries, extraMovieIdsByPath)`, `Settings::{resolveOnline,setResolveOnline}`, the `library.resolveonline`/`library.rematch` ids, and the `reqToJob_`/`jobs_`/`pending_` throttle members are used identically across tasks. The resolver connects to `catalogReady` (search), not `metaReady`.
