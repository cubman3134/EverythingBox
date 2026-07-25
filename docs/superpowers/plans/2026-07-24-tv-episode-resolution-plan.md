# TV / Episode ID-Resolution Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extend the movies-only id-resolver to TV: resolve a local show once to the catalog's series tile id, compose per-episode catalog ids deterministically, and index them so a series tile badges "On disk (N)" and owned episodes prefer-local from the series detail — on aiocatalog (TMDB) and non-NFO'd shows, not only NFO'd Cinemeta shows.

**Architecture:** The resolver searches once per distinct show (grouped from scanned episodes) → `CatalogMatch::bestSeriesMatch` → caches the show's series-tile id(s). `LocalLibrary::buildIndex` then COMPOSES each owned episode's catalog id from the cached series-tile id (`composeEpisodeId`) — so new episodes of a resolved show need no network — and indexes `seriesCount[seriesTileId]` (badge) + `pathById[composedEpisodeId] = episodePath` (prefer-local). Seam A/B and the matching-safety model are untouched.

**Tech Stack:** Qt 6.8.3, the shipped resolver (`CatalogMatch`/`LocalResolveCache`/`CatalogResolver`), `OwnedIndex`, headless probes.

## Global Constraints

- **Branch:** `local/tv-resolution` off main. Standing autonomy through the merge gate. The pre-commit hook auto-bumps the patch version — expected; never hand-edit version lines.
- **Scope:** TV/show resolution only. No TMDB/TVDB client, no series-detail drilling (composition chosen), no getMeta year-verification, no music/books/NFO-writeback/cache-sync. Non-goals per spec. Seam A/B, addon transport, sync transport unchanged.
- **ANCHOR ON FUNCTION NAMES.** Current code (main@a9c1610):

| Concern | Anchor |
|---|---|
| Series/episode id shapes (verified) | aiocatalog series `tmdb:tv:{N}` (`main.js:84`), episode `tmdb:episode:{N}:{s}:{e}` where `{N}`=bare numeric (`main.js:158`); Cinemeta series bare `tt…`, episode `tt…:{s}:{e}` (`AddonManager.cpp:384/436`). All unpadded. |
| Current buildIndex episode branch | `LocalLibrary.cpp` `buildIndex` — `else if (e.kind == Kind::Episode && !e.seriesImdbId.isEmpty()) { epKey = seriesImdbId+":"+num(season)+":"+num(episode); if(!pathById.contains) { insert; seriesCount[seriesImdbId]+=1; } }` |
| VideoEntry fields | `LocalLibrary.h`: `show`, `seriesImdbId`, `season`, `episode`, `kind` |
| CatalogMatch | `CatalogMatch.{h,cpp}`: `normalizeTitle`, `bestMatch(VideoEntry, candidates)` (movies) |
| Resolver Job + enqueue/startJob/onCatalogReady/finishJob | `CatalogResolver.{h,cpp}` — `Job{ id, movie, size, mtime, outstanding, matchedIds, issued, timer }`; enqueue skips `kind != Movie` (`:38`); `isMovieCatalogSource` (`:24`); startJob searches `job->movie.title` (`:74`); onCatalogReady runs `bestMatch` (`:93`); finishJob `putMatched(job->movie.path,…)` / `putNoMatch` (only when `issued && outstanding.isEmpty()`) |
| Cache | `LocalResolveCache.{h,cpp}`: `Entry{size,mtime,ids,matched,ts}`, `byPath_`, `isFresh`, `putMatched`, `putNoMatch`, `matchedIdsByPath`, `clear` |
| MainWindow wiring | `rescanLocalLibrary` passes `resolveCache_->matchedIdsByPath()` → `buildIndex(scanFolder(root), extra)`; the `resolved()` lambda does the same; `resolver_->enqueue(LocalLibrary::index().all())` post-install |
| Seam A / Seam B (untouched) | `browseItems` `ownsId(it.id)`/`ownedEpisodes(it.id)` badge; `resolvePlay`/`openLibraryItem`/`playThemedLeaf` `localPathFor(it.id)` then `localPathFor(it.imdbStreamId)` |

