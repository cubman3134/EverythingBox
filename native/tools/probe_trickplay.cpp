// Headless check of Trickplay (src/core/Trickplay.h) — the seek-preview arithmetic, the cache key, the
// resume point, the eviction order, and the guard that keeps this feature off anything the user does not
// own outright (issue #85, increment 1).
//
// Five things are worth a probe here, and they are all things that fail SILENTLY on a user's machine:
//
//   1. The tile lookup. A frame index one column out shows the wrong second of the film underneath a
//      confident timestamp — the picture and the number disagreeing is the one failure that makes the
//      feature worse than not having it. Section 2 walks the whole sheet, including the SHORT last grid
//      (a film is never a whole number of grids) and a time past the end.
//   2. The cache key. Key by path alone and a replaced file keeps the previous film's thumbnails, which is
//      Jellyfin 10.11's own lesson and is the difference between a nicety and a strip that lies. Section 3.
//   3. Resuming. Every finished grid survives an interrupted run, and the walk picks up at the first one
//      MISSING — not at the count, or a lost grid leaves a permanent hole in the strip. Section 4.
//   4. Eviction. An over-budget cache gives up the least recently used items until it fits, never the item
//      being watched, and deterministically. Section 5.
//   5. The stream guard. Section 1, and it is the reason this file is a probe and not a comment: generating
//      previews means seeking a file three hundred times, and on a debrid link or an IPTV channel that is
//      three hundred range requests against someone else's quota for a courtesy nobody asked for.
//
// The urls below are SHAPES, not credentials: the "signed" fixture carries the literal word placeholder
// where a real link carries a token, because a probe transcript is a log like any other.
//
// Prints TRICKPLAY-OK on success; any failure prints TRICKPLAY-FAIL <cond> and exits non-zero.
#include "Trickplay.h"

#include <QString>
#include <QVector>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "TRICKPLAY-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

