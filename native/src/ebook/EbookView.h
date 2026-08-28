// Paginated EPUB/MOBI/PDF reader. A chapter's HTML is laid into a QTextDocument that is paginated by line
// (QTextDocument page breaks never split a line), and exactly one page is painted - no scrolling, so the
// bottom line can never be clipped behind anything. Left/right clicks flip pages; a top-band click or any
// mouse movement reveals an auto-hiding menu; arrow keys page through. Mirrors the Unity ereader UX.
#pragma once
#include <QWidget>
#include <memory>
#include "EbookSource.h"
#include "ReaderTypography.h"
#include "../theme2/HostedReader.h"

class QListWidget;
class QLabel;
class QFrame;
class QPushButton;
class QTimer;
class QTextDocument;

// Renders one page of a chapter and turns clicks into page/menu requests. The owner (EbookView) drives
// chapter flow; this widget only knows how to paginate and paint the chapter it was given.
//
// Pages flow from a "top" text offset rather than an absolute page grid: the page always begins exactly at
// the first character of topPos_'s line and shows as many whole lines as fit. So the first word never moves
// when the window resizes or the font changes - we just re-find the line for the same offset. A separate
// from-the-start walk yields the "page x / y" counts for the footer.
class BookPageWidget : public QWidget
{
    Q_OBJECT
public:
    explicit BookPageWidget(QWidget* parent = nullptr);

    void setContent(const QString& html, const QString& baseDir); // baseDir resolves relative images
    void setFontPointSize(int pt);
    // Reader typography (issue #135): apply the resolved font family + size, line spacing, page margin,
    // justification and reading-theme colours in one pass. Font size still flows through setFontPointSize for
    // the in-reader A+/A− stepper; this is the fuller apply the settings surface drives. topPos_ is untouched
    // here (the caller captures/restores it around the reflow), so the same words stay at the top of the page.
    void setTypography(const ReaderTypography::Resolved& r);
    void setTopInset(int px);          // reserve space up top so the menu bar overlays margin, not text
    void setFooter(const QString& s);  // small centered line painted in the bottom margin (page x / y)

    // Page count this chapter's HTML would paginate to at the current geometry/font, without disturbing the
    // live view - used to total a book's pages across chapters.
    int  countPages(const QString& html, const QString& baseDir) const;

    int  pageCount() const { return qMax(1, int(pageTops_.size())); }
    int  currentPage() const { return curPage_; } // 0-based, for the from-start grid (footer only)
    bool atFirst() const;
    bool atLast()  const;
    void showFirstPage();
    void showLastPage();
    bool pageForward();   // advance one page by whole lines; false if already at the chapter's end
    bool pageBackward();  // retreat one page by whole lines; false if already at the chapter's start

    // Legacy resume: map an old saved page fraction onto a top offset.
    void setProgress(double f);

    // Reading position as a document character offset - stable across repagination (resize / font change),
    // unlike a page index. topTextPosition() is the first character on the current page.
    int  topTextPosition() const { return topPos_; }
    void scrollToTextPosition(int pos);

signals:
    void prevRequested();   // left half clicked
    void nextRequested();   // right half clicked
    void menuRequested();   // top band clicked, or the mouse moved
    void anchorClicked(const QString& href); // an in-book hyperlink under the click
    void layoutChanged();   // repaginated (resize / font change): page count may have changed

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;

private:
    struct LineGeom { qreal y; qreal h; int pos; }; // document-space top, height, and start offset of a line

    void relayout();           // re-lay the document and rebuild lines_/pageTops_, keeping topPos_'s line
    void rebuildLines();       // flatten the laid-out document into lines_
    void buildPageTops();      // walk lines_ from the start into whole-line pages (for the x / y count)
    void applyDocFormatting(); // (re)apply line spacing + justification to the current document (#135)
    qreal contentH() const { return qMax(1.0, qreal(height()) - topMargin_ - botMargin_); }
    qreal sideMargin() const;  // left/right paper margin in px, derived from marginPct_ and the current width
    qreal contentW() const;    // text column width (fills the available width)
    qreal contentLeft() const; // left edge of the text column
    int  lineIndexForPos(int pos) const;     // index into lines_ of the line containing a document offset
    int  lastFittingLine(int startLine) const; // last whole line that fits a page starting at startLine
    qreal anchorXInLine() const; // x-shift so the anchored word starts the first line (0 if at line start)
    void recomputeCurrentPage(); // curPage_ = which from-start page holds topPos_

    QTextDocument* doc_ = nullptr;
    QVector<LineGeom> lines_;  // every line in the chapter, in order
    QVector<int> pageTops_;    // start offset of each from-start page (footer numbering)
    int   topPos_ = 0;         // document offset of the first line shown
    int   curPage_ = 0;        // 0-based current page in the from-start grid
    int   fontPt_ = 14;
    QString fontFamily_;          // "" => the document's own default family (no override) (#135)
    int   lineSpacingPct_ = 100;  // line height as a % of natural leading (#135)
    int   marginPct_ = 6;         // left/right paper margin as a % of the page width (#135)
    bool  justify_ = false;       // justify paragraphs vs. ragged-right (#135)
    qreal topMargin_  = 56.0; // clears the overlay menu so it never covers text
    qreal botMargin_  = 40.0; // leaves room for the page-number footer
    QString footer_;
};

