# Comic Volume Crossing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reading to the end of a comic volume opens the next volume of that series, with the file already fetched.

**Architecture:** `ChapterRun` grows a third lane (`Catalog`) whose entries are catalog item ids. The run is captured from the issue list you drilled into, or rebuilt from the item's `parentId` when there is none. Crossing into an entry runs the file-provider search the issue row itself runs, downloads into the same SHA1-keyed cache path, and opens it — pre-fetched three pages before the boundary so the press is usually instant.

**Tech Stack:** C++17, Qt 6.8.3 (Widgets/Network/Core), MSVC (Visual Studio 18 2026, x64), Duktape-hosted JS addons, CMake, headless probe binaries as the test suite.

**Spec:** [2026-08-30-comic-volume-crossing-design.md](../specs/2026-08-30-comic-volume-crossing-design.md)

## Global Constraints

- **No AI attribution anywhere.** No `Co-Authored-By: Claude`, no "Generated with", no tool name in a commit body, PR body or issue comment. Repo root `CLAUDE.md` overrides any default that says otherwise.
- **Conventional commit prefixes** (`feat:`, `fix:`, `docs:`, `refactor:`, `test:`) and the standards in `CONTRIBUTING.md`.
- **Every source file in this repo is CRLF.** `native/tools/run-headless-probes.sh` is CRLF and `native/CMakeLists.txt` contains a lone CR. Normalising either breaks it silently. Make edits byte-exact; if your editor rewrites line endings, edit through a script that preserves them.
- **Build config is Release.** `cmake --build build --config Release --parallel --target <t>`. A Debug build is not deployable — the debug Qt DLLs are not present on this machine.
- **Worktree build configure line** (once, if `build/` is absent):
  ```
  cmake -S native -B build -G "Visual Studio 18 2026" -A x64 -DEVERYTHINGBOX_BUILD_APP=ON \
    -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64 \
    -DMPV_INCLUDE_DIR=C:/mpv-dev/include -DMPV_LIBRARY=C:/mpv-dev/libmpv.lib \
    -DSDL2_INCLUDE_DIR=C:/SDL2/include -DSDL2_LIBRARY=C:/SDL2/lib/x64/SDL2.lib \
    "-DRETROPARK_DIR=C:/Users/cubma/Project Goliath/external/RetroPark"
  ```
  `EVERYTHINGBOX_BUILD_APP=ON` is required even when you only want a probe: with `OFF` the configure succeeds and generates 2 probe targets instead of 143.
- **Running a probe needs Qt on PATH:** `PATH="/c/Qt/6.8.3/msvc2022_64/bin:$PATH"`. Exit code 127 means a missing DLL, not a test failure.
- **The whole gate** is `BUILD_DIR=build/Release QT_QPA_PLATFORM=offscreen PATH="/c/Qt/6.8.3/msvc2022_64/bin:/c/mpv-dev:$PATH" bash native/tools/run-headless-probes.sh`. It fails every probe whose binary is older than its sources, so rebuild the full CI probe list (the `Build probes` step of `.github/workflows/ci.yml`) before believing a red run.
- **Old-brand gate:** the previous product name must not appear in any new source or doc. Write `EverythingBox`. Note the app's data directory on disk still uses the old name — never paste a real cache path into a source file or test.
- **Log discipline:** a URL reaches a log only through `logSafeUrl()`; request headers only through `StreamHeaders::logSummary()`. The gate reads log statements, not intent.
- **All UI goes through the nav kit** (`src/ui/nav`). No `QDialog`, `QMessageBox`, `QInputDialog` or top-level windows. This plan adds no new UI surface; keep it that way.

---

## File Structure

| File | Responsibility | Task |
|---|---|---|
| `native/src/comic/ChapterRun.h` | Pure run value type: lanes, ordering, the provider query string | 1, 5 |
| `native/tools/probe_chapterrun.cpp` | Pins everything in `ChapterRun.h` | 1, 5 |
| `native/src/addons/AddonModels.h` / `.cpp` | `MediaItem::parentId`, `MediaItem::chapterRun`, `MediaDetail::parentId` and their JSON | 2, 3 |
| `native/addons/aiocatalog/main.js` | Comic Vine issue rows and issue meta carry `parentId` | 2 |
| `native/addon-protocol/aiocatalog-worker/src/worker.js` | The same, in the Cloudflare-worker copy | 2 |
| `native/tools/probe_addon.cpp` | `--comicorder` fake Comic Vine, extended to `parentId` | 2 |
| `native/tools/run-headless-probes.sh` | Source gate holding the worker copy to the same rule | 2 |
| `native/src/ui/HomeView.cpp` | Captures the run from the drilled-into level and puts it on the opened item | 3 |
| `native/src/core/RemoteDocCache.h` | **New.** Pure: the cache path a remote document url maps to | 4 |
| `native/tools/probe_remotedoccache.cpp` | **New.** Pins that mapping | 4 |
| `native/src/ui/MainWindow.cpp` | Arming, the Catalog crossing, the quiet fetch, the pre-fetch | 3–7 |
| `native/src/ui/MainWindow.h` | Declarations for the above | 3–7 |
| `native/CMakeLists.txt` | Registers `probe_remotedoccache` | 4 |
| `.github/workflows/ci.yml` | Builds `probe_remotedoccache` | 4 |

---

### Task 1: `ChapterRun` grows a third lane

Replaces the `bool local` two-lane switch with an enum, and gives a run the series title the Catalog lane will build queries from. Pure, so it is entirely probe-tested.

**Files:**
- Modify: `native/src/comic/ChapterRun.h:30` (the `local` member), `:150-175` (the two builders)
- Modify: `native/src/ui/MainWindow.cpp:3453` (the lane switch)
- Test: `native/tools/probe_chapterrun.cpp:199`, `:255`

**Interfaces:**
- Consumes: nothing.
- Produces: `ChapterRun::Lane { Files, Chapters, Catalog }`, `ChapterRun::lane`, `ChapterRun::seriesTitle`. `ChapterOrder::fromChapterItems` sets `Lane::Chapters`; `ChapterOrder::fromFileNames` sets `Lane::Files`. Later tasks construct `Lane::Catalog` runs by hand.

- [ ] **Step 1: Change the two existing probe assertions to the new spelling, and add the lane/seriesTitle cases**

In `native/tools/probe_chapterrun.cpp`, replace line 199 `CHECK(!run.local);` with:

```cpp
        CHECK(run.lane == ChapterRun::Lane::Chapters);
```

and line 255 `CHECK(run.local);` with:

```cpp
        CHECK(run.lane == ChapterRun::Lane::Files);
```

Then add this block immediately before the `if (failures == 0)` line at the end of `main()`:

```cpp
    // ---- The Catalog lane: catalog item ids, and the series they belong to --------------------------------
    {
        // A comic issue list, as the Reading column shows it. The titles carry a '#' marker, so the
        // reading-order rule sorts them by number and the string order (#1, #10, #2) never survives.
        QVector<ChapterRun::Entry> listed;
        listed.append({ QStringLiteral("comicvine:issue:1"), QStringLiteral("#1 — Volume 1") });
        listed.append({ QStringLiteral("comicvine:issue:10"), QStringLiteral("#10 — Volume 10") });
        listed.append({ QStringLiteral("comicvine:issue:2"), QStringLiteral("#2 — Volume 2") });
        ChapterRun run = ChapterOrder::fromChapterItems(listed, QStringLiteral("comicvine:issue:2"));
        run.lane = ChapterRun::Lane::Catalog;
        run.seriesTitle = QStringLiteral("Fairy Tail");
        CHECK(run.isValid());
        CHECK(run.lane == ChapterRun::Lane::Catalog);
        CHECK(run.seriesTitle == QStringLiteral("Fairy Tail"));
        // Reading order by hand: 1, 2, 10. Volume 2 is the middle one.
        CHECK(run.entries.value(0).title == QStringLiteral("#1 — Volume 1"));
        CHECK(run.entries.value(1).title == QStringLiteral("#2 — Volume 2"));
        CHECK(run.entries.value(2).title == QStringLiteral("#10 — Volume 10"));
        CHECK(run.index == 1);
        CHECK(run.hasNext());
        CHECK(run.entries.value(run.index + 1).id == QStringLiteral("comicvine:issue:10"));
    }
    {
        // A default-constructed run is the Files lane and has no series: every existing caller that never
        // touches these two fields keeps the behaviour it had when the flag was a bool defaulting to false.
        const ChapterRun fresh;
        CHECK(fresh.lane == ChapterRun::Lane::Files);
        CHECK(fresh.seriesTitle.isEmpty());
    }

```

- [ ] **Step 2: Run the probe build to verify it fails**

Run: `cmake --build build --config Release --parallel --target probe_chapterrun`
Expected: FAIL, `error C2039: 'Lane': is not a member of 'ChapterRun'`.

- [ ] **Step 3: Add the enum and the two fields**

In `native/src/comic/ChapterRun.h`, replace the `bool local` member (line 30) with:

```cpp
    // WHAT THE ENTRIES ARE, which is also how a boundary press opens one. Three lanes, because there are
    // three genuinely different ways a comic reaches this reader and they share nothing but the ordering.
    enum class Lane
    {
        Files,      // `id` is a path on disk — the archives of one folder. Opened synchronously.
        Chapters,   // `id` is a manga chapter item id — resolved to page images, packed into a CBZ.
        Catalog,    // `id` is a catalog item id (a comic issue) — searched for at a file provider,
                    // downloaded, then opened. See `seriesTitle`, which is half of that search.
    };
    Lane lane = Lane::Files;  // Files is the default because a run built by hand, from nothing, is a folder
                              // one — which is what `bool local = false` meant before this was an enum.
    // The container every entry belongs to, for the lanes that need to name it. Set for Catalog (the
    // provider search is "<seriesTitle> <number>"), empty elsewhere. It lives on the RUN and not on each
    // entry because it is a property of the run: the capture guards exist precisely to ensure every entry
    // in a run comes from one container.
    QString seriesTitle;
```

In `fromChapterItems`, replace `run.local = false;` with:

```cpp
        run.lane = ChapterRun::Lane::Chapters;
```

In `fromFileNames`, replace `run.local = true;` with:

```cpp
        run.lane = ChapterRun::Lane::Files;
```

- [ ] **Step 4: Update the one consumer**

In `native/src/ui/MainWindow.cpp:3453`, replace:

```cpp
    if (comicRun_.local) { openLocalChapter(target, dir); return; }
```

with:

```cpp
    if (comicRun_.lane == ChapterRun::Lane::Files) { openLocalChapter(target, dir); return; }
```

- [ ] **Step 5: Run the probe to verify it passes**

Run: `cmake --build build --config Release --parallel --target probe_chapterrun` then
`PATH="/c/Qt/6.8.3/msvc2022_64/bin:$PATH" ./build/Release/probe_chapterrun.exe`
Expected: `CHAPTERRUN-OK`, exit 0.

- [ ] **Step 6: Build the app to prove no other consumer existed**

Run: `cmake --build build --config Release --parallel --target EverythingBox`
Expected: builds clean. A `'local': is not a member` error here means a call site this task missed — fix it to the enum spelling rather than restoring the bool.

- [ ] **Step 7: Commit**

```bash
git add native/src/comic/ChapterRun.h native/src/ui/MainWindow.cpp native/tools/probe_chapterrun.cpp
git commit -m "refactor: a chapter run names its lane instead of flagging one of two"
```

---

### Task 2: An issue tells the client which series it belongs to

`parentId` across the addon boundary, filled by the Comic Vine arm at both the list and meta sites, in both copies of the addon.

**Files:**
- Modify: `native/src/addons/AddonModels.h` (`MediaItem`, `MediaDetail`), `native/src/addons/AddonModels.cpp:183` (`itemFromJson`), `:236` (`MediaDetail::fromJson`)
- Modify: `native/addons/aiocatalog/main.js` (`cvIssues`, `cvIssueMeta`)
- Modify: `native/addon-protocol/aiocatalog-worker/src/worker.js` (the same two functions)
- Modify: `native/tools/run-headless-probes.sh` (the source gate beside the `--comicorder` probe)
- Test: `native/tools/probe_addon.cpp` (the `--comicorder` arm at `:845`)

**Interfaces:**
- Consumes: nothing.
- Produces: `MediaItem::parentId` / `MediaItem::parentTitle` and `MediaDetail::parentId` / `MediaDetail::parentTitle` (all `QString`), parsed from JSON keys of the same names. The Comic Vine issue id shape is `comicvine:issue:<id>` and its parent is `comicvine:volume:<id>`.

- [ ] **Step 1: Extend the `--comicorder` probe to assert `parentId`**

Read `native/tools/probe_addon.cpp` from line 678 (the `--comicorder` header comment) through the end of that arm first — it stands a fake Comic Vine and asserts the order the addon RETURNS. Add to that arm, after its existing order assertions:

```cpp
    // Every issue row names the volume it was drilled from. Without this the reader cannot rebuild a
    // series' list from a resumed volume, and "next volume" is silent for anything opened from Recents.
    for (const MediaItem& row : issues)
    {
        CHECK(row.parentId == QStringLiteral("comicvine:volume:4050"));
        // And what the series is CALLED, because that is what a file provider is searched by. Taking it
        // from the catalog's own title instead would search for the literal word "Issues".
        CHECK(row.parentTitle == QStringLiteral("Fairy Tail"));
    }
```

The fake Comic Vine will need a `volume` object (`{ "id": 4050, "name": "Fairy Tail" }`) on the issue rows
it serves from `/issues/`, and on the single issue it serves from `/issue/`. Add it in the same shape the
live API returns.

using whatever the arm already calls its parsed issue list in place of `issues` (do not rename it), and its own `CHECK` macro spelling.

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake --build build --config Release --parallel --target probe_addon` then
`PATH="/c/Qt/6.8.3/msvc2022_64/bin:$PATH" ./build/Release/probe_addon.exe --comicorder native/addons/aiocatalog/main.js`
Expected: FAIL — `parentId` is not a member of `MediaItem` (compile), and once the field exists, an assertion failure because the addon does not set it.

- [ ] **Step 3: Add the field to both models**

In `native/src/addons/AddonModels.h`, inside `struct MediaItem`, after `expandable`:

```cpp
    // THE CONTAINER THIS ITEM BELONGS TO, as an id the same addon will answer for ("comicvine:volume:1234"
    // for an issue). Empty for everything with no meaningful parent, which is most items. It exists so a
    // leaf can be asked "what else is in your series?" WITHOUT the browse surface that listed it — which is
    // the difference between a resumed comic having a next volume and not having one.
    QString parentId;
    // ...and what that container is CALLED. It travels with the id because the two are wanted together and
    // are known together: a run rebuilt from `parentId` searches a file provider by the series NAME, and the
    // only other place to get one is the children response's own title — which is a catalog heading
    // ("Issues"), not a series. Empty whenever parentId is.
    QString parentTitle;
```

and inside `struct MediaDetail`, after `imdbStreamId`:

```cpp
    // Same meaning as MediaItem::parentId / parentTitle. A resumed item carries an id and nothing else, so
    // its parent has to come back from /meta; this is where it arrives.
    QString parentId;
    QString parentTitle;
