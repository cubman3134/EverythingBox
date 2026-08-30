# Finish a volume and the next one is already there

**Date:** 2026-08-30
**Follows:** d04c41b3 (`fix: the download cache is not a folder of chapters`)
**Extends:** [2026-08-28 chapter auto-advance](2026-08-28-comic-chapter-autoadvance-design.md)

## The problem

Chapter crossing shipped for two lanes: manga chapters browsed from an addon, and comic archives
sitting together in a folder. A comic **issue** — the `comic_issue` rows you get by drilling into a
series in the Reading column — is in neither. Its file is fetched from a file provider on demand and
lands in the app's own download cache, so it is not a browsed manga chapter and its "folder" is a
cache bucket.

For a while it looked like it worked, in the worst possible way. `folderRunFor()` read the cache
bucket as a folder of chapters, and that bucket holds every remote document the app has ever
fetched, named by a SHA1 of its url. Live: finishing Fairy Tail Vol. 2 (`b3ddbd79….cbz`) and pressing
forward tried to open `b969cec8….zip` — a 985 MB ROM archive, three months old, adjacent by nothing
but hash. The reader's own refusal to parse it (`This isn't a readable comic archive`) is the only
reason the crossing did not succeed at opening it. That is fixed in d04c41b3: no run is armed inside
the cache at all.

Which leaves the honest state of things: **for a comic issue, paging past the last page does
nothing.** Finishing a volume means backing out to the volume list and finding the next one by hand —
which is what happened in the report that started this, and it is the exact motion the crossing
feature exists to remove.

The predecessor spec named this upgrade and why it was deferred:

> **Re-querying the provider for the series' chapter list at each boundary** is always complete, even
> past the tail of a paginated list, but costs a round trip per chapter turn and needs a
> parent-series id that a chapter item does not reliably carry.

Both objections are addressable. The parent id is one field the catalog addon already holds and does
not pass on. The round trip stops being a cost at the boundary if it does not happen at the boundary.

## What we are building

Reading to the end of a comic volume opens the next one, with the fetch already done.

- Three pages from the end, the app quietly finds and downloads the next volume into the cache.
- Pressing forward on the last page opens it — instantly if the pre-fetch finished, and under the
  existing sticky notice if it did not.
- Pressing back on the first page opens the previous volume at *its* last page, as the other lanes do.
- This works whether you opened the volume from its issue list or resumed it from Recents, because
  the sibling list is derived from the item, not from what the browse surface happens to be showing.

Everything about the reader's own behaviour — the boundary press, the end-of-chapter hint, the latch,
the generation tag, the sticky notice — is unchanged. This spec adds a third lane underneath them.

## Where "what comes next" comes from

The predecessor captures a run from the level you drilled into. That is kept, and extended to
`comic_issue`; it costs nothing and it is exact. What is new is a second source for the case the
capture cannot cover — a volume reached from Recents, where there is no level and never was one.

**The catalog addon starts telling the client an issue's series.** A `comic_issue` is
`comicvine:issue:<id>` and its meta today reports its series as a display fact ("Series: Fairy
Tail") — a string for a human, useless for a lookup. Comic Vine returns `volume.id` in the same
response that fact is built from, and the issues list is *generated from* the volume it drilled into,
so the id is in hand at both sites and is thrown away at both.

Given the parent id, the sibling list is one `requestDetail` on `comicvine:volume:<vid>` — the very
call the drill-down makes. The reading order needs no new rule: `inReadingOrder` parses `#3 — Volume
3` off the `#` marker it already looks for, and as of 00444ff3 the addon returns issues in numeric
order anyway.

### Why the re-fetch is not a boundary cost

Because it does not run at the boundary. The run is filled in when the volume **opens** — the reader
has a whole volume of reading time to spend a round trip in — and the file for the next entry is
fetched three pages from the end. By the time a boundary press happens, both answers are usually
already sitting there. The press falls back to doing the work synchronously (under the notice the
manga lane already shows) only when the reader got to the end faster than the network did.

## The pieces

### 1. `MediaItem::parentId`, and the addon that fills it

A new field on `MediaItem`, carried across the addon JSON boundary like `id` and `type`:

```cpp
QString parentId;   // the container this item belongs to ("comicvine:volume:<id>" for an issue).
                    // Empty for everything that has no meaningful parent, which is most things.
```

Three edits in the addon (`native/addons/aiocatalog/main.js`):

- `cvIssues(volumeId, page)` sets `parentId` on each row from the volume id it was called with;
- `cvIssueMeta` sets it from `results.volume.id`, which the existing `field_list` already requests;
- the Cloudflare-worker copy (`native/addon-protocol/aiocatalog-worker/src/worker.js`) gets the same
  edit. It cannot run under Duktape, so — as with the issue-ordering fix — it is held to this by a
  source gate beside the probe, not by the probe.

