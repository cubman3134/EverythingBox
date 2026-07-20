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

    void refresh();       // re-emit changed() (page/font/zoom/two-up moved)
    void refreshToc();    // re-emit tocChanged() + changed() (a new document loaded)

public slots:
    void next();
    void prev();
    void chooseFont(int optionIndex);   // book: ThemedChoice.chosen(index) -> apply fontOptions()[index]
    void gotoToc(int i);                // book: jump to the i-th chapter
    void activateSetting(int index);    // pdf/comic: fire the i-th settings row (0=zoom out,1=in,2=fit,3=two-up)
    void cycleSetting(int dir);         // Left/Right on the settings zone: bidirectional font(book)/zoom(pdf/comic)

signals:
    void changed();
    void tocChanged();

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
};