- **Env recipe:** PATH prepend `/c/Qt/6.8.3/msvc2022_64/bin` + `/c/mpv-dev`; build dir `build` (qt.conf, no `QT_PLUGIN_PATH`). **Harness runs RELEASE — build `--config Release`.** App target `mymediavault`. **Build hygiene (a prior implementer stalled):** build ONLY named targets (never a target-less `cmake --build build`); when you ADD a source file, regenerate ONCE with `cmake -S native -B build -DMYMEDIAVAULT_BUILD_APP=ON` (no `-A`); if a build runs >5 min report BLOCKED. Suite: `BUILD_DIR=build bash native/tools/run-headless-probes.sh`.
- **Cinemeta+NFO shows already work** — the resolved keys are ADDED first-wins alongside the existing `seriesImdbId` keys; never remove or regress those. Additive only.

---

### Task 1: pure cores — show matcher + episode-id compose + buildIndex TV keys

**Files:**
- Modify: `native/src/core/CatalogMatch.h` / `.cpp` (`bestSeriesMatch`)
- Modify: `native/src/core/LocalLibrary.h` / `.cpp` (`composeEpisodeId`, `showKeyFor`, `buildIndex` 3rd arg)
- Modify: `native/tools/probe_resolver.cpp` (bestSeriesMatch assertions), `native/tools/probe_locallib.cpp` (composeEpisodeId + buildIndex-TV assertions)

**Interfaces (Produces):**
```cpp
// CatalogMatch.h
namespace CatalogMatch {
    // Index of the accepted SERIES candidate, or -1. type must be series/tv; unique normalized show title;
    // exact seriesImdbId match wins outright; a tt candidate contradicting a non-empty seriesImdbId is skipped.
    int bestSeriesMatch(const QString& showTitle, const QString& seriesImdbId, const QVector<MediaItem>& candidates);
}
// LocalLibrary.h  (in namespace LocalLibrary)
    QString composeEpisodeId(const QString& seriesTileId, int season, int episode); // tmdb:tv:N→tmdb:episode:N:s:e; tt→tt:s:e; else ""
    QString showKeyFor(const VideoEntry& e);                                        // seriesImdbId else "name:"+lower(show)
    OwnedIndex buildIndex(const QVector<VideoEntry>& entries,
                          const QHash<QString, QStringList>& extraMovieIdsByPath = {},
                          const QHash<QString, QStringList>& seriesTileIdsByShow = {});
```

