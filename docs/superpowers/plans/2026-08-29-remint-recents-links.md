# Re-mint a Recents link instead of replaying it — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A Continue Watching row for a debrid-backed movie or audiobook re-mints its stream link when opened, instead of replaying a dead one (#224).

**Architecture:** A Recents row gains four durable, credential-free fields naming the *source* it came from. `MainWindow::openRecent` looks the row up, and when it carries that recipe it calls `AddonManager::resolveStream`/`resolveStreamByImdb` for a fresh URL rather than replaying `RecentItem.path`. The routing decision is a pure function in `RecentStore`, mirroring the existing `relaunchFor` dispatch table, so a headless probe gates it.

**Tech Stack:** C++17, Qt 6 (QtCore/QtNetwork/QtGui), CMake, headless probe binaries under `native/tools/`.

## Global Constraints

- **No credential may be written to a synced store.** #200's invariant: a value in `recent/` must contain no query string. The four new fields are ids, never links. `probe_cloudmerge` §38 (Task 3) is what holds this.
- **Never persist `proxyHeaders`.** #59. `MediaItem.requestHeaders` stays unserialized; re-minting produces fresh headers from the resolve callback.
- **No AI attribution in commits.** No `Co-Authored-By`, no generated-by footer (repo `CLAUDE.md`).
- **Cite the right issue.** The merge carries `Fixes #224` and no other issue number.
- **A new probe registers in three places** or it silently never runs (`CONTRIBUTING.md:399`): `native/CMakeLists.txt`, the runner loop in `native/tools/run-headless-probes.sh`, and the `--target` list in `.github/workflows/ci.yml`. This plan adds **no** new probe — it extends `probe_importers` and `probe_cloudmerge`, which are already registered in all three — so that rule is satisfied by not triggering it.
- **`native/tools/run-headless-probes.sh` is CRLF** and `native/CMakeLists.txt` contains a lone CR. Never normalize line endings in either; edit bytes in place.
- **The working tree is shared with concurrent sessions.** Always `git commit -- <explicit paths>`. Never `git commit -a`. A version-bump hook will add `native/CMakeLists.txt` and `native/src/main.cpp` to every commit; that is expected and correct.

## Spec

`docs/superpowers/specs/2026-08-29-remint-recents-links-design.md`

## File Structure

| File | Responsibility | Change |
|---|---|---|
| `native/src/core/RecentStore.h` | The row shape + the two pure dispatch tables | Modify: 4 fields, `Reopen` enum, `reopenFor`, `find` |
| `native/src/core/RecentStore.cpp` | JSON round-trip, de-dup, cap, scrub | Modify: serialize/read the 4 fields, implement `reopenFor`/`find` |
| `native/src/core/CloudMerge.cpp` | Cross-device union of the recents list | Modify: one stale comment; assert pass-through |
| `native/src/ui/MainWindow.cpp` | Play sites (write the recipe) and `openRecent` (consume it) | Modify: 3 write sites, 1 read site |
| `native/src/ui/MainWindow.h` | `openRecent` + the re-mint helper decl | Modify: one new private method |
| `native/tools/probe_importers.cpp` | Pins `RecentStore`'s pure dispatch tables | Modify: assertions for `reopenFor` + round-trip |
| `native/tools/probe_cloudmerge.cpp` | Pins the #200 credential invariant | Modify: new §38 |

