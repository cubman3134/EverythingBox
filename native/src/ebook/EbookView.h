// Paginated EPUB/MOBI/PDF reader. A chapter's HTML is laid into a QTextDocument that is paginated by line
// (QTextDocument page breaks never split a line), and exactly one page is painted - no scrolling, so the
// bottom line can never be clipped behind anything. Left/right clicks flip pages; a top-band click or any
// mouse movement reveals an auto-hiding menu; arrow keys page through. Mirrors the Unity ereader UX.
#pragma once
#include <QWidget>
#include <memory>
#include "EbookSource.h"

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
    qreal contentH() const { return qMax(1.0, qreal(height()) - topMargin_ - botMargin_); }
    int  lineIndexForPos(int pos) const;     // index into lines_ of the line containing a document offset
    int  lastFittingLine(int startLine) const; // last whole line that fits a page starting at startLine
    void snapTopToLine();      // pull topPos_ back to the start of its line
    void recomputeCurrentPage(); // curPage_ = which from-start page holds topPos_

    QTextDocument* doc_ = nullptr;
    QVector<LineGeom> lines_;  // every line in the chapter, in order
    QVector<int> pageTops_;    // start offset of each from-start page (footer numbering)
    int   topPos_ = 0;         // document offset of the first line shown
    int   curPage_ = 0;        // 0-based current page in the from-start grid
    int   fontPt_ = 14;
    qreal sideMargin_ = 40.0; // left/right paper margin
    qreal topMargin_  = 56.0; // clears the overlay menu so it never covers text
    qreal botMargin_  = 40.0; // leaves room for the page-number footer
    QString footer_;
};

class EbookView : public QWidget
{
    Q_OBJECT
public:
    explicit EbookView(QWidget* parent = nullptr);

    bool openBook(const QString& path, QString* error = nullptr);
    void persist(); // save reading position (called when navigating away)
    void setStreamIssueVisible(bool on); // show the "Issue with Streaming" button (remote/Allarr books only)

signals:
    void homeRequested();
    void backRequested(); // return to the previous screen (the catalog/list) without resetting Home
    void streamIssueRequested(); // user reports a bad file -> ask the provider for the next source

protected:
    void keyPressEvent(QKeyEvent*) override;
    void resizeEvent(QResizeEvent*) override;

private slots:
    void nextPage();
    void prevPage();
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
    static constexpr int kMenuHeight = 56; // overlay menu height; the page reserves this much up top
};
