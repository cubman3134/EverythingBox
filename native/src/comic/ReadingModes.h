// HOW A COMIC IS READ, AS PURE FUNCTIONS (issue #154, increment 1).
//
// A comic reader has to present three different things that are all "a bag of images in reading order":
// a western comic paged left to right, a manga paged right to left, and a KOREAN/CHINESE WEBTOON, which is
// one continuous vertical strip and which paging through is simply wrong. #152 taught the reader the second
// of those from the archive's own ComicInfo.xml; this file adds the third, plus the three display-time
// corrections a scanned page needs (split a double spread, trim the scan's margins, tint the page).
//
// EVERY DECISION IN HERE IS A FREE FUNCTION over plain values — a QSize, a QImage, an int. No widgets, no
// QSettings, no disk, no ComicView. That is what lets probe_readingmodes assert every one of them against a
// hand-computed oracle, and it is why the reader below can be a thin caller: the reader decides WHEN to ask,
// this file decides WHAT the answer is.
//
// ---- THE ONE PAGE SEAM IS NOT FORKED ---------------------------------------------------------------------
//
// PageSupply.h states the rule these modes sit under: a local archive (#134), an addon's page list (#188) and
// an OPDS-PSE stream (#153) all arrive as the SAME list of encoded page images in ComicView. Nothing here
// knows which supplier a page came from, and nothing here may — a reading mode that worked only for local
// CBZs would be the fourth supplier this project deliberately does not have.
//
// ---- WHAT IS DELIBERATELY NOT HERE (increment 2) ----------------------------------------------------------
//
// De-moire, noise reduction and sharpen (the scan-quality set) and the zoom-start position. Their stored
// option names are RESERVED below and nothing writes them, so increment 2 adds a value and a transform and
// changes no shape. Also not here, and not increment 2 either: JOINING two consecutive single pages into one
// landscape spread (the inverse of the split), panel-by-panel navigation, and OCR.
#pragma once
#include "ComicInfo.h"

#include <QImage>
#include <QRect>
#include <QSize>
#include <QString>
#include <QVector>
#include <QtGlobal>

namespace ComicRead
{
    // ---- THE MODE ------------------------------------------------------------------------------------------
    // The values are the PERSISTED values (Settings::comicDisplayOption), which is why they start at 1: 0 is
    // "the user has no opinion", exactly as #152's direction override spells it, so forgetting an override and
    // never having had one are the same stored state.
    enum class Mode
    {
        PagedLtr = 1,   // one page at a time, next page to the right — every comic before #152
        PagedRtl = 2,   // one page at a time, next page to the LEFT — a manga (#152's direction, as a mode)
        Webtoon  = 3,   // one continuous vertical strip, no page boundaries — a long-strip webtoon
    };

    inline bool isPaged(Mode m) { return m == Mode::PagedLtr || m == Mode::PagedRtl; }
    inline bool isRtl(Mode m)   { return m == Mode::PagedRtl; }

    // WHICH MODE A COMIC ACTUALLY OPENS IN. Three inputs, in strict precedence:
    //
    //   1. `modeOverride` — what the user set for THIS SERIES in the reader (1/2/3 above; 0 = never set).
    //      It wins outright, including over a document that declares itself right to left, because a user
    //      edit beats metadata everywhere else in this library and a webtoon is a thing no ComicInfo can say.
    //   2. `directionOverride` — #152's per-series direction (0 none / 1 ltr / 2 rtl). It predates this file
    //      and is still honoured: a series somebody flipped to right-to-left before reading modes existed
    //      keeps reading right to left, as paged-rtl.
    //   3. `embedded` — the archive's own <Manga> field. YesAndRightToLeft is the paged-rtl default (#152).
    //
    // With none of them: paged left to right, which is what every comic did before any of this existed.
    //
    // A WEBTOON IS NEVER INFERRED. Nothing in ComicInfo.xml says "long strip" — publishers and format tags
    // hint at it and a hint is not a statement, and guessing wrong replaces a readable comic with a scroll
    // that never ends. So webtoon is only ever reached by somebody choosing it (decision 1 of the brief:
    // a format/publisher webtoon hint is NOT inferred this increment).
    Mode resolveMode(ComicInfo::Direction embedded, int directionOverride, int modeOverride);

    // ---- PAGE SPLIT ----------------------------------------------------------------------------------------
    // A double-page spread scanned as ONE wide image is unreadable on a narrow screen. Splitting it into two
    // portrait halves is right about 95% of the time and WRONG LOUDLY the rest — a splash page drawn wide, a
    // cover with the spine in it, a webtoon panel — so the detection carries a per-series override.
    enum class Split
    {
        Auto   = 0,   // wider than tall, on a viewport that is not itself landscape (the default)
        Always = 1,   // split every page, whatever its shape and whatever the window is doing
        Never  = 2,   // never split — the escape hatch for the 5%
    };

    // Auto's rule, stated once: the page is WIDER THAN TALL (strictly — a square page is not a spread) and the
    // viewport is not wider than it is tall. The viewport half matters because a wide window shows a wide page
    // perfectly well; it is a phone held upright that cannot. Always ignores both tests by construction (the
    // user said always); Never answers false before either is asked.
    bool shouldSplit(Split over, const QSize& page, const QSize& viewport);