```

In `native/src/addons/AddonModels.cpp`, in `itemFromJson` after the `expandable` line:

```cpp
    it.parentId     = o.value(QStringLiteral("parentId")).toString();
    it.parentTitle  = o.value(QStringLiteral("parentTitle")).toString();
```

and in `MediaDetail::fromJson` after the `imdbStreamId` line:

```cpp
    d.parentId    = o.value(QStringLiteral("parentId")).toString();
    d.parentTitle = o.value(QStringLiteral("parentTitle")).toString();
```

- [ ] **Step 4: Fill it in the addon**

In `native/addons/aiocatalog/main.js`, `cvIssues(volumeId, page)` needs the volume on each row, which its
request does not currently ask for. Add `volume` to its `field_list`:

```js
        "&field_list=id,name,issue_number,image,cover_date,volume"));
```

and add both fields to the pushed row object (the object with `id: "comicvine:issue:" + is.id`):

```js
            parentId: "comicvine:volume:" + volumeId,
            parentTitle: (is.volume && is.volume.name) ? is.volume.name : "",
```

`parentId` comes from the argument rather than from `is.volume.id` on purpose: it is the volume this call
was made FOR, so it is right even on a row whose `volume` object came back thin.

In `cvIssueMeta(id)`, add to the object passed to `metaResult`:

```js
        parentId: (is.volume && is.volume.id) ? ("comicvine:volume:" + is.volume.id) : "",
        parentTitle: (is.volume && is.volume.name) ? is.volume.name : "",
```

`is.volume` is already requested by that function's `field_list`, so it needs no request change.

Check `metaResult()` passes unknown keys through; if it whitelists keys, add `parentId` to the whitelist.

- [ ] **Step 5: Run the probe to verify it passes**

Run: `cmake --build build --config Release --parallel --target probe_addon` then
`PATH="/c/Qt/6.8.3/msvc2022_64/bin:$PATH" ./build/Release/probe_addon.exe --comicorder native/addons/aiocatalog/main.js`
Expected: the arm's OK sentinel, exit 0.

- [ ] **Step 6: Make the same edit in the worker copy, and gate it**

Apply the identical two edits to `native/addon-protocol/aiocatalog-worker/src/worker.js`. It cannot run under Duktape, so it is held by a source gate: find the existing gate in `native/tools/run-headless-probes.sh` that holds the worker copy to the issue-ORDER fix (added alongside `--comicorder`), and extend it — the worker's `cvIssues` must contain `parentId: "comicvine:volume:"`, `parentTitle:` and a `field_list` including `volume`, and its `cvIssueMeta` must contain `parentId:` and `parentTitle:`. Follow that gate's existing shape exactly, including its `note`/`fail` helpers.

After editing the script, run `bash -n native/tools/run-headless-probes.sh` — a merged-in gate section that eats another gate's closing `fi` is a known failure mode here and `bash -n` is what catches it.

- [ ] **Step 7: Run the gate section**

Run the whole suite (see Global Constraints). Expected: `ALL HEADLESS PROBES PASSED`, and the new gate prints PASS rather than being silently absent — confirm by grepping the run's output for the gate's own heading.

- [ ] **Step 8: Commit**

```bash
git add native/src/addons/AddonModels.h native/src/addons/AddonModels.cpp native/addons/aiocatalog/main.js native/addon-protocol/aiocatalog-worker/src/worker.js native/tools/probe_addon.cpp native/tools/run-headless-probes.sh
git commit -m "feat: a comic issue names the series it belongs to"
```

---

### Task 3: Capture the run from the issue list and arm it

The browse path. After this task, finishing a volume you opened from its issue list still does nothing at the boundary — the run is armed but the Catalog lane has no crossing yet (Task 5). What it does prove is that the right run reaches the reader.

**Files:**
- Modify: `native/src/addons/AddonModels.h` (`MediaItem::chapterRun`)
- Modify: `native/src/ui/HomeView.cpp:9556-9561` (the capture), `:7247` (the bridged open's commit lambda)
- Modify: `native/src/ui/MainWindow.cpp:16687` (the `.cbz` arm of `openLibraryItem`)

**Interfaces:**
- Consumes: `ChapterRun::Lane::Catalog`, `ChapterRun::seriesTitle` (Task 1).
- Produces: `MediaItem::chapterRun` — a `ChapterRun`, empty unless the open carried one. `MainWindow::openLibraryItem` arms it in preference to `folderRunFor()`.

- [ ] **Step 1: Give `MediaItem` the run**

`native/src/addons/AddonModels.h` gains an include beside the existing `core/` ones:

```cpp
#include "../comic/ChapterRun.h"    // MediaItem::chapterRun — the volumes either side of an opened issue
```

and, inside `struct MediaItem`, after `bookParts`:

```cpp
    // THE VOLUMES EITHER SIDE OF THIS ONE, when it was opened from a list that knew them. Empty for
    // everything else, which is what leaves every other open behaving exactly as it did.
    //
    // It rides the ITEM for the reason bookParts does, stated where that field is declared: openItem is the
    // ONE door into MainWindow::openLibraryItem, and a parallel signal carrying half the open would be a
    // second door. Never serialized — a Recent rebuilds its run from the item's parentId instead (Task 6).
    ChapterRun chapterRun;
```

Note the header comment at the top of `AddonModels.h` about include cost: `ChapterRun.h` becomes a transitive dependency of most of the tree, so expect one full rebuild after this edit.

- [ ] **Step 2: Capture comic issues in the level, with their series title**

In `native/src/ui/HomeView.cpp`, the capture at line 9556 currently reads:

```cpp
    chapterList_.clear();
    const bool oneContainer = !stack_.isEmpty() && stack_.last().detail
                              && !stack_.last().item.type.startsWith(QLatin1Char('_'));
    if (oneContainer)
        for (const MediaItem& it : items_)
            if (isReadableChapter(it.type)) chapterList_.append({ it.id, it.title });
```

Replace the loop body's test so comic issues are captured too, and remember the container's title:

```cpp
    chapterList_.clear();
    chapterSeriesTitle_.clear();
    const bool oneContainer = !stack_.isEmpty() && stack_.last().detail
                              && !stack_.last().item.type.startsWith(QLatin1Char('_'));
    if (oneContainer)
    {
        // A comic ISSUE joins manga chapters here. The structural guard above is what makes that safe: it
        // is the same guard, and it is the reason a cross-addon search level — where issues of unrelated
        // series sit together — is never remembered as a run.
        for (const MediaItem& it : items_)
            if (isReadableChapter(it.type) || it.type == QStringLiteral("comic_issue"))
                chapterList_.append({ it.id, it.title });
        // The container's own title, which the Catalog lane searches a file provider by. The level's item
        // is the series ("Fairy Tail"); its children are the volumes.
        chapterSeriesTitle_ = stack_.last().item.title;
    }
