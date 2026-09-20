// probe_readingmodes — manga reading modes (issue #154, increment 1): src/comic/ReadingModes, every decision
// it makes, plus the per-series store those decisions are remembered in.
//
// WHY EVERY RULE IS A FREE FUNCTION AND THIS PROBE NEEDS NO WIDGET. The reader that uses them (ComicView) is
// a QWidget over a QScrollArea and cannot be driven headlessly; the DECISIONS — which mode a comic opens in,
// whether a page is a double spread, where the scan's margin ends, what a scroll offset means in pages, what
// a filter does to a pixel — are arithmetic, and arithmetic is testable. So the reader below is a caller and
// this file is the specification.
//
// ORACLES ARE INDEPENDENT: every expected value here is computed by hand in the comment above it, never by
// calling the function under test, and the pre-#154 answer each case rejects is written out where one exists
// (the split that never splits, the crop that never crops, the mode that ignores the override) so a revert
// turns these red rather than merely un-asserted.
//
// WHAT IT PINS:
//
//   1. MODE PRECEDENCE: the per-series mode beats #152's direction override, which beats the archive's own
//      <Manga>, which beats left-to-right. A stored value that is not a mode is "never set", not a mode.
//      A WEBTOON IS NEVER INFERRED from the document — only ever chosen.
//   2. THE STORE round-trips, is scoped per series AND per option, forgets on 0, and survives a series key
//      containing the characters ('/', '=', ']') that would have broken a key-per-series scheme.
//   3. THE SPLIT DECISION either side of aspect ratio 1.0, the viewport half of the rule, all three override
//      values, and WHICH HALF IS SHOWN FIRST in each direction — including that the two halves tile an
//      odd-width page exactly, with no lost middle column — and (#285) WHICH HALF A RESUME REOPENS ON: the
//      stored one while the page still splits, today's entry half in every case where it does not, and
//      today's entry half exactly for a resume written before the half was stored at all.
//   4. THE CROP HEURISTIC on synthetic pages: a uniform margin goes, art at a corner disables it, the
//      tolerance boundary is exactly where it is claimed to be, and a blank page is refused rather than
//      cropped to a postage stamp.
//   5. THE WEBTOON MAPPING: scroll offset <-> (page, fraction) round-trips in both directions, including
//      from an arbitrary resume position, and a page whose size could not be read still gets a slot.
//   6. THE PREFETCH WINDOW: +/-3, forward first, clamped at both ends of the chapter.
//   7. THE COLOUR FILTERS as exact pixel arithmetic, each preset's numbers written out by hand.
//   8. THE STORE KEY: the document's series when there is one, the filename's otherwise — and two chapters
//      of one series land on the SAME key, which is the whole reason the key exists.
//   9. THE SCAN-QUALITY CORRECTIONS (increment 2) as exact pixel arithmetic on synthetic pages whose answer
//      is worked out in the comment above each check: a checkerboard for de-moire (the kernel's whole claim
//      is that it flattens one), a flat field with salt-and-pepper specks for denoise, a step edge for
//      sharpen. Plus: "off" is byte-identical AND not even a copy; a 1x1 and a 2x2 page go through every
//      filter; each filter is the identity on a flat field; THE COMPOSITION ORDER, with the number the
//      swapped order would give written out beside it; and the preset ladder the one control cycles.
//  10. WHERE A ZOOMED PAGE OPENS: top / centre / reading side, in both directions, clamped for a page
//      smaller than the viewport — and that a webtoon ignores the whole question.
//
// Prints READINGMODES-OK on success; any failure prints READINGMODES-FAIL <cond> (line) and exits non-zero.
//
// Isolation: AppPaths::dataDir() is this process's own scratch dir (issue #42), so the settings file this
// writes is its own and goes away with it.
#include "ReadingModes.h"
#include "ComicName.h"
#include "AppPaths.h"
#include "Settings.h"

#include <QDir>
#include <QElapsedTimer>
#include <QImage>
#include <QPoint>
#include <QSize>
#include <QString>
#include <QVector>

#include <cstdio>

static int g_fails = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "READINGMODES-FAIL %s (line %d)\n", #cond, __LINE__); ++g_fails; } \
} while (0)

using ComicRead::Filter;
using ComicRead::Mode;
using ComicRead::Split;

// A page of solid `bg` with a rectangle of `fg` painted into it — the shape a scanned page has: art in the
// middle, margin around it. Built pixel by pixel here rather than by any drawing helper, so the crop's
// expected rectangle is a number this file chose.
static QImage pageWithBlock(int w, int h, QRgb bg, const QRect& block, QRgb fg)
{
    QImage img(w, h, QImage::Format_RGB32);
    img.fill(bg);
    for (int y = block.top(); y <= block.bottom(); ++y)
        for (int x = block.left(); x <= block.right(); ++x)
            img.setPixel(x, y, fg);
    return img;
}

// A page of one solid tone. The flat field every scan-quality filter has to leave alone.
static QImage flatPage(int w, int h, int v)
{
    QImage img(w, h, QImage::Format_RGB32);
    img.fill(qRgb(v, v, v));
    return img;
}

// A CHECKERBOARD alternating every single pixel — the highest frequency an image can carry, and a stand-in
// for the halftone screen a scanned print is printed with.
static QImage checkerPage(int w, int h)
{
    QImage img(w, h, QImage::Format_RGB32);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            img.setPixel(x, y, ((x + y) % 2 == 0) ? qRgb(255, 255, 255) : qRgb(0, 0, 0));
    return img;
}

// A vertical STEP EDGE: `lo` in the columns left of `at`, `hi` from `at` rightwards, every row identical.
static QImage stepPage(int w, int h, int at, int lo, int hi)
{
    QImage img(w, h, QImage::Format_RGB32);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            const int v = x < at ? lo : hi;
            img.setPixel(x, y, qRgb(v, v, v));
        }
    return img;
}

static bool allPixelsAre(const QImage& img, int v)
{
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x)
            if (img.pixel(x, y) != qRgb(v, v, v)) return false;
    return true;
}

