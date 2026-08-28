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

    if (failures == 0) std::printf("CHAPTERRUN-OK\n");
    return failures == 0 ? 0 : 1;
}
