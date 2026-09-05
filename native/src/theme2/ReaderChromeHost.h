// ReaderChromeHost — the themed chrome that composes over ANY hosted raster reader — an EbookView, PdfView or
// ComicView, addressed through the HostedReader interface + a ReaderKind (Task 3 built Book; Task 4 added Pdf/
// Comic) — per the B1 composition decision (VARIANT A). It owns:
//
//   * the reader widget (reader_->asWidget(), reparented in, filling the host);
//   * two OPAQUE strip QQuickWidgets raised over the reader — a TOP bar (title + reader settings + the
//     chapter list when opened) and a BOTTOM bar (prev / progress / next) — both Qt::NoFocus so the reader
//     keeps keyboard focus and receives paging keys (spike constraint 1); geometry-managed in resizeEvent and
//     re-raised once per layout (constraints 2-3); no WA_AlwaysStackOnTop (raster, not GL);
//   * the reader's NavGraph, built by buildReaderNavGraph (NavThemeGraph.h — the ONE shared shape the probe
//     asserts), exposed to the QML strips as the `nav` context property;
//   * a ReaderBridge (`readerBridge` context property) carrying page/chapter/font/toc info and the reader
//     commands the chrome fires.
//
// Chrome auto-hides on idle (a timer) and reveals on Up / a menu key; the Back router owns the reader LEVEL
// (pushed on themed open): Back with chrome visible hides it, Back with chrome hidden pops the level (returns
// to where the reader was opened). In classic mode the host is a transparent passthrough — the reader shows
// its own widget chrome and the strips/graph stay dormant.
#pragma once
#include <QWidget>
#include <QStringList>
#include <QVariantList>
#include <QPointF>

class QTouchEvent;

#include "../ui/nav/NavThemeGraph.h"
#include "HostedReader.h"

class NavGraph;
class QQuickWidget;
class QTimer;

// The `readerBridge` QML sees: page/font/toc/two-up read-props plus the commands the chrome fires (page turn,
// font pick, chapter jump, and — pdf/comic — the zoom/fit/two-up settings). Thin — every call forwards to the
// HostedReader's public wrappers; NO reader logic lives here. Refreshed off the reader's pageInfoChanged so the
// strips mirror raw-key paging/zooming too. `kind` drives which settings the QML lays out (font vs zoom/fit).
class ReaderBridge : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString readerType READ readerType CONSTANT)
    Q_PROPERTY(QString title READ title NOTIFY changed)
    Q_PROPERTY(int page READ page NOTIFY changed)
    Q_PROPERTY(int pageCount READ pageCount NOTIFY changed)
    Q_PROPERTY(QString pageLabel READ pageLabel NOTIFY changed) // "N / M", or a comic spread RANGE "N–N+1 / M"
    Q_PROPERTY(int fontSize READ fontSize NOTIFY changed)
    Q_PROPERTY(QVariantList fontOptions READ fontOptions CONSTANT)
    Q_PROPERTY(int fontIndex READ fontIndex NOTIFY changed)
    Q_PROPERTY(QStringList toc READ toc NOTIFY tocChanged)
    Q_PROPERTY(bool twoUp READ twoUp NOTIFY changed)   // comic: is the two-up spread on
    // Bookmarks (issue #136): the current book's bookmark labels in reading order, refreshed on add/remove.
    Q_PROPERTY(QStringList bookmarks READ bookmarkLabels NOTIFY bookmarksChanged)
    Q_PROPERTY(int bookmarkCount READ bookmarkCount NOTIFY bookmarksChanged)
    // Reading look (book). The engine for all of this already exists — ReaderTypography maps it and the reader
    // applies it live — so these only carry it to a menu that can finally show it.
    Q_PROPERTY(QStringList themeNames READ themeNames CONSTANT)
    Q_PROPERTY(int themeIndex READ themeIndex NOTIFY changed)
    Q_PROPERTY(QStringList fontFamilies READ fontFamilies CONSTANT)
    Q_PROPERTY(int fontFamilyIndex READ fontFamilyIndex NOTIFY changed)
    // Read aloud (issue #145). readAloudAvailable is the ONE gate the QML row reads: false and it draws no
    // read-aloud control at all, which is the state of a build without the Qt TextToSpeech module, a platform
    // with no engine, and every pdf/comic. The rest describe narration so the row can label itself honestly -
    // "Stop" while speaking, "Resume" while paused, the speed and the voice it is actually using.
    Q_PROPERTY(bool readAloudAvailable READ readAloudAvailable NOTIFY changed)
    Q_PROPERTY(bool readAloudActive READ readAloudActive NOTIFY changed)
    Q_PROPERTY(bool readAloudPaused READ readAloudPaused NOTIFY changed)
    Q_PROPERTY(QString readAloudSpeedLabel READ readAloudSpeedLabel NOTIFY changed)
    Q_PROPERTY(QString readAloudVoiceLabel READ readAloudVoiceLabel NOTIFY changed)
