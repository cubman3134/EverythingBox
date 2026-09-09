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
// SINCE ISSUE #302 there is a sixth, and it is the one that spends somebody's electricity: WHEN the job
// may run at all. #85 generated a film's strip between playbacks, so it existed from the second viewing;
// #302 widens the trigger to a walk of the local library on genuine idle, so it exists from the first.
// That is work the user did not ask for at that moment, which makes every refusal in
// TrickplayIdle::evaluate load-bearing — battery above all, since a handheld quietly making thumbnails
// while unplugged is a bug however politely it does it. Sections 7 to 9 drive that predicate over its
// whole input space, assert that #85's own trigger is untouched by any of the new inputs, and pin the
// walk's resume point, its already-cached skip and its stop-do-not-evict rule at the size bound.
//
// Prints TRICKPLAY-OK on success; any failure prints TRICKPLAY-FAIL <cond> and exits non-zero.
#include "Trickplay.h"
#include "TrickplayIdle.h"

#include <QString>
#include <QStringList>
#include <QVector>
#include <algorithm>
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


    // ---- 7. The idle predicate (issue #302) ----------------------------------------------------------
    //
    // #85 made previews between playbacks. #302 lets the same job also run on genuine IDLE, walking the local
    // library, so a film has its strip the FIRST time it is watched. That is uninvited work on somebody
    // else's machine, so this predicate is the whole safety of the increment and it is driven here over its
    // entire input space rather than at the two or three points that seem interesting.
    using TrickplayIdle::Conditions;
    using TrickplayIdle::Power;
    using TrickplayIdle::Verdict;

    // The state in which everything says yes. Every case below is this, with exactly one thing changed —
    // which is the only way to be sure the case is testing what it claims to.
    Conditions ok;
    ok.previewsEnabled = true;
    ok.idleEnabled     = true;
    ok.playing         = false;
    ok.scanning        = false;
    ok.building        = false;
    ok.userActive      = false;
    ok.power           = Power::Mains;
    ok.cacheBytes      = 100;
    ok.boundBytes      = 1000;
    CHECK(TrickplayIdle::evaluate(ok) == Verdict::Go);
    CHECK(TrickplayIdle::mayStart(ok));

    // One condition at a time, and each one names its own refusal — a log line that says "no" is no use for
    // working out why a library never gained previews.
    { Conditions c = ok; c.previewsEnabled = false; CHECK(TrickplayIdle::evaluate(c) == Verdict::PreviewsOff); }
    { Conditions c = ok; c.idleEnabled     = false; CHECK(TrickplayIdle::evaluate(c) == Verdict::IdleOff); }
    { Conditions c = ok; c.playing         = true;  CHECK(TrickplayIdle::evaluate(c) == Verdict::Playing); }
    { Conditions c = ok; c.scanning        = true;  CHECK(TrickplayIdle::evaluate(c) == Verdict::Scanning); }
    { Conditions c = ok; c.building        = true;  CHECK(TrickplayIdle::evaluate(c) == Verdict::Building); }
    { Conditions c = ok; c.userActive      = true;  CHECK(TrickplayIdle::evaluate(c) == Verdict::UserActive); }

    // BATTERY. The issue's own absolute: never. And Unknown refuses too, which is a decision and not an
    // oversight — this is uninvited work, so the burden of proof is on us to show it is free, and a machine
    // that will not say whether it is plugged in has not shown that. A desktop with no battery answers Mains
    // (TrickplayPower), so this costs a real desktop nothing.
    { Conditions c = ok; c.power = Power::Battery; CHECK(TrickplayIdle::evaluate(c) == Verdict::OnBattery); }
    { Conditions c = ok; c.power = Power::Unknown; CHECK(TrickplayIdle::evaluate(c) == Verdict::PowerUnknown); }
    { Conditions c = ok; c.power = Power::Battery; CHECK(!TrickplayIdle::mayStart(c)); }
    { Conditions c = ok; c.power = Power::Unknown; CHECK(!TrickplayIdle::mayStart(c)); }

    // THE BOUND, REACHED. Stop; do not evict. Eviction exists to make room for what the user IS watching, and
    // deleting last night's film to cache one nobody has opened is the cache working against its owner. The
    // predicate's part of that is simply refusing to start another item.
    { Conditions c = ok; c.cacheBytes = 1000; CHECK(TrickplayIdle::evaluate(c) == Verdict::CacheFull); }  // exactly at it
    { Conditions c = ok; c.cacheBytes = 1001; CHECK(TrickplayIdle::evaluate(c) == Verdict::CacheFull); }  // over it
    { Conditions c = ok; c.cacheBytes = 999;  CHECK(TrickplayIdle::evaluate(c) == Verdict::Go); }         // one byte under
    { Conditions c = ok; c.boundBytes = 0; c.cacheBytes = 0; CHECK(TrickplayIdle::evaluate(c) == Verdict::CacheFull); }

    // The reasons are ordered, and the order is asserted so it cannot drift: a user who switched the feature
    // off is told THAT, not that their disk is full. Two refusals at once answer the earlier one.
    { Conditions c = ok; c.previewsEnabled = false; c.playing = true; c.power = Power::Battery;
      CHECK(TrickplayIdle::evaluate(c) == Verdict::PreviewsOff); }
    { Conditions c = ok; c.idleEnabled = false; c.power = Power::Battery;
      CHECK(TrickplayIdle::evaluate(c) == Verdict::IdleOff); }
    { Conditions c = ok; c.playing = true; c.scanning = true; c.building = true;
      CHECK(TrickplayIdle::evaluate(c) == Verdict::Playing); }
    { Conditions c = ok; c.scanning = true; c.building = true; CHECK(TrickplayIdle::evaluate(c) == Verdict::Scanning); }
    { Conditions c = ok; c.building = true; c.userActive = true; CHECK(TrickplayIdle::evaluate(c) == Verdict::Building); }
    { Conditions c = ok; c.userActive = true; c.power = Power::Battery;
      CHECK(TrickplayIdle::evaluate(c) == Verdict::UserActive); }
    { Conditions c = ok; c.power = Power::Battery; c.cacheBytes = 99999;
      CHECK(TrickplayIdle::evaluate(c) == Verdict::OnBattery); }

    // EVERY COMBINATION, exhaustively: 2^6 booleans x 3 power states x 3 cache states. The assertion is
    // stated INDEPENDENTLY of evaluate() rather than by re-running it — "may start" is exactly "all eight
    // permissions hold" — so a rule dropped from the predicate is caught by the conjunction here rather than
    // by a copy of the same mistake.
    {
        const Power powers[3] = { Power::Unknown, Power::Mains, Power::Battery };
        const qint64 caches[3] = { 0, 999, 1000 };   // empty, one byte under the bound, exactly at it
        int seenGo = 0, seenNo = 0;
        for (int bits = 0; bits < 64; ++bits)
            for (int pi = 0; pi < 3; ++pi)
                for (int ci = 0; ci < 3; ++ci)
                {
                    Conditions c;
                    c.previewsEnabled = (bits & 1)  != 0;
                    c.idleEnabled     = (bits & 2)  != 0;
                    c.playing         = (bits & 4)  != 0;
                    c.scanning        = (bits & 8)  != 0;
                    c.building        = (bits & 16) != 0;
                    c.userActive      = (bits & 32) != 0;
                    c.power           = powers[pi];
                    c.cacheBytes      = caches[ci];
                    c.boundBytes      = 1000;
                    const bool expected = c.previewsEnabled && c.idleEnabled && !c.playing && !c.scanning
                                          && !c.building && !c.userActive && c.power == Power::Mains
                                          && c.cacheBytes < c.boundBytes;
                    CHECK(TrickplayIdle::mayStart(c) == expected);
                    CHECK((TrickplayIdle::evaluate(c) == Verdict::Go) == expected);
                    if (expected) ++seenGo; else ++seenNo;
                }
        // …and the sweep really did exercise both answers, so a predicate stuck at one value cannot pass by
        // making every "expected" agree with it.
        CHECK(seenGo == 2);      // Mains x (cache 0, cache 999), with all six booleans in their one good state
        CHECK(seenNo == 574);
    }

    // ---- 8. #85's own trigger, unchanged (the identity assertion) --------------------------------------
    //
    // The point of this section is a NEGATIVE: widening the job onto idle must not narrow the trigger it
    // already had. mayGenerateForOpenedFile takes exactly the two inputs #85 gave it, and the whole
    // cross-product of #302's six new ones is swept past it to assert the answer never moves. A field added
    // to Conditions cannot start gating it, because it does not take a Conditions at all.
    CHECK(TrickplayIdle::mayGenerateForOpenedFile(true, false));    // previews on, nothing playing: generate
    CHECK(!TrickplayIdle::mayGenerateForOpenedFile(true, true));    // …the player has the machine: wait
    CHECK(!TrickplayIdle::mayGenerateForOpenedFile(false, false));  // …previews off: never
    CHECK(!TrickplayIdle::mayGenerateForOpenedFile(false, true));
    {
        const Power powers[3] = { Power::Unknown, Power::Mains, Power::Battery };
        for (int bits = 0; bits < 16; ++bits)
            for (int pi = 0; pi < 3; ++pi)
                for (int prev = 0; prev < 2; ++prev)
                    for (int play = 0; play < 2; ++play)
                    {
                        Conditions c;
                        c.previewsEnabled = prev != 0;
                        c.playing         = play != 0;
                        c.idleEnabled     = (bits & 1) != 0;    // …every one of these is #302's, and…
                        c.scanning        = (bits & 2) != 0;
                        c.building        = (bits & 4) != 0;
                        c.userActive      = (bits & 8) != 0;
                        c.power           = powers[pi];
                        c.cacheBytes      = 999999;             // …the cache being over its bound too
                        c.boundBytes      = 1000;
                        // …none of which may change the between-playbacks answer.
                        CHECK(TrickplayIdle::mayGenerateForOpenedFile(c.previewsEnabled, c.playing)
                              == (c.previewsEnabled && !c.playing));
                        // The idle walk is strictly NARROWER: whatever it permits, #85's trigger permits too.
                        if (TrickplayIdle::mayStart(c))
                            CHECK(TrickplayIdle::mayGenerateForOpenedFile(c.previewsEnabled, c.playing));
                    }
    }
    // A film the user is watching still gets its strip on a machine running from battery, mid-scan and
    // mid-compile. That is #85's bargain and #302 does not revisit it: the user opened the file.
    CHECK(TrickplayIdle::mayGenerateForOpenedFile(true, false));

    // ---- 9. The walk: bounded, resumable, and it does not redo work ------------------------------------
    {
        QStringList lib;
        for (int i = 1; i <= 10; ++i)
            lib << QStringLiteral("D:/Films/%1.mkv").arg(i, 2, 10, QLatin1Char('0'));   // 01..10, sorted

        // A fresh sweep starts at the beginning.
        CHECK(TrickplayIdle::nextAfter(lib, QString()) == 0);

        // INTERRUPTED AFTER FILE 3 OF 10, RESUMES AT 4 — and does not redo 1 to 3. The cursor is the path
        // just completed, so this is the whole property in one line.
        const QString after3 = TrickplayIdle::advanceCursor(lib, 2);          // finished index 2 = "03.mkv"
        CHECK(after3 == QStringLiteral("D:/Films/03.mkv"));
        CHECK(TrickplayIdle::nextAfter(lib, after3) == 3);                    // index 3 = "04.mkv"
        CHECK(lib.at(TrickplayIdle::nextAfter(lib, after3)) == QStringLiteral("D:/Films/04.mkv"));

        // Step it forward one at a time from there and the rest of the library follows in order, exactly
        // once each — the loop the walk actually runs.
        QStringList visited;
        QString cur = after3;
        for (int guard = 0; guard < 20; ++guard)
        {
            const int i = TrickplayIdle::nextAfter(lib, cur);
            if (i < 0) break;
            visited << lib.at(i);
            cur = TrickplayIdle::advanceCursor(lib, i);
            if (cur.isEmpty()) break;      // that was the last file
        }
        CHECK(visited.size() == 7);
        CHECK(visited.first() == QStringLiteral("D:/Films/04.mkv"));
        CHECK(visited.last()  == QStringLiteral("D:/Films/10.mkv"));
        CHECK(!visited.contains(QStringLiteral("D:/Films/01.mkv")));
        CHECK(!visited.contains(QStringLiteral("D:/Films/03.mkv")));

        // The END of a sweep clears the cursor rather than pinning it at the last file, so the next idle
        // period starts at the top and picks up whatever was added since. On an already-swept library that
        // costs one sidecar read per file and never opens a decoder (ItemState::Complete, below).
        CHECK(TrickplayIdle::advanceCursor(lib, 9).isEmpty());
        CHECK(TrickplayIdle::nextAfter(lib, QStringLiteral("D:/Films/10.mkv")) == -1);

        // A cursor naming a file that is GONE (deleted, renamed, an unplugged drive) does not restart the
        // sweep and does not stop it: the answer is still "the first path after it". That is why the cursor
        // is a path and not an index — an index-based resume silently skips a file for ever the first time
        // something is inserted alphabetically before it.
        CHECK(TrickplayIdle::nextAfter(lib, QStringLiteral("D:/Films/03a-was-deleted.mkv")) == 3);
        QStringList grown = lib;
        grown << QStringLiteral("D:/Films/00-new-arrival.mkv");
        std::sort(grown.begin(), grown.end());
        CHECK(grown.at(TrickplayIdle::nextAfter(grown, after3)) == QStringLiteral("D:/Films/04.mkv"));

        // Degenerate inputs answer "nothing to do" rather than index 0 of an empty list.
        CHECK(TrickplayIdle::nextAfter(QStringList(), QString()) == -1);
        CHECK(TrickplayIdle::advanceCursor(lib, -1).isEmpty());
        CHECK(TrickplayIdle::advanceCursor(lib, 99).isEmpty());
        // An empty entry is skipped rather than treated as a path that sorts before everything.
        QStringList holey; holey << QString() << QStringLiteral("D:/Films/01.mkv");
        CHECK(TrickplayIdle::nextAfter(holey, QString()) == 1);
    }

    // ALREADY CACHED IS SKIPPED, and a half-finished item is NOT: it is the cheapest work in the library,
    // because Trickplay::resumeGrid means only its missing grids are captured.
    CHECK(!TrickplayIdle::needsGeneration(TrickplayIdle::ItemState::Complete));
    CHECK(TrickplayIdle::needsGeneration(TrickplayIdle::ItemState::Partial));
    CHECK(TrickplayIdle::needsGeneration(TrickplayIdle::ItemState::NoSheets));

    // THE BOUND REACHED MID-WALK. The worker re-asks this between grids, so a film that was inside the bound
    // when it started and crosses it halfway through stops with the whole grids it has written — rather than
    // evicting something to finish itself.
    CHECK(TrickplayIdle::hasHeadroom(0, 1000));
    CHECK(TrickplayIdle::hasHeadroom(999, 1000));
    CHECK(!TrickplayIdle::hasHeadroom(1000, 1000));    // exactly at the bound is FULL, not "just fits"
    CHECK(!TrickplayIdle::hasHeadroom(1001, 1000));
    CHECK(!TrickplayIdle::hasHeadroom(0, 0));          // previews off spells the bound as zero
    CHECK(!TrickplayIdle::hasHeadroom(0, -1));

    if (failures) { std::fprintf(stderr, "TRICKPLAY-FAIL %d check(s)\n", failures); return 1; }
    std::printf("TRICKPLAY-OK\n");
    return 0;
}
