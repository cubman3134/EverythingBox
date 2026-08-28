# Comic/manga chapter auto-advance — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Pressing next-page on the last page of a comic/manga chapter opens the next chapter (and previous-page on the first page opens the previous chapter at its last page), under a visible loading notice.

**Architecture:** A pure `ChapterRun` value type carries the sibling chapters in reading order. `HomeView` captures it when a chapter list is populated and ships it with the open; `MainWindow` stores it beside the reader and owns the crossing (latch + generation tag + sticky notice, copied from `tryPlayNextEpisode`). `ComicView` only *reports* that a press fell off an end — it never opens anything itself.

**Tech Stack:** C++17, Qt 6 (Widgets), CMake (Visual Studio multi-config generator), the in-tree headless probe suite.

**Spec:** `docs/superpowers/specs/2026-08-28-comic-chapter-autoadvance-design.md`

## Global Constraints

- **No AI attribution in commits.** No `Co-Authored-By: Claude` trailer, no "Generated with Claude Code" line, no tool name in the message body. Conventional prefixes (`feat:`, `fix:`, `docs:`) still apply. See repo root `CLAUDE.md`.
- **The working tree is shared with other sessions.** Never `git commit -a` and never `git add -A`. Always `git add` the exact files you changed. A version-bump hook will add `native/CMakeLists.txt` and `native/src/main.cpp` (the `0.6.x` bump) to your commit — that is expected and correct; leave it alone. Do not switch branches. Work on the current branch.
- **Byte-exact file edits.** `native/tools/run-headless-probes.sh` is CRLF and `native/CMakeLists.txt` hides a lone CR. Append to them; never rewrite or normalise line endings in either file.
- **A new pure component gets a probe registered in three places** (`CONTRIBUTING.md`): `native/CMakeLists.txt`, the runner loop in `native/tools/run-headless-probes.sh`, and the `--target` list in `.github/workflows/ci.yml`. Miss one and the probe silently never runs.
- **Build config is Release on a multi-config generator:** `cmake --build build --config Release --target <t>`. Probe binaries land in `build/Release/`.
- **Build and gate synchronously.** Run the build to completion in one command and read its output; do not hand control back "waiting for a monitor".
- **`QCollator` must come from `NaturalOrder::collator()`** (via `ComicPages::collator()`). A locally constructed `QCollator` with `setNumericMode(true)` is inert under the C locale and orders `page10` before `page2` on CI — issue #205.
- **No new user-facing setting.** The advance is a keypress the user makes; there is no toggle in either settings builder.

---

### Task 1: `ChapterRun` — the pure run type and its ordering

**Files:**
- Create: `native/src/comic/ChapterRun.h`
- Create: `native/tools/probe_chapterrun.cpp`
- Modify: `native/CMakeLists.txt` (add a probe target next to the `probe_tar` block at line ~998)
- Modify: `native/tools/run-headless-probes.sh:676` (the `for p in "probe_… …-OK"` loop)
- Modify: `.github/workflows/ci.yml:68` (the `Build probes` `--target` list)

**Interfaces:**
- Consumes: `ComicPages::collator()` from `native/src/comic/ComicPageOrder.h`.
- Produces, for Tasks 3–5:
  - `struct ChapterRun { struct Entry { QString id; QString title; }; QVector<Entry> entries; int index = -1; bool local = false; bool isValid() const; bool hasNext() const; bool hasPrev() const; }`
  - `double ChapterOrder::chapterNumber(const QString& title, bool* ok)`
  - `QVector<ChapterRun::Entry> ChapterOrder::inReadingOrder(const QVector<ChapterRun::Entry>& listed)`
  - `int ChapterOrder::indexOfId(const ChapterRun& run, const QString& id)`
  - `ChapterRun ChapterOrder::fromChapterItems(const QVector<ChapterRun::Entry>& listed, const QString& currentId)`
  - `ChapterRun ChapterOrder::fromFileNames(const QString& folder, const QStringList& fileNames, const QString& currentFileName)`

- [ ] **Step 1: Write the failing probe**

Create `native/tools/probe_chapterrun.cpp`:

```cpp
// Headless check of the chapter-run ordering (src/comic/ChapterRun.h): the number parsed out of a chapter
// title, the newest-first reversal that turns a provider's DISPLAY order into READING order, the natural
// filename order behind a local folder run, and the neighbour arithmetic the reader's boundary presses use.
// PURE — no widgets, no network, no disk — so it links against QtCore alone. Prints CHAPTERRUN-OK on success;
// any failure prints CHAPTERRUN-FAIL <cond> (line) and exits non-zero.
//
// THE BUG IT PINS: "next chapter" is not "the next row". Providers list chapters newest-first as often as
// oldest-first, so advancing by list position walks a descending list BACKWARDS — press forward at the end of
// chapter 12 and land in chapter 11. inReadingOrder() normalises once, on capture.
//
// ORACLE IS INDEPENDENT OF THE CODE UNDER TEST: every expected order below is written out by hand as the
// sequence a reader would read, never by calling inReadingOrder().
#include "ChapterRun.h"

#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "CHAPTERRUN-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

static QVector<ChapterRun::Entry> entries(const QStringList& titles)
{
    QVector<ChapterRun::Entry> v;
    for (int i = 0; i < titles.size(); ++i)
        v.append({ QStringLiteral("id%1").arg(i), titles[i] });
    return v;
}
static QStringList titlesOf(const QVector<ChapterRun::Entry>& v)
{
    QStringList out;
    for (const ChapterRun::Entry& e : v) out << e.title;
    return out;
}

int main()
{
    // ---- The number parsed out of a title -----------------------------------------------------------------
    {
        bool ok = false;
        CHECK(ChapterOrder::chapterNumber(QStringLiteral("Chapter 12"), &ok) == 12.0);   CHECK(ok);
        CHECK(ChapterOrder::chapterNumber(QStringLiteral("Ch. 12.5"), &ok) == 12.5);     CHECK(ok);
        CHECK(ChapterOrder::chapterNumber(QStringLiteral("#7"), &ok) == 7.0);            CHECK(ok);
        // A volume marker in front of the chapter marker must NOT win: this is 24, not 3.
        CHECK(ChapterOrder::chapterNumber(QStringLiteral("Vol. 3 Ch. 24"), &ok) == 24.0); CHECK(ok);
        // No marker at all: the first number in the title is the chapter.
        CHECK(ChapterOrder::chapterNumber(QStringLiteral("24 - The Sky"), &ok) == 24.0); CHECK(ok);
        // Nothing numeric: reported as unparsed, and the value is not used.
        ChapterOrder::chapterNumber(QStringLiteral("Oneshot"), &ok);
        CHECK(!ok);
    }

    // ---- Newest-first lists are reversed; oldest-first lists are left alone --------------------------------
    {
        // A descending list. Reading order is 10, 11, 12 — written out by hand.
        const QStringList desc{ QStringLiteral("Chapter 12"), QStringLiteral("Chapter 11"), QStringLiteral("Chapter 10") };
        const QStringList read = titlesOf(ChapterOrder::inReadingOrder(entries(desc)));
        CHECK(read.size() == 3);
        CHECK(read.value(0) == QStringLiteral("Chapter 10"));
        CHECK(read.value(1) == QStringLiteral("Chapter 11"));
        CHECK(read.value(2) == QStringLiteral("Chapter 12"));
        // Tripwire against dropping the reversal: the list must NOT come back in the order it went in.
        CHECK(read != desc);
    }
    {
        const QStringList asc{ QStringLiteral("Chapter 1"), QStringLiteral("Chapter 2"), QStringLiteral("Chapter 3") };
        CHECK(titlesOf(ChapterOrder::inReadingOrder(entries(asc))) == asc);
    }
    {
        // Duplicates (two translations of one chapter) and gaps must not defeat the reversal: the rule
        // compares the ends, not every neighbouring pair.
        const QStringList desc{ QStringLiteral("Chapter 9"), QStringLiteral("Chapter 9"),
                                QStringLiteral("Chapter 5"), QStringLiteral("Chapter 1") };
        const QStringList read = titlesOf(ChapterOrder::inReadingOrder(entries(desc)));
        CHECK(read.value(0) == QStringLiteral("Chapter 1"));
        CHECK(read.value(3) == QStringLiteral("Chapter 9"));
    }
    {
        // Nothing parses: keep list order, which is what the user was just looking at.
        const QStringList named{ QStringLiteral("Prologue"), QStringLiteral("Interlude"), QStringLiteral("Epilogue") };
        CHECK(titlesOf(ChapterOrder::inReadingOrder(entries(named))) == named);
    }

    // ---- Building a run from a chapter list ---------------------------------------------------------------
    {
        const QVector<ChapterRun::Entry> listed = entries(
            { QStringLiteral("Chapter 12"), QStringLiteral("Chapter 11"), QStringLiteral("Chapter 10") });
        // "id1" is Chapter 11 — the middle of the run whichever way it is ordered.
        const ChapterRun run = ChapterOrder::fromChapterItems(listed, QStringLiteral("id1"));
        CHECK(run.isValid());
        CHECK(!run.local);
        CHECK(run.index == 1);
        CHECK(run.hasNext());
        CHECK(run.hasPrev());
        CHECK(run.entries.value(run.index + 1).title == QStringLiteral("Chapter 12")); // forward = later chapter
        CHECK(run.entries.value(run.index - 1).title == QStringLiteral("Chapter 10")); // back = earlier chapter
        CHECK(ChapterOrder::indexOfId(run, QStringLiteral("id0")) == 2);               // Chapter 12 moved to the end
        CHECK(ChapterOrder::indexOfId(run, QStringLiteral("nope")) == -1);
    }
    {
        // The current chapter is not in the list (it was opened from somewhere else): no run, no neighbours.
        const ChapterRun run = ChapterOrder::fromChapterItems(entries({ QStringLiteral("Chapter 1") }),
                                                             QStringLiteral("elsewhere"));
        CHECK(!run.isValid());
        CHECK(!run.hasNext());
        CHECK(!run.hasPrev());
    }
    {
        const ChapterRun empty;
        CHECK(!empty.isValid());
        CHECK(!empty.hasNext());
        CHECK(!empty.hasPrev());
    }
    {
        // A one-chapter run is valid and has no neighbours in either direction.
        const ChapterRun run = ChapterOrder::fromChapterItems(entries({ QStringLiteral("Chapter 1") }),
                                                             QStringLiteral("id0"));
        CHECK(run.isValid());
        CHECK(!run.hasNext());
        CHECK(!run.hasPrev());
    }

    // ---- A local folder run: natural filename order, never the newest-first reversal -----------------------
    {
        const QStringList files{ QStringLiteral("ch10.cbz"), QStringLiteral("ch2.cbz"), QStringLiteral("ch1.cbz") };
        const ChapterRun run = ChapterOrder::fromFileNames(QStringLiteral("C:/comics/series"), files,
                                                          QStringLiteral("ch2.cbz"));
        CHECK(run.local);
        CHECK(run.isValid());
        CHECK(run.entries.size() == 3);
        // Natural order by hand: ch1, ch2, ch10 — NOT the lexical ch1, ch10, ch2.
        CHECK(run.entries.value(0).title == QStringLiteral("ch1"));
        CHECK(run.entries.value(1).title == QStringLiteral("ch2"));
        CHECK(run.entries.value(2).title == QStringLiteral("ch10"));
        CHECK(run.index == 1);
        CHECK(run.hasNext());
        CHECK(run.hasPrev());
        // The id is the path to open, folder and all.
        CHECK(run.entries.value(2).id == QStringLiteral("C:/comics/series/ch10.cbz"));
    }
    {
        // A folder holding just this one file: a valid run with nowhere to go.
        const ChapterRun run = ChapterOrder::fromFileNames(QStringLiteral("C:/comics"),
                                                          { QStringLiteral("only.cbz") },
                                                          QStringLiteral("only.cbz"));
        CHECK(run.isValid());
        CHECK(!run.hasNext());
        CHECK(!run.hasPrev());
    }

    if (failures == 0) std::printf("CHAPTERRUN-OK\n");
    return failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Register the probe in all three places**

In `native/CMakeLists.txt`, immediately after the `probe_tar` block (ends at line ~1002, the `target_link_libraries(probe_tar PRIVATE Qt6::Core)` line), append:

```cmake

    # Chapter-run ordering (src/comic/ChapterRun.h): the number parsed out of a chapter title, the newest-first
    # reversal that turns a provider's display order into reading order, and the natural filename order behind a
    # local folder run. Header-only + QtCore (QCollator via NaturalOrder — issue #205), so it links lean.
    add_executable(probe_chapterrun tools/probe_chapterrun.cpp
        src/comic/ChapterRun.h src/comic/ComicPageOrder.h
        src/core/NaturalOrder.h)  # header-only units under test
    target_include_directories(probe_chapterrun PRIVATE src src/comic)
    target_link_libraries(probe_chapterrun PRIVATE Qt6::Core)