- [ ] **Step 1: RED — add probe assertions.** In `native/tools/probe_resolver.cpp` (after the movie `bestMatch` block), add and `#include`s as needed:
```cpp
    // bestSeriesMatch: series/tv type filter + unique title + tt cross-check + contradicted-tt skip.
    {
        QVector<MediaItem> c{ mi("tmdb:tv:1396","Breaking Bad","series"), mi("tmdb:movie:1","Breaking Bad","movie") };
        CHECK(CatalogMatch::bestSeriesMatch("Breaking Bad", QString(), c) == 0);          // picks the series, not the movie
        QVector<MediaItem> c2{ mi("tt0903747","Breaking Bad","series") };
        CHECK(CatalogMatch::bestSeriesMatch("breaking bad", "tt0903747", c2) == 0);       // exact tt wins
        QVector<MediaItem> c3{ mi("tt9999999","Breaking Bad","series") };
        CHECK(CatalogMatch::bestSeriesMatch("Breaking Bad", "tt0903747", c3) == -1);      // contradicted tt skipped
        QVector<MediaItem> c4{ mi("tmdb:tv:1","The Office","series"), mi("tmdb:tv:2","The Office","series") };
        CHECK(CatalogMatch::bestSeriesMatch("The Office", QString(), c4) == -1);          // ambiguous → -1
        QVector<MediaItem> c5{ mi("tmdb:movie:1","Fargo","movie") };
        CHECK(CatalogMatch::bestSeriesMatch("Fargo", QString(), c5) == -1);               // no series candidate
    }
```
In `native/tools/probe_locallib.cpp` (after the existing buildIndex assertions), add:
```cpp
    // composeEpisodeId: aiocatalog tmdb + Cinemeta tt shapes, unpadded; unknown shape → empty.
    CHECK(LocalLibrary::composeEpisodeId("tmdb:tv:1396", 1, 2) == QStringLiteral("tmdb:episode:1396:1:2"));
    CHECK(LocalLibrary::composeEpisodeId("tt0903747", 2, 5) == QStringLiteral("tt0903747:2:5"));
    CHECK(LocalLibrary::composeEpisodeId("kitsu:42", 1, 1).isEmpty());
    // buildIndex composes resolved episode keys + badges the resolved series tile with the owned count.
    {
        LocalLibrary::VideoEntry e1; e1.kind = LocalLibrary::Kind::Episode; e1.path = "/tv/S01E01.mkv";
        e1.show = "Breaking Bad"; e1.seriesImdbId = "tt0903747"; e1.season = 1; e1.episode = 1;
        LocalLibrary::VideoEntry e2 = e1; e2.path = "/tv/S01E02.mkv"; e2.episode = 2;
        QHash<QString, QStringList> byShow; byShow.insert("tt0903747", { "tmdb:tv:1396", "tt0903747" });
        const LocalLibrary::OwnedIndex idx = LocalLibrary::buildIndex({ e1, e2 }, {}, byShow);
        CHECK(idx.localPathFor("tmdb:episode:1396:1:2") == QStringLiteral("/tv/S01E02.mkv"));  // composed tmdb ep
        CHECK(idx.localPathFor("tt0903747:1:1") == QStringLiteral("/tv/S01E01.mkv"));          // existing tt ep
        CHECK(idx.ownsId("tmdb:tv:1396"));                                                     // resolved series tile
        CHECK(idx.ownedEpisodes("tmdb:tv:1396") == 2);                                         // owned count on the tmdb tile
        CHECK(idx.ownedEpisodes("tt0903747") == 2);                                            // and on the tt series id
    }
    // A show with NO seriesImdbId still composes via the name-key group.
    {
        LocalLibrary::VideoEntry n; n.kind = LocalLibrary::Kind::Episode; n.path = "/tv/x.mkv";
        n.show = "The Wire"; n.season = 1; n.episode = 3;  // seriesImdbId empty
        QHash<QString, QStringList> byShow; byShow.insert(LocalLibrary::showKeyFor(n), { "tmdb:tv:1438" });
        const LocalLibrary::OwnedIndex idx = LocalLibrary::buildIndex({ n }, {}, byShow);
        CHECK(idx.localPathFor("tmdb:episode:1438:1:3") == QStringLiteral("/tv/x.mkv"));
        CHECK(idx.ownedEpisodes("tmdb:tv:1438") == 1);
    }
```
Build both probes → they FAIL (undefined `bestSeriesMatch`/`composeEpisodeId`/`showKeyFor`, buildIndex arity).

- [ ] **Step 2: Implement `CatalogMatch::bestSeriesMatch`.** In `CatalogMatch.h` add the declaration (beside `bestMatch`). In `CatalogMatch.cpp` add:
```cpp
int bestSeriesMatch(const QString& showTitle, const QString& seriesImdbId, const QVector<MediaItem>& candidates)
{
    if (!seriesImdbId.isEmpty())
        for (int i = 0; i < candidates.size(); ++i)
            if (candidates[i].id.compare(seriesImdbId, Qt::CaseInsensitive) == 0) return i;

    const QString wt = normalizeTitle(showTitle);
    if (wt.isEmpty()) return -1;

    int hit = -1;
    for (int i = 0; i < candidates.size(); ++i)
    {
        const MediaItem& c = candidates[i];
        if (c.type != QStringLiteral("series") && c.type != QStringLiteral("tv")) continue; // series only
        if (!seriesImdbId.isEmpty()
            && c.id.startsWith(QStringLiteral("tt"), Qt::CaseInsensitive)
            && c.id.compare(seriesImdbId, Qt::CaseInsensitive) != 0) continue;              // contradicted tt
        if (normalizeTitle(c.title) != wt) continue;
        if (hit != -1) return -1;                                                           // ambiguous
        hit = i;
    }
    return hit;
}
```

