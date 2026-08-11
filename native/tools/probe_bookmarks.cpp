// probe_bookmarks — the pure layers of the #136 annotation work: the ReaderAnchor model ("build it once") and
// the per-book BookmarkStore. Neither needs a window, so both are asserted headless here; the on-screen add/
// jump/list in a running reader is not headlessly drivable (the ReaderChromeHost/ReaderBridge glue) and is
// verified by hand — this probe pins the anchor + the store + (in probe_cloudmerge) the sync/merge.
//
// Isolation: AppPaths::dataDir() is this process's own scratch directory (issue #42), so BookmarkStore opens an
// everythingbox.ini that starts empty and is removed at exit. The probe seeds its own profile id — that is the
// per-profile namespace fixture, not a defence against a previous run.
//
// Fixtures are independent of the code under test: anchor field values and the expected reading order are hand-
// computed here, never produced by calling the function being asserted. Prints BOOKMARKS-OK on success; any
// failure prints BOOKMARKS-FAIL <cond> (line) and exits non-zero.
#include "ReaderAnchor.h"
#include "BookmarkStore.h"
#include "Tombstones.h"
#include "ProfileStore.h"
#include "AppPaths.h"
#include "AppBrand.h"

#include <QCoreApplication>
#include <QSettings>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <QVector>
#include <algorithm>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "BOOKMARKS-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

