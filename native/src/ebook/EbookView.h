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
class BookPageWidget : public QWidget
{
    Q_OBJECT
public:
    explicit BookPageWidget(QWidget* parent = nullptr);

    void setContent(const QString& html, const QString& baseDir); // baseDir resolves relative images
    void setFontPointSize(int pt);

    int  pageCount() const { return pageCount_; }
    int  currentPage() const { return page_; }
    bool atFirst() const { return page_ <= 0; }
    bool atLast()  const { return page_ >= pageCount_ - 1; }
    void setCurrentPage(int p);
    void showFirstPage() { setCurrentPage(0); }
    void showLastPage()  { setCurrentPage(pageCount_ - 1); }

    double progress() const { return pageCount_ > 1 ? double(page_) / (pageCount_ - 1) : 0.0; }
    void   setProgress(double f) { setCurrentPage(pageCount_ > 1 ? int(f * (pageCount_ - 1) + 0.5) : 0); }

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
    void relayout();

    QTextDocument* doc_ = nullptr;
    int   page_ = 0;
    int   pageCount_ = 1;
    int   fontPt_ = 14;
    qreal margin_ = 40.0; // page inset (paper margin) on every side
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

    std::unique_ptr<EbookSource> book_; // EpubBook / MobiBook / PdfTextBook, chosen by file content
    BookPageWidget* page_ = nullptr;
    QFrame* menu_ = nullptr;            // auto-hiding top control bar (overlay)
    QPushButton* streamIssueBtn_ = nullptr; // "Issue with Streaming" (hidden unless a remote book)
    QListWidget* tocList_ = nullptr;    // contents panel (overlay, toggled)
    QLabel* pageLabel_ = nullptr;
    QTimer* menuTimer_ = nullptr;
    int chapter_ = -1;
    int fontPt_ = 14;
    bool streamVisible_ = false;
    double restoreProgress_ = -1.0; // page fraction to resume at once the chapter is laid out
};