`parentId` is deliberately generic rather than `volumeId`. The same field is what a manga chapter's
series, an episode's season, or a track's album would use, and nothing about the client half is
comic-specific.

**And `parentTitle` beside it.** A run rebuilt from a parent id searches a file provider by the series
NAME, and the only other place to get one is the children response's own title — which is the addon's
heading for a list (`"Issues"` from the Comic Vine arm), not a series. A run carrying that would
search for "Issues 3" on every resumed volume. So the name travels with the id: one more string in
the same fields, and one more key in a `field_list` already being requested. `cvIssues` does not ask
Comic Vine for `volume` at all today, so that request grows the field; `cvIssueMeta` already has it.

**A resumed item must still know who to ask.** Half of this is already done and the other half is one
field. `recordDocument()` keys a comic's Recent on the catalog item id (`RecentItem::key`), so the
issue id survives a restart. What does not survive is the ADDON: `applyRemintRecipe` deliberately
writes no recipe for a row whose path is local, and a cached volume's path is a file on disk — so
`sourceAddonId` is never stamped on a document row.

So `recordDocument()` stamps it. The field exists on `RecentItem` and is already serialized (as
`saddon`); this only starts filling it for documents. It does NOT create a re-mint recipe and must not
be mistaken for one: the direct route needs `sourceRoute` and `sourceType` as well, both of which stay
empty here, so `reopenFor` still refuses the recipe and replays the path exactly as it does today. The
#224 rule that those three fields are written together or not at all is untouched — `saddon` is not
one of the three.

The alternative to storing it was asking every loaded source that publishes a comic catalog which one
owns the id. There is exactly one such addon today, so it would work and would store nothing; it was
rejected because it answers "who owns this?" with a guess that happens to be right, and the day a
second comic source is installed it becomes a race between two addons to claim an id.

An identifier is not a cached answer: the sibling list is still fetched live, which is the whole point
of choosing this over remembering the list. If the addon is gone or the id no longer resolves, the run
is never armed and the boundary press is silent — the same outcome as before this spec.

### 2. `ChapterRun` grows a third shape

`ChapterRun` distinguishes two lanes today with a `bool local`. A third one makes that boolean a
liability, so it becomes an enum:

```cpp
enum class Lane {
    Files,      // entries are paths on disk (a folder of archives)
    Chapters,   // entries are manga chapter ids, resolved to page images
    Catalog,    // entries are catalog item ids, fetched from a file provider  <-- new
};
Lane lane = Lane::Files;
QString seriesTitle;   // the container's title; the Catalog lane builds its search query from it
```

`seriesTitle` sits on the run rather than on each entry because it is a property of the run: every
entry in it belongs to the same container, and that is the invariant the capture guards enforce.

The header stays pure — no widgets, no network, no I/O, no `MediaItem` — so `probe_chapterrun` keeps
testing all of it directly. The mapping from `bool local` to `Lane` is mechanical, and every existing
call site is updated in the same change.

### 3. Capturing the run

Two sites, in priority order.

**From the level you drilled into.** `HomeView::populate()`'s existing capture is manga-only
(`isReadableChapter(it.type)`). It grows to include `comic_issue`, and records the level's container
title as `seriesTitle`. The structural guard that already sits there is unchanged and now protects
this lane too: only a level that is ONE container drilled into is remembered, never a cross-addon
search or a mixed shelf, because a run spanning unrelated works is worse than no run.

The captured run travels with the open. `openImagePages` already grows a run argument for the manga
lane; the bridged-comic open reaches `MainWindow::openLibraryItem` through `openItem`, so the run
rides the `MediaItem` — the precedent `bookParts` and `pcSources` set, and for the reason recorded
there: `openItem` is the one door into `openLibraryItem`, and a parallel signal would be a second.

**From the item, when there is no captured run.** This is the resumed-from-Recents case, and it takes
one or two calls depending on how much the item knows:

- The item carries a `parentId` (it came from a list, but through a path that captured no run):
  `requestDetail` on the parent, and the run is the children.
- The item carries only an id and a source addon (a Recent): `requestMeta` on the item first, because
  `parentId` is not serialized and a resumed row has never seen one. The meta answer supplies it, and
  then the call above runs. `MediaDetail` therefore grows the same `parentId` field `MediaItem` does —
  the addon half of that (`cvIssueMeta`) is already in §1.