public:
    explicit ReaderBridge(HostedReader* reader, ReaderKind kind, QObject* parent = nullptr);

    QString readerType() const;         // "book" / "pdf" / "comic" — which settings the QML shows
    QString title() const;
    int  page() const;
    int  pageCount() const;
    QString pageLabel() const;          // "N / M", or a comic two-up spread RANGE "N–N+1 / M" (classic-bar parity)
    int  fontSize() const;
    QVariantList fontOptions() const;   // the point sizes the font ThemedChoice offers (book)
    int  fontIndex() const;             // index into fontOptions() nearest the current size (for currentOption)
    QStringList toc() const;
    bool twoUp() const;                 // comic: the double-page spread preference
    int  tocCount() const;              // toc().size() (0 for pdf/comic) — host feeds the readerToc zone count

    // Bookmarks (issue #136): the current book's bookmarks in reading order (a label per bookmark), and their
    // count (the readerBookmarks zone count a future list panel feeds). Empty when the reader has no item key.
    QStringList bookmarkLabels() const;
    int  bookmarkCount() const;

    QStringList themeNames() const;      // the reading themes, in ReaderTypography's own order
    int  themeIndex() const;             // the stored theme, as an index into themeNames()
    QStringList fontFamilies() const;    // "Default" plus the families offered
    int  fontFamilyIndex() const;

    bool readAloudAvailable() const;
    bool readAloudActive() const;
    bool readAloudPaused() const;
    QString readAloudSpeedLabel() const;   // e.g. "1.25x", drawn on the speed control
    QString readAloudVoiceLabel() const;   // the platform voice's own name, or "Voice" when there is none

    void refresh();       // re-emit changed() (page/font/zoom/two-up moved)
    void refreshToc();    // re-emit tocChanged() + changed() (a new document loaded)
    void refreshBookmarks(); // re-emit bookmarksChanged() (a document loaded, or an add/remove elsewhere)

public slots:
    void next();
    void prev();
    void chooseFont(int optionIndex);   // book: ThemedChoice.chosen(index) -> apply fontOptions()[index]
    void gotoToc(int i);                // book: jump to the i-th chapter
    // The bottom strip's progress bar was clicked or dragged. That bar draws `page` against `pageCount`, so a
    // point on it means a page and nothing else: 0.0 is the first page, 1.0 the last. One slot for all three
    // kinds — each reader's own gotoPage decides what landing on a page means for it.
    void gotoFraction(qreal f);
    // Fire the i-th control in the top strip's row. 0 = Exit for every kind; then Book: font -/+, theme,
    // typeface. Pdf/Comic: zoom out/in, fit, and a comic's two-up.
    void activateSetting(int index);

    // Bookmarks (issue #136). addBookmark captures the current spot (chapter+offset for a book, page for pdf/
    // comic) into a ReaderAnchor and stores it; gotoBookmark jumps to the i-th (reading-order) bookmark;
    // removeBookmark deletes it. All keyed on the reader's own itemKey(); no-ops when that is empty.
    void addBookmark();
    void gotoBookmark(int i);
    void removeBookmark(int i);

    // Reading look. Each writes the stored preference and asks the reader to re-read it, so the ONE definition
    // of what a preference means stays in the reader and this never applies anything itself.
    void stepFont(int dir);              // ±1 point, the same step the reader's own stepper takes
    void setTheme(int index);
    void setFontFamily(int index);
    void exitReader();                   // leave the reader entirely — the visible twin of Back

signals:
    void changed();
    void tocChanged();
    void bookmarksChanged();
    void exitRequested();

private:
    HostedReader* reader_;
    ReaderKind    kind_;
};