- [ ] **Step 3: Implement `LocalLibrary::composeEpisodeId` + `showKeyFor`.** In `LocalLibrary.h` add both declarations (in the namespace). In `LocalLibrary.cpp` add:
```cpp
QString composeEpisodeId(const QString& seriesTileId, int season, int episode)
{
    if (seriesTileId.startsWith(QStringLiteral("tmdb:tv:")))
        return QStringLiteral("tmdb:episode:") + seriesTileId.mid(8)   // strip "tmdb:tv:" → bare numeric id
             + QStringLiteral(":") + QString::number(season) + QStringLiteral(":") + QString::number(episode);
    if (seriesTileId.startsWith(QStringLiteral("tt")))
        return seriesTileId + QStringLiteral(":") + QString::number(season) + QStringLiteral(":") + QString::number(episode);
    return QString();   // unknown catalog shape → no episode key (series tile still badges via seriesCount)
}

QString showKeyFor(const VideoEntry& e)
{
    return !e.seriesImdbId.isEmpty() ? e.seriesImdbId : (QStringLiteral("name:") + e.show.toLower());
}
```

- [ ] **Step 4: Extend `buildIndex`.** In `LocalLibrary.h` change the declaration to the 3-arg form above. In `LocalLibrary.cpp`, change the episode branch from `else if (e.kind == Kind::Episode && !e.seriesImdbId.isEmpty())` to `else if (e.kind == Kind::Episode)` and restructure so the existing keying is guarded inside AND the resolved keys are composed:
```cpp
        else if (e.kind == Kind::Episode)
        {
            if (!e.seriesImdbId.isEmpty())                                   // existing NFO/Cinemeta keying (unchanged)
            {
                const QString epKey = e.seriesImdbId + QStringLiteral(":")
                                    + QString::number(e.season) + QStringLiteral(":") + QString::number(e.episode);
                if (!idx.pathById.contains(epKey))
                {
                    idx.pathById.insert(epKey, e.path);
                    idx.seriesCount[e.seriesImdbId] += 1;
                }
            }
            // Resolved catalog series tiles (this track): compose each owned episode's catalog id and index it,
            // and badge the resolved series tile (e.g. tmdb:tv:N) with the owned-episode count.
            const QString sk = showKeyFor(e);
            for (const QString& seriesTileId : seriesTileIdsByShow.value(sk))
            {
                const QString cid = composeEpisodeId(seriesTileId, e.season, e.episode);
                if (cid.isEmpty() || idx.pathById.contains(cid)) continue;   // unknown shape or dup → skip
                idx.pathById.insert(cid, e.path);
                idx.seriesCount[seriesTileId] += 1;                          // distinct owned episode → count once
            }
        }
```
(Keep the movie branch and its `extraMovieIdsByPath` loop exactly as shipped. Confirm existing 1- and 2-arg callers still compile — the 3rd arg is defaulted.)

- [ ] **Step 5: Wire CMake if needed + build GREEN.** `bestSeriesMatch`/`composeEpisodeId`/`showKeyFor` live in existing TUs (CatalogMatch.cpp, LocalLibrary.cpp) already linked by both probes — no CMake change. Build:
```bash
export PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH"
cmake --build build --target probe_resolver probe_locallib --config Release --parallel
BUILD_DIR=build bash native/tools/run-headless-probes.sh   # RESOLVER-OK + LOCALLIB-OK + ALL HEADLESS PROBES PASSED
```

- [ ] **Step 6: Commit.**
```bash
git add native/src/core/CatalogMatch.h native/src/core/CatalogMatch.cpp native/src/core/LocalLibrary.h native/src/core/LocalLibrary.cpp native/tools/probe_resolver.cpp native/tools/probe_locallib.cpp
git commit -m "feat: series matcher + episode-id compose + buildIndex TV keys (tv-resolution T1)"
```

---

### Task 2: LocalResolveCache show-level store

**Files:** Modify `native/src/core/LocalResolveCache.h` / `.cpp`; `native/tools/probe_resolver.cpp` (show-store assertions).