Two round trips is the honest cost of storing nothing, and both are spent at OPEN time, with a volume
of reading between them and the boundary they serve. The alternative — persisting `parentId` on the
Recent row — buys one call and starts a second copy of "what series is this?" that can disagree with
the addon's. Not worth it for a call made once per volume.

Late arming is safe and already modelled: `armComicRun` re-syncs and re-asks the end-of-chapter hint
precisely because a run can be armed after the reader is already showing a page.

The two sources never race: the re-fetch is attempted only when the open carried no run. It is also
gated on the reader still showing that comic when the answers land, by the same test the crossing's
own async steps use — a run armed onto a reader the user has left is the bug `chapterHandoffStillOurs`
exists to prevent, arriving by a new door.

### 4. Crossing into a catalog entry

`openRemoteChapter` gains a `Lane::Catalog` arm. It does what pressing the row in the issue list
does, minus the browse UI:

1. Build the query the bridge builds — `"<seriesTitle> <issue number parsed from the entry title>"`,
   falling back to the entry title when no number parses.
2. `AddonManager::resolveDocumentByQuery(query, seriesTitle, "comic", …)`.
3. Download the resolved url into the remote-docs cache under the same SHA1-of-url name
   `fetchRemoteDocumentThenOpen` uses, so a volume fetched here and one fetched by pressing its row
   are the same file on disk and share a reading position.
4. Open it in the reader and arm the advanced run.

HomeView's bridge block is **not** extracted. The resolve it wraps is already on `AddonManager` and
directly callable; what is HomeView's is the toasts, the disabled Play button and the multi-name
ranking for a game's alternate titles — none of which a comic issue has or needs. Extracting it would
move a lot of code to share nothing.

Every failure ends the way the manga lane's failures end, through the same gate: the latch is
released, the sticky notice comes down, and a superseded or abandoned crossing dies silently rather
than shouting over whatever the user moved on to. The wording differs, because "no readable pages"
is not what a missing *file* means: no provider hit says the copy could not be found, and a provider
error says the provider could not be reached.

### 5. Pre-fetching the next volume

The reader reports page changes already (`pageInfoChanged`, `currentPage()`, `pageCount()`).
`MainWindow` watches for the current page reaching `pageCount() - kPrefetchLead` and starts steps 1–3
above for the next entry, without opening anything.

**Three pages, not the last page.** `reachedLastPage()` exists and would be the smaller change, but a
provider search has a budget of tens of seconds and a volume is several megabytes: starting on the
final page usually means the turn still waits. Three pages of reading is the cheapest lead that
routinely covers a search, and it is a named constant so it can be argued with.

**Rules, each of which exists to stop a specific waste:**

- One volume ahead. Never a queue.
- Only when a run is armed and has a next entry.
- Only forward. Paging backwards across a boundary is rare and does not deserve speculative traffic.
- Once per volume. A reader paging back and forth over the threshold must not restart it.
- Not when the file is already in the cache — the cache check comes first, and a re-read of a series
  you already downloaded costs nothing.
- A pre-fetch is never cancelled by leaving the reader. It is a file being written into a cache; the
  bytes are just as useful next time, and cancelling a half-written download is how partial files get
  left behind. What IS abandoned is the *opening*, which the existing generation tag already governs.

When the boundary press arrives, it looks for the finished file first, attaches to the in-flight
pre-fetch second, and starts the work itself third. The user-visible difference between the three is
only how long the sticky notice is on screen.

## Interaction with the cache-folder guard

d04c41b3 made `folderRunFor()` yield no run inside the app cache. That guard stays exactly as it is,
and this spec does not weaken it: the run for a cached volume comes from the catalog, never from what
else is in the folder. Anything opened from the cache with no catalog run behind it keeps having no
run — which is right, because a file in that bucket genuinely has no neighbours.

## Out of scope

- **Cache eviction.** The remote-docs cache is unbounded — the 985 MB ROM that caused the original
  bug has been sitting in it since 14 August. Pre-fetch makes it grow faster and gives the problem a
  new reason to be solved, but a retention policy is its own change with its own decisions (size cap
  or age cap, what a reading position in a file protects, whether a user-visible "downloaded" list
  should exist at all). Recorded here, not built here.
- **Books and audiobooks.** They bridge through the same path and have no "next"; nothing about this
  applies to them.
- **A "next volume" menu action.** The boundary press and its hint are the whole interface, as in the
  predecessor.
- **Other providers' comics.** The `parentId` plumbing is generic; only the Comic Vine arm of the AIO
  catalog is filled in here, because it is the only one shipping comic issues today.

## Testing

**`probe_chapterrun`** (pure, no I/O) covers the `Lane` migration and that a Catalog run orders `#3 —
Volume 3` style titles by number, including the mixed and ragged cases the predecessor pinned. It
already pins the cache-folder rule from d04c41b3.