```

In `native/tools/run-headless-probes.sh:676` (one long line — the file is CRLF; edit it in place, do not rewrite it), insert `"probe_chapterrun CHAPTERRUN-OK" ` immediately before `"probe_comicfit COMICFIT-OK"`.

In `.github/workflows/ci.yml:68`, insert ` probe_chapterrun` immediately before ` probe_comicfit` in the `--target` list.

- [ ] **Step 3: Run the probe build to verify it fails**

```bash
cmake --build build --config Release --target probe_chapterrun
```

Expected: FAIL — `Cannot open include file: 'ChapterRun.h'`.

- [ ] **Step 4: Write `ChapterRun.h`**

Create `native/src/comic/ChapterRun.h`:

```cpp
// The chapters either side of the one you are reading, in READING order.
//
// "The next chapter" is not "the next row". A provider lists chapters in whichever direction it pleases and
// newest-first is common, so advancing by list position walks a descending list backwards — forward at the end
// of chapter 12 lands in chapter 11. This header normalises the order ONCE, when the run is captured, so every
// consumer downstream can just do index + 1.
//
// Pure: no widgets, no network, no disk. Unit-tested by probe_chapterrun.
#pragma once
#include "ComicPageOrder.h"   // ComicPages::collator() — the #205-safe natural-order collator

#include <QFileInfo>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QVector>

#include <algorithm>

// The chapters of one series (or the comic archives of one folder), plus which of them is open.
struct ChapterRun
{
    // `id` is a chapter item id for a remote run, or the full path to open for a local one. `title` is what
    // the user sees named in the hint and arrival toasts.
    struct Entry { QString id; QString title; };

    QVector<Entry> entries;   // READING order (ascending), already normalised
    int  index = -1;          // the entry currently open; -1 = no run (nothing to advance to)
    bool local = false;       // entries are files on disk, not remote chapter ids

    bool isValid() const { return index >= 0 && index < entries.size(); }
    bool hasNext() const { return isValid() && index + 1 < entries.size(); }
    bool hasPrev() const { return isValid() && index > 0; }
};