class ReaderChromeHost : public QWidget
{
    Q_OBJECT
public:
    ReaderChromeHost(HostedReader* reader, ReaderKind kind, QWidget* parent = nullptr);

    NavGraph* navGraph() const { return graph_; }   // for MainWindow::updateNavForPage (presence marker)
    bool themed() const { return themed_; }
    bool chromeVisible() const { return chromeVisible_; }   // for the UI-test state snapshot
    ReaderKind kind() const { return kind_; }               // for the UI-test snapshot (readerKind/two-up)
    int  readerPage() const;         // reader_->currentPage() — UI-test snapshot
    int  readerPageCount() const;    // reader_->pageCount()
    bool readerTwoUp() const;        // comic: reader_->twoUp()

    // Called each time a book is (re)opened into this host: toggle themed vs classic chrome, refresh the
    // bridge/toc, (re)push the reader level, and — themed — flash the chrome so it's discoverable.
    void present(bool themed);
    // The host is being left for another page (Home button, a different open): drop any lingering reader
    // level silently and hide the chrome. Idempotent.
    void onLeaving();

    // Key/Back arbitration, called from MainWindow's sendNavKey / goBack when this host is the current page.
    // handleNavKey returns true if it consumed the key (drove the graph, revealed, or forwarded a page turn).
    bool handleNavKey(int key);
    void handleBack();

signals:
    void exitRequested();   // "leave the reader, back to where it was opened" — the reader level's onPop

protected:
    void resizeEvent(QResizeEvent*) override;
    bool eventFilter(QObject*, QEvent*) override;

private slots:
    void onReaderPageInfo();      // reader emitted pageInfoChanged() — refresh the bridge (string-based connect)

private:
    void buildStrips();          // lazily create the two QQuickWidget strips (first themed present)
    void layoutStrips();         // geometry-manage + raise (constraint 3)
    void revealChrome();         // show strips + arm auto-hide + land the cursor on the nav bar
    void hideChrome();           // hide strips, collapse the toc, return focus to the reader
    void armAutoHide();          // (re)start the idle timer
    bool arbitrateKey(int key);  // the shared key router for physical + synthetic keys
    bool handleReaderTouch(QTouchEvent* te);      // tap-zones + swipe + pinch (one impl, all three readers)
    // The ONE zone map, shared by a touch tap and a mouse click. Split out because the two arrive as different
    // events but mean the same thing, and having decided the zones twice is how they drift apart.
    void tapAt(const QPointF& pos);
    bool claimsClickAt(const QPointF& pos) const;      // is this click the host's, or the reader's own?
    qreal topBandHeight() const;                       // the menu band, shared by the tap and the click paths
    QPointF toReaderPos(QWidget* from, const QPointF& p) const;  // a child's coords -> the reader's
    void watchReaderTree();                            // filter the reader AND its child widgets
    void onGraphActivated(const QString& zone, int index);
    void onSelectionChanged(const QString& zone, int index);

    HostedReader* reader_ = nullptr;
    ReaderKind    kind_ = ReaderKind::Book;
    NavGraph*     graph_ = nullptr;
    ReaderBridge* bridge_ = nullptr;
    QQuickWidget* topStrip_ = nullptr;
    QQuickWidget* bottomStrip_ = nullptr;
    QTimer*       hideTimer_ = nullptr;

    bool themed_ = false;
    bool chromeVisible_ = false;
    bool tocOpen_ = false;      // the chapter list is expanded (nav cursor on readerToc) -> the top strip grows
    bool levelPushed_ = false;  // exactly one "reader" level is on the graph

    // Touch state (D1 Task 5): the in-flight single-finger start, a multi-touch (pinch) latch, and the current
    // two-finger baseline separation (re-based after each zoom step; 0 = no pinch in flight).
    QPointF touchStart_;
    bool    sawMulti_ = false;
    qreal   pinchBaseDist_ = 0.0;
    // Issue #147: did this sequence START in the OS's reserved edge band? Latched on the press, because a
    // band is a property of where the finger went DOWN, not of where it happened to be when it came up.
    bool    touchInert_ = false;

    // Mouse state, for telling a click from the tail of a drag. The reader is read with a mouse at least as
    // often as with a finger, and until now the zone map was reachable only by touch.
    QPointF mouseStart_;
    bool    mouseDown_ = false;
};