int main()
{
    const QRgb white = qRgb(255, 255, 255);
    const QRgb black = qRgb(0, 0, 0);

    // ---- 1. Which mode a comic opens in ------------------------------------------------------------------
    {
        using D = ComicInfo::Direction;
        // Nothing said anywhere: left to right, which is what every comic did before #152 and before this.
        CHECK(ComicRead::resolveMode(D::Unspecified, 0, 0) == Mode::PagedLtr);
        // The document alone (#152: <Manga>YesAndRightToLeft</Manga>) is the paged-rtl DEFAULT.
        CHECK(ComicRead::resolveMode(D::RightToLeft, 0, 0) == Mode::PagedRtl);
        CHECK(ComicRead::resolveMode(D::LeftToRight, 0, 0) == Mode::PagedLtr);
        // #152's direction override beats the document, in BOTH directions.
        CHECK(ComicRead::resolveMode(D::RightToLeft, 1, 0) == Mode::PagedLtr);
        CHECK(ComicRead::resolveMode(D::LeftToRight, 2, 0) == Mode::PagedRtl);
        CHECK(ComicRead::resolveMode(D::Unspecified, 2, 0) == Mode::PagedRtl);
        // The MODE override beats both of them — including a document that declares itself right to left,
        // which is the case a "the document wins" reading would get wrong.
        CHECK(ComicRead::resolveMode(D::RightToLeft, 0, 3) == Mode::Webtoon);
        CHECK(ComicRead::resolveMode(D::RightToLeft, 1, 3) == Mode::Webtoon);
        CHECK(ComicRead::resolveMode(D::RightToLeft, 0, 1) == Mode::PagedLtr);
        CHECK(ComicRead::resolveMode(D::LeftToRight, 0, 2) == Mode::PagedRtl);
        CHECK(ComicRead::resolveMode(D::Unspecified, 0, 3) == Mode::Webtoon);
        // A stored value that is not one of the three is "never set" — a hand-edited ini cannot invent a mode.
        CHECK(ComicRead::resolveMode(D::RightToLeft, 0, 7) == Mode::PagedRtl);
        CHECK(ComicRead::resolveMode(D::Unspecified, 0, -1) == Mode::PagedLtr);
        // A WEBTOON IS NEVER INFERRED: no document value reaches Webtoon on its own.
        CHECK(ComicRead::resolveMode(D::RightToLeft, 2, 0) != Mode::Webtoon);
        CHECK(ComicRead::resolveMode(D::LeftToRight, 1, 0) != Mode::Webtoon);
        CHECK(ComicRead::isRtl(Mode::PagedRtl) && !ComicRead::isRtl(Mode::PagedLtr));
        CHECK(!ComicRead::isRtl(Mode::Webtoon));
        CHECK(ComicRead::isPaged(Mode::PagedLtr) && ComicRead::isPaged(Mode::PagedRtl));
        CHECK(!ComicRead::isPaged(Mode::Webtoon));
    }

    // ---- 2. The per-series store -------------------------------------------------------------------------
    {
        const QString key = QStringLiteral("solo leveling");
        const QString mode = QString::fromLatin1(ComicRead::Opt::kMode);
        const QString crop = QString::fromLatin1(ComicRead::Opt::kCrop);

        CHECK(Settings::comicDisplayOption(key, mode) == 0);              // never asked == the default
        Settings::setComicDisplayOption(key, mode, int(Mode::Webtoon));
        CHECK(Settings::comicDisplayOption(key, mode) == 3);
        CHECK(Settings::comicDisplayOption(QStringLiteral("bone"), mode) == 0);   // scoped to its series
        CHECK(Settings::comicDisplayOption(key, crop) == 0);                      // scoped to its option
        Settings::setComicDisplayOption(key, crop, 1);
        CHECK(Settings::comicDisplayOption(key, crop) == 1);
        CHECK(Settings::comicDisplayOption(key, mode) == 3);              // one option does not clear another
        // 0 FORGETS rather than recording a third state.
        Settings::setComicDisplayOption(key, crop, 0);
        CHECK(Settings::comicDisplayOption(key, crop) == 0);
        CHECK(Settings::comicDisplayOption(key, mode) == 3);
        Settings::setComicDisplayOption(key, mode, 0);
        CHECK(Settings::comicDisplayOption(key, mode) == 0);

        // The characters that would have broken a key-per-series INI scheme ('/' reads as a group separator;
        // '=' and ']' are escaped by the INI backend).
        const QString awkward = QStringLiteral("g.i. joe / cobra [2009] = a");
        Settings::setComicDisplayOption(awkward, mode, 2);
        CHECK(Settings::comicDisplayOption(awkward, mode) == 2);
        Settings::setComicDisplayOption(awkward, mode, 0);
        // Neither half of an unnameable pair is stored or read.
        CHECK(Settings::comicDisplayOption(QString(), mode) == 0);
        CHECK(Settings::comicDisplayOption(key, QString()) == 0);
        Settings::setComicDisplayOption(QString(), mode, 3);
        Settings::setComicDisplayOption(key, QString(), 3);
        CHECK(Settings::comicDisplayOption(QString(), mode) == 0);

        // END TO END: the document says right to left, the user chose webtoon for the series, and the reader
        // opens the webtoon. This is the pair of calls ComicView::openComic makes.
        Settings::setComicDisplayOption(key, mode, int(Mode::Webtoon));
        CHECK(ComicRead::resolveMode(ComicInfo::Direction::RightToLeft,
                                     Settings::comicDirectionOverride(key),
                                     Settings::comicDisplayOption(key, mode)) == Mode::Webtoon);
        Settings::setComicDisplayOption(key, mode, 0);
        CHECK(ComicRead::resolveMode(ComicInfo::Direction::RightToLeft,
                                     Settings::comicDirectionOverride(key),
                                     Settings::comicDisplayOption(key, mode)) == Mode::PagedRtl);
    }

    // ---- 3. The page-split decision ----------------------------------------------------------------------
    {
        const QSize phone(800, 1200);      // upright: the viewport that cannot show a spread
        const QSize desk(1600, 900);       // landscape: one that can

        // AUTO, either side of aspect ratio 1.0 — the boundary is STRICT, so a square page is not a spread.
        CHECK(!ComicRead::shouldSplit(Split::Auto, QSize(1000, 1001), phone));   // taller than wide
        CHECK(!ComicRead::shouldSplit(Split::Auto, QSize(1000, 1000), phone));   // exactly square
        CHECK(ComicRead::shouldSplit(Split::Auto, QSize(1001, 1000), phone));    // one pixel wider than tall
        CHECK(ComicRead::shouldSplit(Split::Auto, QSize(3400, 2200), phone));    // a real scanned spread
        // AUTO's other half: a landscape window shows a wide page perfectly well.
        CHECK(!ComicRead::shouldSplit(Split::Auto, QSize(3400, 2200), desk));
        // A SQUARE viewport is not a landscape one and DOES split: a 3400-wide spread fitted to 1200px is
        // unreadable, while each half fitted to 1200px is a page. The boundary is stated as "not wider than
        // it is tall" for exactly this case.
        CHECK(ComicRead::shouldSplit(Split::Auto, QSize(3400, 2200), QSize(1200, 1200)));
        CHECK(!ComicRead::shouldSplit(Split::Auto, QSize(3400, 2200), QSize(1201, 1200)));  // one pixel landscape
        // ALWAYS ignores both tests; NEVER answers before either is asked.
        CHECK(ComicRead::shouldSplit(Split::Always, QSize(700, 1000), desk));
        CHECK(ComicRead::shouldSplit(Split::Always, QSize(700, 1000), phone));
        CHECK(!ComicRead::shouldSplit(Split::Never, QSize(3400, 2200), phone));
        CHECK(!ComicRead::shouldSplit(Split::Never, QSize(3400, 2200), desk));
        // An undecodable page is never split, whatever the override says.
        CHECK(!ComicRead::shouldSplit(Split::Always, QSize(0, 0), phone));
        CHECK(!ComicRead::shouldSplit(Split::Auto, QSize(-1, 10), phone));
        // Tripwire against the pre-#154 behaviour (no split at all): the two cases above that must be true.
        CHECK(ComicRead::shouldSplit(Split::Auto, QSize(3400, 2200), phone) != false);

        // WHICH HALF COMES FIRST. Page 1000x1400: the halves are 0..499 and 500..999.
        const QSize page(1000, 1400);
        CHECK(ComicRead::splitHalfRect(page, 0, /*rtl=*/false) == QRect(0, 0, 500, 1400));
        CHECK(ComicRead::splitHalfRect(page, 1, /*rtl=*/false) == QRect(500, 0, 500, 1400));
        CHECK(ComicRead::splitHalfRect(page, 0, /*rtl=*/true)  == QRect(500, 0, 500, 1400)); // manga: right first
        CHECK(ComicRead::splitHalfRect(page, 1, /*rtl=*/true)  == QRect(0, 0, 500, 1400));
        // An ODD width keeps every column: 1001 = 500 + 501, and the two rects tile the page exactly.
        const QSize odd(1001, 1400);
        const QRect l = ComicRead::splitHalfRect(odd, 0, false);
        const QRect r = ComicRead::splitHalfRect(odd, 1, false);
        CHECK(l == QRect(0, 0, 500, 1400));
        CHECK(r == QRect(500, 0, 501, 1400));
        CHECK(l.width() + r.width() == 1001);
        CHECK(r.left() == l.right() + 1);   // no gap and no overlap

        // The two navigation rules a split page adds.
        CHECK(ComicRead::stepStaysInPage(0, +1));    // forward on the first half -> the second half
        CHECK(!ComicRead::stepStaysInPage(1, +1));   // forward on the second half -> the next PAGE
        CHECK(ComicRead::stepStaysInPage(1, -1));
        CHECK(!ComicRead::stepStaysInPage(0, -1));   // back on the first half -> the previous PAGE
        CHECK(!ComicRead::stepStaysInPage(-1, +1));  // a whole page never holds a step
        CHECK(!ComicRead::stepStaysInPage(-1, -1));
        CHECK(ComicRead::entryHalf(true, +1) == 0);  // arriving forwards: first half
        CHECK(ComicRead::entryHalf(true, -1) == 1);  // arriving backwards: the half you would have left
        CHECK(ComicRead::entryHalf(false, +1) == -1);
        CHECK(ComicRead::entryHalf(false, -1) == -1);

        // WHICH HALF A RESUME REOPENS ON (#285). entryHalf above is the answer for a page walked into;
        // resumeHalf is the answer for one come BACK to, and the stored half wins wherever it still names a
        // screen that exists. Every expected value below is written out by hand, as a literal.
        //
        // THE BUG: closed on the SECOND half of a spread that still splits, reopened -> the second half.
        // Before #285 no half was stored and the answer was entryHalf(true, +1) = 0, the first half — the
        // right page and the wrong half, which is the issue in one line.
        CHECK(ComicRead::resumeHalf(1, /*splits=*/true, +1) == 1);
        CHECK(ComicRead::resumeHalf(1, true, +1) != 0);        // ... and 0 is exactly what it used to be
        CHECK(ComicRead::resumeHalf(1, true, -1) == 1);
        // A stored FIRST half is just as much a recorded position: it beats the direction in both of them.
        CHECK(ComicRead::resumeHalf(0, true, +1) == 0);
        CHECK(ComicRead::resumeHalf(0, true, -1) == 0);
        CHECK(ComicRead::resumeHalf(0, true, -1) != 1);        // not "arriving backwards" — arriving back
        // THE PAGE NO LONGER SPLITS — a wider or rotated viewport, split set to Never, or WEBTOON, where a
        // page has no halves at all and ComicView::pageSplits answers false for the mode (section 1 pins
        // isPaged(Webtoon) == false). The stored half names a screen that does not exist, so the whole page
        // comes up: -1, which is today's behaviour untouched.
        CHECK(ComicRead::resumeHalf(1, /*splits=*/false, +1) == -1);
        CHECK(ComicRead::resumeHalf(1, false, -1) == -1);
        CHECK(ComicRead::resumeHalf(0, false, +1) == -1);
        CHECK(ComicRead::resumeHalf(0, false, -1) == -1);
        // NO STORED HALF — every resume written before #285 — is today's entry half EXACTLY, in both
        // directions: forwards the first half, backwards the second, and no half on a page that does not
        // split. This is the case that guarantees an old save opens precisely where it opens now.
        CHECK(ComicRead::kNoStoredHalf == -1);
        CHECK(ComicRead::resumeHalf(ComicRead::kNoStoredHalf, true, +1) == 0);
        CHECK(ComicRead::resumeHalf(ComicRead::kNoStoredHalf, true, -1) == 1);
        CHECK(ComicRead::resumeHalf(ComicRead::kNoStoredHalf, false, +1) == -1);
        CHECK(ComicRead::resumeHalf(ComicRead::kNoStoredHalf, false, -1) == -1);
        // A stored value that is not a half is "none": a hand-edited ini cannot invent a third screen, the
        // same forgiving read section 1 makes of a mode that does not exist.
        CHECK(ComicRead::resumeHalf(2, true, +1) == 0);
        CHECK(ComicRead::resumeHalf(7, true, -1) == 1);
        CHECK(ComicRead::resumeHalf(-5, true, +1) == 0);
        CHECK(ComicRead::resumeHalf(99, true, -1) == 1);
        CHECK(ComicRead::resumeHalf(99, false, -1) == -1);
    }

    // ---- 4. The border-crop heuristic --------------------------------------------------------------------
    {
        // A 200x300 page, white margin, art at (50,100) 100x100. Rows 0..99 and 200..299 are margin; columns
        // 0..49 and 150..199 are margin. The crop is therefore exactly the block.
        const QRect block(50, 100, 100, 100);
        const QImage scan = pageWithBlock(200, 300, white, block, black);
        CHECK(ComicRead::cropRect(scan) == block);
        // Tripwire against "no crop": the pre-#154 answer is the whole page.
        CHECK(ComicRead::cropRect(scan) != QRect(0, 0, 200, 300));

        // A BLACK margin crops the same way — the background is the corners' colour, not "white".
        const QImage inverted = pageWithBlock(200, 300, black, block, white);
        CHECK(ComicRead::cropRect(inverted) == block);

        // ART AT A CORNER disables the crop outright: the four corners no longer agree, so this page is left
        // alone rather than having a guess made about it.
        QImage bleed = scan;
        bleed.setPixel(0, 0, black);
        CHECK(ComicRead::cropRect(bleed) == QRect(0, 0, 200, 300));

        // THE TOLERANCE BOUNDARY. One margin pixel at (10,20) is off-white by exactly the tolerance (255-12
        // = 243): still margin, so the crop is unchanged.
        QImage noisy = scan;
        noisy.setPixel(10, 20, qRgb(243, 243, 243));
        CHECK(ComicRead::cropRect(noisy) == block);
        // One channel further (242 = a difference of 13) and that pixel is CONTENT: row 20 and column 10 both
        // stop their scans there, so the crop becomes x 10..149, y 20..199 = (10,20,140,180).
        QImage dirty = scan;
        dirty.setPixel(10, 20, qRgb(242, 242, 242));
        CHECK(ComicRead::cropRect(dirty) == QRect(10, 20, 140, 180));

        // A BLANK page is background everywhere: both scans run to their per-side caps and would leave a
        // 20x30 postage stamp, so the crop is refused whole.
        QImage blank(200, 300, QImage::Format_RGB32);
        blank.fill(white);
        CHECK(ComicRead::cropRect(blank) == QRect(0, 0, 200, 300));

        // A page too small to be worth measuring is never cropped.
        QImage tiny(8, 8, QImage::Format_RGB32);
        tiny.fill(white);
        CHECK(ComicRead::cropRect(tiny) == QRect(0, 0, 8, 8));
        CHECK(ComicRead::cropRect(QImage()) == QRect(0, 0, 0, 0));

        // The whole pipeline, in its stated order: crop first, then the half, then the tint. The cropped page
        // is 100x100, so its first half (left, ltr) is 50x100 — a split of the RAW page would have been 100x300.
        ComicRead::PageOptions o;
        o.crop = true;
        o.half = 0;
        const QImage half = ComicRead::preparePage(scan, o);
        CHECK(half.width() == 50 && half.height() == 100);
        // Nothing asked for: the page comes back as it went in.
        const QImage untouched = ComicRead::preparePage(scan, ComicRead::PageOptions());
        CHECK(untouched.size() == scan.size());
    }

    // ---- 5. The webtoon strip ----------------------------------------------------------------------------
    {
        // Four pages at a 500px viewport. Drawn heights, by hand:
        //   1000x1500 -> 1500 * 500/1000 =  750
        //   1000x500  ->  500 * 500/1000 =  250
        //    800x1600 -> 1600 * 500/800  = 1000
        //   unreadable                   ->  500 * 1.5 = 750   (a slot, or every page after it is unreachable)
        // Tops are the running sum: 0, 750, 1000, 2000; the strip is 2750 tall.
        QVector<QSize> sizes{ QSize(1000, 1500), QSize(1000, 500), QSize(800, 1600), QSize() };
        const ComicRead::Strip s = ComicRead::stripLayout(sizes, 500);
        CHECK(s.count() == 4);
        CHECK(s.width == 500);
        CHECK(s.heights == QVector<int>({ 750, 250, 1000, 750 }));
        CHECK(s.tops == QVector<int>({ 0, 750, 1000, 2000 }));
        CHECK(s.totalHeight == 2750);

        // Offsets, by hand.
        CHECK(ComicRead::stripOffset(s, 0, 0.0) == 0);
        CHECK(ComicRead::stripOffset(s, 1, 0.0) == 750);
        CHECK(ComicRead::stripOffset(s, 1, 0.5) == 875);      // 750 + 250/2
        CHECK(ComicRead::stripOffset(s, 2, 0.25) == 1250);    // 1000 + 1000/4
        CHECK(ComicRead::stripOffset(s, 99, 0.0) == 2000);    // a page past the end clamps to the last
        CHECK(ComicRead::stripOffset(s, 1, 9.0) == 1000);     // a fraction past 1 clamps to the page's end

        // ... and the same numbers read backwards.
        int p = -1; double f = -1.0;
        ComicRead::stripPositionAt(s, 875, &p, &f);
        CHECK(p == 1 && f == 0.5);
        ComicRead::stripPositionAt(s, 750, &p, &f);
        CHECK(p == 1 && f == 0.0);                            // a page's first pixel is that page, not the last
        ComicRead::stripPositionAt(s, 749, &p, &f);
        CHECK(p == 0);
        ComicRead::stripPositionAt(s, 0, &p, &f);
        CHECK(p == 0 && f == 0.0);
        ComicRead::stripPositionAt(s, 99999, &p, &f);
        CHECK(p == 3);                                        // past the end clamps into the last page

        // THE ROUND TRIP, which is what the resume position depends on: every scroll offset in the strip
        // reads back as a (page, fraction) that maps to exactly that offset again.
        const int failsBefore = g_fails;
        for (int y = 0; y < s.totalHeight; y += 7)
        {
            int pp = 0; double ff = 0.0;
            ComicRead::stripPositionAt(s, y, &pp, &ff);
            CHECK(ComicRead::stripOffset(s, pp, ff) == y);
            if (g_fails != failsBefore) break;   // one failure here is the same failure 393 times
        }
        // And the resume case specifically: close mid-strip, store (page, fraction), come back to the pixel.
        int rp = 0; double rf = 0.0;
        ComicRead::stripPositionAt(s, 1234, &rp, &rf);
        CHECK(rp == 2);
        CHECK(ComicRead::stripOffset(s, rp, rf) == 1234);
        // A relayout at a different width keeps the READING position even though every pixel moved: the same
        // (page, fraction) is a valid offset in the new strip. At 1000px the pages are twice as tall.
        const ComicRead::Strip wide = ComicRead::stripLayout(sizes, 1000);
        CHECK(wide.heights == QVector<int>({ 1500, 500, 2000, 1500 }));
        // rf = (1234-1000)/1000 = 0.234, page 2 of the wide strip starts at 1500+500 = 2000 and is 2000 tall,
        // so the same reading position is 2000 + 0.234*2000 = 2468.
        CHECK(ComicRead::stripOffset(wide, rp, rf) == 2468);
        int wp = 0; double wf = 0.0;
        ComicRead::stripPositionAt(wide, ComicRead::stripOffset(wide, rp, rf), &wp, &wf);
        CHECK(wp == rp);

        // An empty comic has no strip and asks nothing of its callers.
        const ComicRead::Strip none = ComicRead::stripLayout({}, 500);
        CHECK(none.count() == 0 && none.totalHeight == 0);
        CHECK(ComicRead::stripOffset(none, 0, 0.5) == 0);
        int np = 5; double nf = 5.0;
        ComicRead::stripPositionAt(none, 100, &np, &nf);
        CHECK(np == 0 && nf == 0.0);
    }

    // ---- 6. The prefetch window --------------------------------------------------------------------------
    {
        // +/-3 around page 5 of 100, forward first: 5, 6, 4, 7, 3, 8, 2.
        CHECK(ComicRead::prefetchWindow(5, 100, 3) == QVector<int>({ 5, 6, 4, 7, 3, 8, 2 }));
        CHECK(ComicRead::kPrefetchRadius == 3);
        CHECK(ComicRead::prefetchWindow(5, 100) == QVector<int>({ 5, 6, 4, 7, 3, 8, 2 }));   // the default IS 3
        // Clamped at the start and at the end — never an index outside the chapter.
        CHECK(ComicRead::prefetchWindow(0, 100, 3) == QVector<int>({ 0, 1, 2, 3 }));
        CHECK(ComicRead::prefetchWindow(99, 100, 3) == QVector<int>({ 99, 98, 97, 96 }));
        CHECK(ComicRead::prefetchWindow(0, 1, 3) == QVector<int>({ 0 }));
        CHECK(ComicRead::prefetchWindow(0, 0, 3).isEmpty());
        CHECK(ComicRead::prefetchWindow(500, 3, 1) == QVector<int>({ 2, 1 }));   // a current past the end clamps
        // Radius 0 is the paged path's window: this page and nothing else. The webtoon window is WIDER than
        // that on purpose — a flick crosses three pages before one decode finishes.
        CHECK(ComicRead::prefetchWindow(5, 100, 0) == QVector<int>({ 5 }));
        CHECK(ComicRead::prefetchWindow(5, 100, 3).size() == 7);
    }

    // ---- 6b. Decoding off the paint path (#286) ----------------------------------------------------------
    {
        using ComicRead::StripResult;
        const QVector<int> win = ComicRead::prefetchWindow(10, 100);          // 10, 11, 9, 12, 8, 13, 7

        // A rail jump lands where nothing is cached or in flight: every page of the window is requested, the
        // landed page FIRST, and in prefetchWindow's own order after it.
        const QVector<int> jump = ComicRead::stripRequests(win, {}, {}, 1);
        CHECK(jump == win);
        CHECK(!jump.isEmpty() && jump.first() == 10);

        // Already cached, and already in flight under THIS generation: neither is asked for twice. The order
        // of what is left is still the window's.
        const QSet<int> cached{ 11, 8 };
        const QHash<int, quint64> flying{ { 12, quint64(1) }, { 9, quint64(1) } };
        CHECK(ComicRead::stripRequests(win, cached, flying, 1) == QVector<int>({ 10, 13, 7 }));
        // A page cached AND in flight is still not requested.
        CHECK(ComicRead::stripRequests({ 4 }, { 4 }, { { 4, quint64(1) } }, 1).isEmpty());
        // Pages outside the window are never requested, whatever the sets say about them.
        CHECK(!ComicRead::stripRequests(win, {}, { { 50, quint64(1) } }, 1).contains(50));

        // A GENERATION BUMP re-requests: the in-flight marks were made under generation 1, the cache has been
        // cleared since (generation 2), so those old requests will be dropped when they land and the pages
        // must be asked for again now.
        CHECK(ComicRead::stripRequests(win, {}, flying, 2) == win);
        CHECK(ComicRead::stripRequests(win, cached, flying, 2) == QVector<int>({ 10, 9, 12, 13, 7 }));

        // An empty window asks for nothing.
        CHECK(ComicRead::stripRequests({}, {}, {}, 1).isEmpty());

        // ACCEPT: the current generation, the current width, a page inside the current window.
        const StripResult current{ 12, 7, 800 };
        CHECK(ComicRead::acceptStripResult(current, 7, 800, win));
        // DROP: a stale generation (older, and — defensively — newer).
        CHECK(!ComicRead::acceptStripResult(StripResult{ 12, 6, 800 }, 7, 800, win));
        CHECK(!ComicRead::acceptStripResult(StripResult{ 12, 8, 800 }, 7, 800, win));
        // DROP: a stale width (the strip was resized while the page was being scaled).
        CHECK(!ComicRead::acceptStripResult(StripResult{ 12, 7, 799 }, 7, 800, win));
        // DROP: a page the window has moved away from, even at the right generation and width.
        CHECK(!ComicRead::acceptStripResult(StripResult{ 40, 7, 800 }, 7, 800, win));
        CHECK(!ComicRead::acceptStripResult(StripResult{ 14, 7, 800 }, 7, 800, win));   // one past the edge
        CHECK(ComicRead::acceptStripResult(StripResult{ 13, 7, 800 }, 7, 800, win));    // the edge itself
        CHECK(!ComicRead::acceptStripResult(StripResult{ -1, 7, 800 }, 7, 800, win));
        CHECK(!ComicRead::acceptStripResult(current, 7, 800, {}));                       // no window at all

        // THE WORKER'S START CHECK: a job does nothing once its generation is gone or its page left the window.
        CHECK(ComicRead::stripJobWanted(12, 3, 3, 7, 13));
        CHECK(ComicRead::stripJobWanted(7, 3, 3, 7, 13) && ComicRead::stripJobWanted(13, 3, 3, 7, 13));
        CHECK(!ComicRead::stripJobWanted(12, 2, 3, 7, 13));
        CHECK(!ComicRead::stripJobWanted(6, 3, 3, 7, 13));
        CHECK(!ComicRead::stripJobWanted(14, 3, 3, 7, 13));
        CHECK(!ComicRead::stripJobWanted(12, 3, 3, 0, -1));                               // an empty window

        // THE WINDOW NEVER EXCLUDES A PAGE ON SCREEN. Tall pages: the viewport shows one page, and the window
        // is prefetchWindow's +/-3 exactly.
        const ComicRead::Strip tall = ComicRead::stripLayout(QVector<QSize>(30, QSize(900, 9000)), 900);
        CHECK(ComicRead::stripLastVisible(tall, ComicRead::stripOffset(tall, 12, 0.5), 700) == 12);
        CHECK(ComicRead::stripLastVisible(tall, tall.tops[12] + 9000 - 100, 700) == 13);   // straddling a seam
        CHECK(ComicRead::stripWindow(tall, 12, 13) == ComicRead::prefetchWindow(12, 30));
        CHECK(ComicRead::stripWindow(tall, 12, 12) == ComicRead::prefetchWindow(12, 30));
        // Short pages (50 px each at this width): a 400 px viewport at the top shows pages 0..7, which is past
        // +3, so the radius widens to reach page 7 — and keeps prefetchWindow's order.
        const ComicRead::Strip shortPages = ComicRead::stripLayout(QVector<QSize>(20, QSize(100, 50)), 100);
        CHECK(ComicRead::stripLastVisible(shortPages, 0, 400) == 7);
        CHECK(ComicRead::stripLastVisible(shortPages, 25, 400) == 8);                       // 25..424 reaches 8
        CHECK(ComicRead::stripLastVisible(shortPages, 950, 400) == 19);                     // clamped to the end
        CHECK(ComicRead::stripWindow(shortPages, 0, 7) == ComicRead::prefetchWindow(0, 20, 7));
        CHECK(ComicRead::stripWindow(shortPages, 0, 7).contains(7));
        CHECK(ComicRead::stripLastVisible(ComicRead::stripLayout({}, 100), 0, 400) == 0);
        CHECK(ComicRead::stripWindow(ComicRead::stripLayout({}, 100), 0, 0).isEmpty());
    }

    // ---- 6c. The reader's own controls (#397) -------------------------------------------------------------
    // A 1100 x 800 reader in the webtoon strip: the 116-px rail flush right, the strip's 17-px vertical bar
    // just left of it, no horizontal bar. The rail starts at y = 0, under the host's 38-px top band.
    {
        using ComicRead::PointerControls;
        const QRect rail(984, 0, 116, 800);
        const QRect vBar(967, 0, 17, 800);
        const QRect hBar(0, 783, 967, 17);
        const auto controls = [&](bool webtoon, bool railOn, bool vBar_, bool hBar_) {
            PointerControls c;
            c.railShown = ComicRead::railShown(webtoon, railOn);
            c.rail = rail;
            c.vBarShown = vBar_;
            c.vBar = vBar;
            c.hBarShown = hBar_;
            c.hBar = hBar;
            return c;
        };
        const PointerControls strip = controls(true, true, true, false);

        // The rule that shows the rail: only the strip, only with the rail on.
        CHECK(ComicRead::railShown(true, true));
        CHECK(!ComicRead::railShown(true, false));
        CHECK(!ComicRead::railShown(false, true));
        CHECK(!ComicRead::railShown(false, false));

        // A point inside the visible rail is the reader's: a far thumbnail, a top slot under the band, both
        // corners.
        const QPoint onRail(1040, 600);
        CHECK(ComicRead::ownsPointerAt(onRail, strip));
        CHECK(ComicRead::ownsPointerAt(QPoint(1040, 10), strip));
        CHECK(ComicRead::ownsPointerAt(QPoint(984, 0), strip));
        CHECK(ComicRead::ownsPointerAt(QPoint(1099, 799), strip));

        // The same point with the rail switched off, or in a paged mode (where the rail is never shown), is not.
        CHECK(!ComicRead::ownsPointerAt(onRail, controls(true, false, true, false)));
        CHECK(!ComicRead::ownsPointerAt(onRail, controls(false, true, true, false)));
        CHECK(!ComicRead::ownsPointerAt(onRail, controls(false, true, false, false)));

        // A point on the page is never the reader's — it stays a tap zone, including right beside the bar.
        CHECK(!ComicRead::ownsPointerAt(QPoint(480, 400), strip));
        CHECK(!ComicRead::ownsPointerAt(QPoint(966, 400), strip));
        CHECK(!ComicRead::ownsPointerAt(QPoint(480, 10), strip));
        CHECK(!ComicRead::ownsPointerAt(QPoint(-1, 400), strip));
        CHECK(!ComicRead::ownsPointerAt(QPoint(1100, 400), strip));   // one past the rail's right edge

        // The scroll bars: owned while shown, nothing once hidden.
        CHECK(ComicRead::ownsPointerAt(QPoint(975, 400), strip));
        CHECK(!ComicRead::ownsPointerAt(QPoint(975, 400), controls(true, true, false, false)));
        CHECK(ComicRead::ownsPointerAt(QPoint(480, 790), controls(false, false, true, true)));
        CHECK(!ComicRead::ownsPointerAt(QPoint(480, 790), strip));

        // Nothing shown: a point on any of the three old rectangles belongs to the page.
        const PointerControls none = controls(false, false, false, false);
        CHECK(!ComicRead::ownsPointerAt(onRail, none));
        CHECK(!ComicRead::ownsPointerAt(QPoint(975, 400), none));
        CHECK(!ComicRead::ownsPointerAt(QPoint(480, 790), none));
    }

    // ---- 7. The colour filters ---------------------------------------------------------------------------
    {
        const QRgb px = qRgb(200, 100, 50);
        // GREYSCALE is qGray: (200*11 + 100*16 + 50*5) / 32 = (2200 + 1600 + 250) / 32 = 4050/32 = 126.
        const ComicRead::ColorAdjust grey = ComicRead::adjustFor(Filter::Greyscale);
        CHECK(grey.greyscale && grey.brightness == 0 && grey.contrast == 0 && grey.warmth == 0);
        CHECK(ComicRead::adjustPixel(px, grey) == qRgb(126, 126, 126));

        // SEPIA is that grey, warmed: warmth 60 -> +/- 60*40/100 = 24, so red 126+24 and blue 126-24.
        const ComicRead::ColorAdjust sepia = ComicRead::adjustFor(Filter::Sepia);
        CHECK(sepia.greyscale && sepia.warmth == 60);
        CHECK(ComicRead::adjustPixel(px, sepia) == qRgb(150, 126, 102));

        // NIGHT on a blazing-white scan background: brightness -35 -> -35*255/100 = -89 (255-89 = 166), then
        // warmth 25 -> +/- 10. Red 176, green 166, blue 156.
        const ComicRead::ColorAdjust night = ComicRead::adjustFor(Filter::Night);
        CHECK(night.brightness == -35 && night.warmth == 25 && !night.greyscale);
        CHECK(ComicRead::adjustPixel(qRgb(255, 255, 255), night) == qRgb(176, 166, 156));

        // HIGH CONTRAST is about mid-grey: 128 + (200-128)*140/100 = 128 + 100 = 228; 128 + (100-128)*1.4 =
        // 128 - 39 (integer division of -3920/100 truncates toward zero) = 89; 128 + (50-128)*1.4 = 128 - 109
        // = 19. Mid-grey itself never moves.
        const ComicRead::ColorAdjust hc = ComicRead::adjustFor(Filter::HighContrast);
        CHECK(hc.contrast == 40);
        CHECK(ComicRead::adjustPixel(px, hc) == qRgb(228, 89, 19));
        CHECK(ComicRead::adjustPixel(qRgb(128, 128, 128), hc) == qRgb(128, 128, 128));

        // NONE is the identity, and the identity is not merely "close to" the original.
        const ComicRead::ColorAdjust none = ComicRead::adjustFor(Filter::None);
        CHECK(none.isIdentity());
        CHECK(ComicRead::adjustPixel(px, none) == px);

        // Clamping is at the END and only there: white cannot go above 255 or black below 0.
        ComicRead::ColorAdjust up; up.brightness = 100;
        CHECK(ComicRead::adjustPixel(qRgb(255, 255, 255), up) == qRgb(255, 255, 255));
        ComicRead::ColorAdjust down; down.brightness = -100;
        CHECK(ComicRead::adjustPixel(qRgb(0, 0, 0), down) == qRgb(0, 0, 0));

        // Over a whole (tiny) image, and the alpha channel carried through untouched.
        QImage fixture(2, 2, QImage::Format_ARGB32);
        fixture.setPixel(0, 0, qRgba(200, 100, 50, 128));
        fixture.setPixel(1, 0, qRgba(255, 255, 255, 255));
        fixture.setPixel(0, 1, qRgba(0, 0, 0, 255));
        fixture.setPixel(1, 1, qRgba(10, 20, 30, 7));
        const QImage greyed = ComicRead::applyAdjust(fixture, grey);
        CHECK(qRed(greyed.pixel(0, 0)) == 126 && qGreen(greyed.pixel(0, 0)) == 126);
        CHECK(qAlpha(greyed.pixel(0, 0)) == 128);          // alpha is not a colour
        CHECK(qAlpha(greyed.pixel(1, 1)) == 7);
        CHECK(greyed.pixel(1, 0) == qRgba(255, 255, 255, 255));
        CHECK(greyed.pixel(0, 1) == qRgba(0, 0, 0, 255));
        // An identity adjust does not touch the image at all.
        CHECK(ComicRead::applyAdjust(fixture, none).pixel(0, 0) == fixture.pixel(0, 0));

        // The filter runs LAST in the pipeline, after the crop and the split.
        ComicRead::PageOptions o;
        o.crop = true;
        o.adjust = grey;
        const QImage out = ComicRead::preparePage(pageWithBlock(200, 300, white, QRect(50, 100, 100, 100), qRgb(200, 100, 50)), o);
        CHECK(out.width() == 100 && out.height() == 100);        // cropped to the art
        CHECK(qRed(out.pixel(0, 0)) == 126);                     // and then greyed
    }

    // ---- 8. The key a comic's settings are remembered under ----------------------------------------------
    {
        // The document's series decides, folded exactly as the shelf folds it (#134's seriesKey).
        CHECK(ComicRead::seriesKeyFor(QStringLiteral("Saga"), QStringLiteral("/x/anything.cbz"))
              == ComicName::seriesKey(QStringLiteral("Saga")));
        CHECK(ComicRead::seriesKeyFor(QStringLiteral("Saga"), QString())
              == ComicRead::seriesKeyFor(QStringLiteral("saga  "), QString()));   // folded, so a re-spelling is the same series

        // With no document, the FILENAME — and the property that matters is that two chapters of one series
        // land on the same key, or a webtoon would open paged again at chapter two.
        const QString c1 = ComicRead::seriesKeyFor(QString(), QStringLiteral("/x/Solo Leveling 001.cbz"));
        const QString c2 = ComicRead::seriesKeyFor(QString(), QStringLiteral("/x/Solo Leveling 002.cbz"));
        CHECK(!c1.isEmpty());
        CHECK(c1 == c2);
        CHECK(c1 != ComicRead::seriesKeyFor(QString(), QStringLiteral("/x/Tower of God 001.cbz")));
        // A name with no number at all still gets a key — its own cleaned name.
        CHECK(ComicRead::seriesKeyFor(QString(), QStringLiteral("/x/Watchmen.cbz"))
              == ComicName::seriesKey(QStringLiteral("Watchmen")));
        // A document series beats the filename even when the two disagree.
        CHECK(ComicRead::seriesKeyFor(QStringLiteral("Bone"), QStringLiteral("/x/Solo Leveling 001.cbz"))
              == ComicName::seriesKey(QStringLiteral("Bone")));
        // Nothing to go on == no key, and no key stores nothing (section 2).
        CHECK(ComicRead::seriesKeyFor(QString(), QString()).isEmpty());
    }

    // ---- 9. The scan-quality corrections (increment 2) ----------------------------------------------------
    {
        using ComicRead::ScanFixes;

        // ---- 9a. DE-MOIRE is the 3x3 binomial [1 2 1; 2 4 2; 1 2 1]/16 with replicate padding, computed as
        // a [1 2 1] pass along each row (call it h(x,y)) and then [1 2 1] down those, divided once:
        //     out(x,y) = ( h(x,y-1) + 2*h(x,y) + h(x,y+1) + 8 ) / 16
        //
        // On the checkerboard every INTERIOR h is 255 + 2*0 + 255 = 510 for a black pixel and
        // 0 + 2*255 + 0 = 510 for a white one \u2014 the same number, which is the whole point of the kernel \u2014
        // so out = (510 + 1020 + 510 + 8) / 16 = 2048 / 16 = 128 EXACTLY, everywhere inside the border.
        // That is the halftone gone: a screen that alternated 0/255 every pixel is a flat mid grey.
        {
            const QImage checker = checkerPage(8, 8);
            const QImage out = ComicRead::demoire(checker);
            CHECK(out.size() == QSize(8, 8));
            bool interiorFlat = true;
            for (int y = 1; y <= 6; ++y)
                for (int x = 1; x <= 6; ++x)
                    if (out.pixel(x, y) != qRgb(128, 128, 128)) interiorFlat = false;
            CHECK(interiorFlat);
            // Tripwire against "de-moire does nothing": the pre-increment-2 answer is the checkerboard itself.
            CHECK(out.pixel(1, 1) != checker.pixel(1, 1));

            // THE BORDER, by hand, so the replicate padding is pinned rather than assumed. At (0,0) (white):
            //   h(0,0) = s(0,0) + 2*s(0,0) + s(1,0) = 255 + 510 + 0   = 765   (x-1 clamps to x=0)
            //   h(0,1) = s(0,1) + 2*s(0,1) + s(1,1) = 0   + 0   + 255 = 255
            //   out    = (h(0,-1 -> 0) + 2*h(0,0) + h(0,1) + 8) / 16
            //          = (765 + 1530 + 255 + 8) / 16 = 2558 / 16 = 159
            CHECK(out.pixel(0, 0) == qRgb(159, 159, 159));
            CHECK(out.pixel(7, 7) == qRgb(159, 159, 159));   // the far corner is the same figure mirrored
            // ... while a border pixel that is NOT a corner clamps in one axis only and lands back on 128:
            //   h(0,2) = h(0,4) = 255 + 510 + 0 = 765 (white), h(0,3) = 0 + 0 + 255 = 255 (black)
            //   out(0,3) = (765 + 510 + 765 + 8) / 16 = 2048 / 16 = 128
            CHECK(out.pixel(0, 3) == qRgb(128, 128, 128));
        }

        // ---- 9b. DENOISE is the cross median: median{left, centre, right, above, below}, replicate padded.
        // On a flat field an isolated speck is outvoted 4-to-1 and goes; everything else is its own value.
        {
            QImage speckled = flatPage(16, 16, 60);
            speckled.setPixel(5, 5, qRgb(255, 255, 255));   // salt
            speckled.setPixel(9, 9, qRgb(0, 0, 0));         // pepper
            speckled.setPixel(0, 7, qRgb(255, 255, 255));   // on the LEFT EDGE: {c,c,60,60,60} -> 60, gone too
            speckled.setPixel(0, 0, qRgb(255, 255, 255));   // in the CORNER: {c,c,c,60,60} -> c, it survives

            const QImage out = ComicRead::denoise(speckled);
            CHECK(out.pixel(5, 5) == qRgb(60, 60, 60));      // median{60,255,60,60,60} = 60
            CHECK(out.pixel(9, 9) == qRgb(60, 60, 60));      // median{60,0,60,60,60}   = 60
            CHECK(out.pixel(0, 7) == qRgb(60, 60, 60));      // one clamp: median{255,255,60,60,60} = 60
            // A CORNER speck survives, because replicate padding makes the corner pixel three of the five
            // votes: median{255,255,255,60,60} = 255. Stated rather than hidden \u2014 it is one pixel of one
            // corner, and the alternative (special-casing the corner) would be a rule with no other reason.
            CHECK(out.pixel(0, 0) == qRgb(255, 255, 255));
            // Tripwire against "denoise does nothing".
            CHECK(out.pixel(5, 5) != speckled.pixel(5, 5));
            // A NEIGHBOUR of a speck is untouched: median{60,60,60,255,60} = 60.
            CHECK(out.pixel(5, 6) == qRgb(60, 60, 60));

            // A STEP EDGE survives a median EXACTLY \u2014 that is what edge-preserving means. At the last dark
            // column: median{50,50,200,50,50} = 50; at the first bright one: median{50,200,200,200,200} = 200.
            const QImage step = stepPage(8, 8, 4, 50, 200);
            CHECK(ComicRead::denoise(step) == step);

            // IDEMPOTENT on this fixture: the second pass has nothing left to outvote.
            CHECK(ComicRead::denoise(out) == out);
        }

        // ---- 9c. SHARPEN is the unsharp mask at 50%: out = (3*c - blur + 1) / 2, clamped, with `blur` the
        // de-moire kernel. On the step edge every row is identical, so the vertical half of the kernel is the
        // identity and blur(x) = (4*h(x) + 8) / 16 with h the [1 2 1] row pass:
        //   x=1 (flat 50):   h = 50 + 100 + 50   = 200 -> blur = 808  / 16 = 50   -> (150 - 50  + 1)/2 = 50
        //   x=3 (last 50):   h = 50 + 100 + 200  = 350 -> blur = 1408 / 16 = 88   -> (150 - 88  + 1)/2 = 31
        //   x=4 (first 200): h = 50 + 400 + 200  = 650 -> blur = 2608 / 16 = 163  -> (600 - 163 + 1)/2 = 219
        //   x=6 (flat 200):  h = 200 + 400 + 200 = 800 -> blur = 3208 / 16 = 200  -> (600 - 200 + 1)/2 = 200
        // The flat runs come back untouched and the edge gains its halo: darker on the dark side, brighter on
        // the bright side. That is the whole of what an unsharp mask does.
        {
            const QImage step = stepPage(8, 8, 4, 50, 200);
            const QImage out = ComicRead::sharpen(step);
            CHECK(out.pixel(0, 3) == qRgb(50, 50, 50));
            CHECK(out.pixel(1, 3) == qRgb(50, 50, 50));
            CHECK(out.pixel(2, 3) == qRgb(50, 50, 50));
            CHECK(out.pixel(3, 3) == qRgb(31, 31, 31));
            CHECK(out.pixel(4, 3) == qRgb(219, 219, 219));
            CHECK(out.pixel(5, 3) == qRgb(200, 200, 200));
            CHECK(out.pixel(7, 0) == qRgb(200, 200, 200));   // the top row is the same, by replicate padding
            // Tripwire against "sharpen does nothing".
            CHECK(out.pixel(3, 3) != step.pixel(3, 3));
        }

        // ---- 9d. A FLAT FIELD is the fixed point of all three. Nothing to low-pass, nothing to outvote,
        // nothing to sharpen: c == blur, so (3c - c + 1)/2 == c.
        {
            const QImage flat = flatPage(16, 16, 77);
            CHECK(ComicRead::demoire(flat) == flat);
            CHECK(ComicRead::denoise(flat) == flat);
            CHECK(ComicRead::sharpen(flat) == flat);
        }

        // ---- 9e. OFF IS NOT EVEN A COPY. An identity set hands `src` straight back, so a series with no
        // scan fixes pays nothing for the feature existing \u2014 same pixels, same buffer.
        {
            const QImage page = checkerPage(8, 8);
            const QImage out = ComicRead::applyScanFixes(page, ScanFixes());
            CHECK(out == page);
            CHECK(out.constBits() == page.constBits());
        }

        // ---- 9f. THE COMPOSITION ORDER: de-moire, then denoise, then sharpen.
        //
        // The pair that proves it is denoise-then-sharpen over one salt speck on a flat 60 field. In the
        // STATED order the median removes the speck first, so sharpen is handed a flat field and returns it
        // untouched: the answer is a flat 60 page, to the byte.
        //
        // SWAPPED, sharpen goes first and amplifies exactly what the median was going to remove:
        //   blur(5,5) = (240 + 2*630 + 240 + 8)/16 = 109  -> (765 - 109 + 1)/2 = 328 -> clamped to 255
        //   blur(4,5) = (240 + 2*435 + 240 + 8)/16 = 84   -> (180 -  84 + 1)/2 = 48   (a dark halo)
        // and the median then sees median{48,255,48,48,48} = 48. So the speck is not removed at all \u2014 it is
        // replaced by a dark pit two shades off the page. 48, not 60, is what the wrong order ships.
        {
            QImage salted = flatPage(16, 16, 60);
            salted.setPixel(5, 5, qRgb(255, 255, 255));

            ScanFixes f;
            f.denoise = true;
            f.sharpen = true;
            const QImage out = ComicRead::applyScanFixes(salted, f);
            CHECK(out == flatPage(16, 16, 60));
            CHECK(allPixelsAre(out, 60));
            // ... and the swapped order, computed above, gives 48 there.
            CHECK(ComicRead::denoise(ComicRead::sharpen(salted)).pixel(5, 5) == qRgb(48, 48, 48));

            // All three together are exactly sharpen(denoise(demoire(x))) and not any other arrangement.
            ScanFixes all;
            all.demoire = all.denoise = all.sharpen = true;
            const QImage three = ComicRead::applyScanFixes(salted, all);
            CHECK(three == ComicRead::sharpen(ComicRead::denoise(ComicRead::demoire(salted))));
            CHECK(three != ComicRead::demoire(ComicRead::denoise(ComicRead::sharpen(salted))));
            CHECK(three != ComicRead::sharpen(ComicRead::demoire(ComicRead::denoise(salted))));
        }

        // ---- 9g. THE SCAN FIXES RUN AFTER CROP AND SPLIT AND BEFORE THE COLOUR FILTER.
        // A 200x300 page, white margin, a 100x100 block of flat mid grey art at (50,100) with one black
        // speck in it. Crop takes it down to the 100x100 block; the half takes the left 50x100 of THAT; the
        // median then removes the speck (it is at (10,10) of the block, inside the left half); and the
        // greyscale filter runs last over the finished page. If the scan fixes ran before the crop, the
        // kernel's replicate border would be the paper's border and not the art's.
        {
            const QRgb art = qRgb(120, 120, 120);
            QImage scan = pageWithBlock(200, 300, white, QRect(50, 100, 100, 100), art);
            scan.setPixel(60, 110, black);   // a speck 10,10 into the block

            ComicRead::PageOptions o;
            o.crop = true;
            o.half = 0;                      // the left half of the cropped block, ltr
            o.scan.denoise = true;
            const QImage out = ComicRead::preparePage(scan, o);
            CHECK(out.width() == 50 && out.height() == 100);
            CHECK(out.pixel(10, 10) == art);         // the speck is gone, inside the cropped+split page
            CHECK(allPixelsAre(out, 120));
            // Without the scan fixes the same pipeline keeps the speck \u2014 the pre-increment-2 answer.
            ComicRead::PageOptions plain;
            plain.crop = true;
            plain.half = 0;
            CHECK(ComicRead::preparePage(scan, plain).pixel(10, 10) == black);
        }

        // ---- 9h. A 1x1 AND A 2x2 PAGE go through every filter. With replicate padding a 1x1 image is its
        // own whole neighbourhood, so all three are the identity on it; the 2x2 checkerboard is small enough
        // to work out by hand and is the case where EVERY pixel is a border pixel.
        {
            QImage one(1, 1, QImage::Format_RGB32);
            one.fill(qRgb(200, 100, 50));
            CHECK(ComicRead::demoire(one) == one);
            CHECK(ComicRead::denoise(one) == one);
            CHECK(ComicRead::sharpen(one) == one);

            const QImage two = checkerPage(2, 2);
            // h(0,0) = 255+510+0 = 765, h(1,0) = 255+0+0 = 255, h(0,1) = 0+0+255 = 255, h(1,1) = 0+510+255 = 765
            //   out(0,0) = (765 + 1530 + 255 + 8)/16 = 159      out(1,0) = (255 + 510 + 765 + 8)/16 =  96
            //   out(0,1) = (765 +  510 + 255 + 8)/16 =  96      out(1,1) = (255 + 1530 + 765 + 8)/16 = 159
            const QImage blurred = ComicRead::demoire(two);
            CHECK(blurred.pixel(0, 0) == qRgb(159, 159, 159));
            CHECK(blurred.pixel(1, 0) == qRgb(96, 96, 96));
            CHECK(blurred.pixel(0, 1) == qRgb(96, 96, 96));
            CHECK(blurred.pixel(1, 1) == qRgb(159, 159, 159));
            // On a 2x2 every cross has three votes for the centre (two clamps), so the median keeps it;
            // and the unsharp mask saturates each pixel back to the value it already had.
            CHECK(ComicRead::denoise(two) == two);
            CHECK(ComicRead::sharpen(two) == two);

            // A null page is handed straight back rather than crashing anything.
            CHECK(ComicRead::demoire(QImage()).isNull());
            CHECK(ComicRead::denoise(QImage()).isNull());
            CHECK(ComicRead::sharpen(QImage()).isNull());
            ScanFixes all;
            all.demoire = all.denoise = all.sharpen = true;
            CHECK(ComicRead::applyScanFixes(QImage(), all).isNull());
        }

        // ---- 9i. THE PRESET LADDER the one control cycles: off, de-moire, denoise, sharpen, all three.
        {
            CHECK(ComicRead::scanPreset(0).isIdentity());
            CHECK(ComicRead::scanPreset(1).demoire && !ComicRead::scanPreset(1).denoise && !ComicRead::scanPreset(1).sharpen);
            CHECK(!ComicRead::scanPreset(2).demoire && ComicRead::scanPreset(2).denoise && !ComicRead::scanPreset(2).sharpen);
            CHECK(!ComicRead::scanPreset(3).demoire && !ComicRead::scanPreset(3).denoise && ComicRead::scanPreset(3).sharpen);
            CHECK(ComicRead::scanPreset(4).demoire && ComicRead::scanPreset(4).denoise && ComicRead::scanPreset(4).sharpen);
            CHECK(ComicRead::scanPreset(5).isIdentity());    // off the ladder is "off"
            CHECK(ComicRead::scanPreset(-1).isIdentity());

            for (int i = 0; i < ComicRead::kScanPresetCount; ++i)
                CHECK(ComicRead::scanPresetIndex(ComicRead::scanPreset(i)) == i);

            // The cycle, all the way round and back to off.
            ScanFixes f;                                       // off
            f = ComicRead::nextScanPreset(f); CHECK(ComicRead::scanPresetIndex(f) == 1);
            f = ComicRead::nextScanPreset(f); CHECK(ComicRead::scanPresetIndex(f) == 2);
            f = ComicRead::nextScanPreset(f); CHECK(ComicRead::scanPresetIndex(f) == 3);
            f = ComicRead::nextScanPreset(f); CHECK(ComicRead::scanPresetIndex(f) == 4);
            f = ComicRead::nextScanPreset(f); CHECK(f.isIdentity());

            // A combination the ladder does not name (a hand-edited ini) reads as -1 and the next press is off.
            ScanFixes odd;
            odd.denoise = true;
            odd.sharpen = true;
            CHECK(ComicRead::scanPresetIndex(odd) == -1);
            CHECK(ComicRead::nextScanPreset(odd).isIdentity());
        }

        // ---- 9j. THE COST, at a realistic page. Printed, never asserted: a wall-clock number is a property
        // of the machine the probe ran on, and a threshold here would fail on a loaded CI runner while
        // telling nobody anything. The brief's budget is 80 ms per filter at about 1600x2400.
        {
            const QImage big = checkerPage(1600, 2400);
            QElapsedTimer t;
            t.start(); const QImage a = ComicRead::demoire(big); const qint64 msD = t.elapsed();
            t.start(); const QImage b = ComicRead::denoise(big); const qint64 msN = t.elapsed();
            t.start(); const QImage c = ComicRead::sharpen(big); const qint64 msS = t.elapsed();
            CHECK(!a.isNull() && !b.isNull() && !c.isNull());
            std::printf("READINGMODES-TIMING 1600x2400 demoire=%lldms denoise=%lldms sharpen=%lldms\n",
                        (long long)msD, (long long)msN, (long long)msS);
        }
    }

    // ---- 10. Where a zoomed page opens --------------------------------------------------------------------
    {
        using ComicRead::ZoomStart;

        // A page scaled to 1000x3000 inside a 400x800 viewport. The scrollable range is therefore
        // 1000-400 = 600 across and 3000-800 = 2200 down; every answer lives in [0,600] x [0,2200].
        const QSize content(1000, 3000), viewport(400, 800);

        // TOP is what the reader has always done: the vertical bar to 0, the horizontal one left alone.
        CHECK(ComicRead::zoomStartOffset(content, viewport, ZoomStart::Top, false, QPoint(137, 999))
              == QPoint(137, 0));
        CHECK(ComicRead::zoomStartOffset(content, viewport, ZoomStart::Top, true, QPoint(137, 999))
              == QPoint(137, 0));           // the direction changes nothing about Top
        // ... clamped, so a stale horizontal position from a wider page cannot land off the end.
        CHECK(ComicRead::zoomStartOffset(content, viewport, ZoomStart::Top, false, QPoint(5000, 0))
              == QPoint(600, 0));
        CHECK(ComicRead::zoomStartOffset(content, viewport, ZoomStart::Top, false, QPoint(-40, 0))
              == QPoint(0, 0));

        // CENTRE is the middle of both ranges: 600/2 = 300 across, 2200/2 = 1100 down.
        CHECK(ComicRead::zoomStartOffset(content, viewport, ZoomStart::Centre, false, QPoint(137, 999))
              == QPoint(300, 1100));
        CHECK(ComicRead::zoomStartOffset(content, viewport, ZoomStart::Centre, true, QPoint(137, 999))
              == QPoint(300, 1100));

        // READING SIDE is the top at the edge the eye starts from: the left in a left-to-right comic,
        // the RIGHT (x = 600, the far end of the range) in a right-to-left one.
        CHECK(ComicRead::zoomStartOffset(content, viewport, ZoomStart::ReadingSide, false, QPoint(137, 999))
              == QPoint(0, 0));
        CHECK(ComicRead::zoomStartOffset(content, viewport, ZoomStart::ReadingSide, true, QPoint(137, 999))
              == QPoint(600, 0));
        // The two directions must not agree, or the option is decorative.
        CHECK(ComicRead::zoomStartOffset(content, viewport, ZoomStart::ReadingSide, false, QPoint(0, 0))
              != ComicRead::zoomStartOffset(content, viewport, ZoomStart::ReadingSide, true, QPoint(0, 0)));

        // A PAGE SMALLER THAN THE VIEWPORT has nowhere to scroll, so every option answers (0,0) \u2014 including
        // Top, whose remembered horizontal position clamps to 0 along with everything else.
        const QSize small(300, 500);
        CHECK(ComicRead::zoomStartOffset(small, viewport, ZoomStart::Top, false, QPoint(77, 88)) == QPoint(0, 0));
        CHECK(ComicRead::zoomStartOffset(small, viewport, ZoomStart::Centre, false, QPoint(77, 88)) == QPoint(0, 0));
        CHECK(ComicRead::zoomStartOffset(small, viewport, ZoomStart::ReadingSide, true, QPoint(77, 88)) == QPoint(0, 0));
        // A degenerate viewport (the reader before it has been laid out) is not a divide by anything.
        CHECK(ComicRead::zoomStartOffset(content, QSize(0, 0), ZoomStart::Centre, false, QPoint(0, 0))
              == QPoint(500, 1500));

        // A WEBTOON IGNORES IT. The strip owns its own position; only the paged modes ask.
        CHECK(ComicRead::zoomStartApplies(Mode::PagedLtr));
        CHECK(ComicRead::zoomStartApplies(Mode::PagedRtl));
        CHECK(!ComicRead::zoomStartApplies(Mode::Webtoon));

        // The stored value reads forgivingly, exactly as every other option in this file does: 0 (and
        // anything that is not a position) is Top, which is what a series with no opinion gets.
        CHECK(ComicRead::zoomStartFor(0) == ZoomStart::Top);
        CHECK(ComicRead::zoomStartFor(1) == ZoomStart::Centre);
        CHECK(ComicRead::zoomStartFor(2) == ZoomStart::ReadingSide);
        CHECK(ComicRead::zoomStartFor(7) == ZoomStart::Top);
        CHECK(ComicRead::zoomStartFor(-3) == ZoomStart::Top);

        // The store round-trips it under the reserved key, beside increment 1's options and without
        // disturbing them (section 2 owns the general property; this is the new key's own).
        const QString key = QStringLiteral("zoomstart-series");
        Settings::setComicDisplayOption(key, QString::fromLatin1(ComicRead::Opt::kZoomStart), 2);
        Settings::setComicDisplayOption(key, QString::fromLatin1(ComicRead::Opt::kDemoire), 1);
        CHECK(ComicRead::zoomStartFor(Settings::comicDisplayOption(key, QString::fromLatin1(ComicRead::Opt::kZoomStart)))
              == ZoomStart::ReadingSide);
        CHECK(Settings::comicDisplayOption(key, QString::fromLatin1(ComicRead::Opt::kDemoire)) == 1);
        CHECK(Settings::comicDisplayOption(key, QString::fromLatin1(ComicRead::Opt::kDenoise)) == 0);
        CHECK(Settings::comicDisplayOption(key, QString::fromLatin1(ComicRead::Opt::kSharpen)) == 0);
    }

    if (g_fails) { std::fprintf(stderr, "READINGMODES-FAIL %d check(s)\n", g_fails); return 1; }
    std::printf("READINGMODES-OK\n");
    return 0;
}