**Interfaces (Produces):**
```cpp
// LocalResolveCache
    bool isShowFresh(const QString& showKey, qint64 nowSecs, qint64 retryDays = 14) const;
    void putShowMatched(const QString& showKey, const QStringList& seriesTileIds, qint64 nowSecs);
    void putShowNoMatch(const QString& showKey, qint64 nowSecs);
    QHash<QString, QStringList> seriesIdsByShow() const;   // matched shows only
```

- [ ] **Step 1: RED — probe assertions** (in `probe_resolver.cpp`, using the existing `QTemporaryDir` cache):
```cpp
    {
        LocalResolveCache c(cachePath); c.load();
        CHECK(!c.isShowFresh("tt0903747", now));
        c.putShowMatched("tt0903747", { "tmdb:tv:1396", "tt0903747" }, now);
        c.putShowNoMatch("name:the wire", now);
        CHECK(c.isShowFresh("tt0903747", now));                                  // matched
        CHECK(c.isShowFresh("name:the wire", now));                              // nomatch within window
        CHECK(!c.isShowFresh("name:the wire", now + 15LL*86400));                // nomatch past 14d → stale
        c.save();
    }
    {
        LocalResolveCache c(cachePath); c.load();                               // round-trip
        CHECK(c.seriesIdsByShow().value("tt0903747").contains("tmdb:tv:1396"));
        CHECK(!c.seriesIdsByShow().contains("name:the wire"));                   // nomatch not in the snapshot
        c.clear();
        CHECK(!c.isShowFresh("tt0903747", now));                                // clear() drops shows too
    }
```
Build probe_resolver → FAIL (methods undefined).

- [ ] **Step 2: Implement.** In `LocalResolveCache.h` add a `ShowEntry` + `byShow_` and the four methods:
```cpp
    struct ShowEntry { QStringList ids; bool matched = false; qint64 ts = 0; };
    // ... (after the existing public methods) ...
    bool isShowFresh(const QString& showKey, qint64 nowSecs, qint64 retryDays = 14) const;
    void putShowMatched(const QString& showKey, const QStringList& seriesTileIds, qint64 nowSecs);
    void putShowNoMatch(const QString& showKey, qint64 nowSecs);
    QHash<QString, QStringList> seriesIdsByShow() const;
```
Add to `private:` `QHash<QString, ShowEntry> byShow_;`. Change `clear()` to also clear shows:
```cpp
    void clear() { byPath_.clear(); byShow_.clear(); save(); }
```
In `LocalResolveCache.cpp`:
```cpp
bool LocalResolveCache::isShowFresh(const QString& showKey, qint64 nowSecs, qint64 retryDays) const
{
    const auto it = byShow_.constFind(showKey);
    if (it == byShow_.constEnd()) return false;
    if (it.value().matched) return true;                       // a resolved show never expires (until re-match)
    return (nowSecs - it.value().ts) < retryDays * 86400;      // a nomatch is fresh only within the retry window
}
void LocalResolveCache::putShowMatched(const QString& showKey, const QStringList& ids, qint64 nowSecs)
{ byShow_.insert(showKey, ShowEntry{ ids, true, nowSecs }); }
void LocalResolveCache::putShowNoMatch(const QString& showKey, qint64 nowSecs)
{ byShow_.insert(showKey, ShowEntry{ {}, false, nowSecs }); }
QHash<QString, QStringList> LocalResolveCache::seriesIdsByShow() const
{
    QHash<QString, QStringList> out;
    for (auto it = byShow_.constBegin(); it != byShow_.constEnd(); ++it)
        if (it.value().matched && !it.value().ids.isEmpty()) out.insert(it.key(), it.value().ids);
    return out;
}
```
Extend `load()`/`save()` to persist `byShow_` under a top-level `"shows"` object (keep the existing per-path entries at the top level, OR move both under `"paths"`/`"shows"` — SIMPLER: nest both, so wrap the existing path map under a `"paths"` key and add a `"shows"` key; update `load()` to read `root["paths"]`/`root["shows"]` with a fallback to treat a legacy flat object as `paths` for backward compat). Show JSON per entry: `{ ids:[…], matched:bool, ts:num }`.

- [ ] **Step 3: Build GREEN + suite.** `cmake --build build --target probe_resolver --config Release --parallel` → `RESOLVER-OK`; full suite green.

