# Page past the end of a chapter and the next one opens

**Date:** 2026-08-28
**Follows:** a469dc4 (`Merge: a click on a player's progress bar goes to that spot`)

## The problem

Reading a manga series today ends at every chapter boundary. You reach the last page, press Right,
and nothing happens — `ComicView::nextPage()` returns without a word:

```cpp
void ComicView::nextPage()
{
    if (current_ >= pageTotal() - 1) return;
    ...
}
```

To carry on you press Back, wait for the chapter list to come back, find where you were in it, and
pick the next row. That is four deliberate actions to continue reading a thing you were already
reading, repeated at every chapter — and a chapter is short. A folder of `.cbz` files on disk has
exactly the same shape and exactly the same dead end.

The app already knows how to do the opposite of this for video. `MainWindow::tryPlayNextEpisode()`
resolves and plays the next episode at the end of the current one, under an "Up next — finding the
next episode…" notice, with a latch so it cannot fire twice and a generation tag so a stale resolve
cannot drag you into an episode you have since navigated away from. Reading has no equivalent.

### What makes reading different from playing

Two things, and they decide most of the design.

**Nothing ends on its own.** A video reaches EOF; a page just sits there. So the trigger is not a
natural end — it is the press that is currently a no-op. That press is already the user asking for
the next thing, which is why this does not need a settings toggle: it is not autoplay, it is a
button that stops doing nothing.

**The boundary is slow and visible.** Opening a remote chapter resolves page URLs against the
provider, then downloads every page image, then packs a CBZ. That is seconds, sometimes more, with
the last page of the old chapter still on screen. Without a notice it reads as a dead keypress —
the exact failure the press is meant to fix. Hence a sticky notice for the whole hand-off.

## What we are building

Pressing next-page on the last page of a chapter opens the next chapter. Pressing previous-page on
the first page opens the previous chapter **at its last page**, so paging back and forth across a
boundary is continuous in both directions. A brief toast on reaching the last page says another
chapter is waiting. Both lanes are covered: remote manga chapters from the addon, and local comic
archives sitting in one folder.

## Where "what comes next" comes from

The reader is handed the answer when the chapter opens, rather than working it out at the boundary.

`HomeView::populate()` is, in its own comment, "the ONE ingress every row of `items_` passes
through" — so it is where a level's chapter list can be remembered, and it grows correctly as
further pages of a paginated list are appended. The remembered run is normalised into reading order
and travels to `MainWindow` with the open: `HomeView::openImagePages(title, key, pageUrls)` grows a
fourth argument carrying it, and the reader's copy is stored beside the reader. Sending the run with
the open, rather than exposing a "what follows this id?" query, is what keeps the reader's behaviour
independent of what the browse surface is showing by the time a boundary is reached.

Two alternatives were considered and rejected. **Asking `HomeView` at the boundary** avoids the copy
but ties reader behaviour to whatever the browse surface happens to be showing at that moment.
**Re-querying the provider for the series' chapter list at each boundary** is always complete, even
past the tail of a paginated list, but costs a round trip per chapter turn and needs a parent-series
id that a chapter item does not reliably carry. The captured run is enough for every path that
reaches the reader today; the re-query is the upgrade if the paginated tail ever bites.

## The pieces

### 1. `ChapterRun` — a pure value type

New header, `native/src/comic/ChapterRun.h`. No Qt widgets, no network, no I/O — so a probe can test
all of it directly, the way `comicSpreadScale` and `ComicPages::collator` already are.

```cpp
struct ChapterRun {
    struct Entry { QString id; QString title; }; // id = chapter item id, or a file path when local
    QVector<Entry> entries;   // READING order (ascending)
    int  index = -1;          // the entry currently open; -1 = no run
    bool local = false;       // entries are files on disk, not remote chapter ids
};
```

with free functions `inReadingOrder(listed)`, `nextIndex(run)`, `prevIndex(run)`, `indexOfId(run, id)`.

**The ordering rule.** Providers list chapters in whichever direction they please, and newest-first
is common. Reading order is not display order, so the run is normalised once, on capture:

1. Parse a chapter number from each title — the number following a `ch` / `chapter` / `#` marker if
   there is one, else the first number in the title. `Vol. 3 Ch. 24` must read as 24, not 3.
   Decimals count: `Ch. 12.5` is a real chapter.
2. If two or more entries parse and the **first parsed number is greater than the last**, the
   provider listed newest-first — reverse the whole list.
3. Otherwise keep list order.

Comparing the ends rather than demanding strict monotonicity is deliberate. Real chapter lists have
duplicates (several translations of one chapter) and gaps; a rule that bails on the first
non-monotonic pair would fall back to list order on lists that are plainly descending. And the
fallback is never wrong-looking — list order is what the user was just looking at.

Local runs skip all of this and order by filename through the existing natural-order collator
(`ComicPages::collator()`, `NaturalOrder.h`), which is what already orders pages inside an archive.
Building that collator inline is inert under the C locale — see issue #205 — so it must come from
there.

### 2. `ComicView` reports boundaries; it does not cross them

The reader gains two members and one signal:

```cpp
void setChapterNeighbours(bool hasPrev, bool hasNext); // MainWindow tells it what exists
signals:
    void chapterAdvanceRequested(int dir); // +1 = past the end, -1 = before the start
    void reachedLastPage();                // for the hint toast
```

`chapterAdvanceRequested` is emitted from exactly the two spots that currently return early in
`nextPage()`/`prevPage()`, and only when the corresponding neighbour flag is set. Everything else
about paging is untouched: no change to decode, scale, spread pairing or resume.

The reader does not open anything itself. It has no `AddonManager`, no notifier and no idea what a
chapter id is; keeping the crossing in `MainWindow` keeps the widget a widget.

Photo mode never gets neighbours set, so paging a folder of holiday pictures is unaffected — the
same line `openFolder()` already draws for resume and bookmarks.

`setChapterNeighbours` is also what the split-view pane never calls, so `MediaPane`'s reader behaves
exactly as it does today.

### 3. `MainWindow` owns the run and the hand-off

New state, named after the `tryPlayNextEpisode` precedent it copies:

```cpp
ChapterRun comicRun_;
bool chapterHandoffPending_ = false;
int  chapterHandoffGen_ = 0;
```

`comicRun_` is set or cleared at **every** comic-open site — the manga path, the local-archive branch
of `openLibraryItem`, and `openFolder` (which clears it). A run left over from a previous read must
never be able to attach itself to an unrelated file.

On `chapterAdvanceRequested(dir)`:

* **No neighbour** → `That's the last chapter.` / `You're at the first chapter.` Stay on the page.
* **A hand-off already pending** → ignore. Holding Right down at the end of a chapter must not start
  two loads.
* **Local run** → open the next archive directly. No network, no sticky notice.
* **Remote run** → latch, bump the generation, post a sticky
  `Loading "Chapter 12"…`, resolve the chapter's pages, then open. A chapter already in the CBZ cache
  skips straight through: `openImagePages` keys its cache by chapter id, so a re-crossed boundary is
  instant.

On arrival: forward changes nothing about how a comic opens — `openComic` restores that file's
stored page, which for a chapter being read for the first time is page 1. Backward overrides it and
jumps to the last page through the existing `gotoPage`, which is the point of the direction. Either way the
sticky notice comes down and a short `Chapter 12` toast confirms the landing, matching
`Up next: %1`. `comicRun_.index` moves to the entry that opened.

**Staleness.** A resolve that comes back with the wrong generation, or after the comic reader has
left the screen, clears the latch and the notice and does nothing else. This is
`nextEpHandoffStillOurs()` applied to reading, and it exists for the same reason: a drop path that
forgets the compensation leaves the file latched under a sticky notice with nothing behind it.

**Failure.** A resolve that returns no pages keeps the wording the chapter list already uses —
licensed chapters are not hosted there — and leaves you on the last page. It does not skip to the
chapter after: silently stepping over content is worse than stopping, and the chapter list is one
Back away.

### 4. The end-of-chapter hint

`reachedLastPage()` with a known next chapter shows a brief toast:

> End of Chapter 11 — page forward for Chapter 12.

Once per opened chapter, so paging back and forth over the last page does not nag. No arrow glyph in
the wording: the controller-aware pad-glyph work (2026-08-28 spec, commits f91b05d / 26e5a83) is
specced but not built, and a bare `→` is wrong on a pad. When it lands, this string is one of its
customers.

There is no matching hint at the first page. Reaching page 1 going backwards is not a moment that
needs announcing, and a toast on every chapter open would be noise.

## Out of scope

* **Prefetching the next chapter** while you read the current one. It would hide the whole wait, and
  it is a second network policy (when to start, how much to hold, what to do about a chapter you
  never reach) attached to a feature that does not need it to work.
* **A settings toggle.** The advance is a keypress the user makes, not something that happens to
  them. A toggle would also cost both settings builders and the sync bundle.
* **Split-view.** `MediaPane` gets no run and no behaviour change.
* **Recent re-entry.** Manga chapters are not recorded in Recent today (`openImagePages` does not
  call `recordDocument`), so every chapter open comes through a list that can supply a run. If they
  are ever recorded, a re-opened chapter simply has no run and behaves as it does today.

## Testing

**`probe_chapterrun`** — new, pure, registered in all three places `CONTRIBUTING.md` requires:

* descending list reversed; ascending list kept
* ragged / unparsable titles keep list order
* `Vol. 3 Ch. 24` parses as 24, not 3; `Ch. 12.5` parses as 12.5
* duplicate chapter numbers (multiple translations) do not defeat the reversal
* single-entry and empty runs: no neighbours, no crash
* `nextIndex`/`prevIndex` at both ends return "none"
* local run building orders `ch2.cbz` before `ch10.cbz` through the natural collator

**Reader boundary behaviour** — pressing next on the last page emits once and does not move the
page; mid-comic presses emit nothing; photo mode emits nothing; a two-up spread on the final pages
reaches the same boundary condition it does today.

**Live pass** under `EB_UITEST` (see `native/tools/uitest.py`): open a real manga chapter, page to
the end, confirm the hint toast, press forward, confirm the sticky loading notice and the arrival
toast, then page backwards across the boundary and confirm it lands on the previous chapter's last
page. Then the same over a folder of local `.cbz` files, which should cross with no notice at all.
