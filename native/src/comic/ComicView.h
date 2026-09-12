// Page-by-page comic reader for comic archives — CBZ/ZIP (miniz), CB7 (the vendored 7z/LZMA SDK behind
// SevenZip.h), CBT (the in-tree Tar.h reader) and CBR (the vendored unarr RAR decoder behind RarComic.h),
// each a container of page images. Collects the image entries, sorts them in natural page order, and shows
// one page at a time with fit-width / zoom and per-file resume - mirroring the PDF reader.
#pragma once
#include <QWidget>
#include <QVector>
#include <QHash>
#include <QPixmap>
#include <QByteArray>
#include <QImage>
#include <QString>
#include <QStringList>
#include <QtGlobal>
#include "ReadingModes.h"          // #154: the mode, the split/crop/filter rules, the webtoon strip
#include "../theme2/HostedReader.h"

class QScrollArea;
class QLabel;
class QPushButton;
class QVBoxLayout;
// #154: the two surfaces webtoon mode adds — the continuous strip and its thumbnail rail. Both are plain
// QWidgets defined in ComicView.cpp: they carry no signals of their own, are never named outside it and so
// need no header and no moc. They live INSIDE the reader rather than in the themed chrome on purpose —
// ComicView is the same widget under both layouts, so one implementation is on both, where a themed NavGraph
// zone would have been on the themed layout only.
class ComicStripWidget;
class ComicRailWidget;

// Fit-to-PAGE scale for a two-page (open-book) spread: the SMALLER of fit-to-width and fit-to-height, so the
// whole spread stays visible in BOTH dimensions. Pure arithmetic — no widgets, no Qt objects — so it is unit-
// tested directly by probe_comicfit. `totalW` is the combined width of the two pages plus the gap between them;
// `commonH` is the height they share after being normalised. Fitting to WIDTH alone (the pre-fix behaviour)
// let two portrait pages form a spread whose scaled height overflowed the viewport, clipping the bottom off the
// default view; clamping by viewportH/commonH keeps the scaled height (commonH*scale) within viewportH.
inline double comicSpreadScale(int viewportW, int viewportH, int totalW, int commonH)
{
    return qMin(double(viewportW) / double(qMax(1, totalW)),
                double(viewportH) / double(qMax(1, commonH)));
}

// The two page-boundary questions, pulled out of nextPage()/prevPage() so the decision that used to be a
// silent early return is a named, unit-tested rule (probe_comicfit). Each is the exact condition under which
// the press has nowhere left to go inside THIS comic — and is therefore the moment to ask for the next one.
inline bool comicPastEnd(int current, int pageTotal) { return current >= pageTotal - 1; }
inline bool comicBeforeStart(int current)            { return current <= 0; }

class ComicView : public QWidget, public HostedReader
{
    Q_OBJECT
public:
    explicit ComicView(QWidget* parent = nullptr);

    bool openComic(const QString& path, QString* error = nullptr);

    // Photo mode (issue #102): reuse this exact render/page/zoom widget over a plain FOLDER of image files
    // instead of a CBZ's ZIP entries. Pages through the folder's images in natural order (PhotoLibrary::
    // imagesInFolder); startFile, when given, opens on that image (open-a-JPEG lands on the one you picked).
    // Photos are decoded with EXIF auto-transform, are never paired book-style (two-up is a comic notion),
    // and carry no per-file resume. The comic path is unaffected — photoMode_ is false for every openComic().
    bool openFolder(const QString& folder, const QString& startFile = QString(), QString* error = nullptr);

    void persist(); // save the current page (called when navigating away)
    void setStreamIssueVisible(bool) {} // no-op stub: a comic has no remote-source swap (chrome uniformity)

    static bool isComicFile(const QString& path); // .cbz/.zip, .cb7, .cbt, .cbr (archives of page images)