- [ ] **Step 4: Commit.**
```bash
git add native/src/core/LocalResolveCache.h native/src/core/LocalResolveCache.cpp native/tools/probe_resolver.cpp
git commit -m "feat: LocalResolveCache show-level store (tv-resolution T2)"
```

---

### Task 3: CatalogResolver group-by-show + show jobs + MainWindow wiring

**Files:** Modify `native/src/core/CatalogResolver.h` / `.cpp`; `native/src/ui/MainWindow.cpp` (pass `seriesIdsByShow()` to both `buildIndex` rebuilds).

**Interfaces:** Consumes T1 (`bestSeriesMatch`, `showKeyFor`), T2 (show-store). Produces: the resolver now resolves shows; `MainWindow` feeds `seriesTileIdsByShow` into `buildIndex`.

- [ ] **Step 1: Extend `Job` + source filter (`CatalogResolver.h`/`.cpp`).** In `Job` add show fields:
```cpp
    struct Job {
        quint64 id = 0;
        bool isShow = false;
        LocalLibrary::VideoEntry movie;             // movie jobs
        QString showKey, showTitle, seriesImdbId;   // show jobs
        qint64 size = 0, mtime = 0;
        QSet<int> outstanding;
        QStringList matchedIds;
        bool issued = false;
        QTimer* timer = nullptr;
    };
```
In `CatalogResolver.cpp` add a series-source predicate beside `isMovieCatalogSource`:
```cpp
static bool isSeriesCatalogSource(AddonManager* m, LoadedAddon* s)
{
    if (!s || !s->isMediaSource()) return false;
    for (const AddonCatalog& c : m->catalogs(s))
        if (c.type == QStringLiteral("series") || c.type == QStringLiteral("tv") || c.type == QStringLiteral("mixed"))
            return true;
    return false;
}
```

- [ ] **Step 2: `enqueue` — also group episodes into show jobs.** Replace the movies-only loop so it keeps movie jobs AND builds one job per distinct not-fresh show. Add `#include "CatalogMatch.h"` is NOT needed (show grouping uses `LocalLibrary::showKeyFor`). New `enqueue`:
```cpp
void CatalogResolver::enqueue(const QVector<LocalLibrary::VideoEntry>& entries)
{
    if (!Settings::resolveOnline()) return;
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    QSet<QString> showsThisCall;   // dedup distinct shows within this enqueue
    for (const LocalLibrary::VideoEntry& e : entries)
    {
        if (e.kind == LocalLibrary::Kind::Movie)
        {
            if (seen_.contains(e.path)) continue;
            const QFileInfo fi(e.path);
            const qint64 size = fi.size();
            const qint64 mtime = fi.lastModified().toSecsSinceEpoch();
            if (cache_->isFresh(e.path, size, mtime, now)) continue;
            seen_.insert(e.path);
            auto job = QSharedPointer<Job>::create();
            job->id = nextId_++; job->movie = e; job->size = size; job->mtime = mtime;
            pending_.append(job);
        }
        else if (e.kind == LocalLibrary::Kind::Episode)
        {
            const QString sk = LocalLibrary::showKeyFor(e);
            const QString seenKey = QStringLiteral("show:") + sk;   // disjoint from movie paths
            if (showsThisCall.contains(sk) || seen_.contains(seenKey)) continue;
            if (cache_->isShowFresh(sk, now)) continue;             // resolved / nomatch-in-window
            showsThisCall.insert(sk); seen_.insert(seenKey);
            auto job = QSharedPointer<Job>::create();
            job->id = nextId_++; job->isShow = true; job->showKey = sk;
            job->showTitle = e.show; job->seriesImdbId = e.seriesImdbId;
            pending_.append(job);
        }
    }
    pump();
}
```

