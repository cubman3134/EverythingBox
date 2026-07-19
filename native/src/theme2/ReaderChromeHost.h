// ReaderChromeHost — the themed chrome that composes over a HOSTED raster reader (EbookView this task; Pdf/
// Comic reuse it in Tasks 4-5), per the B1 composition decision (VARIANT A). It owns:
//
//   * the reader widget (reparented in, filling the host);
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

class EbookView;
class NavGraph;
class QQuickWidget;
class QTimer;

// The `readerBridge` QML sees: page/chapter/font/toc read-props plus the commands the chrome fires (page
// turn, font pick, chapter jump). Thin — every call forwards to EbookView's public wrappers; NO reader logic
// lives here. Refreshed off EbookView::pageInfoChanged so the strips mirror raw-arrow paging too.
class ReaderBridge : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString readerType READ readerType CONSTANT)
    Q_PROPERTY(QString title READ title NOTIFY changed)
    Q_PROPERTY(int page READ page NOTIFY changed)
    Q_PROPERTY(int pageCount READ pageCount NOTIFY changed)
    Q_PROPERTY(int fontSize READ fontSize NOTIFY changed)
    Q_PROPERTY(QVariantList fontOptions READ fontOptions CONSTANT)
    Q_PROPERTY(int fontIndex READ fontIndex NOTIFY changed)
    Q_PROPERTY(QStringList toc READ toc NOTIFY tocChanged)
public:
    explicit ReaderBridge(EbookView* reader, QObject* parent = nullptr);

    QString readerType() const { return QStringLiteral("book"); }
    QString title() const;
    int  page() const;
    int  pageCount() const;
    int  fontSize() const;
    QVariantList fontOptions() const;   // the point sizes the font ThemedChoice offers
    int  fontIndex() const;             // index into fontOptions() nearest the current size (for currentOption)
    QStringList toc() const;

    void refresh();       // re-emit changed() (page/chapter/font moved)
    void refreshToc();    // re-emit tocChanged() + changed() (a new book loaded)

public slots:
    void next();
    void prev();
    void chooseFont(int optionIndex);   // ThemedChoice.chosen(index) -> apply fontOptions()[index]
    void gotoToc(int i);

signals:
    void changed();
    void tocChanged();

private:
    EbookView* reader_;
};

class ReaderChromeHost : public QWidget
{
    Q_OBJECT
public:
    ReaderChromeHost(EbookView* reader, QWidget* parent = nullptr);

    NavGraph* navGraph() const { return graph_; }   // for MainWindow::updateNavForPage (presence marker)
    bool themed() const { return themed_; }
    bool chromeVisible() const { return chromeVisible_; }   // for the UI-test state snapshot

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

private:
    void buildStrips();          // lazily create the two QQuickWidget strips (first themed present)
    void layoutStrips();         // geometry-manage + raise (constraint 3)
    void revealChrome();         // show strips + arm auto-hide + land the cursor on the nav bar
    void hideChrome();           // hide strips, collapse the toc, return focus to the reader
    void armAutoHide();          // (re)start the idle timer
    bool arbitrateKey(int key);  // the shared key router for physical + synthetic keys
    void onGraphActivated(const QString& zone, int index);
    void onSelectionChanged(const QString& zone, int index);

    EbookView*    reader_ = nullptr;
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
