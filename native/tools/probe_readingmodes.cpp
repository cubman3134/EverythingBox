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
//      odd-width page exactly, with no lost middle column.
//   4. THE CROP HEURISTIC on synthetic pages: a uniform margin goes, art at a corner disables it, the
//      tolerance boundary is exactly where it is claimed to be, and a blank page is refused rather than
//      cropped to a postage stamp.
//   5. THE WEBTOON MAPPING: scroll offset <-> (page, fraction) round-trips in both directions, including
//      from an arbitrary resume position, and a page whose size could not be read still gets a slot.
//   6. THE PREFETCH WINDOW: +/-3, forward first, clamped at both ends of the chapter.
//   7. THE COLOUR FILTERS as exact pixel arithmetic, each preset's numbers written out by hand.
//   8. THE STORE KEY: the document's series when there is one, the filename's otherwise — and two chapters
//      of one series land on the SAME key, which is the whole reason the key exists.
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
#include <QImage>
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

    if (g_fails) { std::fprintf(stderr, "READINGMODES-FAIL %d check(s)\n", g_fails); return 1; }
    std::printf("READINGMODES-OK\n");
    return 0;
}