- [ ] **Step 3: `startJob` — search the right sources + title.** Branch on `isShow`:
```cpp
void CatalogResolver::startJob(const QSharedPointer<Job>& job)
{
    jobs_.insert(job->id, job);
    const QString query = job->isShow ? job->showTitle : job->movie.title;
    for (LoadedAddon* s : addons_->sources())
    {
        const bool ok = job->isShow ? isSeriesCatalogSource(addons_, s) : isMovieCatalogSource(addons_, s);
        if (!ok) continue;
        const int reqId = addons_->requestSearch(s, query);
        if (reqId >= 0) { job->issued = true; job->outstanding.insert(reqId); reqToJob_.insert(reqId, job->id); }
    }
    if (job->outstanding.isEmpty()) { const quint64 id = job->id; QTimer::singleShot(0, this, [this, id]{ finishJob(id); }); return; }
    job->timer = new QTimer(this); job->timer->setSingleShot(true);
    const quint64 id = job->id;
    connect(job->timer, &QTimer::timeout, this, [this, id]{ finishJob(id); });
    job->timer->start(12000);
}
```

- [ ] **Step 4: `onCatalogReady` — pick the matcher by kind.** Change the match line:
```cpp
    const int idx = job->isShow
        ? CatalogMatch::bestSeriesMatch(job->showTitle, job->seriesImdbId, catalog.items)
        : CatalogMatch::bestMatch(job->movie, catalog.items);
    if (idx >= 0) { const QString id = catalog.items[idx].id; if (!id.isEmpty() && !job->matchedIds.contains(id)) job->matchedIds << id; }
```

- [ ] **Step 5: `finishJob` — record show vs movie.** Branch the cache writes:
```cpp
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    if (!job->matchedIds.isEmpty()) {
        if (job->isShow) cache_->putShowMatched(job->showKey, job->matchedIds, now);
        else             cache_->putMatched(job->movie.path, job->size, job->mtime, job->matchedIds, now);
        cacheDirty_ = true;
    }
    else if (job->issued && job->outstanding.isEmpty()) {
        if (job->isShow) cache_->putShowNoMatch(job->showKey, now);
        else             cache_->putNoMatch(job->movie.path, job->size, job->mtime, now);
        cacheDirty_ = true;
    }
```
(Keep the timer cleanup + lingering-reqToJob_ removal + `scheduleResolvedSignal()` + `pump()` exactly as shipped.)

- [ ] **Step 6: MainWindow — feed `seriesTileIdsByShow` into both rebuilds.** In `native/src/ui/MainWindow.cpp`, in BOTH `rescanLocalLibrary()` and the `resolved()` lambda, capture the show snapshot on the main thread and pass it as `buildIndex`'s 3rd arg. For each site, change:
```cpp
    const QHash<QString, QStringList> extra = resolveCache_ ? resolveCache_->matchedIdsByPath()
                                                            : QHash<QString, QStringList>{};
    const QHash<QString, QStringList> shows = resolveCache_ ? resolveCache_->seriesIdsByShow()
                                                            : QHash<QString, QStringList>{};
    // ... in the worker lambda capture [libRoot, extra, shows] ...
    w->setFuture(QtConcurrent::run([libRoot, extra, shows] {
        return LocalLibrary::buildIndex(LocalLibrary::scanFolder(libRoot), extra, shows);
    }));
```
(Both by value → thread-safe. The `gen == libScanGen_` guard on `rescan` and the read-gen guard on `resolved()` are unchanged.)

- [ ] **Step 7: Build + suite.** New source? No (all edits to existing files). Build:
```bash
cmake --build build --target mymediavault probe_resolver --config Release --parallel
BUILD_DIR=build bash native/tools/run-headless-probes.sh   # ALL HEADLESS PROBES PASSED
```

- [ ] **Step 8: Commit.**
```bash
git add native/src/core/CatalogResolver.h native/src/core/CatalogResolver.cpp native/src/ui/MainWindow.cpp
git commit -m "feat: CatalogResolver group-by-show + show jobs + wiring (tv-resolution T3)"
```

---

### Task 4: close-out — live verify + fable + merge