    // WHICH WAY THIS COMIC READS (issue #152). Set by openComic() from the archive's own ComicInfo.xml
    // (<Manga>YesAndRightToLeft</Manga>) under the user's per-series override, and false for every comic
    // that says nothing — which is every comic that opened before this existed, so the reader's behaviour is
    // unchanged for all of them. Right to left swaps TWO things and nothing else: which arrow key advances,
    // and which side of an open-book spread each page is drawn on. The PAGE ORDER is untouched — a manga's
    // pages are numbered in reading order by the people who scanned it, and reversing them here would undo
    // that (and put the cover last).
    bool rightToLeft() const { return rtl_; }

    // ---- READING MODES (issue #154) -------------------------------------------------------------------------
    // The mode this comic is being read in, and the five per-series controls the reader offers over it. Each
    // cycle writes the new value to the series' store and applies it to the page on screen immediately — there
    // is no apply step and no dialog, because every one of them is a thing you judge by looking at the page.
    //
    // WHY THESE ARE ON ComicView AND NOT IN GENERAL SETTINGS. They are PER SERIES, and a per-series control
    // belongs where the series is: in front of you, while you read it. The one global reading surface (#147's)
    // holds the settings that are the same for every book you own; these are not. Both layouts reach the same
    // five methods — the classic bar's buttons below and the themed chrome's control row through
    // comicControlLabels()/comicActivateControl() — so the feature exists once and is on both.
    ComicRead::Mode readingMode() const { return mode_; }
    void cycleReadingMode();     // paged L2R -> paged R2L -> webtoon -> paged L2R
    void cycleSplitOverride();   // auto -> always -> never -> auto
    void toggleBorderCrop();
    void cycleColorFilter();     // none -> greyscale -> sepia -> night -> high contrast -> none
    void toggleThumbnailRail();  // webtoon only; turning it on also gives it the reader's key cursor

    // The themed chrome's extra comic controls, as labels + "is it on" + one activation by index. ONE generic
    // trio rather than ten typed virtuals: the QML row appends one entry per label and fires back the index it
    // drew, so increment 2's scan-quality controls are a label and a case, not a signature change in three files.
    QStringList   comicControlLabels() const override;
    QVector<bool> comicControlActive() const override;
    void          comicActivateControl(int index) override;
    QString       pageLabelNote() const override;   // "(approx)" in the strip, "half 1 of 2" over a split page

    // ---- Hosted mode (themed reader chrome, Plan B1 Task 4) ----------------------------------------------
    // Mirrors EbookView/PdfView: setHostedChrome(true) hides the reader's own bottom control bar so the themed
    // ReaderChromeHost strips drive everything through the thin wrappers below (ZERO render/scroll change — the
    // wrappers wrap what the buttons already call). A comic's settings are zoom in/out + fit + a two-up (double-
    // page spread) toggle; no font, no toc. pageInfoChanged() mirrors page/zoom/spread moves into the chrome.
    QWidget* asWidget() override { return this; }
    void setHostedChrome(bool on) override;
    int  currentPage() const override { return current_ + 1; } // 1-based (leftmost page of the current spread)
    int  pageCount()  const override { return qMax(1, pageTotal()); }
    int  chromeTopReserve() const override { return 38; } // themed top strip height (no reserved page inset)
    void zoomDelta(int steps) override;  // + = zoom in, - = zoom out (per step, matching the +/- buttons)
    void fitWidth() override;
    void setTwoUp(bool on) override;     // enable/disable the double-page spread preference
    bool twoUp() const override { return twoUpEnabled_; }