namespace ChapterOrder
{
    // The chapter number a title names, or ok=false when it names none. The chapter MARKER wins over any
    // number in front of it, because "Vol. 3 Ch. 24" is chapter 24 and reading it as volume 3 would order a
    // whole series by its volumes. Only when there is no marker does the first number in the title count.
    inline double chapterNumber(const QString& title, bool* ok)
    {
        static const QRegularExpression marked(
            QStringLiteral("(?:\\bch(?:apter)?\\b\\.?|#)\\s*(\\d+(?:\\.\\d+)?)"),
            QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression anyNum(QStringLiteral("(\\d+(?:\\.\\d+)?)"));
        QRegularExpressionMatch m = marked.match(title);
        if (!m.hasMatch()) m = anyNum.match(title);
        if (!m.hasMatch()) { if (ok) *ok = false; return -1.0; }
        bool parsed = false;
        const double v = m.captured(1).toDouble(&parsed);
        if (ok) *ok = parsed;
        return parsed ? v : -1.0;
    }

    // Display order -> reading order. Compares the FIRST parsed number with the LAST rather than demanding
    // strict monotonicity: real chapter lists carry duplicates (several translations of one chapter) and gaps,
    // and a rule that bailed on the first non-monotonic pair would leave plainly-descending lists reversed the
    // wrong way. When too little parses to tell, list order stands — that is what the user was just looking at.
    inline QVector<ChapterRun::Entry> inReadingOrder(const QVector<ChapterRun::Entry>& listed)
    {
        double first = 0.0, last = 0.0;
        int parsedCount = 0;
        for (const ChapterRun::Entry& e : listed)
        {
            bool ok = false;
            const double v = chapterNumber(e.title, &ok);
            if (!ok) continue;
            if (parsedCount == 0) first = v;
            last = v;
            ++parsedCount;
        }
        if (parsedCount < 2 || first <= last) return listed;
        QVector<ChapterRun::Entry> out = listed;
        std::reverse(out.begin(), out.end());
        return out;
    }

    inline int indexOfId(const ChapterRun& run, const QString& id)
    {
        for (int i = 0; i < run.entries.size(); ++i)
            if (run.entries[i].id == id) return i;
        return -1;
    }

    // A run over a browsed chapter list. `currentId` not being in the list leaves index at -1, which reads as
    // "no neighbours" everywhere downstream — a chapter opened from somewhere the list never covered simply
    // behaves as it did before this feature existed.
    inline ChapterRun fromChapterItems(const QVector<ChapterRun::Entry>& listed, const QString& currentId)
    {
        ChapterRun run;
        run.local = false;
        run.entries = inReadingOrder(listed);
        run.index = indexOfId(run, currentId);
        return run;
    }

    // A run over the comic archives sitting in one folder. Natural filename order (ch2 before ch10) through
    // the shared collator, and NEVER the newest-first reversal above: a folder listing is not a provider's
    // display order, and a file named "Chapter 12.cbz" beside "Chapter 2.cbz" is already in reading order.
    inline ChapterRun fromFileNames(const QString& folder, const QStringList& fileNames,
                                    const QString& currentFileName)
    {
        ChapterRun run;
        run.local = true;
        QStringList names = fileNames;
        const QCollator coll = ComicPages::collator();
        std::sort(names.begin(), names.end(),
                  [&coll](const QString& a, const QString& b) { return coll.compare(a, b) < 0; });
        for (const QString& n : names)
            run.entries.append({ folder + QStringLiteral("/") + n, QFileInfo(n).completeBaseName() });
        run.index = indexOfId(run, folder + QStringLiteral("/") + currentFileName);
        return run;
    }
}
```

- [ ] **Step 5: Build and run the probe to verify it passes**

```bash
cmake --build build --config Release --target probe_chapterrun && ./build/Release/probe_chapterrun.exe
```

Expected: build succeeds, output is exactly `CHAPTERRUN-OK`, exit code 0.

- [ ] **Step 6: Verify the probe actually runs in the suite**

```bash
BUILD_DIR=build bash native/tools/run-headless-probes.sh 2>&1 | grep -i chapterrun
```

Expected: a line naming `probe_chapterrun` with a pass. If nothing matches, the runner registration in Step 2 did not take — fix it before committing. (`probe_addon` was maintained for a long time while wired into neither the runner nor CI, so every assertion in it gated nothing.)

- [ ] **Step 7: Commit**

```bash
git add native/src/comic/ChapterRun.h native/tools/probe_chapterrun.cpp native/CMakeLists.txt native/tools/run-headless-probes.sh .github/workflows/ci.yml && git commit -m "feat: a chapter run knows which chapter comes next in reading order"
```

---

### Task 2: `ComicView` reports the two boundary presses

**Files:**
- Modify: `native/src/comic/ComicView.h` (the free functions beside `comicSpreadScale` at line ~25; the public/slots/signals blocks; the private members)
- Modify: `native/src/comic/ComicView.cpp:456-465` (`nextPage`/`prevPage`), `:357-371` (`showPage`), `:300-325` (`openFolder`), `:219-291` (`openComic`)
- Modify: `native/tools/probe_comicfit.cpp` (add boundary-predicate assertions)

**Interfaces:**
- Consumes: nothing from Task 1 — `ComicView` deliberately knows nothing about `ChapterRun`.
- Produces, for Task 3:
  - `signals: void chapterAdvanceRequested(int dir)` — `+1` past the end, `-1` before the start, emitted
    UNCONDITIONALLY: the reader reports the boundary and nothing else, and `MainWindow` alone decides what
    one means
  - `signals: void reachedLastPage()`
  - `inline bool comicPastEnd(int current, int pageTotal)` and `inline bool comicBeforeStart(int current)` in `ComicView.h`

- [ ] **Step 1: Write the failing assertions**

In `native/tools/probe_comicfit.cpp`, immediately before the `if (failures == 0) std::printf("COMICFIT-OK\n");` line at the end of `main()`, add:

```cpp
    // ---- The two page-boundary predicates (src/comic/ComicView.h) ------------------------------------------
    // These decide when a next/previous press falls off the end of the comic — the presses that used to be
    // silent no-ops and now ask for the neighbouring chapter. Written out by hand: a 10-page comic is past the
    // end only when the leftmost page shown is page 10 (index 9), whether or not a spread is on screen.
    {
        CHECK(!comicPastEnd(0, 10));
        CHECK(!comicPastEnd(8, 10));
        CHECK(comicPastEnd(9, 10));
        CHECK(comicPastEnd(9, 9));       // a defensive out-of-range current still reads as "past the end"
        CHECK(comicPastEnd(0, 1));       // a one-page comic is at its end on page one
        CHECK(comicBeforeStart(0));
        CHECK(!comicBeforeStart(1));
        CHECK(!comicBeforeStart(9));
    }
```

- [ ] **Step 2: Run it to verify it fails**

```bash
cmake --build build --config Release --target probe_comicfit
```

Expected: FAIL — `'comicPastEnd': identifier not found`.

- [ ] **Step 3: Add the predicates to `ComicView.h`**

In `native/src/comic/ComicView.h`, immediately after the closing `}` of `comicSpreadScale` (line ~29, before `class ComicView`), add:

```cpp
// The two page-boundary questions, pulled out of nextPage()/prevPage() so the decision that used to be a
// silent early return is a named, unit-tested rule (probe_comicfit). Each is the exact condition under which
// the press has nowhere left to go inside THIS comic — and is therefore the moment to ask for the next one.
inline bool comicPastEnd(int current, int pageTotal) { return current >= pageTotal - 1; }
inline bool comicBeforeStart(int current)            { return current <= 0; }
```

- [ ] **Step 4: Add the two signals to `ComicView.h`**

Nothing else: the reader is told nothing about its neighbours. It has no `AddonManager`, no notifier and no
idea what a chapter id is, so what a boundary press MEANS is entirely `MainWindow`'s to know — and a pair of
mirrored `hasPrev`/`hasNext` flags could never disagree with the run they were pushed from, while making
`MainWindow`'s "That's the last chapter." message unreachable dead code.

In the `signals:` block, after `void pageInfoChanged();`, add:

```cpp
    // A page press fell off an end: +1 = past the last page, -1 = before the first. Emitted UNCONDITIONALLY —
    // the reader reports the boundary and nothing else. It has no AddonManager, no notifier and no idea what a
    // chapter id is, so whether a neighbour exists (and what to say when it does not) is MainWindow's to know;
    // a comic with no run there is silent, exactly as this press always was. MediaPane's own ComicView never
    // connects this, so the split pane's boundary presses stay the inert no-op they have always been.
    void chapterAdvanceRequested(int dir);
    void reachedLastPage();                // the last page is now on screen (hint that another chapter follows)
```

- [ ] **Step 5: Emit from the two presses**

In `native/src/comic/ComicView.cpp`, replace `nextPage()`/`prevPage()` (lines 456-465) with:

```cpp
void ComicView::nextPage()
{
    // The press that used to be a silent no-op: at the last page it reports the boundary instead. Nothing is
    // opened here, and nothing is judged here either — MainWindow owns the crossing, knows whether a next
    // chapter exists, and is the only place that can say "That's the last chapter." when one does not. So the
    // report is unconditional; a comic with no run there is answered with the same silence as before.
    if (comicPastEnd(current_, pageTotal()))
    {
        emit chapterAdvanceRequested(+1);
        return;
    }
    showPage(qMin(current_ + (spreadActive() ? 2 : 1), pageTotal() - 1)); // advance a whole spread in book mode
}
void ComicView::prevPage()
{
    if (comicBeforeStart(current_))
    {
        emit chapterAdvanceRequested(-1);   // likewise unconditional — see nextPage()
        return;
    }
    showPage(qMax(current_ - ((fit_ && twoUp_) ? 2 : 1), 0));
}
```

- [ ] **Step 6: Announce the last page**

In `showPage` (line ~370), replace the final `emit pageInfoChanged();` with:

```cpp
    emit pageInfoChanged();                     // mirror the page move into the themed chrome
    // The end of the chapter is the one moment worth telling the user another one is waiting. MainWindow owns
    // the once-per-open throttling — this fires every time the last page comes up, including on the way back.
    if (!photoMode_ && comicPastEnd(current_, pageTotal())) emit reachedLastPage();
