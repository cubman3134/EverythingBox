// Page-by-page comic reader for CBZ archives (a ZIP of page images). Opens the archive with miniz, sorts
// the image entries in natural page order, and shows one page at a time with fit-width / zoom and per-file
// resume - mirroring the PDF reader. (CBR/CB7/CBT would need a RAR/7z/tar decoder; CBZ covers most comics.)
#pragma once
#include <QWidget>
#include <QVector>
#include <QByteArray>
#include <QImage>
#include <QString>
#include "../theme2/HostedReader.h"

class QScrollArea;
class QLabel;

class ComicView : public QWidget, public HostedReader
{
    Q_OBJECT
public:
    explicit ComicView(QWidget* parent = nullptr);

    bool openComic(const QString& path, QString* error = nullptr);
    void persist(); // save the current page (called when navigating away)
    void setStreamIssueVisible(bool) {} // no-op stub: a comic has no remote-source swap (chrome uniformity)

    static bool isComicFile(const QString& path); // .cbz (and plain .zip of images)

    // ---- Hosted mode (themed reader chrome, Plan B1 Task 4) ----------------------------------------------
    // Mirrors EbookView/PdfView: setHostedChrome(true) hides the reader's own bottom control bar so the themed
    // ReaderChromeHost strips drive everything through the thin wrappers below (ZERO render/scroll change — the
    // wrappers wrap what the buttons already call). A comic's settings are zoom in/out + fit + a two-up (double-
    // page spread) toggle; no font, no toc. pageInfoChanged() mirrors page/zoom/spread moves into the chrome.
    QWidget* asWidget() override { return this; }
    void setHostedChrome(bool on) override;
    int  currentPage() const override { return current_ + 1; } // 1-based (leftmost page of the current spread)
    int  pageCount()  const override { return qMax(1, int(pages_.size())); }
    int  chromeTopReserve() const override { return 38; } // themed top strip height (no reserved page inset)
    void zoomDelta(int steps) override;  // + = zoom in, - = zoom out (per step, matching the +/- buttons)
    void fitWidth() override;
    void setTwoUp(bool on) override;     // enable/disable the double-page spread preference
    bool twoUp() const override { return twoUpEnabled_; }

signals:
    void homeRequested();
    void backRequested(); // return to the previous screen (e.g. the chapter list) without resetting Home
    void pageInfoChanged(); // page/zoom/spread changed — hosted chrome refresh

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
    void showPage(int index);
    void rescale();
    void updateLabel();

    bool spreadActive() const override; // currently showing two pages side by side (HostedReader: themed label range)

    QVector<QByteArray> pages_; // each entry = one page's encoded image bytes (jpg/png/…)
    int current_ = 0;
    QImage image_;             // the decoded current page
    qreal zoom_ = 1.0;
    bool fit_ = true;          // fit-to-width vs. manual zoom
    bool twoUp_ = false;       // viewport is wide enough to pair pages book-style (set during rescale)
    bool twoUpEnabled_ = true; // user preference: allow the spread (default on = the prior auto behaviour)
    bool hosted_ = false;
    QString path_;

    QWidget* bar_ = nullptr;   // the bottom control bar (hidden in hosted/themed mode)
    QScrollArea* scroll_ = nullptr;
    QLabel* imageLabel_ = nullptr;
    QLabel* pageLabel_ = nullptr;
};