int main()
{
    using Trickplay::Refusal;

    // ---- 1. The stream guard -------------------------------------------------------------------------
    //
    // The shapes this app actually hands the player: a local path off "Open Video…" or the local library, a
    // downloads-for-keeps file, a network share, an addon-resolved http(s) stream, a debrid link, an IPTV
    // channel, and mpv's own pseudo-protocols. Only the first two may be previewed.

    // Rooted local paths, both spellings, on either OS. Written platform-independently on purpose: this
    // predicate must answer the same on the Windows dev machine and on the Linux CI runner, or the gate is
    // asserting something other than what ships.
    CHECK(Trickplay::classify(QStringLiteral("D:/Films/Solaris (1972).mkv")) == Refusal::None);
    CHECK(Trickplay::classify(QStringLiteral("D:\\Films\\Solaris (1972).mkv")) == Refusal::None);
    CHECK(Trickplay::classify(QStringLiteral("C:/EverythingBox/downloads/keep/ep01.mp4")) == Refusal::None);
    CHECK(Trickplay::classify(QStringLiteral("/srv/media/films/Stalker.mkv")) == Refusal::None);
    CHECK(Trickplay::eligible(QStringLiteral("/srv/media/films/Stalker.mkv")));

    // A file: url is a spelling of a local path, so it is unwrapped and judged as one…
    CHECK(Trickplay::classify(QStringLiteral("file:///D:/Films/Solaris.mkv")) == Refusal::None);
    // …which is exactly what makes the UNC-shaped one come out as a share rather than as "a local file".
    CHECK(Trickplay::classify(QStringLiteral("file://nas/media/Solaris.mkv")) == Refusal::Unc);
    CHECK(Trickplay::classify(QStringLiteral("\\\\nas\\media\\Solaris.mkv")) == Refusal::Unc);
    CHECK(Trickplay::classify(QStringLiteral("//nas/media/Solaris.mkv")) == Refusal::Unc);

    // Streams. Every one of these is refused for the same reason and each is a shape this app produces.
    CHECK(Trickplay::classify(QStringLiteral("http://example.invalid/stream/1.mkv")) == Refusal::Url);
    CHECK(Trickplay::classify(QStringLiteral("https://cdn.example.invalid/v/abc/film.mp4")) == Refusal::Url);
    // A signed debrid link: host, a long opaque path, an expiry and a signature. The values here are the
    // word "placeholder" precisely because this string ends up in a probe transcript.
    CHECK(Trickplay::classify(QStringLiteral(
              "https://dl.example.invalid/d/placeholder/film.mkv?token=placeholder&exp=0")) == Refusal::Url);
    // An IPTV channel, in the three transports an M3U list uses.
    CHECK(Trickplay::classify(QStringLiteral("http://iptv.example.invalid:8080/live/ch/1.ts")) == Refusal::Url);
    CHECK(Trickplay::classify(QStringLiteral("udp://@239.0.0.1:1234")) == Refusal::Url);
    CHECK(Trickplay::classify(QStringLiteral("rtsp://cam.example.invalid/stream")) == Refusal::Url);
    // mpv's own pseudo-protocols and a magnet: none of them is a file we own.
    CHECK(Trickplay::classify(QStringLiteral("av://lavfi:testsrc")) == Refusal::Url);
    CHECK(Trickplay::classify(QStringLiteral("edl://%20%7C")) == Refusal::Url);
    CHECK(Trickplay::classify(QStringLiteral("bd://title/1")) == Refusal::Url);
    CHECK(Trickplay::classify(QStringLiteral("magnet:?xt=urn:btih:0000")) == Refusal::Url);
    CHECK(!Trickplay::eligible(QStringLiteral("https://cdn.example.invalid/v/abc/film.mp4")));

    // Nothing, and something we cannot key. A relative path is refused rather than resolved: the key IS the
    // path, and "film.mkv" names a different file from every directory it is read in.
    CHECK(Trickplay::classify(QString()) == Refusal::Nothing);
    CHECK(Trickplay::classify(QStringLiteral("   ")) == Refusal::Nothing);
    CHECK(Trickplay::classify(QStringLiteral("film.mkv")) == Refusal::Relative);
    CHECK(Trickplay::classify(QStringLiteral("../Films/film.mkv")) == Refusal::Relative);

    // The one-letter-scheme rule, stated as an assertion because it is the trap: "C:" is a drive and never a
    // protocol, and a predicate that read it as one would refuse every file on Windows.
    CHECK(Trickplay::classify(QStringLiteral("C:/x/y.mkv")) == Refusal::None);

    // ---- 2. The sheet: time -> grid, row, column ------------------------------------------------------
    //
    // 5x5 grids of 10 s frames = 25 frames (250 s) per grid. 63 frames is 2 full grids and a third holding
    // 13 — a deliberately SHORT last grid, because that is the case a lookup written against "25 per grid"
    // gets wrong only at the end of the film, which is where nobody tests.
    Trickplay::Layout l;
    l.intervalMs = 10000; l.tileW = 320; l.tileH = 180; l.cols = 5; l.rows = 5; l.frameCount = 63;
    CHECK(l.valid());
    CHECK(l.perGrid() == 25);
    CHECK(Trickplay::gridCount(l) == 3);
    CHECK(Trickplay::framesInGrid(l, 0) == 25);
    CHECK(Trickplay::framesInGrid(l, 1) == 25);
    CHECK(Trickplay::framesInGrid(l, 2) == 13);   // the short one
    CHECK(Trickplay::framesInGrid(l, 3) == 0);    // past the end of the sheet set
    CHECK(Trickplay::framesInGrid(l, -1) == 0);

    // A short grid is written only as tall as it needs: 13 frames of a 5-wide sheet is 3 rows, not 5.
    CHECK(Trickplay::rowsInGrid(l, 0) == 5);
    CHECK(Trickplay::rowsInGrid(l, 2) == 3);
    CHECK(Trickplay::gridPixelHeight(l, 0) == 5 * 180);
    CHECK(Trickplay::gridPixelHeight(l, 2) == 3 * 180);
    CHECK(Trickplay::gridPixelWidth(l, 2) == 5 * 320);

    // Frame counts from a duration. The frame at 0 counts, so the arithmetic is ceil — floor leaves the tail
    // of every single file with no preview at all.
    CHECK(Trickplay::frameCountFor(25000, 10000) == 3);    // 0 s, 10 s, 20 s
    CHECK(Trickplay::frameCountFor(30000, 10000) == 3);
    CHECK(Trickplay::frameCountFor(30001, 10000) == 4);
    CHECK(Trickplay::frameCountFor(0, 10000) == 0);
    CHECK(Trickplay::frameCountFor(-1, 10000) == 0);

    // The lookup itself, at the boundaries that matter.
    CHECK(Trickplay::frameAt(l, 0) == 0);
    CHECK(Trickplay::frameAt(l, 9999) == 0);
    CHECK(Trickplay::frameAt(l, 10000) == 1);
    CHECK(Trickplay::frameAt(l, 249999) == 24);      // last frame of grid 0
    CHECK(Trickplay::frameAt(l, 250000) == 25);      // first frame of grid 1

    Trickplay::Tile t0 = Trickplay::tileAt(l, 0);
    CHECK(t0.ok() && t0.grid == 0 && t0.row == 0 && t0.col == 0);
    Trickplay::Tile t6 = Trickplay::tileAt(l, 6 * 10000);   // frame 6 -> row 1, col 1
    CHECK(t6.ok() && t6.grid == 0 && t6.row == 1 && t6.col == 1);
    // Frame 7 is OFF THE DIAGONAL, and it is here for one reason: on a square 5x5 sheet every frame whose
    // row equals its column survives a row/column swap unchanged, so a probe built only out of 0, 6, 24 and
    // 62 asserts nothing at all about which way round the tile is read. (Measured: that exact matrix let
    // the swap live.) 7 -> row 1, col 2, and it dies the moment they trade places.
    Trickplay::Tile t7 = Trickplay::tileAt(l, 7 * 10000);
    CHECK(t7.ok() && t7.grid == 0 && t7.row == 1 && t7.col == 2);
    Trickplay::Tile t13 = Trickplay::tileAt(l, 13 * 10000);  // -> row 2, col 3
    CHECK(t13.ok() && t13.grid == 0 && t13.row == 2 && t13.col == 3);
    Trickplay::Tile t24 = Trickplay::tileAt(l, 24 * 10000);
    CHECK(t24.ok() && t24.grid == 0 && t24.row == 4 && t24.col == 4);
    Trickplay::Tile t25 = Trickplay::tileAt(l, 25 * 10000); // first tile of the NEXT grid, top-left again
    CHECK(t25.ok() && t25.grid == 1 && t25.row == 0 && t25.col == 0);
    Trickplay::Tile t62 = Trickplay::tileAt(l, 62 * 10000); // last frame: grid 2, 13th tile -> row 2, col 2
    CHECK(t62.ok() && t62.grid == 2 && t62.row == 2 && t62.col == 2);

    // Pixel origin inside the grid image — the blit source rectangle, and the other half of "the picture
    // matches the number".
    CHECK(Trickplay::tilePixelX(l, t6) == 1 * 320);
    CHECK(Trickplay::tilePixelY(l, t6) == 1 * 180);
    CHECK(Trickplay::tilePixelX(l, t62) == 2 * 320);
    CHECK(Trickplay::tilePixelY(l, t62) == 2 * 180);
    CHECK(Trickplay::tilePixelX(l, t7) == 2 * 320);   // off-diagonal again: x and y are not interchangeable
    CHECK(Trickplay::tilePixelY(l, t7) == 1 * 180);

    // A time PAST THE END clamps to the last frame we have, and a negative time to the first. A drag lands
    // a hair past the reported duration routinely (VBR files over-report by a frame or two); the honest
    // answer there is the last thumbnail, never a blank one.
    CHECK(Trickplay::frameAt(l, 630000) == 62);
    CHECK(Trickplay::frameAt(l, 99999999) == 62);
    CHECK(Trickplay::tileAt(l, 99999999).grid == 2);
    CHECK(Trickplay::frameAt(l, -5000) == 0);

    // The timestamp under the thumbnail is the frame's CAPTURE time, so the label can never claim a second
    // the picture is not of.
    CHECK(Trickplay::frameTimeMs(l, 0) == 0);
    CHECK(Trickplay::frameTimeMs(l, 62) == 620000);

    // No strip at all: the "silently, today's behaviour" case. -1 and an unusable tile, not a zero index.
    Trickplay::Layout empty = l; empty.frameCount = 0;
    CHECK(Trickplay::gridCount(empty) == 0);
    CHECK(Trickplay::frameAt(empty, 5000) == -1);
    CHECK(!Trickplay::tileAt(empty, 5000).ok());

    // A broken layout is refused rather than divided by. A zero interval is a division by zero and a zero
    // column count puts every tile in column 0 — both are "wrong picture, confident label" on a live scrub.
    Trickplay::Layout broken = l; broken.intervalMs = 0;
    CHECK(!broken.valid());
    CHECK(Trickplay::frameAt(broken, 5000) == -1);
    Trickplay::Layout broken2 = l; broken2.cols = 0;
    CHECK(!broken2.valid());
    CHECK(Trickplay::frameAt(broken2, 5000) == -1);

    // The file name grids are stored under: zero-padded so the listing sorts the way the grids run.
    CHECK(Trickplay::gridFileName(0) == QStringLiteral("g0000.jpg"));
    CHECK(Trickplay::gridFileName(7) == QStringLiteral("g0007.jpg"));
    CHECK(Trickplay::gridFileName(1234) == QStringLiteral("g1234.jpg"));

    // ---- 3. The cache key ----------------------------------------------------------------------------
    const QString p = QStringLiteral("D:/Films/Solaris (1972).mkv");
    const QString kBase = Trickplay::cacheKey(p, 1700000000, 8123456789LL);

    // Stable: the same file, asked twice, is the same item. (Nothing is cached at all if it is not.)
    CHECK(Trickplay::cacheKey(p, 1700000000, 8123456789LL) == kBase);
    CHECK(!kBase.isEmpty());

    // The replacement rule, both halves. A new mtime is a new item — an edited, re-encoded or re-downloaded
    // file must not inherit the previous film's strip…
    CHECK(Trickplay::cacheKey(p, 1700000001, 8123456789LL) != kBase);
    // …and so is a new size, which is what catches the replacement that PRESERVED the timestamp (rsync -t,
    // a restore from backup, an archiver): mtime alone would hand that file the old thumbnails.
    CHECK(Trickplay::cacheKey(p, 1700000000, 8123456790LL) != kBase);

    // A different file is a different item, even at the same instant and the same size.
    CHECK(Trickplay::cacheKey(QStringLiteral("D:/Films/Stalker (1979).mkv"), 1700000000, 8123456789LL) != kBase);

    // …and an UNRELATED touch changes nothing. Only this file's own path, mtime and size are in the key, so
    // a sibling being rewritten, the directory's own mtime moving, or this file merely being READ leaves the
    // strip in place. (Both lines are the same call: the key has no input but the three named ones.)
    CHECK(Trickplay::cacheKey(p, 1700000000, 8123456789LL) == kBase);   // after a sibling changed
    CHECK(Trickplay::cacheKey(p, 1700000000, 8123456789LL) == kBase);   // after this file was only read

    // Separator spelling is not identity: one file, one item, however the path was written down.
    CHECK(Trickplay::cacheKey(QStringLiteral("D:\\Films\\Solaris (1972).mkv"), 1700000000, 8123456789LL) == kBase);

    // ---- 4. The sidecar round trip -------------------------------------------------------------------
    Trickplay::Index ix;
    ix.layout = l;
    ix.durationMs    = 629000;
    ix.sourceMtime   = 1700000000;
    ix.sourceSize    = 8123456789LL;
    ix.sourceName    = QStringLiteral("Solaris (1972).mkv");
    ix.completeGrids = 2;

    Trickplay::Index back;
    CHECK(Trickplay::readIndex(Trickplay::writeIndex(ix), &back));
    CHECK(back.version == 1);
    CHECK(back.layout.intervalMs == 10000);
    CHECK(back.layout.tileW == 320 && back.layout.tileH == 180);
    CHECK(back.layout.cols == 5 && back.layout.rows == 5);
    CHECK(back.layout.frameCount == 63);
    CHECK(back.durationMs == 629000);
    // The two halves of the key survive, so a cache directory can say for itself which file it belongs to —
    // a size this large is exactly where a JSON number read back as an int would have lost its low bits.
    CHECK(back.sourceMtime == 1700000000);
    CHECK(back.sourceSize == 8123456789LL);
    CHECK(back.sourceName == QStringLiteral("Solaris (1972).mkv"));
    CHECK(back.completeGrids == 2);
    // And the lookup off the round-tripped layout is the same lookup, which is the property that actually
    // matters: the sidecar is only ever read back to do this.
    CHECK(Trickplay::tileAt(back.layout, 62 * 10000).grid == 2);
    CHECK(Trickplay::tileAt(back.layout, 62 * 10000).row == 2);
    CHECK(Trickplay::tileAt(back.layout, 62 * 10000).col == 2);

    // Strictness. A sidecar that does not parse, is of a version we do not know, or describes a layout
    // nothing can be looked up in must read as "no previews" — never as previews with a broken layout,
    // which is how a tile lookup starts dividing by zero on somebody's machine.
    Trickplay::Index dummy;
    CHECK(!Trickplay::readIndex(QByteArray("not json at all"), &dummy));
    CHECK(!Trickplay::readIndex(QByteArray("{\"version\":1,"), &dummy));      // truncated write
    CHECK(!Trickplay::readIndex(QByteArray(), &dummy));
    {
        Trickplay::Index v2 = ix; v2.version = 2;
        CHECK(!Trickplay::readIndex(Trickplay::writeIndex(v2), &dummy));
        // A layout nothing can be looked up in. completeGrids is set to 0 DELIBERATELY: a broken layout has
        // no grids, so any non-zero count would be rejected by the completeGrids check below instead and
        // this case would prove nothing about the layout check at all. (Measured: with completeGrids left at
        // 2, deleting the layout validity check entirely still passed.)
        Trickplay::Index bad = ix; bad.layout.intervalMs = 0; bad.completeGrids = 0;
        CHECK(!Trickplay::readIndex(Trickplay::writeIndex(bad), &dummy));
        Trickplay::Index bad2 = ix; bad2.layout.cols = 0; bad2.completeGrids = 0;
        CHECK(!Trickplay::readIndex(Trickplay::writeIndex(bad2), &dummy));
        Trickplay::Index over = ix; over.completeGrids = 99;   // more grids claimed than the sheet has
        CHECK(!Trickplay::readIndex(Trickplay::writeIndex(over), &dummy));
    }

    // ---- 5. Resuming ---------------------------------------------------------------------------------
    //
    // "Interrupted after grid 2 of 5 resumes at 3" — grids 0 and 1 are on disk, so the walk answers index 2,
    // which is the third grid.
    Trickplay::Layout five = l; five.frameCount = 5 * 25;   // exactly 5 grids
    CHECK(Trickplay::gridCount(five) == 5);
    CHECK(Trickplay::resumeGrid(five, QVector<bool>{ true, true, false, false, false }) == 2);
    CHECK(Trickplay::resumeGrid(five, QVector<bool>{ false, false, false, false, false }) == 0);
    CHECK(Trickplay::resumeGrid(five, QVector<bool>{ true, true, true, true, false }) == 4);
    // Nothing missing: the answer is the grid count, which is both "already done" and the loop's bound.
    CHECK(Trickplay::resumeGrid(five, QVector<bool>{ true, true, true, true, true }) == 5);
    // The first HOLE, not the count. A directory holding 0, 1 and 3 (grid 2 lost to a failed rename) must
    // resume at 2; answering 4 would leave that gap in the strip permanently.
    CHECK(Trickplay::resumeGrid(five, QVector<bool>{ true, true, false, true, false }) == 2);
    // A short vector (a directory listing that ran out) is missing grids, not complete ones.
    CHECK(Trickplay::resumeGrid(five, QVector<bool>{ true, true }) == 2);
    CHECK(Trickplay::resumeGrid(five, QVector<bool>()) == 0);

    // ---- 6. Eviction ---------------------------------------------------------------------------------
    {
        QVector<Trickplay::CacheEntry> c;
        c.push_back({ QStringLiteral("old"),    300, 1000 });
        c.push_back({ QStringLiteral("mid"),    300, 2000 });
        c.push_back({ QStringLiteral("recent"), 300, 3000 });

        // Inside the bound: nothing is touched. An LRU that evicts while it fits is a cache that deletes work
        // it is about to redo.
        CHECK(Trickplay::planEviction(c, 900).isEmpty());
        CHECK(Trickplay::planEviction(c, 5000).isEmpty());

        // Over by one item: the LEAST recently used goes, and only it — the sweep stops the moment it fits.
        const QStringList one = Trickplay::planEviction(c, 800);
        CHECK(one.size() == 1);
        CHECK(one.value(0) == QStringLiteral("old"));

        // Further over: the two oldest, oldest first.
        const QStringList two = Trickplay::planEviction(c, 400);
        CHECK(two.size() == 2);
        CHECK(two.value(0) == QStringLiteral("old"));
        CHECK(two.value(1) == QStringLiteral("mid"));

        // The item being watched is never evicted, however old it is. Deleting the strip out from under a
        // live scrub — or the half-finished grids of the file the job is walking — is how a sweep becomes a
        // loop that generates and destroys the same work forever.
        const QStringList kept = Trickplay::planEviction(c, 400, QStringLiteral("old"));
        CHECK(!kept.contains(QStringLiteral("old")));
        CHECK(kept.value(0) == QStringLiteral("mid"));

        // A bound of zero clears everything except the protected item. (Zero is how the setting spells
        // "previews off": nothing may be kept, and the item in hand still is not deleted mid-scrub.)
        const QStringList all = Trickplay::planEviction(c, 0, QStringLiteral("recent"));
        CHECK(all.size() == 2);
        CHECK(!all.contains(QStringLiteral("recent")));

        // Deterministic on a tie: two items last used in the same second evict in key order, so the same
        // cache state gives the same answer on two runs. Without this the order is whatever the directory
        // listing happened to be, and nothing about eviction is testable.
        QVector<Trickplay::CacheEntry> tie;
        tie.push_back({ QStringLiteral("bbb"), 100, 5000 });
        tie.push_back({ QStringLiteral("aaa"), 100, 5000 });
        const QStringList tied = Trickplay::planEviction(tie, 100);
        CHECK(tied.size() == 1);
        CHECK(tied.value(0) == QStringLiteral("aaa"));
    }

    if (failures) { std::fprintf(stderr, "TRICKPLAY-FAIL %d check(s)\n", failures); return 1; }
    std::printf("TRICKPLAY-OK\n");
    return 0;
}