`MainWindow.cpp` is already the build-time ceiling (#186) and this plan adds ~90 lines to it. Splitting it is explicitly out of scope here — #186 owns that, and doing it under a bug fix would bury this change in a 20,000-line move diff.

---

### Task 1: The row carries its recipe

**Files:**
- Modify: `native/src/core/RecentStore.h:32-54` (the `RecentItem` struct), `:57-62` (the namespace)
- Modify: `native/src/core/RecentStore.cpp:63-79` (`saveList`), `:80-101` (`list`)
- Test: `native/tools/probe_importers.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: `RecentItem::sourceAddonId`, `::sourceItemId`, `::sourceRoute`, `::sourceType` (all `QString`); `RecentStore::find(const QString& pathOrKey) -> RecentItem` (a default-constructed item, i.e. `path.isEmpty()`, on miss). Tasks 2, 4, 5 and 6 all depend on these exact names.

- [ ] **Step 1: Write the failing test**

Append to `native/tools/probe_importers.cpp`, immediately after the existing `relaunchFor` block that ends at line 210:

```cpp
    // ---- #224: a Recents row carries the recipe to re-mint its link -------------------------------------
    //
    // The four fields are ids, never links: an addon manifest id, an item id, and two enum-ish strings. None
    // may ever hold a url with a query — that is #200's invariant and probe_cloudmerge §38 is what holds it
    // across the sync boundary. Here we only pin that they round-trip.
    {
        RecentStore::clear();
        RecentItem in;
        in.path  = QStringLiteral("https://store-034.example/dld/6f1e/movie.mkv");
        in.title = QStringLiteral("A Film");
        in.kind  = QStringLiteral("video");
        in.key   = QStringLiteral("eyJ0IjoiQSBGaWxtIiwiaCI6ImRlYWRiZWVm");
        in.sourceAddonId = QStringLiteral("com.example.allarr");
        in.sourceItemId  = QStringLiteral("eyJ0IjoiQSBGaWxtIiwiaCI6ImRlYWRiZWVm");
        in.sourceRoute   = QStringLiteral("direct");
        in.sourceType    = QStringLiteral("movie");
        RecentStore::add(in);

        const QVector<RecentItem> got = RecentStore::list();
        CHECK(got.size() == 1);
        CHECK(got[0].sourceAddonId == QStringLiteral("com.example.allarr"));
        CHECK(got[0].sourceItemId  == QStringLiteral("eyJ0IjoiQSBGaWxtIiwiaCI6ImRlYWRiZWVm"));
        CHECK(got[0].sourceRoute   == QStringLiteral("direct"));
        CHECK(got[0].sourceType    == QStringLiteral("movie"));

        // find() by either identity. openRecent has the path and the resume key and nothing else, so this is
        // the lookup that lets it reach the recipe without widening HomeView's openRecent signal.
        CHECK(RecentStore::find(in.key).sourceAddonId == QStringLiteral("com.example.allarr"));
        CHECK(RecentStore::find(in.path).sourceAddonId == QStringLiteral("com.example.allarr"));
        CHECK(RecentStore::find(QStringLiteral("nothing-here")).path.isEmpty());

        // A LEGACY ROW — written before this change — reads back with the four fields empty and is not
        // corrupted by their absence. This is the assertion that stops the fix from eating existing recents.
        RecentStore::clear();
        RecentItem legacy;
        legacy.path = QStringLiteral("C:\\Users\\me\\Videos\\old.mkv");
        legacy.kind = QStringLiteral("video");
        RecentStore::add(legacy);
        const QVector<RecentItem> old = RecentStore::list();
        CHECK(old.size() == 1);
        CHECK(old[0].sourceAddonId.isEmpty());
        CHECK(old[0].sourceRoute.isEmpty());
        CHECK(old[0].path == QStringLiteral("C:\\Users\\me\\Videos\\old.mkv"));
        RecentStore::clear();
    }
```

- [ ] **Step 2: Run it to verify it fails**

```bash
cmake --build build --target probe_importers 2>&1 | tail -20
```

Expected: compile FAILS with `'sourceAddonId': is not a member of 'RecentItem'`.

- [ ] **Step 3: Add the fields**

In `native/src/core/RecentStore.h`, after the `ts` member (line 54), inside `struct RecentItem`:

```cpp
    // ---- #224: the recipe for MINTING this row's link, never the link itself ----------------------------
    //
    // A debrid stream url is signed and short-lived, and since #200 the stored `path` has had its query —
    // i.e. its credential — removed before it ever reaches the ini. Replaying it therefore cannot work, and
    // never could for longer than the signing window. These four fields are what a re-open needs to ask the
    // SOURCE for a new link instead, and they are chosen so that none of them is a secret: an addon manifest
    // id, an item id, and two closed vocabularies. The item id for a file provider is the base64url release
    // blob (engine-side IndexerSearchSource::EncodeId), which deliberately omits the indexer's API key and
    // carries the release's info hash — so re-minting returns the IDENTICAL file and the resume position
    // stays exact. Empty on every row written before this existed, and on every row whose source cannot
    // re-resolve (local files, pasted links, Subsonic/Jellyfin/IPTV); those keep replaying `path`.
    QString sourceAddonId; // the resolving addon's manifest id (AddonManager::sourceById)
    QString sourceItemId;  // "meta:<blob>" release id, or an IMDB stream id ("tt123" / "ttShow:s:e")
    QString sourceRoute;   // "direct" -> resolveStream(addon,item) | "imdb" -> resolveStreamByImdb(type,id)
    QString sourceType;    // the MediaItem type the resolve needs ("movie", "series", "audiobook", …)
```

`sourceRoute` is recorded rather than inferred from the id's shape: a future id format that happened to start with `tt` would otherwise route silently to the wrong resolver.

In the `namespace RecentStore` block, after `remove`:

```cpp
    // The row filed under `pathOrKey` (its key when it has one, else its path — identOf's rule, the same
    // identity remove() matches on). A default-constructed RecentItem (path empty) when nothing matches.
    // openRecent uses this to reach a row's re-mint recipe: HomeView::openRecent hands over the path, kind,
    // resume key, title and thumb, and widening that signal to carry four more strings would push source
    // routing into three unrelated HomeView call sites. The lookup costs one already-cached ini read.
    RecentItem find(const QString& pathOrKey);
```

- [ ] **Step 4: Serialize them**

In `native/src/core/RecentStore.cpp`, inside `saveList`'s loop, after the `system` line (line 73):

```cpp
        if (!it.sourceAddonId.isEmpty()) o.insert(QStringLiteral("saddon"), it.sourceAddonId);
        if (!it.sourceItemId.isEmpty())  o.insert(QStringLiteral("sitem"),  it.sourceItemId);
        if (!it.sourceRoute.isEmpty())   o.insert(QStringLiteral("sroute"), it.sourceRoute);
        if (!it.sourceType.isEmpty())    o.insert(QStringLiteral("stype"),  it.sourceType);
```

Short json names because this list is capped at 40 rows and rides the CloudMerge document as a single string value; the long names would add roughly 1 KB of key text per full list for nothing. Written only when non-empty, matching how `thumb`/`key`/`system` already behave, so a legacy row's bytes do not grow.

In `list()`, after the `system` line (line 95):

```cpp
        it.sourceAddonId = o.value(QStringLiteral("saddon")).toString();
        it.sourceItemId  = o.value(QStringLiteral("sitem")).toString();
        it.sourceRoute   = o.value(QStringLiteral("sroute")).toString();
        it.sourceType    = o.value(QStringLiteral("stype")).toString();
```

- [ ] **Step 5: Implement `find`**

In `native/src/core/RecentStore.cpp`, after `RecentStore::remove`'s definition:

```cpp
RecentItem RecentStore::find(const QString& pathOrKey)
{
    if (pathOrKey.isEmpty()) return {};
    for (const RecentItem& it : list())
        if (it.key == pathOrKey || samePathAs(it.path, pathOrKey)) return it;
    return {};
}
```

`samePathAs` (not `==`) for the path arm, for the reason already recorded above it: one file reaches this store both as the platform spells it and as JSON stored it, so the separators and the case can differ.

- [ ] **Step 6: Run the probe to verify it passes**

```bash
cmake --build build --target probe_importers 2>&1 | tail -5 && ./build/probe_importers
```

Expected: `IMPORTERS-OK`, exit 0.

- [ ] **Step 7: Commit**

```bash
git commit -m "feat: a Recents row records the source that can re-mint its link" -- native/src/core/RecentStore.h native/src/core/RecentStore.cpp native/tools/probe_importers.cpp
```

---

### Task 2: The pure re-open dispatch

**Files:**
- Modify: `native/src/core/RecentStore.h` (namespace block), `native/src/core/RecentStore.cpp`
- Test: `native/tools/probe_importers.cpp`

**Interfaces:**
- Consumes: `RecentItem`'s four fields from Task 1.
- Produces: `enum class RecentStore::Reopen { ReplayPath, ResolveDirect, ResolveImdb, SourceMissing };` and `Reopen reopenFor(const RecentItem& it, bool addonAvailable);`. Task 5 switches on exactly this enum.

This exists as a pure function for the same reason `relaunchFor` does: the app's `openRecent` switch and the headless probe then share one definition of the routing, and a mutation to it is killable without a window.

- [ ] **Step 1: Write the failing test**

Append to `native/tools/probe_importers.cpp`, after the Task 1 block:

```cpp
    // ---- #224: the re-open routing table --------------------------------------------------------------
    {
        using RO = RecentStore::Reopen;
        RecentItem bare;                                  // a local file / legacy row: no recipe at all
        bare.path = QStringLiteral("C:\\x\\y.mkv");
        CHECK(RecentStore::reopenFor(bare, false) == RO::ReplayPath);
        CHECK(RecentStore::reopenFor(bare, true)  == RO::ReplayPath); // an installed addon is irrelevant here

        RecentItem direct;
        direct.path = QStringLiteral("https://h.example/dld/6f1e/m.mkv");
        direct.sourceAddonId = QStringLiteral("com.example.allarr");
        direct.sourceItemId  = QStringLiteral("eyJ0IjoiQSBGaWxt");
        direct.sourceRoute   = QStringLiteral("direct");
        direct.sourceType    = QStringLiteral("movie");
        CHECK(RecentStore::reopenFor(direct, true)  == RO::ResolveDirect);
        // The addon this row names is not installed on THIS device. #77 (roster sync) is open, so a row that
        // synced from another device can legitimately name one that is absent — a defined degradation with
        // its own message, NOT a silent fall back to replaying a link that cannot work.
        CHECK(RecentStore::reopenFor(direct, false) == RO::SourceMissing);

        RecentItem imdb = direct;
        imdb.sourceRoute  = QStringLiteral("imdb");
        imdb.sourceItemId = QStringLiteral("tt0111161");
        CHECK(RecentStore::reopenFor(imdb, true) == RO::ResolveImdb);
        // The imdb route resolves across every installed stream provider rather than one named addon, so a
        // missing named addon does not disqualify it.
        CHECK(RecentStore::reopenFor(imdb, false) == RO::ResolveImdb);

        // A HALF-WRITTEN RECIPE IS NOT A RECIPE. A row with a route but no item id (a truncated peer blob, a
        // hand-edited ini) must fall back to today's behaviour rather than calling resolve with an empty id,
        // which every provider answers with "no source" — a dead end wearing a different message.
        RecentItem partial = direct;
        partial.sourceItemId.clear();
        CHECK(RecentStore::reopenFor(partial, true) == RO::ReplayPath);
        RecentItem noRoute = direct;
        noRoute.sourceRoute.clear();
        CHECK(RecentStore::reopenFor(noRoute, true) == RO::ReplayPath);
        // An UNKNOWN route string — a row written by a newer build than this one — replays rather than
        // guessing. Forward compatibility costs one comparison here and a wrong guess costs a 403.
        RecentItem future = direct;
        future.sourceRoute = QStringLiteral("torrentstream");
        CHECK(RecentStore::reopenFor(future, true) == RO::ReplayPath);
    }
```

- [ ] **Step 2: Run it to verify it fails**

```bash
cmake --build build --target probe_importers 2>&1 | tail -20
```

Expected: compile FAILS with `'Reopen': is not a member of 'RecentStore'`.

- [ ] **Step 3: Declare the table**

In `native/src/core/RecentStore.h`, after the `Relaunch` enum and `relaunchFor`:

```cpp
    // How a Recent re-open gets its playable url (#224) — the second pure dispatch table, sibling to
    // relaunchFor above and pinned by the same probe. MainWindow::openRecent switches on this so the app and
    // the headless probe share one definition of the routing.
    //
    //   ReplayPath    — open RecentItem.path, the behaviour that predates #224. Local files, pasted links,
    //                   Subsonic/Jellyfin/IPTV rows, legacy rows, and any recipe that is not complete.
    //   ResolveDirect — resolveStream(sourceById(sourceAddonId), item{sourceItemId, sourceType})
    //   ResolveImdb   — resolveStreamByImdb(sourceType, sourceItemId)
    //   SourceMissing — a complete direct recipe naming an addon this device does not have. Says so.
    enum class Reopen { ReplayPath, ResolveDirect, ResolveImdb, SourceMissing };
    // `addonAvailable` is whether AddonManager::sourceById(it.sourceAddonId) is non-null. Passed IN rather
    // than looked up, so this unit keeps no dependency on the addon layer and stays linkable into a probe
    // that has none — the same rule preferGroup follows into resolveStream.
    Reopen reopenFor(const RecentItem& it, bool addonAvailable);
```

- [ ] **Step 4: Implement it**

In `native/src/core/RecentStore.cpp`, after `relaunchFor`'s definition:

```cpp
RecentStore::Reopen RecentStore::reopenFor(const RecentItem& it, bool addonAvailable)
{
    // Both halves or neither. A route with no id would call resolve with an empty id, which every provider
    // answers "no source" — the same dead end as before, wearing a message that blames the source instead.
    if (it.sourceItemId.isEmpty() || it.sourceRoute.isEmpty()) return Reopen::ReplayPath;
    if (it.sourceRoute == QLatin1String("imdb")) return Reopen::ResolveImdb;
    if (it.sourceRoute == QLatin1String("direct"))
        return addonAvailable ? Reopen::ResolveDirect : Reopen::SourceMissing;
    return Reopen::ReplayPath;   // an unknown route: a newer build wrote this row. Replay, never guess.
}
```

- [ ] **Step 5: Run the probe to verify it passes**

```bash
cmake --build build --target probe_importers 2>&1 | tail -5 && ./build/probe_importers
```

Expected: `IMPORTERS-OK`, exit 0.

- [ ] **Step 6: Commit**

```bash
git commit -m "feat: a pure dispatch table for how a Recent re-open gets its url" -- native/src/core/RecentStore.h native/src/core/RecentStore.cpp native/tools/probe_importers.cpp
```

---

### Task 3: The four fields cross the sync boundary without becoming a leak

**Files:**
- Modify: `native/src/core/CloudMerge.cpp:309-324` (`scrubRecentRow`'s comment), `:421-424` (a stale field list)
- Test: `native/tools/probe_cloudmerge.cpp` (new §38, before the final sentinel at the end of file)

**Interfaces:**
- Consumes: Task 1's four fields and their json names `saddon`/`sitem`/`sroute`/`stype`.
- Produces: nothing consumed by later tasks.

`scrubRecentRow` opens with `QJsonObject o = in;` and then rewrites four named fields, so anything else **passes through untouched**. That is why the new fields survive a merge with no code change — and precisely why they need an explicit assertion. "It works because nobody filters it" is the exact shape of the hole #200 came from.

- [ ] **Step 1: Write the failing test**

In `native/tools/probe_cloudmerge.cpp`, insert immediately before the closing `if (failures == 0) { std::puts("CLOUDMERGE-OK"); return 0; }`:

```cpp
    // ---- 38. #224: the re-mint recipe crosses the merge, and is not a credential ------------------------
    //
    // NO REAL CREDENTIAL APPEARS HERE. The token below is invented; only its SHAPE is real.
    //
    // scrubRecentRow copies the row (`QJsonObject o = in;`) and rewrites four named fields, so the #224
    // fields ride through by DEFAULT rather than by decision. Both halves of that need pinning: that they
    // arrive at all (a peer's row must be re-mintable, or cross-device Continue Watching still dead-ends),
    // and that none of them can carry a query — which is the whole #200 invariant, and the one a future
    // field would break by being added without thought.
    {
        wipeStores();
        useProfile(QStringLiteral("cm38"));

        const QString tok = QStringLiteral("?token=nOtaReAlToKeN000000000000000000000");
        const QString blob = QStringLiteral("eyJ0IjoiQSBGaWxtIiwiaCI6ImRlYWRiZWVmIn0");

        QJsonObject peer;
        peer.insert(QStringLiteral("path"), QStringLiteral("https://store-038.example/dld/6f1e/m.mkv") + tok);
        peer.insert(QStringLiteral("title"), QStringLiteral("A Film"));
        peer.insert(QStringLiteral("kind"), QStringLiteral("video"));
        peer.insert(QStringLiteral("key"), blob);
        peer.insert(QStringLiteral("saddon"), QStringLiteral("com.example.allarr"));
        peer.insert(QStringLiteral("sitem"), blob);
        peer.insert(QStringLiteral("sroute"), QStringLiteral("direct"));
        peer.insert(QStringLiteral("stype"), QStringLiteral("movie"));
        peer.insert(QStringLiteral("ts"), 1000.0);

        const QJsonObject out = scrubRecentRow(peer);

        // 38a. The recipe SURVIVES. Without this the peer's row re-opens as a bare path and #224 is only
        // fixed on the device that wrote the row.
        CHECK(out.value(QStringLiteral("saddon")).toString() == QStringLiteral("com.example.allarr"));
        CHECK(out.value(QStringLiteral("sitem")).toString() == blob);
        CHECK(out.value(QStringLiteral("sroute")).toString() == QStringLiteral("direct"));
        CHECK(out.value(QStringLiteral("stype")).toString() == QStringLiteral("movie"));

        // 38b. The path is still scrubbed — §34's rule is untouched by any of this.
        CHECK(out.value(QStringLiteral("path")).toString()
              == QStringLiteral("https://store-038.example/dld/6f1e/m.mkv"));

        // 38c. THE INVARIANT. Not one of the four may carry a query. They are ids by construction — an addon
        // manifest id, a base64url release blob, and two closed vocabularies — so this holds today by what
        // they ARE. It is asserted because the next field added here will be added by someone who has not
        // read #200, and this is the line that stops them.
        for (const char* f : { "saddon", "sitem", "sroute", "stype" })
        {
            const QString v = out.value(QLatin1String(f)).toString();
            CHECK(!v.contains(QLatin1Char('?')));
            CHECK(!StoredUrl::carriesCredential(v));
            CHECK(StoredUrl::location(v) == v);   // a scrub of it would be a no-op: nothing to take off
        }

        // 38d. A row that arrives WITHOUT the recipe (a peer on an older build) is untouched and does not
        // grow empty keys — the legacy row stays byte-identical, so the merge cannot churn every device's
        // list into a rewrite storm the first time one device upgrades.
        QJsonObject legacy;
        legacy.insert(QStringLiteral("path"), QStringLiteral("C:\\x\\y.mkv"));
        legacy.insert(QStringLiteral("kind"), QStringLiteral("video"));
        const QJsonObject legacyOut = scrubRecentRow(legacy);
        CHECK(!legacyOut.contains(QStringLiteral("saddon")));
        CHECK(!legacyOut.contains(QStringLiteral("sroute")));
        CHECK(legacyOut.value(QStringLiteral("path")).toString() == QStringLiteral("C:\\x\\y.mkv"));

        wipeStores();
        useProfile(QStringLiteral("cmA"));
    }
```

- [ ] **Step 2: Run it to verify it fails**

```bash
cmake --build build --target probe_cloudmerge 2>&1 | tail -5 && ./build/probe_cloudmerge
```

Expected: builds, then FAILS — `CLOUDMERGE: 4 check(s) failed`, exit 1, because §38a's four assertions read fields no writer produces yet. (38b/c/d pass already; 38c passes vacuously on empty strings, which is why 38a must fail first and be seen to.)

- [ ] **Step 3: Confirm the failure is the expected one, then satisfy it**

No production change is needed for 38a — `scrubRecentRow` already passes unknown fields through. The failure above is the probe reading fields that Task 1's writer puts in the ini but that this hand-built `peer` object supplies directly. Re-read the failure output: if any of 38a's four failed, the pass-through is **not** working and `scrubRecentRow` must be inspected before continuing. If they pass and only 38c failed, a field is carrying a query and the writer in Task 4 is wrong.

Expected on re-run after Step 2's diagnosis: all of §38 passes with no production edit. This step is a deliberate checkpoint — the probe is proving an *existing* property, so seeing it go green without a code change is the correct outcome, not a skipped step.

```bash
./build/probe_cloudmerge
```

Expected: `CLOUDMERGE-OK`, exit 0.

- [ ] **Step 4: Fix the now-stale field list**

`native/src/core/CloudMerge.cpp:422-423` enumerates what a recents row holds, to justify using `canon()` over `tieKey()`. Task 1 made it wrong. Replace:

```cpp
        // Deliberately canon() and not tieKey(): a recents entry has no addonId field at all (RecentStore
        // writes path/title/kind/thumb/key/system/ts), so normalizing here would be motion with no reachable
        // effect and no mutation could ever kill it.
```

with:

```cpp
        // Deliberately canon() and not tieKey(): tieKey normalizes an "addonId" field, and a recents row has
        // none (RecentStore writes path/title/kind/thumb/key/system/ts, plus #224's saddon/sitem/sroute/stype
        // — `saddon` IS an addon manifest id but is not spelled "addonId" and is not a tie input), so
        // normalizing here would be motion with no reachable effect and no mutation could ever kill it.
```

The distinction matters and is worth the words: a reader who sees `saddon` arrive and remembers only "recents has no addonId" will reasonably conclude the comment is now a lie and "fix" it by switching to `tieKey`, changing the cap-40 cut order for every user on a merge.

- [ ] **Step 5: Update `scrubRecentRow`'s own comment**

At `native/src/core/CloudMerge.cpp:309`, replace `the same four fields RecentStore::add scrubs on the way in` with:

```cpp
// One recents row, credential-free (issue #200). Rewrites the same four url-shaped fields RecentStore::add
// scrubs on the way in, by the same rules, so a row that arrives from a peer is indistinguishable from one
// written here. Every OTHER field rides through untouched (`QJsonObject o = in;`) — which is how #224's
// saddon/sitem/sroute/stype reach a peer, and why probe_cloudmerge §38c asserts that none of them can
// carry a query. A new field added here is credential-free by argument or it does not belong in this store.
// Spelled over the raw json rather than through RecentItem because this pass never builds one.
```

- [ ] **Step 6: Verify and commit**

```bash
cmake --build build --target probe_cloudmerge 2>&1 | tail -5 && ./build/probe_cloudmerge
```

Expected: `CLOUDMERGE-OK`, exit 0.

```bash
git commit -m "test: pin that a Recents re-mint recipe crosses the merge and carries no credential" -- native/src/core/CloudMerge.cpp native/tools/probe_cloudmerge.cpp
```

---

### Task 4: Record the recipe at the play sites

**Files:**
- Modify: `native/src/ui/MainWindow.cpp:15674-15718` (the catalog video stream leaf), `:6134-6142` (the remote audiobook)
- Test: manual, plus Task 5's live gate. No probe: these sites need a `MainWindow`.

**Interfaces:**
- Consumes: Task 1's four fields.
- Produces: rows in the ini carrying a complete recipe, which Task 5 consumes.

Three `RecentStore::add` calls write a remote row. All three must set the recipe or the row they write is the one that still dead-ends.

- [ ] **Step 1: Add a helper that builds the recipe once**

In `native/src/ui/MainWindow.cpp`, above the function containing line 15674, add a file-local helper:

```cpp
// The #224 re-mint recipe for a playable that a source just resolved. One spelling for all three
// RecentStore::add sites below, because the failure mode of three copies is that two get updated.
//
// The route is decided by what the item HAS, not by what resolved it: an item carrying an imdbStreamId can
// be re-resolved across every installed stream provider, which survives the addon that served it being
// uninstalled. A file-provider item without one can only be re-asked of the addon that knows its id space,
// so that route names the addon. `sourceAddonId` is recorded on BOTH routes — the imdb route ignores it,
// but it costs one short string and it is the only record of which addon actually served this play.
static void applyRemintRecipe(RecentItem& row, const MediaItem& item)
{
    row.sourceAddonId = item.sourceAddonId;
    if (!item.imdbStreamId.isEmpty())
    {
        row.sourceRoute  = QStringLiteral("imdb");
        row.sourceItemId = item.imdbStreamId;
        // resolveStreamByImdb takes the STREMIO type ("movie"/"series"), which is what an episode's parent
        // is; item.type on an episode leaf is "episode", which that call does not accept.
        row.sourceType = item.imdbStreamId.contains(QLatin1Char(':')) ? QStringLiteral("series")
                                                                      : QStringLiteral("movie");
        return;
    }
    if (item.sourceAddonId.isEmpty() || item.id.isEmpty()) return; // no recipe: the row replays its path
    row.sourceRoute  = QStringLiteral("direct");
    row.sourceItemId = item.id;
    row.sourceType   = item.type;
}
```

- [ ] **Step 2: Apply it at the catalog video leaf's two write sites**

`MainWindow.cpp:15691` (the external-player route) currently reads:

```cpp
            RecentStore::add({ url, rt, QStringLiteral("video"), item.thumbnailUrl, rkey });
```

Replace with:

```cpp
            RecentItem row{ url, rt, QStringLiteral("video"), item.thumbnailUrl, rkey };
            applyRemintRecipe(row, item);
            RecentStore::add(row);
```

`MainWindow.cpp:15717` (the built-in player route) currently reads:

```cpp
        RecentStore::add({ url, title, QStringLiteral("video"), item.thumbnailUrl, rkey });
```

Replace with:

```cpp
        RecentItem row{ url, title, QStringLiteral("video"), item.thumbnailUrl, rkey };
        applyRemintRecipe(row, item);
        RecentStore::add(row);
```

Both, not one. The external-player route writes a Recents row too, and a row written by a VLC hand-off that cannot be re-opened here would be a second, subtler version of this bug.

- [ ] **Step 3: Apply it at the remote audiobook site**

`MainWindow.cpp:6141` currently reads:

```cpp
    RecentStore::add({ item.url.isEmpty() ? firstPartUrl : item.url, item.title,
                       QStringLiteral("audio"), item.thumbnailUrl, bookKey });
```

Replace with:

```cpp
    RecentItem row{ item.url.isEmpty() ? firstPartUrl : item.url, item.title,
                    QStringLiteral("audio"), item.thumbnailUrl, bookKey };
    applyRemintRecipe(row, item);
    RecentStore::add(row);
```

- [ ] **Step 4: Retire the deferral comment that sits above it**

The block at `MainWindow.cpp:6134-6140` says this defect is "not this book's problem to solve … Noted as its own defect rather than papered over." #224 is that defect and this is the fix, so the note must not survive to tell a future reader the problem is still open. Replace those seven lines with:

```cpp
    // The PATH is the same one openAudioStream has always recorded for a remote recording, and it is a
    // signed link whose credential StoredUrl::location (correctly) removes before it reaches the ini. That
    // used to make the row un-re-openable, which was #224 and is fixed here rather than in the path: the row
    // now carries the recipe to MINT a new link, so what the path has lost no longer matters. See
    // RecentItem's #224 block, and openRecent's remintAndOpen for the consuming half.
```

- [ ] **Step 5: Build and verify a row is written with its recipe**

```bash
cmake --build build --target EverythingBox --config Release 2>&1 | tail -5
```

Expected: builds clean.

Then play one catalog movie in the app, exit, and read the row back:

```bash
grep -o '"saddon":"[^"]*"' "$LOCALAPPDATA/EverythingBox/everythingbox.ini" | head -3
```

Expected: at least one `"saddon":"…"` line. If empty, `item.sourceAddonId` is not set on the play path — check that the leaf at `MainWindow.cpp:15647` receives an item that carries it, and read `MediaItem::sourceAddonId`'s comment at `AddonModels.h:159` (it is set "when it's surfaced outside its own catalog", which may not cover the ordinary in-catalog play; if so, set it at the resolve callback rather than working around it here).

- [ ] **Step 6: Commit**

```bash
git commit -m "feat: record the re-mint recipe when a stream or remote audiobook is played" -- native/src/ui/MainWindow.cpp
```

---

### Task 5: Re-mint on open

**Files:**
- Modify: `native/src/ui/MainWindow.h` (one private method), `native/src/ui/MainWindow.cpp:10854` (`openRecent`) and the url arm at `:10991`
- Test: the live gate in Task 8.

**Interfaces:**
- Consumes: `RecentStore::find`, `RecentStore::reopenFor`, `RecentStore::Reopen` (Tasks 1-2); `AddonManager::sourceById`, `::resolveStream`, `::resolveStreamByImdb`, `StreamCb`.
- Produces: `MainWindow::remintAndOpen(const RecentItem&, const QString& resumeKey)`, consumed by Task 6.

- [ ] **Step 1: Declare the helper**

In `native/src/ui/MainWindow.h`, beside `openRecent`'s declaration at line 98:

```cpp
    // #224: re-resolve a Recents row's source and open the FRESH url, instead of replaying the stored one.
    // A debrid link is signed and short-lived, and since #200 the stored path has had its credential removed
    // before it was ever written — so replay cannot work and the row needs a new link, not a better-preserved
    // old one. Routed by RecentStore::reopenFor; only reached for a row that carries a complete recipe.
    void remintAndOpen(const RecentItem& row, const QString& resumeKey);
```

- [ ] **Step 2: Route the url arm through it**

In `MainWindow.cpp`, the arm at `:10991-10992` currently reads:

```cpp
    if (isUrl && kind == QStringLiteral("audio")) openAudioStream(path, resumeKey, title, thumb);
    else if (isUrl)                              openStreamUrl(path, resumeKey, title);
```

Replace with:

```cpp
    // #224: a REMOTE row re-mints its link rather than replaying one. The stored path lost its credential to
    // #200's scrub before it was written, so replaying it is a guaranteed failure for exactly the rows people
    // use Continue Watching for. reopenFor decides; a row with no recipe still replays, which is every local
    // file, pasted link, and Subsonic/Jellyfin/IPTV row.
    if (isUrl)
    {
        const RecentItem row = RecentStore::find(resumeKey.isEmpty() ? path : resumeKey);
        const bool haveAddon = mgr_ && mgr_->sourceById(row.sourceAddonId) != nullptr;
        switch (RecentStore::reopenFor(row, haveAddon))
        {
            case RecentStore::Reopen::ResolveDirect:
            case RecentStore::Reopen::ResolveImdb:
                remintAndOpen(row, resumeKey);
                return;
            case RecentStore::Reopen::SourceMissing:
                notify(tr("“%1” came from an add-on that isn't installed here, so its link can't be "
                          "refreshed. Install it, or open the item from its shelf.").arg(title), kFeedbackLong);
                return;
            case RecentStore::Reopen::ReplayPath:
                break;   // fall through to the pre-#224 behaviour below
        }
    }
    if (isUrl && kind == QStringLiteral("audio")) openAudioStream(path, resumeKey, title, thumb);
    else if (isUrl)                              openStreamUrl(path, resumeKey, title);
```

- [ ] **Step 3: Implement `remintAndOpen`**

Add after `openRecent`'s definition in `MainWindow.cpp`:

```cpp
void MainWindow::remintAndOpen(const RecentItem& row, const QString& resumeKey)
{
    // Say what is happening. A re-mint is a network round trip through the debrid provider — createtorrent,
    // a mylist poll, then requestdl — which on a cached release is a second or three and on a cold one is
    // longer. Silence there reads exactly like the freeze this issue is about.
    notify(tr("Getting a fresh link for “%1”…").arg(row.title), 0);

    const QString title = row.title;
    const QString thumb = row.thumb;
    const QString kind  = row.kind;
    const QString rkey  = resumeKey.isEmpty() ? row.key : resumeKey;

    auto onResolved = [this, title, thumb, kind, rkey](const QString& url, const QString& mime,
                                                       const StreamHeaders::Headers& headers)
    {
        Q_UNUSED(mime);
        if (url.isEmpty())
        {
            // The source could not mint one: the release is no longer on the account, or no longer cached.
            // Report it and OFFER the swap — never take it. Silently substituting another release drops the
            // viewer some way into a different cut with a resume position that refers to nothing, and gives
            // them nothing on screen explaining why. takeStreamNotice carries the source's own reason when it
            // had one ("caching started", "no seeds"), which is far better than anything invented here.
            const QString why = mgr_ ? mgr_->takeStreamNotice() : QString();
            notify(why.isEmpty()
                       ? tr("Couldn't get a fresh link for “%1”. The release may no longer be on your debrid "
                            "account — use “Issue with Streaming” to try another source.").arg(title)
                       : tr("Couldn't get a fresh link for “%1”: %2").arg(title, why),
                   kFeedbackLong);
            return;
        }
        notifier_->hidePlayerNotice();
        // The FRESH headers ride the callback, exactly as StreamCb's contract requires (AddonManager.h:28:
        // a member holding "the last stream's headers" outlives its stream and sends host A's Referer to
        // host B). They are used here and never written down — #59 is untouched by this change.
        if (kind == QStringLiteral("audio")) openAudioStream(url, rkey, title, thumb);
        else                                 openStreamUrl(url, rkey, title, headers);
    };

    if (row.sourceRoute == QLatin1String("imdb"))
    {
        mgr_->resolveStreamByImdb(row.sourceType, row.sourceItemId, onResolved);
        return;
    }
    LoadedAddon* src = mgr_->sourceById(row.sourceAddonId);
    MediaItem item;
    item.id    = row.sourceItemId;
    item.type  = row.sourceType;
    item.title = row.title;
    mgr_->resolveStream(src, item, onResolved);
}
```

- [ ] **Step 4: Give `openStreamUrl` the headers it now receives**

Check `MainWindow.h:393`. If `openStreamUrl` does not already take a `const StreamHeaders::Headers&` fourth parameter, add one defaulted to `{}` and pass it through to `player_->play(url, headers)` in its body, mirroring how the catalog leaf at `MainWindow.cpp:15716` already calls `player_->play(url, item.requestHeaders)`. Without this the re-minted stream plays bare and a gated source 403s — which is #59's failure wearing this feature's name.

- [ ] **Step 5: Build**

```bash
cmake --build build --target EverythingBox --config Release 2>&1 | tail -5
```

Expected: builds clean.

- [ ] **Step 6: Commit**

```bash
git commit -m "feat: a Recents row re-mints its link on open instead of replaying a dead one" -- native/src/ui/MainWindow.cpp native/src/ui/MainWindow.h
```

---

### Task 6: A multi-file audiobook re-mints the right part

**Files:**
- Modify: `native/src/ui/MainWindow.cpp` (`remintAndOpen`'s audio arm)
- Test: the live gate in Task 8.

**Interfaces:**
- Consumes: `remintAndOpen` (Task 5); `MainWindow::openRemoteAudiobook(const MediaItem&, const QString& firstPartUrl)` at `:6049`; `MediaItem::bookParts`; `RemoteAudiobook::partToken(bookKey, fileName)` at `RemoteAudiobook.h:180`.
- Produces: nothing consumed by later tasks.

Task 5's audio arm calls `openAudioStream`, which plays **one** file. For a release with more than one part that reopens the book as a single recording, losing the queue. The re-listed release is what rebuilds it.

- [ ] **Step 1: Branch on the re-listed parts**

In `remintAndOpen`'s `onResolved`, replace the final two lines from Task 5:

```cpp
        if (kind == QStringLiteral("audio")) openAudioStream(url, rkey, title, thumb);
        else                                 openStreamUrl(url, rkey, title, headers);
```

with:

```cpp
        if (kind != QStringLiteral("audio")) { openStreamUrl(url, rkey, title, headers); return; }
        // A MULTI-PART BOOK REBUILDS ITS QUEUE, not just its first link. openAudioStream plays one file, so
        // taking it here would reopen a fifteen-hour book as whichever part the source happened to return —
        // #214's original defect, reintroduced by the re-mint path. openRemoteAudiobook rebuilds the part
        // table and the queue from the re-listed release; the part TOKENS it derives are stable across
        // releases by construction (RemoteAudiobook::partToken hashes bookKey + fileName), which is exactly
        // why the resume row written before this re-mint still names a part this queue contains.
        if (resolved.bookParts.size() > 1) { openRemoteAudiobook(resolved, url); return; }
        openAudioStream(url, rkey, title, thumb);
```

- [ ] **Step 2: Carry the resolved item into the callback**

`StreamCb` hands back only `(url, mime, headers)` — not the `MediaItem` the resolve populated. Read `AddonManager::resolveStream`'s implementation and confirm how `bookParts` reaches the caller today at `MainWindow.cpp:15647` (`if (item.bookParts.size() > 1)`). That leaf has the item because it *is* the item's leaf; `remintAndOpen` constructs a bare one.

Two ways to close that gap. Prefer the first:

1. If `resolveStream` populates `bookParts` on a `MediaItem&` the caller owns, capture that item by shared pointer and read it in the callback:

```cpp
    auto item = std::make_shared<MediaItem>();
    item->id    = row.sourceItemId;
    item->type  = row.sourceType;
    item->title = row.title;
    item->thumbnailUrl = row.thumb;
```

   capture `item` in `onResolved`, and use `*item` as `resolved`.

2. If it does not, add a `listReleaseParts(LoadedAddon*, const QString& itemId, std::function<void(QVector<RemoteAudiobook::Part>)>)` to `AddonManager` mirroring how the leaf obtains them, and call it before opening.

Determine which by reading `resolveStream` and the `bookParts` assignment before writing either. Do not guess — a wrong choice here silently reopens every audiobook as a single part, which is the exact failure this task exists to prevent and which a smoke test on a one-part book would not catch.

- [ ] **Step 3: Build**

```bash
cmake --build build --target EverythingBox --config Release 2>&1 | tail -5
```

Expected: builds clean.

- [ ] **Step 4: Commit**

```bash
git commit -m "feat: re-minting a remote audiobook rebuilds its part queue, not just its first link" -- native/src/ui/MainWindow.cpp
```

---

### Task 7: A re-minted stream can swap source

**Files:**
- Modify: `native/src/ui/MainWindow.cpp:10857`, and `remintAndOpen`'s success path

**Interfaces:**
- Consumes: `remintAndOpen` (Task 5), `HomeView::requestNextSource` (`HomeView.cpp:6217`), `HomeView::lastPlay_`.
- Produces: nothing.

`openRecent` opens with `currentNextSourceCapable_ = false;` and the comment *"a Recent re-open has no live Allarr context to swap sources"*. After Task 5 that is no longer true on the re-mint path, and the failure message in Task 5 tells the user to use a button that is hidden.

- [ ] **Step 1: Correct the claim at the top of `openRecent`**

```cpp
    // Cleared here and set again by remintAndOpen's success path (#224). It WAS unconditionally false,
    // because a Recent re-open replayed a stored url and had no idea which source produced it. A row that
    // carries a re-mint recipe now does, so the swap is available on a resumed stream — which matters most
    // exactly here, since the reason to swap is usually that the release went bad on the account.
    currentNextSourceCapable_ = false;
```

- [ ] **Step 2: Seed `lastPlay_` on the re-mint so the swap has context**

`requestNextSource` returns *"No alternate source to try."* unless `lastPlay_.addon` or `lastPlay_.viaImdb` is set, and `lastPlay_` lives in `HomeView`. Add a `HomeView` slot that seeds it from a recipe, and call it from `remintAndOpen`'s success path:

```cpp
// HomeView.h, beside requestNextSource:
    // Seed the alternate-source context from a Recents re-mint (#224), so "Issue with Streaming" works on a
    // resumed stream. Without this the swap the failure message points at is a button that says there is
    // nothing to try.
    void seedNextSourceFromRecipe(const QString& addonId, const QString& itemId,
                                  const QString& route, const QString& type);
```

```cpp
// HomeView.cpp, beside requestNextSource:
void HomeView::seedNextSourceFromRecipe(const QString& addonId, const QString& itemId,
                                        const QString& route, const QString& type)
{
    lastPlay_ = {};                       // a re-mint starts a new swap chain: attempt 0 is this link
    if (route == QLatin1String("imdb")) { lastPlay_.viaImdb = true; lastPlay_.imdbType = type; lastPlay_.imdbId = itemId; }
    else                                { lastPlay_.addon = mgr_->sourceById(addonId); }
    lastPlay_.item.id = itemId;
    lastPlay_.item.type = type;
}
```

Call it from `remintAndOpen` immediately before opening, and set `currentNextSourceCapable_ = true;` there.

- [ ] **Step 3: Build and verify the button appears**

```bash
cmake --build build --target EverythingBox --config Release 2>&1 | tail -5
```

Then, with the app running under `EB_UITEST=1`, open a re-mintable Continue Watching row and screenshot the player:

```bash
python native/tools/uitest.py shot remint-swap-button.png
```

Expected: the "Issue with Streaming" overlay is visible top-left beside Back.

- [ ] **Step 4: Commit**

```bash
git commit -m "feat: offer a source swap on a re-minted stream" -- native/src/ui/MainWindow.cpp native/src/ui/HomeView.cpp native/src/ui/HomeView.h
```

---

### Task 8: Full gate, live verification, and close #224

**Files:**
- Modify: `CHANGELOG.md`

- [ ] **Step 1: Run the whole probe suite**

Per `CONTRIBUTING.md`, and because a merge can eat a gate's closing `fi` and leave the suite passing while a section never runs:

```bash
bash -n native/tools/run-headless-probes.sh && bash native/tools/run-headless-probes.sh 2>&1 | tail -30
```

Expected: `bash -n` silent, then every sentinel present and a green summary. Confirm `IMPORTERS-OK` and `CLOUDMERGE-OK` are both in the output — not merely that the run exited 0.

- [ ] **Step 2: Rebuild everything and check for new warnings**

```bash
cmake --build build --config Release 2>&1 | grep -iE "error|warning C" | head -20
```

Expected: no new errors or warnings attributable to the changed files.

- [ ] **Step 3: The live gate**

This is the only step that proves the issue is fixed; every probe above tests a pure function. Deploy Release to `C:\EverythingBox-app` (never Debug — the debug DLLs are not there), then:

1. Play a TorBox-backed movie from a catalog shelf. Watch ~2 minutes. Exit to Home.
2. Confirm the Recents row carries a recipe:
   ```bash
   grep -o '"sroute":"[^"]*"' "$LOCALAPPDATA/EverythingBox/everythingbox.ini" | head
   ```
   Expected: at least one `"sroute":"direct"` or `"sroute":"imdb"`.
3. Force the stored link to be stale — it already is, since #200 stripped its token. Restart the app so nothing is cached in memory.
4. Open the row from Continue Watching. Expected: a brief "Getting a fresh link…" notice, then playback **resuming at roughly 2 minutes**, not 0:00 and not the expiry message.
5. Repeat 1-4 with a multi-file audiobook, resuming mid-way through part 2 or later. Expected: the queue is rebuilt and playback resumes **in the same part** at the same position.
6. Negative case: uninstall the source addon, then open the row. Expected: the "add-on that isn't installed here" message from Task 5, not the old expiry message and not a crash.

Record what actually happened for each of the six, with the observed resume position. If step 4 or 5 shows 0:00, the resume key is not surviving the re-mint — check `rkey` in `remintAndOpen` against `session_->beginResume`.

- [ ] **Step 4: Changelog**

Add under the unreleased section of `CHANGELOG.md`:

```markdown
- Continue Watching now refreshes a stream's link when you open it, instead of replaying a saved one. A
  movie or audiobook you come back to days later plays and resumes where you left off, rather than
  reporting that its link expired. (#224)
```

- [ ] **Step 5: Commit and merge**

```bash
git commit -m "docs: changelog for re-minting Continue Watching links" -- CHANGELOG.md
```

The merge commit carries `Fixes #224` and no other issue number. Not #200, not #203, not #214 — each is cited in the code comments as context, and citing a neighbour is precisely how #158 and #170 sat open for days while already implemented.

---

## Self-Review

**Spec coverage:**

| Spec section | Task |
|---|---|
| Stored recipe, credential-free | 1 |
| `sourceRoute` recorded not inferred | 1 (field comment), 2 (unknown-route assertion) |
| Two fidelity tiers (blob vs imdb) | 4 (`applyRemintRecipe` route choice) |
| Opening a row — recipe present | 5 |
| Opening a row — no recipe | 2 (`ReplayPath`), 5 (fall-through) |
| Opening a row — addon absent | 2 (`SourceMissing`), 5 (message) |
| `currentNextSourceCapable_` | 7 |
| When the re-mint fails | 5 (`onResolved` empty-url arm) |
| Audiobooks | 6 |
| `probe_cloudmerge` credential invariant | 3 |
| Routing-fork probe | 2 |
| Round-trip / legacy rows | 1 |
| Live gate | 8 |

**Deviation from the spec, deliberate:** the spec named **three** fields; this plan uses **four**. `resolveStreamByImdb(type, id, …)` requires a type, and `resolveStream` needs `MediaItem::type` — so a `sourceType` is load-bearing, not decoration. Splitting route from type also keeps the "recorded, not inferred" property the spec argued for. The spec has been updated to match.

**Placeholder scan:** none. Task 6 Step 2 presents two implementations and requires reading `resolveStream` to choose — that is a genuine unknown identified as such, with both branches written out and the consequence of guessing stated, not a "TBD".

**Type consistency:** `sourceAddonId`/`sourceItemId`/`sourceRoute`/`sourceType` used identically in Tasks 1, 2, 3 (as json `saddon`/`sitem`/`sroute`/`stype`), 4, 5 and 7. `RecentStore::Reopen`'s four enumerators are spelled the same in Task 2's declaration, its test, and Task 5's switch. `remintAndOpen(const RecentItem&, const QString&)` is declared in Task 5 Step 1 and used in Task 5 Step 2 and Task 6.