    // Bookmarks (issue #136). A comic's stable natural key is its archive path — the same basis its resume
    // position hashes (comicKey() hashes exactly this), so one identity names both. A PHOTO folder returns
    // an empty key on purpose: photos carry no per-file resume and accrue no reading stats, and a folder of
    // holiday pictures is not a book you keep your place in — the chrome no-ops on an empty key, so the
    // bookmark controls stay inert there exactly as they do before a file is open.
    QString itemKey() const override { return photoMode_ ? QString() : path_; }
    // Jump to a 0-based page — the whole of a comic bookmark's anchor. showPage() is the one page-change
    // path (decode, rescale, label, stats, pageInfoChanged) and ignores an out-of-range index, so a
    // bookmark that outlived its file cannot land the reader on a page that isn't there.
    // #285: a jump is a move, so it spends the resume's half — a bookmark that lands on a spread opens on the
    // half the reading direction gives it, not on the one some earlier close happened to leave in the store.
    void gotoPage(int page0) override { resumeHalf_ = ComicRead::kNoStoredHalf; showPage(page0); }

signals:
    void homeRequested();
    void backRequested(); // return to the previous screen (e.g. the chapter list) without resetting Home
    // Discord presence: what is open and how far in. Emitted at the same page-turn edge the consumption
    // accrual uses, so reading needs no new timer and no new bookkeeping of its own.
    void readingProgress(const QString& title, const QString& subtitle);
    void pageInfoChanged(); // page/zoom/spread changed — hosted chrome refresh
    // A page press fell off an end: +1 = past the last page, -1 = before the first. Emitted UNCONDITIONALLY —
    // the reader reports the boundary and nothing else. It has no AddonManager, no notifier and no idea what a
    // chapter id is, so whether a neighbour exists (and what to say when it does not) is MainWindow's to know;
    // a comic with no run there is silent, exactly as this press always was. MediaPane's own ComicView never
    // connects this, so the split pane's boundary presses stay the inert no-op they have always been.
    void chapterAdvanceRequested(int dir);
    void reachedLastPage();                // the last page is now on screen (hint that another chapter follows)

public slots:
    void nextPage() override;
    void prevPage() override;

protected:
    void keyPressEvent(QKeyEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void showEvent(QShowEvent*) override;

private slots:
    void zoomIn();
    void zoomOut();

private:
    // `comicInfoXml`, when given, is filled with the archive's root ComicInfo.xml (#152) — out of the temp
    // dir this branch has to create anyway, so the document costs nothing on top of the extraction and the
    // archive is never decoded a second time to reach it.
    bool loadCb7Pages(const QString& path, QVector<QByteArray>& pages, QString* error,
                      QByteArray* comicInfoXml = nullptr); // .cb7 via SevenZip → temp dir
    bool loadCbtPages(const QString& path, QVector<QByteArray>& pages, QString* error); // .cbt via the in-tree Tar reader
    bool loadCbrPages(const QString& path, QVector<QByteArray>& pages, QString* error); // .cbr via unarr (RarComic.h)
    // `dir` is which way the reader ARRIVED, and it decides one thing only: which half of a page that splits
    // is on screen (#154 — arriving backwards shows the second half first). +1 for every caller that is not
    // prevPage(), which is what it always was.
    void showPage(int index, int dir = +1);
    void rescale();
    void updateLabel();

    // ---- #154 internals -------------------------------------------------------------------------------------
    void installModeControls();         // the classic bar's five per-series buttons
    void installModeSurfaces(QVBoxLayout* column);   // the strip + the rail, as one row beside the scroll area
    void readDisplayOptions();          // load this comic's per-series options out of the store
    void writeOption(const char* option, int value);
    void applyMode();                   // swap the scroll area between the paged label and the webtoon strip
    void rebuildStrip();                // (re)compute the strip layout for the current viewport width
    void relayoutStrip();               // rebuild AND land back on the same reading position
    void scrollToPosition(int page, double fraction);
    void scrollByViewport(int dir);     // webtoon: Up/Down, a viewport fraction at a time
    void railStep(int delta);
    void setRailFocus(bool on);   // (the rail is an incomplete type in ComicView.cpp)
    void onStripScrolled();             // the scroll bar moved: which page is that, and what to prefetch
    void prefetchAround(int page);
    void currentPosition(int* page, double* fraction) const;   // where the reader is, in resume terms
    bool pageSplits(int index) const;   // does page `index` split, under the override and this viewport
    QSize rawPageSize(int index) const; // the page's own pixel size, from its header — no decode
    QImage preparedPage(int index, int half) const;            // decode + crop + split + filter
    QPixmap stripPixmap(int index);     // webtoon: the prepared page scaled to the strip's width (cached)
    QPixmap railThumb(int index);       // webtoon: a small thumbnail for the rail (cached)
    void updateBarButtons();            // relabel the classic bar's five per-series buttons
    ComicRead::PageOptions optionsFor(int half) const;

    friend class ComicStripWidget;
    friend class ComicRailWidget;
    int  pageTotal() const;         // pages_.size() in comic mode, photoFiles_.size() in photo mode
    QImage decodeAt(int index) const; // decode page/photo bytes (EXIF auto-transform when in photo mode)

    bool spreadActive() const override; // currently showing two pages side by side (HostedReader: themed label range)

    QVector<QByteArray> pages_; // each entry = one page's encoded image bytes (jpg/png/…)
    int current_ = 0;
    QImage image_;             // the decoded current page
    qreal zoom_ = 1.0;
    bool fit_ = true;          // fit-to-width vs. manual zoom
    bool twoUp_ = false;       // viewport is wide enough to pair pages book-style (set during rescale)
    bool twoUpEnabled_ = true; // user preference: allow the spread (default on = the prior auto behaviour)
    bool hosted_ = false;
    bool rtl_ = false;            // #152: this comic reads right to left (ComicInfo <Manga>, user override wins)
    bool photoMode_ = false;      // true after openFolder(): source is a folder of files, not a ZIP's entries
    QStringList photoFiles_;      // photo mode: the folder's image files, natural order (comic mode: empty)
    QString path_;

    // ---- #154 state -----------------------------------------------------------------------------------------
    // All five are per SERIES, read at open and written the moment one is changed. seriesKey_ empty means this
    // comic has nothing to remember settings under (a photo folder), and every write is then a no-op.
    ComicRead::Mode   mode_   = ComicRead::Mode::PagedLtr;
    ComicRead::Split  split_  = ComicRead::Split::Auto;
    ComicRead::Filter filter_ = ComicRead::Filter::None;
    bool crop_ = false;
    bool railOn_ = false;
    QString seriesKey_;
    int  half_ = -1;              // paged split: -1 whole page, 0 first half on screen, 1 second half
    // #285: the half this comic's stored resume recorded, held from open until the reader navigates away from
    // it — the same shape resumeFraction_ has for the webtoon strip. kNoStoredHalf means the resume names no
    // half, which is every resume written before #285 and every one closed on a page that was not split.
    int  resumeHalf_ = ComicRead::kNoStoredHalf;
    QVector<QSize> pageSizes_;    // every page's own size, read from its HEADER at open (no decode)
    ComicRead::Strip strip_;      // webtoon: where each page starts in the strip, for the current width
    QHash<int, QPixmap> stripCache_;  // webtoon: prepared pages at strip width, held to the prefetch window
    QHash<int, QPixmap> railCache_;   // webtoon: rail thumbnails (small; kept for the whole comic)
    int  stripCacheWidth_ = 0;    // the width stripCache_ was built at — a resize invalidates it whole
    bool railFocus_ = false;      // the rail holds the key cursor (Up/Down step it, Enter jumps)
    int  railIndex_ = 0;
    double resumeFraction_ = 0.0; // webtoon: the stored fraction into the resume page, until it is applied
    bool inScrollUpdate_ = false; // guards the scrollbar -> current_ -> scrollbar loop

    QWidget* bar_ = nullptr;   // the bottom control bar (hidden in hosted/themed mode)
    QScrollArea* scroll_ = nullptr;
    QLabel* imageLabel_ = nullptr;
    QLabel* pageLabel_ = nullptr;
    ComicStripWidget* stripWidget_ = nullptr;   // #154: the continuous webtoon strip (scroll_'s widget there)
    ComicRailWidget*  railWidget_  = nullptr;   // #154: the thumbnail rail beside it
    QPushButton* modeBtn_ = nullptr;            // the classic bar's five per-series controls
    QPushButton* splitBtn_ = nullptr;
    QPushButton* cropBtn_ = nullptr;
    QPushButton* filterBtn_ = nullptr;
    QPushButton* railBtn_ = nullptr;
};