```

`openComic`/`openFolder` need no change at all. The reader holds no per-comic chapter state that could go
stale, so opening a new file leaves nothing behind: the run belongs to `MainWindow`, which arms one — or an
empty one, for a photo folder (issue #102) — at every open site.

- [ ] **Step 7: Build and run the probe to verify it passes**

```bash
cmake --build build --config Release --target probe_comicfit everythingbox && ./build/Release/probe_comicfit.exe
```

Expected: both targets build, output is exactly `COMICFIT-OK`, exit code 0. Building `everythingbox` too is what proves the header change did not break the app's own compile.

- [ ] **Step 8: Commit**

```bash
git add native/src/comic/ComicView.h native/src/comic/ComicView.cpp native/tools/probe_comicfit.cpp && git commit -m "feat: the comic reader reports a page press that falls off either end"
```

---

### Task 3: The local folder lane, end to end

**Files:**
- Modify: `native/src/ui/MainWindow.h` (new members beside `comic_` at line ~704; new private methods beside `tryPlayNextEpisode` at line ~1087)
- Modify: `native/src/ui/MainWindow.cpp:1668-1716` (the `comic_` connect block), `:15256-15262` (the local `.cbz` branch of `openLibraryItem`), `:2980` (`presentComic`)

**Interfaces:**
- Consumes: `ChapterRun`, `ChapterOrder::fromFileNames`, `ChapterOrder::indexOfId` (Task 1); `ComicView::chapterAdvanceRequested`, `reachedLastPage`, `ComicView::isComicFile`, `ComicView::gotoPage`, `ComicView::pageCount` (Task 2).
- Produces, for Tasks 4–5:
  - `ChapterRun MainWindow::comicRun_`
  - `void MainWindow::armComicRun(const ChapterRun& run)` — stores it, records the file it belongs to, resets the hint throttle
  - `ChapterRun MainWindow::folderRunFor(const QString& comicPath) const` — the run over the comic archives sharing that file's folder
  - `void MainWindow::onChapterAdvanceRequested(int dir)`
  - `void MainWindow::openLocalChapter(int targetIndex, int dir)`
  - `bool MainWindow::chapterHandoffPending_`, `int MainWindow::chapterHandoffGen_`, `bool MainWindow::chapterHintShown_`

- [ ] **Step 1: Add the state and the wiring**

In `native/src/ui/MainWindow.h`, add near the top with the other includes:

```cpp
#include "../comic/ChapterRun.h"
```

After the `ReaderChromeHost* comicHost_ = nullptr;` line (~705), add:

```cpp
    // The chapters either side of the comic currently open — a browsed manga chapter list (HomeView captures
    // it and ships it with the open) or the other archives in a local file's folder. Set or CLEARED at every
    // comic-open site: a run left over from a previous read must never attach itself to an unrelated file.
    ChapterRun comicRun_;
    // The hand-off latch and its staleness tag, the shape nextEpPending_/nextEpGen_ already use here. A remote
    // chapter crossing is asynchronous, so a second press must not start a second load and a resolve that
    // comes back after the reader has gone must not drag the user into a chapter they left.
    bool chapterHandoffPending_ = false;
    int  chapterHandoffGen_ = 0;
    bool chapterHintShown_ = false;   // the end-of-chapter hint is once per opened chapter, not once per press
    // The file comicRun_ was armed FOR. ComicView::reachedLastPage() carries no payload and fires from inside
    // openComic() — before this controller can arm the new run — so without an identity to compare against,
    // the hint would name a chapter from the comic the reader just LEFT. Every single-page comic reaches that
    // window on every open (page clamps to 0, which is already the last page), so it is not a rare race.
    QString comicRunKey_;
```

In the private methods block after `bool nextEpHandoffStillOurs(int gen);` (~line 1100), add:

```cpp
    // ---- Chapter auto-advance (paging past the end of a comic/manga chapter) -----------------------------
    void armComicRun(const ChapterRun& run);        // store it + note the file it belongs to, reset the hint
    bool comicAtLastPage() const;                   // is the reader showing the final page right now?
    ChapterRun folderRunFor(const QString& comicPath) const; // the archives sharing this file's folder
    void onChapterAdvanceRequested(int dir);        // a boundary press: cross to the neighbouring chapter
    void openLocalChapter(int targetIndex, int dir); // the local-file lane (synchronous, no network)
    void onComicReachedLastPage();                  // the once-per-chapter "another one follows" hint
```

- [ ] **Step 2: Implement the shared arming, the hint, and the local crossing**

In `native/src/ui/MainWindow.cpp`, immediately after `MainWindow::presentComic()` (which ends around line 2995), add:

```cpp
// ---- Chapter auto-advance ----------------------------------------------------------------------------------
// Paging past the last page of a chapter opens the next one; paging back off page one opens the previous one at
// ITS last page. The press is the whole trigger — a comic has no natural end to chain from the way a video does,
// and the press it replaces was a silent no-op, which is why this needs no autoplay-style setting.
//
// The reader reports the boundary and nothing else (ComicView::chapterAdvanceRequested). Everything that knows
// what a chapter IS lives here, next to tryPlayNextEpisode(), whose latch and generation tag this copies.

void MainWindow::armComicRun(const ChapterRun& run)
{
    comicRun_ = run;
    chapterHintShown_ = false;                 // a new chapter gets its own one hint
    comicRunKey_ = comic_ ? comic_->itemKey() : QString(); // the file this run belongs to (see comicRunKey_)
    // Nothing is pushed into the reader: it reports every boundary press unconditionally and this run is the
    // only answer to what one means. Mirroring hasPrev()/hasNext() into it would only let the two disagree.
    // The reader may ALREADY be sitting on the last page by the time we arm — a comic resumed at its end, a
    // one-page chapter, a bookmark restore. Its reachedLastPage() fired during the open, when the run still
    // named the previous file and was correctly ignored, so this is the only place that case can be caught.
    if (comicAtLastPage()) onComicReachedLastPage();
}