```

Declare `QString chapterSeriesTitle_;` in `native/src/ui/HomeView.h` beside `chapterList_`.

- [ ] **Step 3: Make `chapterRunFor` produce a Catalog run for a comic issue**

In `native/src/ui/HomeView.cpp:5391`, replace:

```cpp
ChapterRun HomeView::chapterRunFor(const QString& currentId) const
{
    return ChapterOrder::fromChapterItems(chapterList_, currentId);
}
```

with:

```cpp
// `catalogLane` is what the CALLER is about to open: a manga chapter resolves to page images, a comic issue
// is searched for at a file provider. The list and its order are identical either way — only the lane and
// the series name differ — so both go through one builder.
ChapterRun HomeView::chapterRunFor(const QString& currentId, bool catalogLane) const
{
    ChapterRun run = ChapterOrder::fromChapterItems(chapterList_, currentId);
    if (catalogLane)
    {
        run.lane = ChapterRun::Lane::Catalog;
        run.seriesTitle = chapterSeriesTitle_;
    }
    return run;
}
```

Update the declaration in `native/src/ui/HomeView.h` to `ChapterRun chapterRunFor(const QString& currentId, bool catalogLane = false) const;` — the default keeps the manga call site at line 7138 unchanged.

- [ ] **Step 4: Put the run on the item the bridge opens**

In `native/src/ui/HomeView.cpp`, in the `localBridge` block, capture the run next to where `queries`/`wantTitles` are built (before the resolve is fired, so browsing on during the search cannot change it):

```cpp
        // Captured NOW for the same reason the manga lane captures now: the run is "the list this issue was
        // opened from", and a search takes long enough to browse somewhere else in.
        const ChapterRun issueRun = (it.type == QStringLiteral("comic_issue"))
                                        ? chapterRunFor(it.id, /*catalogLane*/ true)
                                        : ChapterRun{};
```

Add `issueRun` to the capture list of the `*commit` lambda, and in its hit arm — the branch that builds `MediaItem m = it;` and emits `openItem(m)` — add before the emit:

```cpp
                    m.chapterRun = issueRun;   // the volumes either side of this one
```

- [ ] **Step 5: Arm the carried run instead of the folder run**

In `native/src/ui/MainWindow.cpp`, the `.cbz` arm of `openLibraryItem` (line 16687) currently reads:

```cpp
        armComicRun(folderRunFor(url)); // the archives beside this one are its chapters
```

Replace with:

```cpp
        // A run the OPEN brought with it wins over one derived from the folder: it names real neighbours
        // from the list this issue was opened from, where the folder — for a provider-fetched volume — is
        // the app's own cache and yields nothing at all (see ChapterOrder::isCachePath).
        armComicRun(item.chapterRun.isValid() ? item.chapterRun : folderRunFor(url));
```

- [ ] **Step 6: Build and run the whole gate**

Run: `cmake --build build --config Release --parallel --target EverythingBox` (expect a long rebuild — see Step 1), then the full suite.
Expected: builds clean; `ALL HEADLESS PROBES PASSED`.

- [ ] **Step 7: Commit**

```bash
git add native/src/addons/AddonModels.h native/src/ui/HomeView.h native/src/ui/HomeView.cpp native/src/ui/MainWindow.cpp
git commit -m "feat: an opened comic issue carries the volumes either side of it"
```

---

### Task 4: One name for a cached remote document, and a quiet way to fetch one

`fetchRemoteDocumentThenOpen` builds its cache path inline and can only download-then-open. The crossing needs the same path and a download that opens nothing. Extract the path rule as a pure function (so both agree by construction, which is what makes a pre-fetched file the file the open finds), and extract the streaming half of the download so the quiet fetcher does not copy it.

**Files:**
- Create: `native/src/core/RemoteDocCache.h`
- Create: `native/tools/probe_remotedoccache.cpp`
- Modify: `native/CMakeLists.txt` (register the probe), `.github/workflows/ci.yml:68` (build it), `native/tools/run-headless-probes.sh` (run it)
- Modify: `native/src/ui/MainWindow.cpp:17152` (`fetchRemoteDocumentThenOpen`), `native/src/ui/MainWindow.h`

**Interfaces:**
- Consumes: nothing.
- Produces:
  - `RemoteDocCache::dir()` → `QString`, the `remote-docs` folder (created on demand by the caller, not here).
  - `RemoteDocCache::pathFor(const QString& url, const QString& ext)` → `QString`, `<dir>/<sha1-of-url><ext>`.
  - `MainWindow::streamReplyToFile(QNetworkReply*, const QString& partPath, const QString& finalPath, std::function<void(bool ok, const QString& err)> done)` — private.
  - `MainWindow::fetchDocumentToCache(const QString& url, const StreamHeaders::Headers&, const QString& ext, std::function<void(const QString& path)> done)` — private; empty path on failure, and an immediate callback when the file is already cached.

- [ ] **Step 1: Write the failing test for the path rule**

Create `native/tools/probe_remotedoccache.cpp`:

```cpp
// Headless check of the remote-document cache naming (src/core/RemoteDocCache.h). PURE — QtCore only.
// Prints REMOTEDOCCACHE-OK on success; any failure prints REMOTEDOCCACHE-FAIL <cond> (line) and exits 1.
//
// THE BUG IT PINS: two callers now write into this cache — the open that fetches a document because you
// pressed it, and the pre-fetch that fetches the next volume before you ask for it. They are only the same
// file if they agree on its NAME, and "agree" cannot mean "both happen to build the same string inline".
#include "../src/core/RemoteDocCache.h"

#include <QCryptographicHash>
#include <QDir>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "REMOTEDOCCACHE-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

int main()
{
    const QString url = QStringLiteral("https://example.test/a/file.cbz?token=abc");
    const QString p = RemoteDocCache::pathFor(url, QStringLiteral(".cbz"));

    // The name is the SHA1 of the whole url, hex, plus the extension — and the extension is the caller's,
    // because the url's own often has none (a signed link) or the wrong one.
    const QString sha = QString::fromUtf8(
        QCryptographicHash::hash(url.toUtf8(), QCryptographicHash::Sha1).toHex());
    CHECK(QFileInfo(p).fileName() == sha + QStringLiteral(".cbz"));
    CHECK(p.startsWith(RemoteDocCache::dir()));
    CHECK(QDir::cleanPath(QFileInfo(p).absolutePath()) == QDir::cleanPath(RemoteDocCache::dir()));

    // Same url, same answer — the property the two callers depend on.
    CHECK(RemoteDocCache::pathFor(url, QStringLiteral(".cbz")) == p);
    // A different url is a different file, and a different extension is a different file.
    CHECK(RemoteDocCache::pathFor(url + QStringLiteral("x"), QStringLiteral(".cbz")) != p);
    CHECK(RemoteDocCache::pathFor(url, QStringLiteral(".epub")) != p);
    // An empty url has no cache identity at all: say so, rather than hand back the hash of "".
    CHECK(RemoteDocCache::pathFor(QString(), QStringLiteral(".cbz")).isEmpty());

    if (failures == 0) std::printf("REMOTEDOCCACHE-OK\n");
    return failures == 0 ? 0 : 1;
}
```

Register it in `native/CMakeLists.txt` beside a QtCore-only probe (copy the two-line pattern `probe_chapterrun` uses at :1009, listing `src/core/RemoteDocCache.h` as its source), add `probe_remotedoccache` to the target list on line 68 of `.github/workflows/ci.yml`, and add `"probe_remotedoccache REMOTEDOCCACHE-OK"` to the `for p in ...` list in `native/tools/run-headless-probes.sh:676`.

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake --build build --config Release --parallel --target probe_remotedoccache`
Expected: FAIL — `Cannot open include file: '../src/core/RemoteDocCache.h'`.

- [ ] **Step 3: Write the header**

Create `native/src/core/RemoteDocCache.h`:

```cpp
// WHERE A REMOTE DOCUMENT LANDS ON DISK, as a pure function of its url.
//
// A book/comic/ROM fetched from a provider is cached under a SHA1 of the url that produced it, so opening
// the same thing twice does not download it twice. That rule used to live inline in the one function that
// fetched one. It has two callers now — the open you asked for, and the pre-fetch of the next volume — and
// a pre-fetched file is only useful if the open looks for it under exactly the same name. Two inline copies
// of a hashing rule are two chances to disagree, silently, in the direction of "downloads it again".
//
// The extension is the CALLER'S, not the url's: a signed provider link routinely ends in a token, and the
// reader dispatches on the extension of the path it is handed.
//
// Pure — no I/O, no directory creation (the caller mkpath's, as it always did). Pinned by
// probe_remotedoccache.
#pragma once
#include <QCryptographicHash>
#include <QStandardPaths>
#include <QString>

namespace RemoteDocCache
{
    inline QString dir()
    {
        return QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
               + QStringLiteral("/remote-docs");
    }

    // "" for an empty url: a file with no source has no cache identity, and hashing "" would give every
    // such call the SAME name — one cache entry that different documents would overwrite in turn.
    inline QString pathFor(const QString& url, const QString& ext)
    {
        if (url.isEmpty()) return QString();
        const QString hash = QString::fromUtf8(
            QCryptographicHash::hash(url.toUtf8(), QCryptographicHash::Sha1).toHex());
        return dir() + QStringLiteral("/") + hash + ext;
    }
}
```

- [ ] **Step 4: Run it to verify it passes**

Run: `cmake --build build --config Release --parallel --target probe_remotedoccache` then
`PATH="/c/Qt/6.8.3/msvc2022_64/bin:$PATH" ./build/Release/probe_remotedoccache.exe`
Expected: `REMOTEDOCCACHE-OK`, exit 0.

- [ ] **Step 5: Use it in `fetchRemoteDocumentThenOpen`, changing nothing else**

In `native/src/ui/MainWindow.cpp:17152`, replace the four lines that build `dir`, `hash` and `localPath` with:

```cpp
    const QString dir = RemoteDocCache::dir();
    QDir().mkpath(dir);
    const QString localPath = RemoteDocCache::pathFor(item.url, ext);
```

Add `#include "../core/RemoteDocCache.h"` to the includes. The resulting path must be byte-identical to the old one — same hash of the same string, same folder — so nothing already in the cache is orphaned.

- [ ] **Step 6: Extract the streaming half of the download**

Declare in `native/src/ui/MainWindow.h` (private):

```cpp
    // Stream a reply's body to `partPath` and rename it onto `finalPath` when it arrives whole. `done`
    // reports ok=false with a sentence for every way it can fail — transport, HTTP >= 400, a write error,
    // an empty body — and the .part is removed on each. Owns nothing else: no toast, no status bar, no
    // opening. The two callers differ in all of that and in none of this.
    void streamReplyToFile(QNetworkReply* reply, const QString& partPath, const QString& finalPath,
                           std::function<void(bool ok, const QString& err)> done);
```

Move the body of the existing `finished` handler in `fetchRemoteDocumentThenOpen` (the non-curl path, from `part->write(reply->readAll());` through the rename) into it, keeping every check in the same order and with the same wording, and have `fetchRemoteDocumentThenOpen` call it — reporting through its existing toast/status-bar lines from the `done` callback. The `downloadProgress` feedback stays at the call site: it is the half that is about the user, not about the file.

- [ ] **Step 7: Add the quiet fetcher**

Declare in `native/src/ui/MainWindow.h` (private):

```cpp
    // Fetch a document into the remote-doc cache WITHOUT opening it, and without saying anything on screen.
    // `done` receives the cached path, or "" if it could not be fetched. Calls back immediately when the
    // file is already cached, so a caller never has to check first.
    //
    // This is the pre-fetch's downloader and the Catalog crossing's downloader. It deliberately shows no
    // feedback: the crossing already has a sticky notice of its own, and a pre-fetch is speculative work the
    // reader did not ask for and must not be interrupted by.
    void fetchDocumentToCache(const QString& url, const StreamHeaders::Headers& headers, const QString& ext,
                              std::function<void(const QString& path)> done);
```

Implement it in `native/src/ui/MainWindow.cpp` beside `fetchRemoteDocumentThenOpen`:

```cpp
void MainWindow::fetchDocumentToCache(const QString& url, const StreamHeaders::Headers& headers,
                                      const QString& ext, std::function<void(const QString& path)> done)
{
    const QString localPath = RemoteDocCache::pathFor(url, ext);
    if (localPath.isEmpty()) { done(QString()); return; }
    if (QFileInfo::exists(localPath) && QFileInfo(localPath).size() > 0) { done(localPath); return; }
    QDir().mkpath(RemoteDocCache::dir());

    if (!docNam_) docNam_ = new QNetworkAccessManager(this);
    const QString partPath = localPath + QStringLiteral(".part");
    mwLog(QStringLiteral("prefetch: GET %1 -> %2").arg(logSafeUrl(url), QFileInfo(localPath).fileName()));

    QNetworkRequest rq{QUrl(url)};
    rq.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(AppBrand::kUserAgent));
    QNetworkReply* reply = NetHeaderApply::get(docNam_, rq, headers, url, [this](bool allowed, const QUrl& to) {
        if (!allowed)
            mwLog(QStringLiteral("prefetch: cross-origin redirect -> %1, refusing to carry this source's "
                                 "headers there").arg(logSafeUrl(to.toString())));
    });
    streamReplyToFile(reply, partPath, localPath, [this, localPath, done](bool ok, const QString& err) {
        if (!ok) mwLog(QStringLiteral("prefetch: failed: %1").arg(err));
        done(ok ? localPath : QString());
    });
}
```

- [ ] **Step 8: Build and run the whole gate**

Run: `cmake --build build --config Release --parallel --target EverythingBox probe_remotedoccache`, then the full suite.
Expected: builds clean; `ALL HEADLESS PROBES PASSED` and `REMOTEDOCCACHE-OK` among the printed checks.

- [ ] **Step 9: Commit**

```bash
git add native/src/core/RemoteDocCache.h native/tools/probe_remotedoccache.cpp native/CMakeLists.txt .github/workflows/ci.yml native/tools/run-headless-probes.sh native/src/ui/MainWindow.h native/src/ui/MainWindow.cpp
git commit -m "refactor: one name for a cached remote document, and a fetch that opens nothing"
```

---

### Task 5: Cross into the next volume

The Catalog arm of `openRemoteChapter`. After this task the boundary press works; it is simply slow the first time, because nothing is pre-fetched yet.

**Files:**
- Modify: `native/src/comic/ChapterRun.h` (the query builder)
- Modify: `native/tools/probe_chapterrun.cpp` (its test)
- Modify: `native/src/ui/MainWindow.cpp:3453` (`openRemoteChapter`), `native/src/ui/MainWindow.h`

**Interfaces:**
- Consumes: `ChapterRun::Lane::Catalog`, `seriesTitle` (Task 1); `MainWindow::fetchDocumentToCache` (Task 4).
- Produces: `ChapterOrder::providerQuery(const QString& seriesTitle, const QString& entryTitle)` → `QString`. `MainWindow::openCatalogChapter(int targetIndex, int dir)` — private.

- [ ] **Step 1: Write the failing test for the query builder**

Add to `native/tools/probe_chapterrun.cpp`, before the `if (failures == 0)` line:

```cpp
    // ---- The provider query a Catalog crossing searches with ----------------------------------------------
    {
        // The series plus the issue NUMBER, which is what the row press builds by hand today. The entry
        // title is a display string ("#3 — Volume 3") and searching a provider with it finds nothing.
        CHECK(ChapterOrder::providerQuery(QStringLiteral("Fairy Tail"), QStringLiteral("#3 — Volume 3"))
              == QStringLiteral("Fairy Tail 3"));
        CHECK(ChapterOrder::providerQuery(QStringLiteral("Fairy Tail"), QStringLiteral("#12"))
              == QStringLiteral("Fairy Tail 12"));
        // A decimal issue keeps its decimal: 12.5 is a real issue and "12" is a different one.
        CHECK(ChapterOrder::providerQuery(QStringLiteral("Saga"), QStringLiteral("Ch. 12.5"))
              == QStringLiteral("Saga 12.5"));
        // No number in the title: search the series and the title, because dropping the title would search
        // for the series alone and open whatever came back — a one-shot or an annual, confidently wrong.
        CHECK(ChapterOrder::providerQuery(QStringLiteral("Saga"), QStringLiteral("Special"))
              == QStringLiteral("Saga Special"));
        // No series (a run built without one): the entry title is all there is.
        CHECK(ChapterOrder::providerQuery(QString(), QStringLiteral("#3 — Volume 3"))
              == QStringLiteral("#3 — Volume 3"));
        // Nothing at all in, nothing out — the caller must not search for "".
        CHECK(ChapterOrder::providerQuery(QString(), QString()).isEmpty());
    }

```

- [ ] **Step 2: Run it to verify it fails**

Run: `cmake --build build --config Release --parallel --target probe_chapterrun`
Expected: FAIL, `error C2039: 'providerQuery': is not a member of 'ChapterOrder'`.

- [ ] **Step 3: Write the query builder**

In `native/src/comic/ChapterRun.h`, inside `namespace ChapterOrder`, after `chapterNumber`:

```cpp
    // WHAT TO ASK A FILE PROVIDER FOR, for one entry of a Catalog run. The row press builds this string by
    // hand from the series it drilled into and the number in the title; a crossing has to build the same
    // one, or "next volume" searches for something the row press would never have searched for.
    //
    // The NUMBER, not the title. An entry title is written for a human ("#3 — Volume 3") and a provider
    // search on it finds nothing. When no number parses, the title goes in whole rather than being dropped:
    // searching for the series alone would return SOME copy of SOME issue, and the crossing would open it.
    inline QString providerQuery(const QString& seriesTitle, const QString& entryTitle)
    {
        bool ok = false;
        const double n = chapterNumber(entryTitle, &ok);
        // %g so 12.5 stays "12.5" and 3 stays "3" rather than becoming "3.000000".
        const QString tail = ok ? QString::number(n, 'g', 10) : entryTitle.trimmed();
        if (seriesTitle.trimmed().isEmpty()) return tail;
        if (tail.isEmpty()) return seriesTitle.trimmed();
        return seriesTitle.trimmed() + QLatin1Char(' ') + tail;
    }
```

- [ ] **Step 4: Run it to verify it passes**

Run: `cmake --build build --config Release --parallel --target probe_chapterrun` then
`PATH="/c/Qt/6.8.3/msvc2022_64/bin:$PATH" ./build/Release/probe_chapterrun.exe`
Expected: `CHAPTERRUN-OK`, exit 0.

- [ ] **Step 5: Route the boundary press to a Catalog lane**

In `native/src/ui/MainWindow.cpp`, `onChapterAdvanceRequested`, replace the lane dispatch:

```cpp
    if (comicRun_.lane == ChapterRun::Lane::Files) { openLocalChapter(target, dir); return; }
```

with:

```cpp
    if (comicRun_.lane == ChapterRun::Lane::Files)   { openLocalChapter(target, dir); return; }
    if (comicRun_.lane == ChapterRun::Lane::Catalog) { openCatalogChapter(target, dir); return; }
```

- [ ] **Step 6: Write the Catalog crossing**

Declare `void openCatalogChapter(int targetIndex, int dir);` in `native/src/ui/MainWindow.h` beside `openRemoteChapter`, and implement it in `native/src/ui/MainWindow.cpp` immediately after `openRemoteChapter`:

```cpp
// The catalog lane: the next VOLUME of this series, which is a file somebody else is holding. Two async
// steps where the manga lane has one — find a copy, then fetch it — under the same latch, the same sticky
// notice and the same generation tag, because the wait is longer and every reason those exist is stronger
// here. The notice stays up across both steps: they are one wait as far as the reader is concerned.
void MainWindow::openCatalogChapter(int targetIndex, int dir)
{
    const ChapterRun::Entry entry = comicRun_.entries[targetIndex];
    ChapterRun run = comicRun_;
    run.index = targetIndex;

    const QString query = ChapterOrder::providerQuery(comicRun_.seriesTitle, entry.title);
    if (query.isEmpty())   // nothing to search for: refuse rather than search for everything
    {
        notify(tr("Can't work out what to look for after “%1”.").arg(entry.title), kFeedbackLong);
        return;
    }

    chapterHandoffPending_ = true;
    const int gen = chapterHandoffGen_;
    notify(tr("Loading “%1”…").arg(entry.title), 0);   // sticky: this can take a while
    mwLog(QStringLiteral("chapter: catalog advance (%1) -> \"%2\"").arg(dir).arg(entry.title));

    // A copy already fetched (by an earlier read, or by the pre-fetch that ran three pages ago) skips
    // straight to the open — which is the whole point of the pre-fetch.
    if (!prefetchedPath_.isEmpty() && prefetchedKey_ == entry.id
        && QFileInfo(prefetchedPath_).size() > 0)
    {
        openCrossedVolume(prefetchedPath_, entry.title, run, dir, gen);
        return;
    }

    addons_->resolveDocumentByQuery(query, comicRun_.seriesTitle, QStringLiteral("comic"),
                                    [this, gen, entry, run, dir](const AddonManager::DocFind& found) {
        if (!chapterHandoffStillOurs(gen)) return;   // superseded, or the reader is gone — already compensated
        if (!found.providerError.isEmpty())
        {
            chapterHandoffPending_ = false;
            notify(tr("Can't reach the file provider: %1.").arg(found.providerError), kFeedbackLong);
            return;
        }
        if (found.url.isEmpty())
        {
            chapterHandoffPending_ = false;
            notify(tr("No copies of “%1” were found.").arg(entry.title), kFeedbackLong);
            return;   // stay on the last page; the volume list is one Back away
        }
        const QString ext = QStringLiteral(".cbz");
        fetchDocumentToCache(found.url, {}, ext, [this, gen, entry, run, dir](const QString& path) {
            if (!chapterHandoffStillOurs(gen)) return;
            if (path.isEmpty())
            {
                chapterHandoffPending_ = false;
                notify(tr("Couldn't download “%1”.").arg(entry.title), kFeedbackLong);
                return;
            }
            openCrossedVolume(path, entry.title, run, dir, gen);
        });
    });
}
```

Add the shared ending as a private helper `void openCrossedVolume(const QString& path, const QString& title, const ChapterRun& run, int dir, int gen);` whose body is the `openCbz` lambda from `openImagePages` with `cbzPath` replaced by `path` and `landOnLastPage` by `dir < 0` — open, arm, land on the last page when crossing backwards, `partPlaybackForReader()`, persist the other readers, `presentComic()`, arrival toast, log. Declare `prefetchedKey_`/`prefetchedPath_` as `QString` members in `native/src/ui/MainWindow.h` (Task 7 fills them; here they are simply always empty).

- [ ] **Step 7: Build and run the whole gate**

Run: `cmake --build build --config Release --parallel --target EverythingBox`, then the full suite.
Expected: builds clean; `ALL HEADLESS PROBES PASSED`.

- [ ] **Step 8: Commit**

```bash
git add native/src/comic/ChapterRun.h native/tools/probe_chapterrun.cpp native/src/ui/MainWindow.h native/src/ui/MainWindow.cpp
git commit -m "feat: page past the end of a comic volume and the next one opens"
```

---

### Task 6: Rebuild the run for a volume opened from Recents

