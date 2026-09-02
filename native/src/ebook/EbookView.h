// Paginated EPUB/MOBI/PDF reader. A chapter's HTML is laid into a QTextDocument that is paginated by line
// (QTextDocument page breaks never split a line), and exactly one page is painted - no scrolling, so the
// bottom line can never be clipped behind anything. Left/right clicks flip pages; a top-band click or any
// mouse movement reveals an auto-hiding menu; arrow keys page through. Mirrors the Unity ereader UX.
#pragma once
#include <QWidget>
#include <memory>
#include "EbookSource.h"
#include "ReaderTypography.h"
#include "ReadAloudTarget.h"
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

    // ---- Read aloud (issue #145) --------------------------------------------------------------------------
    // The chapter's text in the SAME document coordinates topTextPosition/scrollToTextPosition speak: the
    // divider's offsets are therefore reader offsets, and no translation layer sits between what is spoken and
    // where the reader is.
    QString plainText() const;
    // Paint a highlight behind [start, end). An empty or inverted range clears it. Purely visual - the
    // position is moved by ensurePosVisible below, never by this.
    void setSpokenRange(int start, int end);
    // Turn the page if `pos` is not on the one being shown, landing on the line that holds it. A no-op when it
    // is already visible, so narration inside a page does not shuffle the text under the reader's eye.
    void ensurePosVisible(int pos);

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
    int   spokenStart_ = -1;   // the spoken paragraph's highlight range; -1 = nothing is being narrated (#145)
    int   spokenEnd_   = -1;
    qreal topMargin_  = 56.0; // clears the overlay menu so it never covers text
    qreal botMargin_  = 40.0; // leaves room for the page-number footer
    QString footer_;
};

class EbookView : public QWidget, public HostedReader, public ReadAloudTarget
{
    Q_OBJECT
public:
    explicit EbookView(QWidget* parent = nullptr);

    bool openBook(const QString& path, QString* error = nullptr);
    // Save the reading position (called when navigating away). posOverride >= 0 records THAT document offset
    // instead of the top of the page - how read-aloud makes the spoken paragraph the reading position without
    // re-anchoring a page whose text has not needed to move (issue #145).
    void persist(int posOverride = -1);
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

    // ---- Read aloud (issue #145) ---------------------------------------------------------------------------
    // The HostedReader commands the themed chrome fires and the classic bar's buttons call - one set, both
    // layouts. All of them are inert (and none of the controls are drawn) unless the build has the Qt
    // TextToSpeech module AND the platform offers an engine, which is what readAloudAvailable() answers.
    bool readAloudAvailable() const override;
    bool readAloudActive() const override;
    bool readAloudPaused() const override;
    void toggleReadAloud() override;
    void readAloudTogglePause() override;
    void readAloudSkip(int paragraphs) override;
    double readAloudSpeed() const override;
    void readAloudCycleSpeed() override;
    QString readAloudVoiceName() const override;
    void readAloudCycleVoice() override;

    // ReadAloudTarget - the narrow seam the controller drives. The position contract lives in ReadAloudTarget.h:
    // raShowSpoken is the reader ARRIVING at the spoken paragraph (page turned, highlighted, position persisted
    // through the same store a page turn writes), so the spoken paragraph IS the reading position.
    QString raChapterText() const override;
    int  raChapterIndex() const override { return chapter_; }
    int  raChapterCount() const override;
    bool raGotoChapter(int index) override;
    int  raCurrentOffset() const override;
    void raShowSpoken(int start, int end) override;
    void raClearSpoken() override;
    QString raBookKey() const override;
    QString raPreferredLanguage() const override;
    void raNarrationChanged() override;

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
    // Discord presence: what is open and how far in. Emitted at the same page-turn edge the consumption
    // accrual uses, so reading needs no new timer and no new bookkeeping of its own.
    void readingProgress(const QString& title, const QString& subtitle);
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
    // The forward-progress edge: consumption stats' high-water page (what #136's furthest-read carries) plus
    // the Discord presence line. Factored out of nextPage() so narration accrues through exactly the same
    // reckoning a page turn does, rather than a second one that could drift from it.
    void accrueReadingProgress();
    void syncReadAloudButtons();   // classic bar: the labels that mirror narration's state
    void restoreState();
    void layoutOverlays();
    void recomputeBookPages(); // tally each chapter's page count for a book-wide "page x / y"
    int  globalPage() const;   // 1-based page within the whole book at the current spot

    std::unique_ptr<EbookSource> book_; // EpubBook / MobiBook / PdfTextBook, chosen by file content
    BookPageWidget* page_ = nullptr;
    QFrame* menu_ = nullptr;            // auto-hiding top control bar (overlay)
    QPushButton* streamIssueBtn_ = nullptr; // "Issue with Streaming" (hidden unless a remote book)
    // Read-aloud controls on the CLASSIC bar (issue #145). Created only when read-aloud is available, so a
    // build without the TextToSpeech module shows the bar it always showed.
    QPushButton* raBtn_      = nullptr;   // Read aloud / Stop
    QPushButton* raPauseBtn_ = nullptr;   // Pause / Resume
    QPushButton* raSpeedBtn_ = nullptr;   // the shared #140 speed, stepped
    QPushButton* raVoiceBtn_ = nullptr;   // the platform voice, stepped
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
    class ReadAloudController* readAloud_ = nullptr;  // owned; null when the build has no TextToSpeech module
    static constexpr int kMenuHeight = 38; // overlay menu height; the page reserves this much up top
};