// Is the reader showing the final page? Mirrors ComicView's own comicPastEnd() through the 1-based hosted
// accessors, so a two-up spread answers the same way the reader's own boundary press does.
bool MainWindow::comicAtLastPage() const
{
    return comic_ && comic_->currentPage() >= comic_->pageCount();
}

// The comic archives sharing this file's folder ARE its chapters — paging past the last page opens the next
// file. Written once because three open sites need it (the library branch, the open-a-file branch, and the
// local crossing itself re-derives nothing). A folder holding only this file yields a valid run with no
// neighbours, which reads as exactly the behaviour the reader always had.
ChapterRun MainWindow::folderRunFor(const QString& comicPath) const
{
    const QFileInfo fi(comicPath);
    QStringList siblings;
    const QFileInfoList found = QDir(fi.absolutePath()).entryInfoList(QDir::Files, QDir::NoSort);
    for (const QFileInfo& f : found)
        if (ComicView::isComicFile(f.filePath())) siblings << f.fileName();
    return ChapterOrder::fromFileNames(fi.absolutePath(), siblings, fi.fileName());
}

// The last page is on screen. Say once, briefly, that another chapter follows — otherwise nobody discovers the
// press, because until now it did nothing. No arrow glyph in the wording: the pad-glyph work is specced and not
// yet built, and a bare "→" is wrong on a controller.
void MainWindow::onComicReachedLastPage()
{
    if (chapterHintShown_ || !comicRun_.hasNext()) return;
    // The run must belong to the file actually open. openComic() sets its path and shows the resumed page —
    // emitting this — BEFORE returning to the caller that arms the new run, so during that window comicRun_
    // still describes the previous comic. Arming re-syncs the two and re-asks (see armComicRun).
    if (comic_ && comic_->itemKey() != comicRunKey_) return;
    chapterHintShown_ = true;
    notify(tr("End of “%1” — page forward for “%2”.")
               .arg(comicRun_.entries[comicRun_.index].title,
                    comicRun_.entries[comicRun_.index + 1].title), kFeedbackShort);
}

void MainWindow::onChapterAdvanceRequested(int dir)
{
    const bool forward = dir > 0;
    if (!comicRun_.isValid()) return;                    // no run: exactly the no-op this press always was
    if (forward ? !comicRun_.hasNext() : !comicRun_.hasPrev())
    {
        notify(forward ? tr("That's the last chapter.") : tr("You're at the first chapter."), kFeedbackShort);
        return;
    }
    // A hand-off already in flight owns this boundary. Holding the key down at the end of a chapter would
    // otherwise start a second load, and the later one would re-open a chapter the first already opened.
    if (chapterHandoffPending_) return;
    const int target = comicRun_.index + (forward ? 1 : -1);
    if (comicRun_.local) { openLocalChapter(target, dir); return; }
    openRemoteChapter(target, dir);                       // Task 5
}

// The local lane: the next archive in this file's folder. Synchronous — no resolve, no download — so there is
// nothing to put a loading notice on. Landing backwards jumps to the last page, which is the point of the
// direction: paging back across a boundary must continue the reading, not restart the previous chapter.
void MainWindow::openLocalChapter(int targetIndex, int dir)
{
    const ChapterRun::Entry entry = comicRun_.entries[targetIndex];
    QString err;
    if (!comic_->openComic(entry.id, &err))
    { notify(tr("Can't open “%1”: %2").arg(entry.title, err), kFeedbackLong); return; }
    ChapterRun run = comicRun_;
    run.index = targetIndex;
    armComicRun(run);                                     // the run now names the file that is open
    if (dir < 0) comic_->gotoPage(comic_->pageCount() - 1);
    notify(entry.title, kFeedbackShort);
    mwLog(QStringLiteral("chapter: local advance (%1) -> \"%2\"").arg(dir).arg(entry.title));
}
```

Declare `openRemoteChapter` now so this compiles as one piece — Task 5 fills it in. In `MainWindow.h`, beside the other new methods, add:

```cpp
    void openRemoteChapter(int targetIndex, int dir);  // the addon lane (async: resolve, download, open)
```

and in `MainWindow.cpp`, immediately after `openLocalChapter`, add the stub:

```cpp
void MainWindow::openRemoteChapter(int, int) {}
```

- [ ] **Step 3: Connect the reader's two signals**

In `native/src/ui/MainWindow.cpp`, after the `connect(comic_, &ComicView::backRequested, this, returnFromReader);` line (~1716), add:

```cpp
    connect(comic_, &ComicView::chapterAdvanceRequested, this, &MainWindow::onChapterAdvanceRequested);
    connect(comic_, &ComicView::reachedLastPage, this, &MainWindow::onComicReachedLastPage);
```

- [ ] **Step 4: Build the run at the local-comic open site**

In the `.cbz`/`.cb7`/`.cbt` branch of `openLibraryItem` (~line 15256), replace:

```cpp
        if (!comic_->openComic(url, &err)) { notify(tr("Can't open comic: %1").arg(err), kFeedbackLong); return; }
        partPlaybackForReader(); book_->persist(); pdf_->persist();
        presentComic();
        recordDocument();
```

with:

```cpp
        if (!comic_->openComic(url, &err)) { notify(tr("Can't open comic: %1").arg(err), kFeedbackLong); return; }
        partPlaybackForReader(); book_->persist(); pdf_->persist();
        armComicRun(folderRunFor(url)); // the archives beside this one are its chapters
        presentComic();
        recordDocument();
```

- [ ] **Step 5: Clear the run wherever a comic opens WITHOUT one**

Every site that opens the comic reader must either arm a run or clear it. A run left over from a previous
read attaching itself to an unrelated file is the failure this step exists to prevent, and there are three
such sites besides the one above.

In `openLibraryItem`'s photo branch (~line 15266), immediately after the `openFolder` call succeeds, add:

```cpp
        armComicRun(ChapterRun{}); // a photo folder is not a series (issue #102)
```

In the "open a file" comic branch (~line 6303 — the `else if (ext == QStringLiteral("cbz") || …)` arm that
calls `comic_->openComic(f, &err)` and then `presentComic()`), give it the same folder run the library branch
gets, since a file opened this way sits in a folder exactly like one opened from the library. Add one line
after the `persist()` calls:

```cpp
        armComicRun(folderRunFor(f)); // the archives beside this one are its chapters