    // The rectangle of one half of a split page. `order` is READING ORDER, not geometry: 0 is the half shown
    // FIRST and 1 the half shown second, and `rtl` is what decides which side each of those is — in a manga
    // the right half is read first. Odd widths keep every column: the left half is w/2 wide and the right half
    // takes the remainder, so the two rects tile the page exactly with no lost middle column.
    QRect splitHalfRect(const QSize& page, int order, bool rtl);

    // The two navigation rules a split page adds, as predicates rather than as branches buried in nextPage().
    // `half` is -1 (the page is whole), 0 (first half on screen) or 1 (second half).
    //
    // stepStaysInPage: a forward press on the first half moves to the second half of the SAME page, and a
    // backward press on the second half moves back to the first — those two presses never change the page.
    inline bool stepStaysInPage(int half, int dir)
    {
        return (dir > 0 && half == 0) || (dir < 0 && half == 1);
    }
    // entryHalf: which half is on screen when a page is ENTERED. Arriving forwards lands on the first half;
    // arriving BACKWARDS lands on the second, because reading backwards through a spread that showed you its
    // second half last should show you its second half first.
    inline int entryHalf(bool splits, int dir) { return !splits ? -1 : (dir < 0 ? 1 : 0); }

    // ---- BORDER CROP ---------------------------------------------------------------------------------------
    // Trim the uniform margin a scanner leaves around the art. The heuristic, in full:
    //
    //   * The background colour is the image's FOUR CORNERS, and they must agree with each other within the
    //     tolerance. Art that reaches any corner therefore disables the crop for that page outright — which is
    //     the direction to fail in, because an over-eager crop eats artwork and a refused one costs nothing.
    //   * A row (then a column) is trimmed while every pixel in it is within the tolerance of that colour.
    //     Columns are scanned only over the rows that survived, so a margin that is L-shaped still goes.
    //   * NOTHING IS EVER TRIMMED PAST kMaxTrimPercent from any one side, and a result that keeps less than
    //     kMinKeepPercent of either dimension (or is smaller than 16x16) is DISCARDED WHOLE. A blank page is
    //     background everywhere, so both scans run to their limits and meet in a postage stamp of nothing;
    //     the per-side cap alone does not catch that, because two capped sides still leave 10%.
    //
    // Display-time only. Nothing in this file ever writes an image back to its archive.
    inline constexpr int kCropTolerance   = 12;   // per-channel, 0..255 — a JPEG's white margin is not one white
    inline constexpr int kMaxTrimPercent  = 45;   // per side
    inline constexpr int kMinKeepPercent  = 25;   // of each dimension, or the crop is refused whole
    inline constexpr int kMinCroppedPx    = 16;
    QRect cropRect(const QImage& img, int tolerance = kCropTolerance);

    // ---- COLOUR FILTERS ------------------------------------------------------------------------------------
    // Display-time only, per series. The knobs are a struct so increment 2's additions have somewhere to go
    // and so a preset is a NAMED SET OF NUMBERS rather than its own transform.
    struct ColorAdjust
    {
        bool greyscale  = false;   // luminance (qGray), applied FIRST and to the original pixel
        int  brightness = 0;       // -100..100 -> each channel +/- brightness*255/100
        int  contrast   = 0;       // -100..100 -> (v-128) * (100+contrast)/100 + 128, about mid-grey
        int  warmth     = 0;       // -100..100 -> red +warmth*40/100, blue -warmth*40/100 (sepia's engine)

        bool isIdentity() const
        { return !greyscale && brightness == 0 && contrast == 0 && warmth == 0; }
    };

    enum class Filter
    {
        None         = 0,   // the default: the page as it is
        Greyscale    = 1,
        Sepia        = 2,   // greyscale + warmth: the "aged paper" that a blazing-white scan is not
        Night        = 3,   // dimmed and warmed, for reading in the dark without dimming the whole screen
        HighContrast = 4,
    };

    ColorAdjust adjustFor(Filter f);

    // ONE CLAMP, AT THE END. Each step feeds the next unclamped, so brightness followed by contrast is one
    // composed transform rather than two rounded ones; only the final channel value is clipped to 0..255.
    // Alpha is carried through untouched.
    QRgb adjustPixel(QRgb p, const ColorAdjust& a);
    QImage applyAdjust(const QImage& src, const ColorAdjust& a);   // identity adjust returns src unchanged

    // ---- THE WHOLE DISPLAY PIPELINE FOR ONE PAGE -----------------------------------------------------------
    // ORDER IS LOAD-BEARING: crop, then split, then filter.
    //   * Crop BEFORE split, because the midpoint of a spread with a 5% white margin down one side is not the
    //     midpoint of the art — splitting first would cut one half short and pad the other.
    //   * Filter LAST, because the crop's corner test reads the scan's own colours and a sepia tint would
    //     move them (and because tinting pixels that are about to be thrown away is wasted work).
    struct PageOptions
    {
        bool  crop = false;
        int   cropTolerance = kCropTolerance;
        int   half = -1;          // -1 whole page, 0 first half, 1 second half (see splitHalfRect)
        bool  rtl  = false;       // which side "first" is, when half >= 0
        ColorAdjust adjust;
    };
    QImage preparePage(const QImage& src, const PageOptions& o);