- [ ] **Step 1: Live verify (portable throwaway, aiocatalog).** Copy the deployed data dir (aiocatalog + TMDB key), cloud-stripped; real app untouched; `MMV_UITEST` + `native/tools/uitest.py`. Seed `library/folder` with a fixture SHOW: `Breaking Bad/Season 01/Breaking Bad - S01E01.mkv` + `S01E02.mkv` (tiny; no tvshow.nfo). Launch → let the resolver search aiocatalog for "Breaking Bad" → `...\scratchpad\...\localresolve.json` `shows` gains `matched:true` + a `tmdb:tv:{id}`. Then browse aiocatalog to the Breaking Bad **series tile** → verify **"On disk (2)" badge** → `tv-badge.png`; drill into the series → season → the **owned episodes badge + prefer-local** (activating an owned episode opens the on-disk file, not a stream) → `tv-episode-play.png`. Negative: a non-owned show shows no badge. If the two-level aiocatalog drill or a matching tile can't be reached, record honestly + rely on the probes (matcher + compose + buildIndex are pure-covered). Do NOT fabricate screenshots.
- [ ] **Step 2: Perf sanity.** Resolution is off the render/hot path (show searches + debounced rebuild; Seam A/B O(1)). Confirm no lag in the live pass; a one-line note suffices unless lag is seen (then a 3-run baseline).
- [ ] **Step 3: Fable review.** `scripts/review-package $(git merge-base main HEAD) HEAD`, most-capable model. Dimensions: the compose table correctness (aiocatalog `tmdb:tv:N`→`tmdb:episode:N:s:e` bare-numeric strip; Cinemeta `tt`→`tt:s:e`; unknown→empty, series still badges); buildIndex additivity (existing `seriesImdbId` keys + movie `extraMovieIdsByPath` untouched; resolved keys first-wins; per-distinct-episode count); `bestSeriesMatch` never mis-badges (type filter, unique title, contradicted-tt skip, ambiguous→−1); the resolver's show-grouping + `show:`-prefixed `seen_` dedup (no collision with movie paths); show jobs vs movie jobs share the throttle/timeout/non-collision machinery without regression; the show-store timeout-safe nomatch (same rule as movies); MainWindow passes `shows` by value to both rebuilds (thread-safe, gen-guarded); Cinemeta+NFO shows unregressed; Seam A/B untouched. Fix rounds → merge.
- [ ] **Step 4: Merge + push + redeploy.** Spec Status → complete (live result recorded). Merge `local/tv-resolution` → main (resolve any version-line conflict by taking the higher patch), rebuild the combined tree, full suite green (**build all probe targets incl. probe_browse/probe_perf/probe_resolver/probe_importers/probe_locallib** to catch any latent link break), push, delete the branch, redeploy Release to `C:\MyMediaVault-app` (md5-verify), update `.superpowers/sdd/progress.md`, mark the chapter.

## Self-Review (done at write time)

- **Spec coverage:** compose-not-drill ✅T1 (composeEpisodeId); show matcher ✅T1; buildIndex series-tile→count + episode-id→path ✅T1; group-by-show ✅T3; show-level cache ✅T2; MainWindow wiring ✅T3; live (aiocatalog series badge + episode prefer-local) ✅T4; Cinemeta+NFO unregressed ✅ (existing keys kept, additive). Non-goals (drill/TMDB-client/year-verify/music/books) not built ✅.
- **Placeholder scan:** every code step carries full code; the `load()`/`save()` nesting is described with the exact JSON shape + backward-compat fallback (not a placeholder — a concrete instruction).
- **Type consistency:** `bestSeriesMatch(showTitle, seriesImdbId, candidates)`, `composeEpisodeId(seriesTileId, season, episode)`, `showKeyFor(entry)`, `buildIndex(entries, extraMovieIdsByPath, seriesTileIdsByShow)`, cache `isShowFresh/putShowMatched/putShowNoMatch/seriesIdsByShow`, `Job{isShow,showKey,showTitle,seriesImdbId}`, `isSeriesCatalogSource` — all consistent across T1-T3. The `showKeyFor` grouping key is identical in the resolver (T3) and buildIndex (T1), so the cache's `seriesIdsByShow` keys line up with buildIndex's lookups.
- **Ambiguity resolved:** compose lives in `LocalLibrary` (clean link graph, probe_locallib coverage); the resolver resolves the SHOW once and buildIndex composes per-episode (so new episodes need no network); `seen_` uses a `show:` prefix to keep show/movie dedup disjoint.