```

And in `MainWindow::openImagePages`'s `openCbz` lambda (~line 15748), immediately after the `openComic`
success line, add:

```cpp
        armComicRun(ChapterRun{}); // Task 4 replaces this with the browsed chapter run
```

- [ ] **Step 6: Build the app**

```bash
cmake --build build --config Release --target everythingbox
```

Expected: builds clean, no warnings about `comicRun_` or the new methods.

- [ ] **Step 7: Verify by hand against real files**

Put three comic archives in one folder named so natural order matters (`ch1.cbz`, `ch2.cbz`, `ch10.cbz`), open `ch2.cbz` from the app, and check all four:

1. paging to the last page shows the hint toast naming `ch10`;
2. one more forward press opens `ch10` at page 1 with a `ch10` toast;
3. paging back off page one of `ch10` opens `ch2` **at its last page**;
4. forward off the end of `ch10` says `That's the last chapter.` and stays put.

Then open a single archive in a folder of its own and confirm paging past the end does nothing at all and shows no toast.

- [ ] **Step 8: Commit**

```bash
git add native/src/ui/MainWindow.h native/src/ui/MainWindow.cpp && git commit -m "feat: page past the end of a local comic into the next file in its folder"
```

---

### Task 4: `HomeView` captures the browsed chapter list and ships it with the open

**Files:**
- Modify: `native/src/ui/HomeView.h:349` (the `openImagePages` signal), the private member block near `items_` at line ~1006
- Modify: `native/src/ui/HomeView.cpp:9418-9424` (the tail of `populate()`), `:7043-7055` (the readable-chapter branch of `resolvePlay`)
- Modify: `native/src/ui/MainWindow.h:173`, `native/src/ui/MainWindow.cpp:711` and `:15734` (`openImagePages`)

**Interfaces:**
- Consumes: `ChapterRun`, `ChapterOrder::fromChapterItems` (Task 1); `MainWindow::armComicRun` (Task 3).
- Produces, for Task 5:
  - `void HomeView::openImagePages(const QString& title, const QString& key, const QStringList& pageUrls, const ChapterRun& run)`
  - `void MainWindow::openImagePages(const QString& title, const QString& key, const QStringList& pageUrls, const ChapterRun& run)`
  - `ChapterRun HomeView::chapterRunFor(const QString& currentId) const`

- [ ] **Step 1: Remember the chapter list at the one ingress**

In `native/src/ui/HomeView.h`, after the `QVector<MediaItem> items_;` line (~1006), add:

```cpp
    // The manga chapters of the level last populated, in the order the provider listed them. Kept beside
    // items_ rather than derived from it at read time because drilling into a chapter's DETAIL page clears
    // items_, and that is one of the two places a chapter is opened from.
    QVector<ChapterRun::Entry> chapterList_;
```

and add near the top includes:

```cpp
#include "../comic/ChapterRun.h"
```

In the public methods block, beside the other small accessors, add:

```cpp
    // The run to hand the reader when `currentId` is opened: the remembered chapter list, normalised into
    // reading order. An empty/absent list yields an invalid run, which reads as "no neighbours".
    ChapterRun chapterRunFor(const QString& currentId) const;
```

- [ ] **Step 2: Fill it in `populate()`**

In `native/src/ui/HomeView.cpp`, immediately after the `for (const MediaItem& src : cat.items)` loop that pushes into `items_` (~line 9424, the loop ending in `items_.push_back(correctedRow(src));`), add:

```cpp
    // A level of manga chapters is a reading run: remember it so opening one can tell the reader what follows.
    // Rebuilt from the WHOLE of items_ on every pass, so an infinite-scroll append grows the run rather than
    // replacing it with just the newest page.
    chapterList_.clear();
    for (const MediaItem& it : items_)
        if (isReadableChapter(it.type)) chapterList_.append({ it.id, it.title });
```

And add the accessor next to `scrapedRow()` (~line 5375):

```cpp
ChapterRun HomeView::chapterRunFor(const QString& currentId) const
{
    return ChapterOrder::fromChapterItems(chapterList_, currentId);
}
```

- [ ] **Step 3: Widen the signal and ship the run**

In `native/src/ui/HomeView.h:349`, replace the signal with:

```cpp
    // manga chapter: the page images to assemble, plus the chapters either side of it in the list it came from
    void openImagePages(const QString& title, const QString& key, const QStringList& pageUrls,
                        const ChapterRun& run);
```

In `native/src/ui/HomeView.cpp`, in the readable-chapter branch of `resolvePlay` (~line 7043), replace:

```cpp
            else { hideToast(); emit openImagePages(title, key, pages); }
```

with:

```cpp
            else { hideToast(); emit openImagePages(title, key, pages, chapterRunFor(key)); }
```

- [ ] **Step 4: Take the run in `MainWindow` and arm it**

In `native/src/ui/MainWindow.h:173`, replace the declaration with:

```cpp
    void openImagePages(const QString& title, const QString& key, const QStringList& pageUrls,
                        const ChapterRun& run);
```

In `native/src/ui/MainWindow.cpp:15734`, replace the definition's signature to match, and in its `openCbz` lambda replace the `armComicRun(ChapterRun{});` line added in Task 3 with:

```cpp
        armComicRun(run); // the chapters either side of this one, as the list it was opened from had them
```

capturing `run` in the lambda:

```cpp
    auto openCbz = [this, cbzPath, title, run] {
```

The connect at line 711 needs no change — the signatures still match — but rebuild to confirm it resolves.

- [ ] **Step 5: Build**

```bash
cmake --build build --config Release --target everythingbox
```

Expected: builds clean. A `QObject::connect: signal not found` at runtime means the two signatures drifted; they must match exactly.

- [ ] **Step 6: Verify by hand**

Open a manga series, drill into its chapter list, open a chapter, page to the end. The hint toast must name **the next chapter in reading order** — check it against the series' actual numbering, not against the row below it in the list, and check a series the provider lists newest-first. Pressing forward does nothing yet (Task 5).

- [ ] **Step 7: Commit**

```bash
git add native/src/ui/HomeView.h native/src/ui/HomeView.cpp native/src/ui/MainWindow.h native/src/ui/MainWindow.cpp && git commit -m "feat: a manga chapter opens knowing which chapters sit either side of it"
```

---

### Task 5: The remote crossing — resolve, download, open

