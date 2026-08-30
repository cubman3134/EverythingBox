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
// AND THE BUG AFTER THAT: pointing a list in the right direction is not enough. A fully-numbered list is
// stable-sorted by chapter number, because a real MangaDex series files a stray "Ch. 232" inside volume 1 and
// the list reads 7, 7.5, 232, 8, 9 — ascending end to end, so nothing was reversed and "after 7.5" came out as
// 232. A PARTIALLY-numbered list is still only pointed, never sorted: see the mixed cases below.
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
static QStringList idsOf(const QVector<ChapterRun::Entry>& v)
{
    QStringList out;
    for (const ChapterRun::Entry& e : v) out << e.id;
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
        ok = true;   // so !ok below can only come from the call, not from a stale false
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
        CHECK(read.value(1) == QStringLiteral("Chapter 5"));
        CHECK(read.value(3) == QStringLiteral("Chapter 9"));
    }
    {
        // Nothing parses: keep list order, which is what the user was just looking at.
        const QStringList named{ QStringLiteral("Prologue"), QStringLiteral("Interlude"), QStringLiteral("Epilogue") };
        CHECK(titlesOf(ChapterOrder::inReadingOrder(entries(named))) == named);
    }

    // ---- A FULLY-NUMBERED list is SORTED, not merely pointed in a direction ---------------------------------
    {
        // THE LIVE CASE THIS RULE EXISTS FOR. MangaDex files a stray Ch. 232 inside volume 1, and the provider
        // sorts by volume first, so the display list runs 7, 7.5, 232, 8, 9 — ascending end-to-end, so the old
        // end-comparison left it exactly as listed and "next after 7.5" came out as 232. Reading order, written
        // out by hand as a reader would read it: 7, 7.5, 8, 9, 232.
        const QStringList listed{ QStringLiteral("Ch. 7"), QStringLiteral("Ch. 7.5"), QStringLiteral("Ch. 232"),
                                  QStringLiteral("Ch. 8"), QStringLiteral("Ch. 9") };
        const QStringList read = titlesOf(ChapterOrder::inReadingOrder(entries(listed)));
        CHECK(read.size() == 5);
        CHECK(read.value(0) == QStringLiteral("Ch. 7"));
        CHECK(read.value(1) == QStringLiteral("Ch. 7.5"));
        CHECK(read.value(2) == QStringLiteral("Ch. 8"));
        CHECK(read.value(3) == QStringLiteral("Ch. 9"));
        CHECK(read.value(4) == QStringLiteral("Ch. 232"));
        // Named directly, because this is the press that would otherwise open the wrong chapter: the entry
        // AFTER Ch. 7.5 is Ch. 8, never Ch. 232.
        CHECK(read.value(read.indexOf(QStringLiteral("Ch. 7.5")) + 1) == QStringLiteral("Ch. 8"));
        // Tripwire against a rule that only ever chooses a direction: this list must NOT come back as listed,
        // and reversing it would not produce the answer either.
        CHECK(read != listed);
        QStringList reversed = listed;
        std::reverse(reversed.begin(), reversed.end());
        CHECK(read != reversed);
    }
    {
        // A fully-numbered DESCENDING list ends up ascending. Hand-written reading order: 2, 3, 11, 20.
        const QStringList desc{ QStringLiteral("Ch. 20"), QStringLiteral("Ch. 11"), QStringLiteral("Ch. 3"),
                                QStringLiteral("Ch. 2") };
        const QStringList read = titlesOf(ChapterOrder::inReadingOrder(entries(desc)));
        CHECK(read.size() == 4);
        CHECK(read.value(0) == QStringLiteral("Ch. 2"));
        CHECK(read.value(1) == QStringLiteral("Ch. 3"));
        CHECK(read.value(2) == QStringLiteral("Ch. 11"));
        CHECK(read.value(3) == QStringLiteral("Ch. 20"));
    }

    // ---- Entries sharing a chapter number keep the provider's relative order (STABLE sort) ------------------
    {
        // Two translations of chapter 1, listed after chapter 2. Ids make the two 1s distinguishable — without
        // them the assertion could not see a swap at all. Hand-written: the 1s first, in the order the provider
        // listed them (id1 then id2), then the 2. This small case DOCUMENTS the rule but does not enforce it on
        // its own — std::sort falls back to insertion sort at this size and happens to stay stable, so the
        // large case below is the one that actually goes red when stable_sort becomes sort. Keep both.
        const QVector<ChapterRun::Entry> listed = entries(
            { QStringLiteral("Ch. 2"), QStringLiteral("Ch. 1"), QStringLiteral("Ch. 1") });
        const QStringList ids = idsOf(ChapterOrder::inReadingOrder(listed));
        CHECK(ids.size() == 3);
        CHECK(ids.value(0) == QStringLiteral("id1"));
        CHECK(ids.value(1) == QStringLiteral("id2"));
        CHECK(ids.value(2) == QStringLiteral("id0"));
    }
    {
        // The same rule at a size an unstable sort actually reorders at: entries alternating Ch. 1 / Ch. 2, so
        // each number carries half of them as duplicates. Hand-written expectation: every even-indexed entry
        // (the Ch. 1s) in listed order, then every odd-indexed entry (the Ch. 2s) in listed order.
        //
        // THE COUNT IS THE TEETH. An introsort only shuffles equal keys once the range is too big for its
        // insertion-sort fallback (32 elements on MSVC, 16 on libstdc++); below that an unstable sort comes out
        // stable anyway and this case would pass on a plain std::sort — going quietly toothless rather than
        // red. 400 is far enough clear of any plausible cutoff that a toolchain would have to change character
        // to reach it. Nothing else here depends on the number; raise it, never lower it.
        const int kEntryCount = 400;               // total entries: 200 of each of the two numbers
        QStringList titles;
        for (int i = 0; i < kEntryCount; ++i)
            titles << (i % 2 == 0 ? QStringLiteral("Ch. 1") : QStringLiteral("Ch. 2"));
        QStringList expected;
        for (int i = 0; i < kEntryCount; i += 2) expected << QStringLiteral("id%1").arg(i);
        for (int i = 1; i < kEntryCount; i += 2) expected << QStringLiteral("id%1").arg(i);
        CHECK(idsOf(ChapterOrder::inReadingOrder(entries(titles))) == expected);
    }

    // ---- A PARTIALLY-numbered list is never sorted: the end comparison still decides -------------------------
    {
        // Some titles carry no number, so sorting would have to invent a position for them. The provider's
        // order is the better guess: first parsed (2) <= last parsed (5), so the list stands AS LISTED — note
        // Ch. 9 stays ahead of Ch. 5, which a sort would have swapped.
        const QStringList mixed{ QStringLiteral("Ch. 2"), QStringLiteral("Bonus"), QStringLiteral("Ch. 9"),
                                 QStringLiteral("Ch. 5") };
        const QStringList read = titlesOf(ChapterOrder::inReadingOrder(entries(mixed)));
        CHECK(read == mixed);
        CHECK(read.value(2) == QStringLiteral("Ch. 9"));  // NOT sorted: 9 before 5, exactly as listed
        CHECK(read.value(3) == QStringLiteral("Ch. 5"));
    }
    {
        // The same shape pointed the other way: first parsed (5) > last parsed (2), so the WHOLE list reverses,
        // unparsed entries carried along with it. Hand-written: Ch. 2, Extras, Ch. 5.
        const QStringList mixed{ QStringLiteral("Ch. 5"), QStringLiteral("Extras"), QStringLiteral("Ch. 2") };
        const QStringList read = titlesOf(ChapterOrder::inReadingOrder(entries(mixed)));
        CHECK(read.size() == 3);
        CHECK(read.value(0) == QStringLiteral("Ch. 2"));
        CHECK(read.value(1) == QStringLiteral("Extras"));
        CHECK(read.value(2) == QStringLiteral("Ch. 5"));
    }

    // ---- Building a run from a chapter list ---------------------------------------------------------------
    {
        const QVector<ChapterRun::Entry> listed = entries(
            { QStringLiteral("Chapter 12"), QStringLiteral("Chapter 11"), QStringLiteral("Chapter 10") });
        // "id1" is Chapter 11 — the middle of the run whichever way it is ordered.
        const ChapterRun run = ChapterOrder::fromChapterItems(listed, QStringLiteral("id1"));
        CHECK(run.isValid());
        CHECK(run.lane == ChapterRun::Lane::Chapters);
        CHECK(run.index == 1);
        CHECK(run.hasNext());
        CHECK(run.hasPrev());
        CHECK(run.entries.value(run.index + 1).title == QStringLiteral("Chapter 12")); // forward = later chapter
        CHECK(run.entries.value(run.index - 1).title == QStringLiteral("Chapter 10")); // back = earlier chapter
        CHECK(ChapterOrder::indexOfId(run, QStringLiteral("id0")) == 2);               // Chapter 12 moved to the end
        CHECK(ChapterOrder::indexOfId(run, QStringLiteral("nope")) == -1);
    }
    {
        // An ALREADY-ASCENDING list keeps its ids where they were: the run is not reversed unconditionally.
        const QVector<ChapterRun::Entry> listed = entries(
            { QStringLiteral("Chapter 1"), QStringLiteral("Chapter 2"), QStringLiteral("Chapter 3") });
        const ChapterRun run = ChapterOrder::fromChapterItems(listed, QStringLiteral("id0"));
        CHECK(run.index == 0);
        CHECK(!run.hasPrev());
        CHECK(run.hasNext());
        CHECK(run.entries.value(1).title == QStringLiteral("Chapter 2"));
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
    {
        // The LAST chapter of a run: forward is the boundary the reader must not walk past.
        const ChapterRun run = ChapterOrder::fromChapterItems(
            entries({ QStringLiteral("Chapter 12"), QStringLiteral("Chapter 11") }), QStringLiteral("id0"));
        CHECK(run.isValid());
        CHECK(run.index == 1);
        CHECK(!run.hasNext());
        CHECK(run.hasPrev());
    }

    // ---- A local folder run: natural filename order, never the newest-first reversal -----------------------
    {
        const QStringList files{ QStringLiteral("ch10.cbz"), QStringLiteral("ch2.cbz"), QStringLiteral("ch1.cbz") };
        const ChapterRun run = ChapterOrder::fromFileNames(QStringLiteral("C:/comics/series"), files,
                                                          QStringLiteral("ch2.cbz"));
        CHECK(run.lane == ChapterRun::Lane::Files);
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
        CHECK(run.entries.value(0).id == QStringLiteral("C:/comics/series/ch1.cbz"));
        CHECK(run.entries.value(2).id == QStringLiteral("C:/comics/series/ch10.cbz"));
    }
    {
        // A DESCENDING-looking folder listing is still ordered by name, never reversed: "Chapter 12.cbz"
        // beside "Chapter 2.cbz" is already in reading order, and a folder is not a provider's display order.
        const QStringList files{ QStringLiteral("Chapter 12.cbz"), QStringLiteral("Chapter 2.cbz"),
                                 QStringLiteral("Chapter 1.cbz") };
        const ChapterRun run = ChapterOrder::fromFileNames(QStringLiteral("C:/comics/series"), files,
                                                          QStringLiteral("Chapter 12.cbz"));
        CHECK(run.entries.value(0).title == QStringLiteral("Chapter 1"));
        CHECK(run.entries.value(1).title == QStringLiteral("Chapter 2"));
        CHECK(run.entries.value(2).title == QStringLiteral("Chapter 12"));
        CHECK(run.index == 2);
        CHECK(!run.hasNext());
    }
    {
        // The open file is not in the folder listing: no run, exactly as for a chapter list that misses it.
        const ChapterRun run = ChapterOrder::fromFileNames(QStringLiteral("C:/comics"),
                                                          { QStringLiteral("a.cbz") },
                                                          QStringLiteral("elsewhere.cbz"));
        CHECK(!run.isValid());
        CHECK(!run.hasNext());
        CHECK(!run.hasPrev());
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

    // ---- The app's own cache is not a series folder -------------------------------------------------------
    {
        // Live bug: Fairy Tail Vol. 2, fetched from a provider, is cached as <sha1>.cbz in ONE flat folder
        // beside every other remote document the app has ever opened. Sorted by hash, the file after it was
        // a 985 MB ROM archive named .zip, and paging off the end of the volume tried to open it as the next
        // chapter.
        const QString cache = QStringLiteral("C:/Users/x/AppData/Local/EverythingBox/cache");
        CHECK(ChapterOrder::isCachePath(cache + QStringLiteral("/remote-docs"), cache));
        CHECK(ChapterOrder::isCachePath(cache + QStringLiteral("/manga"), cache));
        CHECK(ChapterOrder::isCachePath(cache, cache));                     // the root itself
        // Case-insensitively: the folder comes back from QFileInfo and the root from QStandardPaths, and on
        // Windows those two disagree about case often enough to matter.
        CHECK(ChapterOrder::isCachePath(cache.toUpper() + QStringLiteral("/remote-docs"), cache));
        // A trailing separator names the same folder: both sides are cleaned before they are compared.
        CHECK(ChapterOrder::isCachePath(cache + QStringLiteral("/remote-docs/"), cache + QStringLiteral("/")));
        // A folder the USER keeps comics in is untouched — including one whose name merely STARTS with the
        // cache root's, which a prefix test without the separator would swallow.
        CHECK(!ChapterOrder::isCachePath(QStringLiteral("C:/comics/series"), cache));
        CHECK(!ChapterOrder::isCachePath(cache + QStringLiteral("-comics"), cache));
        // The cache is ABOVE the folder or it is nothing to do with it: a comics folder with a cache dir
        // inside it is still a series folder.
        CHECK(!ChapterOrder::isCachePath(QStringLiteral("C:/comics"), QStringLiteral("C:/comics/cache")));
        // Nothing to compare against (QStandardPaths can hand back ""): say no rather than match everything.
        CHECK(!ChapterOrder::isCachePath(cache, QString()));
        CHECK(!ChapterOrder::isCachePath(QString(), cache));
    }

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

    if (failures == 0) std::printf("CHAPTERRUN-OK\n");
    return failures == 0 ? 0 : 1;
}
