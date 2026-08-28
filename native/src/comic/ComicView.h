// Page-by-page comic reader for comic archives — CBZ/ZIP (miniz), CB7 (the vendored 7z/LZMA SDK behind
// SevenZip.h), and CBT (the in-tree Tar.h reader), each a container of page images. Collects the image
// entries, sorts them in natural page order, and shows one page at a time with fit-width / zoom and per-file
// resume - mirroring the PDF reader. (CBR still needs a RAR decoder; CBZ/CB7/CBT cover most comics.)
#pragma once
#include <QWidget>
#include <QVector>
#include <QByteArray>
#include <QImage>
#include <QString>
#include <QStringList>
#include <QtGlobal>
#include "../theme2/HostedReader.h"

class QScrollArea;
class QLabel;

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

    static bool isComicFile(const QString& path); // .cbz/.zip, .cb7, .cbt (archives of page images)

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

    // ---- Chapter neighbours (auto-advance) --------------------------------------------------------------
    // What sits either side of this comic in its series, as MainWindow knows it: a browsed manga chapter list,
    // or the other archives in this file's folder. The reader only REPORTS that a press fell off an end —
    // it has no AddonManager, no notifier and no idea what a chapter id is, so the crossing itself lives in
    // MainWindow. Never set for a photo folder (issue #102): a folder of holiday pictures is not a series.
    void setChapterNeighbours(bool hasPrev, bool hasNext) { hasPrevChapter_ = hasPrev; hasNextChapter_ = hasNext; }

    // Bookmarks (issue #136). A comic's stable natural key is its archive path — the same basis its resume
    // position hashes (comicKey() hashes exactly this), so one identity names both. A PHOTO folder returns
    // an empty key on purpose: photos carry no per-file resume and accrue no reading stats, and a folder of
    // holiday pictures is not a book you keep your place in — the chrome no-ops on an empty key, so the
    // bookmark controls stay inert there exactly as they do before a file is open.
    QString itemKey() const override { return photoMode_ ? QString() : path_; }
    // Jump to a 0-based page — the whole of a comic bookmark's anchor. showPage() is the one page-change
    // path (decode, rescale, label, stats, pageInfoChanged) and ignores an out-of-range index, so a
    // bookmark that outlived its file cannot land the reader on a page that isn't there.
    void gotoPage(int page0) override { showPage(page0); }

signals:
    void homeRequested();
    void backRequested(); // return to the previous screen (e.g. the chapter list) without resetting Home
    void pageInfoChanged(); // page/zoom/spread changed — hosted chrome refresh
    void chapterAdvanceRequested(int dir); // a page press fell off an end: +1 = past the last page, -1 = before the first
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
    bool loadCb7Pages(const QString& path, QVector<QByteArray>& pages, QString* error); // .cb7 via SevenZip → temp dir
    bool loadCbtPages(const QString& path, QVector<QByteArray>& pages, QString* error); // .cbt via the in-tree Tar reader
    void showPage(int index);
    void rescale();
    void updateLabel();
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
    bool photoMode_ = false;      // true after openFolder(): source is a folder of files, not a ZIP's entries
    bool hasPrevChapter_ = false; // a chapter/file exists before this one (set by MainWindow, cleared on every open)
    bool hasNextChapter_ = false; // ... and after it
    QStringList photoFiles_;      // photo mode: the folder's image files, natural order (comic mode: empty)
    QString path_;

    QWidget* bar_ = nullptr;   // the bottom control bar (hidden in hosted/themed mode)
    QScrollArea* scroll_ = nullptr;
    QLabel* imageLabel_ = nullptr;
    QLabel* pageLabel_ = nullptr;
};