**Files:**
- Modify: `native/src/ui/MainWindow.cpp` (replace the `openRemoteChapter` stub added in Task 3; `openImagePages` at ~15734 gains a landing argument)
- Modify: `native/src/ui/MainWindow.h` (the `openImagePages` declaration)

**Interfaces:**
- Consumes: `comicRun_`, `chapterHandoffPending_`, `chapterHandoffGen_`, `armComicRun` (Task 3); `AddonManager::resolveMangaChapterPages(const QString& chapterItemId, std::function<void(const QStringList&)> cb)`; `Notifier` via `notify(text, ms)` / `hideNotice()`.
- Produces: nothing further — this is the last task.

- [ ] **Step 1: Give `openImagePages` a landing direction**

In `native/src/ui/MainWindow.h`, replace the declaration with:

```cpp
    // `landOnLastPage` opens the chapter at its FINAL page instead of its stored/first one — what paging
    // BACKWARDS across a chapter boundary means. Every other caller leaves it false.
    void openImagePages(const QString& title, const QString& key, const QStringList& pageUrls,
                        const ChapterRun& run, bool landOnLastPage = false);
```

In `native/src/ui/MainWindow.cpp`, match the signature and, inside the `openCbz` lambda, after `armComicRun(run);`, add:

```cpp
        if (landOnLastPage) comic_->gotoPage(comic_->pageCount() - 1);
```

capturing it: `auto openCbz = [this, cbzPath, title, run, landOnLastPage] {`.

- [ ] **Step 2: Bump the generation wherever the comic reader stops owning the screen**

In `armComicRun`, add as its first line:

```cpp
    ++chapterHandoffGen_;                      // a new chapter is open: any resolve still in flight is stale
    chapterHandoffPending_ = false;
```

- [ ] **Step 3: Implement the crossing**

Replace the `void MainWindow::openRemoteChapter(int, int) {}` stub with:

```cpp
// The remote lane: resolve the neighbouring chapter's page images, download them and open the result. Slow
// enough to need saying so — the last page of the chapter you just finished stays on screen throughout, and
// without a notice the press reads as dead, which is the very failure this feature exists to fix.
void MainWindow::openRemoteChapter(int targetIndex, int dir)
{
    const ChapterRun::Entry entry = comicRun_.entries[targetIndex];
    ChapterRun run = comicRun_;
    run.index = targetIndex;

    chapterHandoffPending_ = true;
    const int gen = chapterHandoffGen_;
    notify(tr("Loading “%1”…").arg(entry.title), 0);   // sticky: this can take a while
    mwLog(QStringLiteral("chapter: remote advance (%1) -> \"%2\"").arg(dir).arg(entry.title));

    addons_->resolveMangaChapterPages(entry.id, [this, gen, entry, run, dir](const QStringList& pages) {
        // Still ours to act on? A newer open bumped the generation, or the user left the reader entirely.
        // A `false` here is the hand-off DYING, so it also performs the compensation — otherwise the sticky
        // notice sits there with nothing behind it and the latch never clears.
        if (gen != chapterHandoffGen_) return;
        chapterHandoffPending_ = false;
        if (!comicOnScreen()) { hideNotice(); return; }
        if (pages.isEmpty())
        {
            hideNotice();
            notify(tr("No readable pages for “%1”. Licensed/official English chapters aren't hosted here — "
                      "try another chapter or title.").arg(entry.title), kFeedbackLong);
            return;   // stay on the last page; the chapter list is one Back away
        }
        hideNotice();
        openImagePages(entry.title, entry.id, pages, run, /*landOnLastPage*/ dir < 0);
        notify(entry.title, kFeedbackShort);
    });
}
```

`comicOnScreen()` is new. Declare it in `MainWindow.h` beside the other chapter methods as
`bool comicOnScreen() const;` and define it immediately above `openRemoteChapter`, mirroring the structure of
`presentComic()` (line ~2980) — including its `#ifdef`, because `comicHost_` is only usable as a `QWidget*`
in a QML build:

```cpp
// Is the comic reader still the page on screen? The reader occupies either the themed chrome host or the bare
// widget, exactly as presentComic() chose. A late chapter resolve must not present a reader the user has left.
bool MainWindow::comicOnScreen() const
{
#ifdef EB_HAVE_QML
    if (comicHost_ && stack_->currentWidget() == comicHost_) return true;
#endif
    return stack_->currentWidget() == comic_;
}
```

- [ ] **Step 4: Build**

```bash
cmake --build build --config Release --target everythingbox
```

Expected: builds clean.

- [ ] **Step 5: Run the full probe gate**

```bash
BUILD_DIR=build bash native/tools/run-headless-probes.sh 2>&1 | tail -3
```

Expected: `VERDICT=PASS`. Read the tail, not the exit status of the pipe — the script's own verdict is the gate. If a probe is red, fix it before committing; a red gate is never "unrelated".

- [ ] **Step 6: Verify on real content**

Open a real manga chapter from the shelf, page to the last page, and check the whole sequence:

1. the hint toast names the next chapter;
2. one forward press shows the sticky `Loading "…"` notice, which stays up for the whole resolve+download;
3. the next chapter opens at page 1 and the notice is replaced by the short arrival toast;
4. paging back off page one returns to the previous chapter **at its last page**, and does so instantly the second time (the CBZ cache);
5. pressing forward repeatedly during a load starts exactly one load — check `stream_debug.log` for a single `chapter: remote advance` line per crossing;
6. pressing Back while a load is in flight leaves you on the chapter list with no notice stuck on screen and does not yank you into the reader when the resolve lands;
7. forward at the final chapter says `That's the last chapter.`

For the driven variant, use `EB_UITEST=1` with `native/tools/uitest.py` (`key`/`state`/`shot`) so the run needs no foreground focus.

- [ ] **Step 7: Commit**

```bash
git add native/src/ui/MainWindow.h native/src/ui/MainWindow.cpp && git commit -m "feat: page past the end of a manga chapter into the next one"
```

---

## Notes for the reviewer

- **Nothing was added to either settings builder**, deliberately: the advance is a keypress the user makes, not something that happens to them. See the spec's "Out of scope".
- **The split-view pane (`MediaPane`) is untouched.** Its own `ComicView` never connects `chapterAdvanceRequested`, so a boundary press there stays the inert no-op it has always been.
- **Prefetching is out of scope.** The next chapter is fetched when you ask for it, not before.