class EbookView : public QWidget, public HostedReader
{
    Q_OBJECT
public:
    explicit EbookView(QWidget* parent = nullptr);

    bool openBook(const QString& path, QString* error = nullptr);
    void persist(); // save reading position (called when navigating away)
    void setStreamIssueVisible(bool on); // show the "Issue with Streaming" button (remote/Allarr books only)

    // HostedReader (themed chrome host, Plan B1): the book is the reader widget; font + toc are its settings.
    QWidget* asWidget() override { return this; }
    int chromeTopReserve() const override { return kMenuHeight; }

    // ---- Hosted mode (themed reader chrome, Plan B1 Task 3) ----------------------------------------------
    // In hosted mode the reader renders page text ONLY: its own auto-hiding widget menu (menu_), contents
    // panel (tocList_) and stream-issue button are suppressed, and revealMenu() is a no-op — the surrounding
    // themed chrome (ReaderChromeHost) drives everything through the thin wrappers below. Pagination logic is
    // untouched: these wrappers just expose what the widget menu's buttons already call.
    void setHostedChrome(bool on) override;
    bool isHostedChrome() const { return hosted_; }

    int  currentPage() const override;   // 1-based page within the whole book at the current spot (globalPage)
    int  pageCount()  const override;    // book-wide page total (falls back to the current chapter's count)
    QStringList tocTitles() const override;   // chapter/section titles, in spine order (for the themed toc zone)
    int  fontPt() const override { return fontPt_; }
    // Bookmarks (issue #136): a book anchor is spine (chapter) + character offset, and jumping restores both.
    QString itemKey() const override;                    // the book's stable natural key (its source path)
    int  spineIndex() const override { return chapter_; }
    int  textOffset() const override;                    // the current top character offset (repagination-stable)
    void gotoSpineOffset(int spine, int offset) override;
    // Jump to a 0-based BOOK-WIDE page (the scale the themed chrome's progress bar draws): find the chapter
    // that page falls in, load it if we are not already there, and land on the page inside it.
    void gotoPage(int page0) override;
    // The height (px) the page reserves up top for chrome — the reader inset the widget menu used, which the
    // themed top strip must match so page text sits below it, not under it (spike constraint reconfirmation).
    static int topChromeReserve() { return kMenuHeight; }

public slots:
    void nextPage() override;   // advance one page (crossing into the next chapter at a chapter end)
    void prevPage() override;   // retreat one page (crossing into the previous chapter at a chapter start)
    void fontDelta(int dPt) override; // change the reading font by dPt points (8..40), keeping the reading spot
    void gotoTocIndex(int i) override;   // jump to the i-th toc entry's chapter
    // Re-read the stored reader typography (font/size/spacing/margin/justify/theme) and apply it live, keeping
    // the reader on the same words across the reflow. Called when a Settings ▸ Reading row changes (#135).
    void applyReaderTypography();
    void reloadTypography() override { applyReaderTypography(); }   // HostedReader: the themed chrome's hook

signals:
    void homeRequested();
    void backRequested(); // return to the previous screen (the catalog/list) without resetting Home
    void streamIssueRequested(); // user reports a bad file -> ask the provider for the next source
    void pageInfoChanged(); // page/chapter/font changed (paged, repaginated, resumed) — hosted chrome refresh

protected:
    void keyPressEvent(QKeyEvent*) override;
    void resizeEvent(QResizeEvent*) override;

private slots:
    void biggerFont();
    void smallerFont();
    void toggleContents();
    void onTocActivated();
    void onAnchorClicked(const QString& href); // follow an in-book hyperlink to its chapter
    void revealMenu();      // show the top menu and (re)arm its auto-hide timer
    void hideMenuIfIdle();  // timer tick: hide the menu unless the pointer is on it
    void updatePageLabel();

private:
    void loadChapter(int index, bool toLast = false);
    void restoreState();
    void layoutOverlays();
    void recomputeBookPages(); // tally each chapter's page count for a book-wide "page x / y"
    int  globalPage() const;   // 1-based page within the whole book at the current spot

    std::unique_ptr<EbookSource> book_; // EpubBook / MobiBook / PdfTextBook, chosen by file content
    BookPageWidget* page_ = nullptr;
    QFrame* menu_ = nullptr;            // auto-hiding top control bar (overlay)
    QPushButton* streamIssueBtn_ = nullptr; // "Issue with Streaming" (hidden unless a remote book)
    QListWidget* tocList_ = nullptr;    // contents panel (overlay, toggled)
    QLabel* pageLabel_ = nullptr;
    QTimer* menuTimer_ = nullptr;
    QTimer* repagTimer_ = nullptr;      // debounces book-wide repagination after a resize
    QVector<int> chapterStart_;         // cumulative page offset where each chapter begins
    int totalPages_ = 0;                // book-wide page total at the current font/size
    int chapter_ = -1;
    int fontPt_ = 14;
    bool streamVisible_ = false;
    int    restorePos_ = -1;       // document offset to resume at once the chapter is laid out (-1 = none)
    double restoreFrac_ = -1.0;    // legacy fallback: page fraction from older saves (-1 = none)
    bool   hosted_ = false;        // hosted mode: themed chrome drives us; suppress our own menu/toc/reveal
    static constexpr int kMenuHeight = 38; // overlay menu height; the page reserves this much up top
};