**Files:**
- Modify: `native/src/ui/MainWindow.cpp` (`recordDocument` at `:16999`, the `.cbz` arm at `:16687`), `native/src/ui/MainWindow.h`

**Interfaces:**
- Consumes: `MediaItem::parentId`, `MediaDetail::parentId` (Task 2); `ChapterRun::Lane::Catalog` (Task 1).
- Produces: `MainWindow::rebuildCatalogRun(const MediaItem& item)` — private; arms a run asynchronously or does nothing.

- [ ] **Step 1: Record the addon on a document Recent**

In `native/src/ui/MainWindow.cpp`, the `recordDocument` lambda currently reads:

```cpp
    auto recordDocument = [&] {
        const QString t = item.title.isEmpty() ? QFileInfo(url).completeBaseName() : item.title;
        RecentStore::add({ url, t, QStringLiteral("document"), item.thumbnailUrl, item.id });
    };
```

Extend the row with the source addon:

```cpp
    auto recordDocument = [&] {
        const QString t = item.title.isEmpty() ? QFileInfo(url).completeBaseName() : item.title;
        RecentItem row{ url, t, QStringLiteral("document"), item.thumbnailUrl, item.id };
        // WHO TO ASK ABOUT THIS ITEM LATER, which a document row has never carried. applyRemintRecipe writes
        // a source only for a row whose path is a network link, and a cached comic's path is a file — so a
        // resumed volume had an item id and nobody to ask it of.
        //
        // This is NOT a re-mint recipe and must not be read as one: the direct route needs sourceRoute and
        // sourceType as well, both left empty here, so reopenFor still refuses and replays the path exactly
        // as it does today. #224's "all three fields or none" rule is about those three; this is not one.
        row.sourceAddonId = item.sourceAddonId;
        RecentStore::add(row);
    };
```

Check `RecentItem`'s aggregate initialiser order in `native/src/core/RecentStore.h` and match it; if the five-field brace-init no longer compiles, assign the fields by name instead.

- [ ] **Step 2: Ask for the siblings when the open brought no run**

Declare `void rebuildCatalogRun(const MediaItem& item);` in `native/src/ui/MainWindow.h` and implement it in `native/src/ui/MainWindow.cpp` beside `armComicRun`:

```cpp
// A volume resumed from Recents has an id, an addon and no list. Ask what series it belongs to, then ask
// that series for its volumes, and arm the run when the answers land. Both calls are spent at OPEN time —
// there is a volume of reading between them and the boundary they serve — which is what keeps a crossing
// free of the round trip the predecessor spec rejected it for.
//
// Every ending is silent. This is speculative work the user did not ask for: if the addon is gone, the id
// no longer resolves, or the reader has moved on, the boundary press stays the no-op it already was.
void MainWindow::rebuildCatalogRun(const MediaItem& item)
{
    if (item.type != QStringLiteral("comic_issue") && item.parentId.isEmpty()) return;
    LoadedAddon* src = addons_ ? addons_->sourceById(item.sourceAddonId) : nullptr;
    if (!src) return;

    const QString openKey = comic_ ? comic_->itemKey() : QString();
    // The siblings of a known parent, and the run they make.
    auto askChildren = [this, src, item, openKey](const QString& parentId, const QString& seriesTitle) {
        MediaItem parent;
        parent.id = parentId;
        parent.type = QStringLiteral("comic");
        parent.expandable = true;
        const int req = addons_->requestDetail(src, parent, 1);
        auto* conn = new QMetaObject::Connection;
        *conn = connect(addons_, &AddonManager::catalogReady, this,
                        [this, req, item, openKey, seriesTitle, conn](int id, const MediaCatalog& cat) {
            if (id != req) return;
            disconnect(*conn); delete conn;
            // The reader must still be showing the comic this was asked for: arming a run onto a reader the
            // user has left is exactly what chapterHandoffStillOurs prevents for the crossing's own steps.
            if (!comicOnScreen() || !comic_ || comic_->itemKey() != openKey) return;
            QVector<ChapterRun::Entry> listed;
            for (const MediaItem& child : cat.items)
                if (child.type == QStringLiteral("comic_issue")) listed.append({ child.id, child.title });
            if (listed.size() < 2) return;   // a series of one is not a run
            ChapterRun run = ChapterOrder::fromChapterItems(listed, item.id);
            if (!run.isValid()) return;      // this volume is not in its own series' list: say nothing
            run.lane = ChapterRun::Lane::Catalog;
            // The SERIES name, which arrived with the parent id — never cat.title, which is the catalog
            // heading the addon puts on a children response ("Issues") and would search a file provider
            // for a series by that name.
            run.seriesTitle = seriesTitle;
            if (run.seriesTitle.isEmpty()) return;   // nothing to search by: leave the press the no-op it was
            armComicRun(run);
        });
    };

    if (!item.parentId.isEmpty()) { askChildren(item.parentId, item.parentTitle); return; }

    // No parent on the item (a Recent): /meta knows it.
    const int metaReq = addons_->requestMeta(src, item);
    auto* mconn = new QMetaObject::Connection;
    *mconn = connect(addons_, &AddonManager::metaReady, this,
                     [this, metaReq, askChildren, openKey, mconn](int id, const MediaDetail& d) {
        if (id != metaReq) return;
        disconnect(*mconn); delete mconn;
        if (!comicOnScreen() || !comic_ || comic_->itemKey() != openKey) return;
        if (d.parentId.isEmpty()) return;
        askChildren(d.parentId, d.parentTitle);
    });
}
```

Note what is NOT used here: `cat.title`. A children response's title is the addon's heading for the list — the Comic Vine arm returns the literal `"Issues"` — and a run carrying that as its series name would search a file provider for "Issues 3". `parentTitle` (Task 2) exists precisely so this does not have to guess.

- [ ] **Step 3: Call it when the open carried no run**

In the `.cbz` arm of `openLibraryItem`, extend the arming line from Task 3:

```cpp
        if (item.chapterRun.isValid()) armComicRun(item.chapterRun);
        else { armComicRun(folderRunFor(url)); rebuildCatalogRun(item); }
```

- [ ] **Step 4: Build and run the whole gate**

Run: `cmake --build build --config Release --parallel --target EverythingBox`, then the full suite.
Expected: builds clean; `ALL HEADLESS PROBES PASSED`.

- [ ] **Step 5: Commit**

```bash
git add native/src/ui/MainWindow.h native/src/ui/MainWindow.cpp
git commit -m "feat: a resumed comic volume asks its series what comes next"
```

---

### Task 7: Pre-fetch the next volume three pages out

**Files:**
- Modify: `native/src/ui/MainWindow.cpp` (connect near `:1946`, the pre-fetch itself), `native/src/ui/MainWindow.h`

**Interfaces:**
- Consumes: `fetchDocumentToCache` (Task 4); `prefetchedKey_` / `prefetchedPath_` (Task 5).
- Produces: nothing later tasks build on.

- [ ] **Step 1: Connect the page signal**

In `native/src/ui/MainWindow.cpp`, beside the existing comic connections at line 1946:

```cpp
    connect(comic_, &ComicView::pageInfoChanged, this, &MainWindow::onComicPageChanged);
```

- [ ] **Step 2: Write the trigger**

Declare in `native/src/ui/MainWindow.h` (private): `void onComicPageChanged();`, `void prefetchNextVolume();`, and