// Build the three kinds by hand (the fixture — not via any store or helper under test).
static ReaderAnchor bookAnchor(int spine, int offset, int endOffset = -1)
{
    ReaderAnchor a; a.kind = ReaderAnchor::Book; a.spine = spine; a.offset = offset; a.endOffset = endOffset;
    return a;
}
static ReaderAnchor pdfAnchor(int page, int rx = -1, int ry = -1, int rw = -1, int rh = -1)
{
    ReaderAnchor a; a.kind = ReaderAnchor::Pdf; a.page = page;
    a.regionX = rx; a.regionY = ry; a.regionW = rw; a.regionH = rh; return a;
}
static ReaderAnchor comicAnchor(int page)
{
    ReaderAnchor a; a.kind = ReaderAnchor::Comic; a.page = page; return a;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ---- 1. ReaderAnchor toJson/fromJson round-trip, each kind (fromJson(toJson(a)) == a) --------------------
    {
        // Book — with the reserved highlight range end set, so endOffset is proven to survive the round-trip.
        const ReaderAnchor b = bookAnchor(3, 500, 812);
        const QJsonObject bj = b.toJson();
        // toJson writes the fields (hand-checked values, independent of fromJson).
        CHECK(bj.value(QStringLiteral("kind")).toInt() == int(ReaderAnchor::Book));
        CHECK(bj.value(QStringLiteral("spine")).toInt() == 3);
        CHECK(bj.value(QStringLiteral("offset")).toInt() == 500);
        CHECK(bj.value(QStringLiteral("endOffset")).toInt() == 812);
        const ReaderAnchor b2 = ReaderAnchor::fromJson(bj);
        CHECK(b2 == b);
        CHECK(b2.spine == 3 && b2.offset == 500 && b2.endOffset == 812);

        // A point book anchor (a bookmark) — endOffset stays -1 through the round-trip.
        const ReaderAnchor bp = bookAnchor(1, 40);
        CHECK(bp.endOffset == -1);
        CHECK(ReaderAnchor::fromJson(bp.toJson()) == bp);
        CHECK(!bp.isRange() && bookAnchor(1, 40, 90).isRange());   // a range is endOffset >= 0 (highlights, reserved)

        // Pdf — page + optional region.
        const ReaderAnchor p = pdfAnchor(7, 100, 200, 300, 400);
        const QJsonObject pj = p.toJson();
        CHECK(pj.value(QStringLiteral("page")).toInt() == 7);
        CHECK(pj.value(QStringLiteral("regionW")).toInt() == 300);
        const ReaderAnchor p2 = ReaderAnchor::fromJson(pj);
        CHECK(p2 == p);
        CHECK(p2.page == 7 && p2.regionX == 100 && p2.regionH == 400);

        // Comic — page only; region fields default -1 both ways.
        const ReaderAnchor c = comicAnchor(12);
        CHECK(c.regionX == -1 && c.regionW == -1);
        const ReaderAnchor c2 = ReaderAnchor::fromJson(c.toJson());
        CHECK(c2 == c);
        CHECK(c2.kind == ReaderAnchor::Comic && c2.page == 12);

        // A byte-for-byte re-serialise is stable (the id derivation in BookmarkStore relies on it).
        CHECK(QJsonDocument(b.toJson()).toJson(QJsonDocument::Compact)
              == QJsonDocument(ReaderAnchor::fromJson(b.toJson()).toJson()).toJson(QJsonDocument::Compact));

        // A forged kind int reads back as Book (defined, never an out-of-range enum in the comparator).
        QJsonObject forged = c.toJson(); forged.insert(QStringLiteral("kind"), 99);
        CHECK(ReaderAnchor::fromJson(forged).kind == ReaderAnchor::Book);
    }

    // ---- 2. Reading-order comparator: book = spine-then-offset; pdf/comic = page ----------------------------
    {
        // BOOK: spine dominates offset. Expected order (hand-computed): (0,900) < (1,10) < (1,50) < (2,0).
        QVector<ReaderAnchor> books = { bookAnchor(2, 0), bookAnchor(1, 50), bookAnchor(0, 900), bookAnchor(1, 10) };
        std::sort(books.begin(), books.end(),
                  [](const ReaderAnchor& x, const ReaderAnchor& y) { return ReaderAnchor::inReadingOrder(x, y); });
        CHECK(books[0] == bookAnchor(0, 900));   // earliest chapter first, even though its offset is the largest
        CHECK(books[1] == bookAnchor(1, 10));    // within a chapter, smaller offset first
        CHECK(books[2] == bookAnchor(1, 50));
        CHECK(books[3] == bookAnchor(2, 0));

        // A larger offset in an EARLIER chapter still sorts before a smaller offset in a LATER chapter — the
        // one relation a spine-ignoring (offset-only) comparator would get wrong.
        CHECK(ReaderAnchor::inReadingOrder(bookAnchor(0, 900), bookAnchor(1, 10)));
        CHECK(!ReaderAnchor::inReadingOrder(bookAnchor(1, 10), bookAnchor(0, 900)));
        // Within a chapter, offset decides.
        CHECK(ReaderAnchor::inReadingOrder(bookAnchor(1, 10), bookAnchor(1, 50)));

        // COMIC/PDF: page decides.
        QVector<ReaderAnchor> comics = { comicAnchor(5), comicAnchor(1), comicAnchor(3) };
        std::sort(comics.begin(), comics.end(),
                  [](const ReaderAnchor& x, const ReaderAnchor& y) { return ReaderAnchor::inReadingOrder(x, y); });
        CHECK(comics[0] == comicAnchor(1) && comics[1] == comicAnchor(3) && comics[2] == comicAnchor(5));
        CHECK(ReaderAnchor::inReadingOrder(pdfAnchor(2), pdfAnchor(9)));
        CHECK(!ReaderAnchor::inReadingOrder(pdfAnchor(9), pdfAnchor(2)));
    }

    // ---- 3. BookmarkStore: add/list-sorted, per-book filtering, deterministic id ----------------------------
    {
        ProfileStore::setCurrent(QStringLiteral("bmtest"));
        const QString bookA = QStringLiteral("/library/Dune.epub");
        const QString bookB = QStringLiteral("/library/Neuromancer.epub");

        // idFor is deterministic, position-sensitive, and book-sensitive — and INDEPENDENT of time (it is a pure
        // function of book + anchor). These are the properties the merge identity relies on.
        const QString id_a1 = BookmarkStore::idFor(bookA, bookAnchor(1, 100));
        CHECK(id_a1 == BookmarkStore::idFor(bookA, bookAnchor(1, 100)));               // deterministic
        CHECK(id_a1 != BookmarkStore::idFor(bookA, bookAnchor(1, 101)));               // position-sensitive (offset)
        CHECK(id_a1 != BookmarkStore::idFor(bookA, bookAnchor(2, 100)));               // position-sensitive (spine)
        CHECK(id_a1 != BookmarkStore::idFor(bookB, bookAnchor(1, 100)));               // book-sensitive
        CHECK(BookmarkStore::idFor(QString(), bookAnchor(1, 100)).isEmpty());          // empty book -> empty id

        // Add three bookmarks to book A OUT of reading order + one to book B.
        BookmarkStore::add(bookA, bookAnchor(2, 5),   QStringLiteral("Ch3"));
        BookmarkStore::add(bookA, bookAnchor(0, 900), QStringLiteral("Prologue"));
        BookmarkStore::add(bookA, bookAnchor(1, 40),  QStringLiteral("Ch2"));
        BookmarkStore::add(bookB, bookAnchor(0, 0),   QStringLiteral("Start"));

        // list(bookA) returns ONLY book A's, in reading order (Prologue @ 0/900, Ch2 @ 1/40, Ch3 @ 2/5).
        const QVector<BookmarkStore::Bookmark> la = BookmarkStore::list(bookA);
        CHECK(la.size() == 3);
        CHECK(la[0].label == QStringLiteral("Prologue"));
        CHECK(la[1].label == QStringLiteral("Ch2"));
        CHECK(la[2].label == QStringLiteral("Ch3"));
        CHECK(la[0].anchor == bookAnchor(0, 900) && la[2].anchor == bookAnchor(2, 5));
        for (const BookmarkStore::Bookmark& b : la) CHECK(b.bookKey == bookA);          // no book-B leakage
        CHECK(BookmarkStore::list(bookB).size() == 1);
        CHECK(BookmarkStore::list(QString()).isEmpty());                                // empty key -> empty

        // Idempotent by position: re-adding the SAME spot folds onto the one id (updates label/ts), never dupes.
        const int before = BookmarkStore::list(bookA).size();
        BookmarkStore::add(bookA, bookAnchor(1, 40), QStringLiteral("Ch2 (renamed)"));
        const QVector<BookmarkStore::Bookmark> la2 = BookmarkStore::list(bookA);
        CHECK(la2.size() == before);                                                    // no duplicate row
        bool renamed = false;
        for (const BookmarkStore::Bookmark& b : la2)
            if (b.anchor == bookAnchor(1, 40)) renamed = (b.label == QStringLiteral("Ch2 (renamed)"));
        CHECK(renamed);                                                                 // the fold refreshed the label
    }

    // ---- 4. BookmarkStore::remove deletes the row AND leaves a delete tombstone -----------------------------
    {
        const QString book = QStringLiteral("/library/Hyperion.epub");
        const BookmarkStore::Bookmark keep = BookmarkStore::add(book, bookAnchor(0, 10), QStringLiteral("keep"));
        const BookmarkStore::Bookmark gone = BookmarkStore::add(book, bookAnchor(1, 20), QStringLiteral("gone"));
        CHECK(BookmarkStore::list(book).size() == 2);

        BookmarkStore::remove(gone.id);
        const QVector<BookmarkStore::Bookmark> after = BookmarkStore::list(book);
        CHECK(after.size() == 1 && after[0].id == keep.id);                             // the row is gone
        // A delete TOMBSTONE was recorded for the removed id (so a peer cannot resurrect it on merge).
        bool tombed = false;
        for (const Tombstones::Entry& e : Tombstones::all(BookmarkStore::tombstoneStore()))
            if (e.key == gone.id) tombed = true;
        CHECK(tombed);
        // The surviving bookmark carries NO tombstone (remove tombstoned only the removed id).
        bool keepTombed = false;
        for (const Tombstones::Entry& e : Tombstones::all(BookmarkStore::tombstoneStore()))
            if (e.key == keep.id) keepTombed = true;
        CHECK(!keepTombed);

        // Re-adding the removed spot resurrects it AND clears its tombstone (the "deletion undone" path — a
        // resurrected row must not self-suppress on the next merge).
        const BookmarkStore::Bookmark back = BookmarkStore::add(book, bookAnchor(1, 20), QStringLiteral("back"));
        CHECK(back.id == gone.id);                                                      // same position -> same id
        bool stillTombed = false;
        for (const Tombstones::Entry& e : Tombstones::all(BookmarkStore::tombstoneStore()))
            if (e.key == gone.id) stillTombed = true;
        CHECK(!stillTombed);                                                            // the re-add cleared it
        CHECK(BookmarkStore::list(book).size() == 2);
    }

    if (failures == 0) { std::puts("BOOKMARKS-OK"); return 0; }
    std::fprintf(stderr, "BOOKMARKS: %d check(s) failed\n", failures);
    return 1;
}