    // ---- WEBTOON: THE CONTINUOUS STRIP ---------------------------------------------------------------------
    // The pages are stitched EDGE TO EDGE with no gap and no page furniture — that is the whole point, and it
    // is why there is no gap parameter to get wrong. Each page is width-fitted to the viewport, so its drawn
    // height is h * viewportW / w and the strip is the running sum of those.
    //
    // WHY THE LAYOUT IS BUILT FROM RAW PAGE SIZES. A size comes from the image's HEADER (QImageReader::size),
    // which costs microseconds and no decode, so a 200-page strip lays out instantly at open. That is also why
    // BORDER CROP AND PAGE SPLIT DO NOT APPLY IN WEBTOON MODE and the reader does not offer them there: both
    // change a page's shape, so both would change every following page's offset — and the resume position,
    // which is stored as (page, fraction), would land somewhere else every time a crop rect came out slightly
    // different. It costs nothing real: a long strip is drawn digitally, edge to edge, with no scan margins
    // and no double spreads. The colour filters DO apply, because a tint changes no geometry.
    struct Strip
    {
        QVector<int> tops;          // tops[i] = the y offset page i starts at, in strip pixels
        QVector<int> heights;       // heights[i] = its drawn height (never 0 — see kUnreadablePageAspect)
        int totalHeight = 0;
        int width = 0;              // the viewport width this layout was computed for
        int count() const { return tops.size(); }
    };

    // A page whose size could not be read still needs a slot, or every page after it would be unreachable.
    // It gets a portrait box of this aspect, which is roughly a comic page.
    inline constexpr double kUnreadablePageAspect = 1.5;

    Strip stripLayout(const QVector<QSize>& pageSizes, int viewportW);

    // The two halves of the SCROLL <-> (page, fraction) mapping — the pair the resume position round-trips
    // through, so they are inverses by construction and probe_readingmodes asserts exactly that.
    int  stripOffset(const Strip& s, int page, double fraction);
    void stripPositionAt(const Strip& s, int y, int* page, double* fraction);

    // NEIGHBOUR PREFETCH, +/-3 PAGES. Scroll velocity outruns page-turn latency: a finger flick crosses three
    // pages before a single decode finishes, so the window is deliberately wider than the paged path's one.
    // The result is in PRIORITY ORDER — the current page, then forward, then back, alternating outwards —
    // because a caller with a budget spends it from the front, and forward is where the reader is going.
    inline constexpr int kPrefetchRadius = 3;
    QVector<int> prefetchWindow(int current, int total, int radius = kPrefetchRadius);

    // How far Up/Down move in webtoon mode, as a fraction of the viewport height. Less than 1 on purpose:
    // a full-viewport jump loses the line you were on at the seam between two presses.
    inline constexpr double kScrollFraction = 0.85;

    // ---- THE PER-SERIES STORE ------------------------------------------------------------------------------
    // The option NAMES, in one place, because they are strings that cross into QSettings and a second spelling
    // of one is a silently forgotten setting. Every option is an int and 0 always means "the default", which
    // is what lets the store forget an option rather than record a third state (Settings.h says why).
    namespace Opt
    {
        inline constexpr char kMode[]   = "mode";     // ComicRead::Mode, 0 = the document/direction decide
        inline constexpr char kSplit[]  = "split";    // ComicRead::Split
        inline constexpr char kCrop[]   = "crop";     // 0 off, 1 on
        inline constexpr char kFilter[] = "filter";   // ComicRead::Filter
        inline constexpr char kRail[]   = "rail";     // 0 off, 1 on (the webtoon thumbnail rail)

        // RESERVED FOR INCREMENT 2 — declared so the two increments cannot pick different spellings, and
        // deliberately unread and unwritten by anything in this one.
        inline constexpr char kDemoire[]   = "demoire";
        inline constexpr char kDenoise[]   = "denoise";
        inline constexpr char kSharpen[]   = "sharpen";
        inline constexpr char kZoomStart[] = "zoomStart";
    }

    // THE KEY A COMIC'S SETTINGS ARE REMEMBERED UNDER. The document's <Series> when it has one (the same
    // folded key #152's direction override uses and the same one the shelf groups by), and otherwise the
    // FILENAME, through ComicName's parse: its series when the name carries one, its cleaned name when it
    // does not.
    //
    // WHY THE FILENAME FALLBACK IS ALLOWED HERE AND NOT IN THE SHELF. ComicName.h refuses to group a bare
    // trailing number without a corroborating sibling because a MISFILED comic is hidden inside somebody
    // else's series, where nobody will look. A shared reading MODE has no such failure: the worst case is
    // that two similarly named files open in the same mode, which is visible on screen and one press from
    // fixed. A comic that could remember nothing at all would be the worse outcome, because chapter 2 of a
    // webtoon would open paged every time.
    QString seriesKeyFor(const QString& comicInfoSeries, const QString& filePath);
}