```cpp
    // How many pages before the end the next volume starts being fetched. Three, not one: a provider search
    // has a budget of tens of seconds and a volume is several megabytes, so starting on the LAST page means
    // the boundary press still waits — which is the whole thing this is here to prevent. Named because it is
    // the one number in this feature worth arguing about.
    static constexpr int kPrefetchLead = 3;
    // `prefetchedKey_` and `prefetchedPath_` already exist from Task 5 — do not declare them again. This
    // task adds only the once-per-volume latch:
    QString prefetchStartedFor_; // the entry id a fetch is already running or finished for
```

Implement in `native/src/ui/MainWindow.cpp` beside `onComicReachedLastPage`:

```cpp
// Every page turn in the comic reader arrives here. It does nothing at all except decide whether the next
// volume is close enough to be worth fetching — the reader itself knows nothing about runs, providers or
// caches, and this keeps it that way.
void MainWindow::onComicPageChanged()
{
    if (!comic_ || comicRun_.lane != ChapterRun::Lane::Catalog || !comicRun_.hasNext()) return;
    if (comic_->itemKey() != comicRunKey_) return;   // the run belongs to a comic that is no longer open
    const int total = comic_->pageCount();
    if (total <= 0 || comic_->currentPage() < total - kPrefetchLead) return;
    prefetchNextVolume();
}

// Fetch the next volume's FILE, quietly, before anybody asks for it. One volume ahead, one attempt per
// volume, forward only — the three rules that keep this from becoming a downloader.
//
// It is never cancelled by the reader leaving. The bytes are a file in a cache; they are just as useful the
// next time this series is opened, and abandoning a half-written download is how .part files accumulate.
// What IS abandoned when the user moves on is the OPENING, which the crossing's generation tag governs.
void MainWindow::prefetchNextVolume()
{
    const ChapterRun::Entry next = comicRun_.entries[comicRun_.index + 1];
    if (prefetchStartedFor_ == next.id) return;      // already running, or already done, for this one
    prefetchStartedFor_ = next.id;

    const QString query = ChapterOrder::providerQuery(comicRun_.seriesTitle, next.title);
    if (query.isEmpty()) return;
    mwLog(QStringLiteral("prefetch: looking ahead to \"%1\"").arg(next.title));

    addons_->resolveDocumentByQuery(query, comicRun_.seriesTitle, QStringLiteral("comic"),
                                    [this, next](const AddonManager::DocFind& found) {
        if (found.url.isEmpty()) return;             // silent: nobody asked for this
        fetchDocumentToCache(found.url, {}, QStringLiteral(".cbz"), [this, next](const QString& path) {
            if (path.isEmpty()) return;
            // Publish it only if the run it belongs to is still the one open. A reader who left mid-fetch
            // gets the file in the cache and no stale pointer to it.
            if (comicRun_.lane != ChapterRun::Lane::Catalog || !comicRun_.hasNext()
                || comicRun_.entries[comicRun_.index + 1].id != next.id) return;
            prefetchedKey_ = next.id;
            prefetchedPath_ = path;
            mwLog(QStringLiteral("prefetch: \"%1\" is ready").arg(next.title));
        });
    });
}
```

- [ ] **Step 3: Clear the pre-fetch state when a run is armed**

In `MainWindow::armComicRun` (`:3308`), beside the existing state it resets, add:

```cpp
    // A new run means the previous run's look-ahead is about a volume that is no longer next.
    prefetchedKey_.clear();
    prefetchedPath_.clear();
    prefetchStartedFor_.clear();
```

- [ ] **Step 4: Build and run the whole gate**

Run: `cmake --build build --config Release --parallel --target EverythingBox`, then the full suite.
Expected: builds clean; `ALL HEADLESS PROBES PASSED`.

- [ ] **Step 5: Commit**

```bash
git add native/src/ui/MainWindow.h native/src/ui/MainWindow.cpp
git commit -m "feat: the next volume is fetched three pages before you need it"
```

---

### Task 8: Drive it in the real app, twice

No headless probe reaches the crossing. This task is the only evidence the feature works, and it crosses TWO boundaries because an operation that works exactly once is the failure this class of feature produces (see the #224 re-mint work).

**Files:**
- Modify: none necessarily — this is a drive plus whatever it finds.

- [ ] **Step 1: Deploy the build under a private name**

Another session may be running `EverythingBox.exe` from a build tree and kills by process name before launching its own. Copy the exe to a different name in the same folder so a kill-by-name sweep misses it:

```bash
cp build/Release/EverythingBox.exe build/Release/EBVolCross.exe
```

- [ ] **Step 2: Drive it**

Launch with `EB_UITEST=1` and a private pipe name (`EB_UITEST_PIPE=EB-volcross`) and drive with `native/tools/uitest.py` (`key` / `state` / `shot`). The drive:

1. Open the Reading column, drill into a comic series with at least three volumes.
2. Open volume 1. Assert the reader is showing it (`state`).
3. Page to the last page. Assert a `prefetch: "…" is ready` line appears in the log before the boundary press.
4. Press forward. Assert the reader is showing volume 2 — not an error toast, and not still volume 1.
5. Page to the last page of volume 2 and press forward again. Assert volume 3.
6. Press back on volume 3's first page. Assert volume 2, showing its LAST page.

- [ ] **Step 3: Repeat from Recents**

Close the app, relaunch, open the same volume from the Reading/Recents row rather than from the series list, and repeat steps 3–4. This is the Task 6 path and it is the one with two round trips behind it; a run that never arms shows up here as a silent boundary press.

- [ ] **Step 4: Delete the private copy**

```bash
rm build/Release/EBVolCross.exe
```

It sits inside the folder the exe-folder contamination gate fingerprints; leaving it there fails the gate.

- [ ] **Step 5: Commit any fixes the drive found, then update the spec's status**

Append a short "What the drive found" section to the spec with anything the live run contradicted. A spec that describes a design nobody has run is worth less than one that records where it was wrong.

```bash
git add -A
git commit -m "docs: record what driving the comic-volume crossing found"
```

---

## Self-Review

**Spec coverage:**

| Spec section | Task |
|---|---|
| §1 `MediaItem::parentId` (+ `parentTitle`, see below) + addon + worker copy + gate | 2 |
| §1 the source addon on a document Recent | 6 (Step 1) |
| §2 `ChapterRun` third lane + `seriesTitle` | 1 |
| §3 capture from the drilled-into level | 3 |
| §3 re-fetch by `parentId`, meta hop for a Recent | 6 |
| §3 gated on the reader still showing the comic | 6 (Step 2) |
| §4 Catalog crossing, same cache path, failure wording | 4, 5 |
| §5 pre-fetch, lead of three, the six rules | 7 |
| Cache-folder guard unchanged | 3 (Step 5) — `folderRunFor` still the fallback, still guarded |
| Testing: probe_chapterrun, probe_addon, query builder, EB_UITEST twice | 1, 2, 5, 8 |

**One addition to the spec:** the spec describes `parentId` alone. Building the plan showed that a run
rebuilt from it has no honest source for the series NAME — the children response's title is the addon's
heading ("Issues"), and the provider search is `"<series> <number>"`. So `parentTitle` travels beside
`parentId` at every site: one extra string in fields already being added, one extra key in a `field_list`
already being requested. Without it the resumed-volume lane searches for the wrong thing every time.

**Known gap, deliberately left:** the spec's `.cbz` assumption. `openCatalogChapter` and `prefetchNextVolume` both hardcode `.cbz` as the extension, where `fetchRemoteDocumentThenOpen` derives one from the resolved url and mime. A provider that answers with a `.cb7`, `.cbt` or `.pdf` copy will be cached under the wrong extension and refused by the reader. Task 5 Step 6 and Task 7 Step 2 should derive the extension the same way the existing bridge does if that derivation is a callable function; if it is not, extract it, and if extracting it turns out to be more than a few lines, stop and raise it rather than widening this plan silently.
