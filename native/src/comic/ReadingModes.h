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
// ---- INCREMENT 2, AND WHAT IS STILL NOT HERE --------------------------------------------------------------
//
// De-moire, denoise and sharpen (the scan-quality set) and the zoom-start position, under the option names
// increment 1 reserved. They are free functions over a QImage exactly as the colour filters are, they run
// where a page is already being prepared (ComicView::preparedPage, or the webtoon strip's decode worker) and
// NEVER on the paint path, and each is off by default.
//
// STILL NOT HERE AND NOT PLANNED: JOINING two consecutive single pages into one landscape spread (the inverse
// of the split). Increment 1 declared it not planned and this increment does not reopen it - whether the
// reader should ever do it is the issue owner's call. Also not here: panel-by-panel navigation, and OCR.
#pragma once
#include "ComicInfo.h"

#include <QHash>
#include <QImage>
#include <QPoint>
#include <QRect>
#include <QSet>
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

    // WHICH HALF A RESUME REOPENS ON (issue #285). entryHalf above answers for a page you WALKED into, and it
    // is the whole answer while you are reading: the direction you arrived from decides. A resume is not a
    // walk — it is a position that was recorded — so the half that was on screen when the comic was closed is
    // part of that position, and reading it back is what stops a spread left on its second half from reopening
    // on its first.
    //
    // IT IS HONOURED ONLY WHERE IT STILL MEANS SOMETHING. Whether a page splits depends on the viewport and on
    // the mode, and both can differ from the moment the position was written: the window was widened or
    // rotated, the override was set to Never, the series was switched to webtoon (where a page never splits at
    // all — see the webtoon note below; ComicView::pageSplits answers false for every non-paged mode). In each
    // of those the stored half names a screen that no longer exists, so the answer falls back to entryHalf's,
    // unchanged.
    //
    // A STORED VALUE THAT IS NOT 0 OR 1 IS "NONE" — the same forgiving read resolveMode makes of a hand-edited
    // ini — and that is also the OLD-SAVE case: a resume written before #285 carries no half, reads as
    // kNoStoredHalf, and therefore opens exactly where it opens today, in both directions. It is the promise
    // the stored fraction made when it was added, in the same words (ComicView::openComic).
    inline constexpr int kNoStoredHalf = -1;
    inline int resumeHalf(int storedHalf, bool splits, int dir)
    {
        if (splits && (storedHalf == 0 || storedHalf == 1)) return storedHalf;
        return entryHalf(splits, dir);
    }

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

    // ---- SCAN-QUALITY CORRECTIONS (increment 2) ------------------------------------------------------------
    // Three corrections for a page that was SCANNED rather than drawn. Each is off / on - not a slider - and
    // each is a 3x3 neighbourhood pass with REPLICATE padding at the border (the edge pixel is repeated), so
    // every one of them is total: a 1x1 page and a 2x2 page go through unharmed rather than being special
    // cases nobody tests.
    //
    //   * DE-MOIRE is the 3x3 BINOMIAL low-pass [1 2 1; 2 4 2; 1 2 1]/16, computed separably (a [1 2 1] pass
    //     along each row, then [1 2 1] down the accumulated rows) with the division done ONCE at the end, so
    //     the integer result is the exact 2D kernel's and not two roundings of it. That kernel is exactly a
    //     halftone killer: a screen alternating between two tones every pixel is the highest frequency an
    //     image can carry, and this kernel's response to it is their weighted mean everywhere - a black and
    //     white checkerboard comes out a flat 128. It is the mildest kernel that does that, which is why text
    //     a few pixels wide survives it; anything wider would be the mush the brief refuses.
    //   * DENOISE is a 3x3 CROSS MEDIAN - the median of the five pixels {left, centre, right, above, below}.
    //     A median is edge preserving by construction (at a step edge the majority side wins outright, so the
    //     edge stays exactly where it was), and it removes impulse noise exactly rather than smearing it: an
    //     isolated white or black speck is outvoted 4-to-1 and disappears. The CROSS rather than the full 3x3
    //     box is what makes it light - it reads five pixels instead of nine, costs six compare-exchanges
    //     instead of nineteen, and leaves a one-pixel diagonal (which a full median erases) standing.
    //   * SHARPEN is an UNSHARP MASK at kSharpenPercent: out = centre + (centre - blur) / 2, with `blur` the
    //     SAME binomial kernel de-moire uses - one low-pass, stated once, used in both directions. Computed
    //     as (3*centre - blur + 1) / 2 so the halving rounds rather than truncates, then clamped to 0..255.
    //
    // ORDER, WHERE THEY COMPOSE AND WHY (preparePage below states it again in code):
    //
    //     crop -> split -> DE-MOIRE -> DENOISE -> SHARPEN -> colour filter
    //
    // De-moire first, because it is the only one that removes a frequency and the other two should work on a
    // page that no longer has a halftone screen in it (a median over a screen preserves the screen - that is
    // what edge preserving means - and sharpening one puts it back louder). Denoise before sharpen, for the
    // one reason that matters: sharpening amplifies exactly what a denoise removes, so the other order ships
    // a page with its specks doubled. And all three BEFORE the colour filter, so the filter still sees a
    // finished page - the same reason increment 1 put the filter last.
    struct ScanFixes
    {
        bool demoire = false;
        bool denoise = false;
        bool sharpen = false;

        bool isIdentity() const { return !demoire && !denoise && !sharpen; }
        bool operator==(const ScanFixes& o) const
        { return demoire == o.demoire && denoise == o.denoise && sharpen == o.sharpen; }
        bool operator!=(const ScanFixes& o) const { return !(*this == o); }
    };

    inline constexpr int kSharpenPercent = 50;

    QImage demoire(const QImage& src);   // the binomial low-pass
    QImage denoise(const QImage& src);   // the cross median
    QImage sharpen(const QImage& src);   // the unsharp mask, at kSharpenPercent

    // All three in their stated order. An identity set returns `src` ITSELF - not a copy, not a converted
    // copy - so "off" is byte-identical to no scan fixes at all and costs nothing to have the option of.
    QImage applyScanFixes(const QImage& src, const ScanFixes& f);

    // ---- THE SCAN-FIX CONTROL: ONE LADDER, NOT THREE BUTTONS -----------------------------------------------
    // The three corrections are ONE control on both layouts, cycling a ladder of named presets, for two
    // reasons. The classic bar already carries five per-series buttons and three more would be nine in a row
    // on a phone-width window; and these three are not three independent questions - they are one ("is this a
    // scan of a printed page?"), whose useful answers are a short list. The ladder:
    //
    //     0 off   1 de-moire   2 denoise   3 sharpen   4 all three ("print")
    //
    // Each single correction is still reachable on its own, and the combination a scanned print actually
    // needs is one press from off. The three values are STORED SEPARATELY (kDemoire/kDenoise/kSharpen), so the
    // store holds the state and not the ladder position: a hand-edited ini can name any of the eight
    // combinations, the label then names what is on, and the next press goes to "off" (scanPresetIndex
    // answers -1 for a set the ladder does not name, and nextScanPreset restarts from there).
    inline constexpr int kScanPresetCount = 5;
    ScanFixes scanPreset(int index);              // an index outside 0..4 is "off"
    int       scanPresetIndex(const ScanFixes& f);// 0..4, or -1 for a combination the ladder does not name
    ScanFixes nextScanPreset(const ScanFixes& f);

    // ---- WHERE A ZOOMED PAGE OPENS (increment 2) -----------------------------------------------------------
    // A page zoomed past fit-to-width is bigger than the viewport, so opening it means CHOOSING a scroll
    // offset. Per series:
    //
    //   * Top          the top of the page, with the horizontal position LEFT WHERE IT WAS. That is exactly
    //                  what the reader has always done (showPage set the vertical bar to 0 and touched
    //                  nothing else), which is why it is the default and why a series with no stored opinion
    //                  reads as it.
    //   * Centre       the middle of the page in both axes - for a zoom that is about looking at the art.
    //   * ReadingSide  the top, at the edge the eye starts from: the LEFT in a left-to-right comic, the RIGHT
    //                  in a right-to-left one. In an LTR comic that x is 0, which is where an unscrolled bar
    //                  already is; the option earns its place in a manga, where the first panel of a zoomed
    //                  page is at the far end from where a scroll bar starts.
    //
    // `current` is where the scroll bars are NOW, and only Top reads it. Every answer is clamped into
    // [0, content - viewport] per axis, so a page SMALLER than the viewport gives (0, 0) for all three: there
    // is nowhere to scroll and no option can invent somewhere.
    //
    // WEBTOON IGNORES IT ENTIRELY (zoomStartApplies). The strip owns its own position - it is one continuous
    // scroll whose offset IS the resume position, and re-aiming it at a page boundary would fight both the
    // stored position and the reader's thumb.
    enum class ZoomStart
    {
        Top         = 0,   // the stored default: 0 is "no opinion" everywhere in this file
        Centre      = 1,
        ReadingSide = 2,
    };

    inline bool zoomStartApplies(Mode m) { return isPaged(m); }
    inline ZoomStart zoomStartFor(int stored)
    {
        return stored == int(ZoomStart::Centre)      ? ZoomStart::Centre
             : stored == int(ZoomStart::ReadingSide) ? ZoomStart::ReadingSide
                                                     : ZoomStart::Top;   // anything else is "never set"
    }
    QPoint zoomStartOffset(const QSize& content, const QSize& viewport, ZoomStart z, bool rtl,
                           const QPoint& current);

    // ---- THE WHOLE DISPLAY PIPELINE FOR ONE PAGE -----------------------------------------------------------
    // ORDER IS LOAD-BEARING: crop, then split, then the scan fixes, then the colour filter.
    //   * Crop BEFORE split, because the midpoint of a spread with a 5% white margin down one side is not the
    //     midpoint of the art - splitting first would cut one half short and pad the other.
    //   * The SCAN FIXES after both, so each runs over the pixels that will actually be shown rather than
    //     over a margin about to be trimmed or a half about to be thrown away - and so a 3x3 kernel's
    //     replicate-padded border is the border of the page on screen.
    //   * Filter LAST, because the crop's corner test reads the scan's own colours and a sepia tint would
    //     move them (and because tinting pixels that are about to be thrown away is wasted work).
    struct PageOptions
    {
        bool  crop = false;
        int   cropTolerance = kCropTolerance;
        int   half = -1;          // -1 whole page, 0 first half, 1 second half (see splitHalfRect)
        bool  rtl  = false;       // which side "first" is, when half >= 0
        ScanFixes   scan;         // increment 2: de-moire / denoise / sharpen, in that order
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

    // ---- WEBTOON: DECODING OFF THE PAINT PATH (issue #286) -------------------------------------------------
    // A jump from the thumbnail rail lands on a page nobody has decoded, and a webtoon page can be 800x12000.
    // The strip therefore never decodes while it paints: a page that is not ready is drawn as a placeholder of
    // its laid-out height, a worker decodes + prepares + scales it, and the GUI thread only turns the finished
    // QImage into a pixmap. These are the bookkeeping DECISIONS of that arrangement — which pages to ask a
    // worker for, whether a worker should still bother once it starts, and whether a finished page may be
    // drawn — as free functions over plain values, so probe_readingmodes asserts them and ComicView only
    // applies the answers.
    //
    // THE GENERATION is a counter ComicView bumps every time its strip cache is emptied (a width change, a
    // filter change, leaving the strip, opening something else). Every request carries the generation it was
    // made under, and an answer from an older generation describes pages that no longer exist on screen.

    // What a worker hands back, minus the image: which page, under which generation, at which strip width.
    struct StripResult
    {
        int     page = -1;
        quint64 generation = 0;
        int     width = 0;
    };

    // WHICH PAGES TO REQUEST, IN ORDER. `window` is the prefetch window in its priority order (prefetchWindow's:
    // the landed page first, then forward before back), and the answer keeps that order. A page is skipped when
    // it is already `cached`, or when it is already in flight UNDER THIS GENERATION — `inFlight` maps a page to
    // the generation its outstanding request was made under, so a request left over from before a cache clear
    // does not stop the page being asked for again at the new one.
    QVector<int> stripRequests(const QVector<int>& window, const QSet<int>& cached,
                               const QHash<int, quint64>& inFlight, quint64 generation);

    // MAY A FINISHED PAGE BE DRAWN. Only when its generation is the current one, its width is the strip's
    // current width, and its page is still inside the current window. Anything else is dropped rather than
    // cached — a page outside the window would be evicted again at the next scroll, so caching it would only
    // turn the window back into a leak.
    bool acceptStripResult(const StripResult& r, quint64 generation, int width, const QVector<int>& window);

    // THE WORKER'S START CHECK. A flick across 200 pages queues far more jobs than will ever be looked at, so a
    // job re-reads the live generation and window bounds when it actually starts, and does no work unless it
    // is still wanted. `windowFirst..windowLast` is the window's index range (prefetchWindow is contiguous).
    bool stripJobWanted(int page, quint64 jobGeneration, quint64 liveGeneration, int windowFirst, int windowLast);

    // THE WINDOW NEVER EXCLUDES A PAGE ON SCREEN. prefetchWindow's +/-3 is counted from the page at the TOP of
    // the viewport; a strip cut into pages shorter than a third of the viewport would put visible pages beyond
    // it, and those would stay placeholders because their results fall outside the window. stripLastVisible is
    // the last page intersecting [y, y + viewportH); stripWindow widens the radius just enough to reach it,
    // and is exactly prefetchWindow(current, count) whenever the pages are taller than that.
    int stripLastVisible(const Strip& s, int y, int viewportH);
    QVector<int> stripWindow(const Strip& s, int current, int lastVisible);

    // ---- THE READER'S OWN CONTROLS (#397) ------------------------------------------------------------------
    // The themed chrome host filters every pointer press inside the reader and, for a comic, turns it into the
    // tap-zone map. The comic has controls of its own inside that same widget — the webtoon thumbnail rail and
    // the scroll area's bars — and a press on one of those is not a page tap. Only the comic knows where they
    // are, so it answers the host's question with this, and the host never guesses by widget type.
    //
    // The rail shows only in the webtoon strip with the rail switched on; applyMode and the rail toggle show it
    // by this same rule, so "shown" and "owned" cannot disagree.
    inline bool railShown(bool webtoon, bool railOn) { return webtoon && railOn; }

    // Every rectangle is in the READER's coordinates. A control that is not shown owns nothing, whatever its
    // last geometry says: a hidden widget keeps its old rectangle.
    struct PointerControls
    {
        bool  railShown = false;
        QRect rail;
        bool  vBarShown = false;   // the scroll area's vertical bar (the strip's, in webtoon mode)
        QRect vBar;
        bool  hBarShown = false;   // ... and its horizontal one (a zoomed paged page)
        QRect hBar;
    };
    bool ownsPointerAt(const QPoint& p, const PointerControls& c);

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

        // INCREMENT 2, under the names increment 1 reserved. Each of the three corrections stores its own
        // 0/1 (the control is one ladder, the STATE is three flags - see nextScanPreset); zoomStart holds a
        // ComicRead::ZoomStart, with 0 = Top = the default, like every other option here.
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