**`probe_addon --comicorder`** stands a fake Comic Vine in front of the shipping `main.js`. It grows
assertions that every issue row carries the `parentId` of the volume it was drilled from, and that
`cvIssueMeta` reports one. The worker copy is held to the same by the source gate that sits beside
that probe.

**A new pure test for the query builder** — series title plus entry title in, provider query out —
because that string is the whole difference between finding Volume 3 and finding nothing, and it is
the one piece of this that is cheap to get wrong and expensive to notice.

**`EB_UITEST`** for the crossing itself, which is the part no headless probe can reach: open a
volume, page to the end, press forward, assert the reader is showing the next volume. The
[re-mint work](2026-08-29-remint-recents-links-design.md) recorded why a crossing must be exercised
more than once in a row — an operation that works exactly once is the failure mode this class of
feature produces — so the drive crosses two boundaries, not one.

## Risks

**Speculative downloads.** Pre-fetch spends bandwidth and disk on a volume the reader may abandon on
the last page. Bounded to one volume, one direction, one attempt. The honest mitigation is the
eviction policy that is out of scope above.

**A wrong "next".** The run is built from a container's children, so the failure mode is not "the
wrong series" (the capture guard and the parent id both prevent that) but "the wrong issue of the
right series" — a series whose numbering restarts per volume, which the predecessor already documents
as an accepted interleave. The crossing makes that misread *open* something rather than merely name
it, which is a real escalation of a known trade.

**A provider match that is not the volume asked for.** `resolveDocumentByQuery` is a search, and a
search that cannot find Volume 3 may return something else. It is given the series title to judge
against, which is the same protection the issue row itself has when pressed by hand — but a crossing
that opens the wrong file silently is worse than a row press that does, because nobody chose it. The
arrival toast names the volume it opened, so the reader is told what they got.

## What the drive found

Driven live on 2026-08-30 against the real catalog and file provider: a 63-volume series resumed from
Recents, two forward boundaries crossed, one backward, with the look-ahead running. It worked — and it
found four things the design and the plan had both missed, every one of them invisible to the probe
suite because every one of them is a silence.

**The crossing worked exactly once per volume, and the plan predicted the shape of that without seeing
it here.** The run was attached to the item at the bottom of the bridge, below the prefer-local early
return — so the open that DOWNLOADED a volume carried its neighbours and every re-open of that volume,
which is what a reader actually does, carried nothing. The capture now sits above every exit from that
block. This is the same class as the re-mint work's works-EXACTLY-once trap, arrived at from a different
direction, which is why the drive opens a volume that was already on disk.

**A Recent could never rebuild its run, three times over.** The row's item is typed `document` (a reader
kind, not a catalog type) so the type test excluded the exact case it was written for; `openDocumentPath`
takes a bare path, so a Recents replay never reached the code that rebuilds at all; and an addon routes
`/meta` BY TYPE, so an item without one gets `{}` back — which reads identically to "this has no
series". All three are fixed, and the last is why the rebuild now states the type it is asking about.

**`cat.title` really is `"Issues"`.** The children response came back titled exactly that, in the live
run, confirming the reason `parentTitle` was added: a run taking its series name from the children
response would have searched a file provider for "Issues 4" on every resumed volume.

**The look-ahead and the boundary press raced for one file.** §5 says the press should attach to an
in-flight pre-fetch; the first implementation only checked for a FINISHED one, so a press that landed
mid-fetch started a second download to the same path. Both renamed, one lost, and the crossing reported
"couldn't download" a volume that was on disk by the time the message appeared. `fetchDocumentToCache`
now coalesces by destination path, which fixes the class rather than the instance: one fetch per file,
however many callers want it, and they all get the same answer.

### Still open

**A crossed-into volume is not recorded in Recents.** `openCrossedComic` opens and arms; it does not call
`recordDocument`. Read three volumes by paging across boundaries, leave, and Continue Reading still shows
the one you started from. The reading POSITION is kept (the reader persists that by path), so nothing is
lost — but the shelf is wrong, and it is wrong in the direction of hiding what you were actually reading.

**Rows written before this change carry no addon id**, so an existing Continue Reading entry cannot
rebuild its run until that volume is opened once from its series list. Nothing backfills it, and nothing
can without asking every installed addon who owns an id.

**The addon half must ship with the client half.** The client change is inert against a deployed
`addons/aiocatalog/main.js` that predates it — `parentId` simply never arrives — and that is exactly what
the first drive attempt looked like: a rebuild that fired, asked, and got nothing back.

