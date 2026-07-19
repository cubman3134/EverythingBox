#include "ReaderChromeHost.h"
#include "../ebook/EbookView.h"
#include "../ui/nav/NavGraph.h"

#include <QQuickWidget>
#include <QQuickItem>
#include <QQmlContext>
#include <QVBoxLayout>
#include <QTimer>
#include <QKeyEvent>
#include <QResizeEvent>
#include <QColor>
#include <QUrl>
#include <algorithm>

// ---- ReaderBridge -------------------------------------------------------------------------------------------

ReaderBridge::ReaderBridge(EbookView* reader, QObject* parent) : QObject(parent), reader_(reader) {}

QString ReaderBridge::title() const { return reader_ ? QString() : QString(); } // reserved; label built in QML
int  ReaderBridge::page() const      { return reader_ ? reader_->currentPage() : 0; }
int  ReaderBridge::pageCount() const { return reader_ ? reader_->pageCount() : 0; }
int  ReaderBridge::fontSize() const  { return reader_ ? reader_->fontPt() : 14; }

QVariantList ReaderBridge::fontOptions() const
{
    // The discrete sizes the font ThemedChoice offers (matches EbookView's 8..40 clamp; ±2-ish steps).
    static const int sizes[] = { 10, 12, 14, 16, 18, 20, 24, 28 };
    QVariantList out;
    for (int s : sizes) out << s;
    return out;
}

int ReaderBridge::fontIndex() const
{
    const QVariantList opts = fontOptions();
    const int cur = fontSize();
    int best = 0, bestDelta = 1 << 30;
    for (int i = 0; i < opts.size(); ++i)
    {
        const int d = std::abs(opts[i].toInt() - cur);
        if (d < bestDelta) { bestDelta = d; best = i; }
    }
    return best;
}

QStringList ReaderBridge::toc() const { return reader_ ? reader_->tocTitles() : QStringList(); }

void ReaderBridge::refresh()    { emit changed(); }
void ReaderBridge::refreshToc() { emit tocChanged(); emit changed(); }

void ReaderBridge::next() { if (reader_) reader_->nextPage(); }
void ReaderBridge::prev() { if (reader_) reader_->prevPage(); }

void ReaderBridge::chooseFont(int optionIndex)
{
    const QVariantList opts = fontOptions();
    if (!reader_ || optionIndex < 0 || optionIndex >= opts.size()) return;
    reader_->fontDelta(opts[optionIndex].toInt() - reader_->fontPt()); // apply the delta to reach the pick
}

void ReaderBridge::gotoToc(int i) { if (reader_) reader_->gotoTocIndex(i); }

// ---- ReaderChromeHost ---------------------------------------------------------------------------------------

ReaderChromeHost::ReaderChromeHost(EbookView* reader, QWidget* parent)
    : QWidget(parent), reader_(reader)
{
    // The reader fills the host; the strips are raised over it (created lazily on the first themed present).
    reader_->setParent(this);
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->addWidget(reader_);

    graph_ = new NavGraph(this);
    buildReaderNavGraph(*graph_, ReaderKind::Book);
    bridge_ = new ReaderBridge(reader_, this);

    hideTimer_ = new QTimer(this);
    hideTimer_->setSingleShot(true);
    connect(hideTimer_, &QTimer::timeout, this, [this] {
        if ((topStrip_ && topStrip_->underMouse()) || (bottomStrip_ && bottomStrip_->underMouse()))
        { armAutoHide(); return; } // keep the chrome up while the pointer is on it
        hideChrome();
    });

    // The strips mirror page/chapter/font (incl. raw-arrow paging done by the reader itself).
    connect(reader_, &EbookView::pageInfoChanged, this, [this] { bridge_->refresh(); });
    connect(graph_, &NavGraph::activated, this, &ReaderChromeHost::onGraphActivated);
    connect(graph_, &NavGraph::selectionChanged, this, &ReaderChromeHost::onSelectionChanged);

    // Physical AND synthetic (controller) keys arrive at the focused reader; the host arbitrates them.
    reader_->installEventFilter(this);
}

void ReaderChromeHost::buildStrips()
{
    if (topStrip_) return;
    auto makeStrip = [this](const QString& region) {
        auto* qv = new QQuickWidget(this);
        qv->setResizeMode(QQuickWidget::SizeRootObjectToView);
        qv->setClearColor(QColor(QStringLiteral("#0E1218")));   // OPAQUE (Variant A); matches the theme dark
        qv->setFocusPolicy(Qt::NoFocus);                        // spike constraint 1: reader keeps key focus
        qv->rootContext()->setContextProperty(QStringLiteral("nav"), graph_);
        qv->rootContext()->setContextProperty(QStringLiteral("readerBridge"), bridge_);
        // region + barHeight are CONTEXT properties set BEFORE setSource so the region Loaders resolve to the
        // right sub-tree at creation. (A root property set AFTER setSource loads the QML with region defaulted
        // to "top" first, which would transiently create — then destroy — the OTHER strip's font ThemedChoice,
        // and its onDestruction would removeZone("readerSettings") out from under the real one.)
        qv->rootContext()->setContextProperty(QStringLiteral("chromeRegion"), region);
        qv->rootContext()->setContextProperty(QStringLiteral("chromeBarHeight"),
                                              EbookView::topChromeReserve());
        qv->setSource(QUrl(QStringLiteral("qrc:/theme2/elements/ReaderChrome.qml")));
        qv->hide();
        return qv;
    };
    topStrip_ = makeStrip(QStringLiteral("top"));
    bottomStrip_ = makeStrip(QStringLiteral("bottom"));
}

void ReaderChromeHost::layoutStrips()
{
    if (!topStrip_) return;
    const int w = width(), h = height();
    const int barH = EbookView::topChromeReserve();          // the reserved top inset — align the strip to it
    const int botH = qMax(barH, 46);
    const int tocH = tocOpen_ ? qBound(120, h * 2 / 5, h - barH - botH - 8) : 0;
    topStrip_->setGeometry(0, 0, w, barH + tocH);
    bottomStrip_->setGeometry(0, h - botH, w, botH);
    if (chromeVisible_) { topStrip_->raise(); bottomStrip_->raise(); } // raise ONCE per (re)layout (constraint 2)
}

void ReaderChromeHost::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    layoutStrips();
}

void ReaderChromeHost::present(bool themed)
{
    themed_ = themed;
    reader_->setHostedChrome(themed);
    if (!themed) { onLeaving(); return; }   // classic: transparent passthrough, no strips/graph

    buildStrips();
    bridge_->refreshToc();
    // The host owns the chrome zone counts (like the detail view's syncDetailZone), so navigation never
    // depends on QML self-registration timing: Book has one settings row (the font ThemedChoice) and a toc
    // sized to the book's chapters.
    graph_->setZoneCount(QStringLiteral("readerSettings"), 1);
    graph_->setZoneCount(QStringLiteral("readerToc"), bridge_->toc().size());
    // The Back router owns the reader level: exactly one, pushed on themed open; its onPop returns us to
    // where the reader was opened (chrome-hidden Back pops it — see handleBack()).
    if (!levelPushed_)
    {
        graph_->pushLevel(QStringLiteral("reader"), [this] { levelPushed_ = false; emit exitRequested(); });
        levelPushed_ = true;
    }
    revealChrome();          // flash the chrome so it's discoverable, then auto-hide
}

void ReaderChromeHost::onLeaving()
{
    if (levelPushed_) { graph_->popLevelSilent(); levelPushed_ = false; } // no onPop: we're already leaving
    chromeVisible_ = false;
    tocOpen_ = false;
    if (topStrip_) topStrip_->hide();
    if (bottomStrip_) bottomStrip_->hide();
    if (hideTimer_) hideTimer_->stop();
}

void ReaderChromeHost::revealChrome()
{
    if (!themed_) return;
    buildStrips();
    chromeVisible_ = true;
    graph_->select(QStringLiteral("readerNav"), 0); // land the cursor on the nav bar when the chrome appears
    topStrip_->show();
    bottomStrip_->show();
    layoutStrips();
    armAutoHide();
}

void ReaderChromeHost::hideChrome()
{
    chromeVisible_ = false;
    tocOpen_ = false;
    if (topStrip_) topStrip_->hide();
    if (bottomStrip_) bottomStrip_->hide();
    if (hideTimer_) hideTimer_->stop();
    reader_->setFocus();
}

void ReaderChromeHost::armAutoHide() { if (themed_ && chromeVisible_) hideTimer_->start(4200); }

void ReaderChromeHost::onSelectionChanged(const QString& zone, int)
{
    // The chapter list expands the top strip when the cursor is on it (a temporary panel over the page, like
    // the classic contents overlay). Re-geometry only on a real toc-open transition.
    const bool open = (zone == QStringLiteral("readerToc"));
    if (open != tocOpen_) { tocOpen_ = open; layoutStrips(); }
}

void ReaderChromeHost::onGraphActivated(const QString& zone, int index)
{
    if (zone == QStringLiteral("readerNav"))
    {
        if (index == 0)      bridge_->prev();
        else if (index == 2) bridge_->next();
        // index 1 = the progress readout: no activation for a book
        armAutoHide();
    }
    else if (zone == QStringLiteral("readerToc"))
    {
        bridge_->gotoToc(index);
        hideChrome();   // jumping to a chapter dismisses the chrome, mirroring the classic toc click
    }
    // readerSettings activation is owned by the ThemedChoice in the QML (it opens its own inline picker).
}

bool ReaderChromeHost::handleNavKey(int key) { return arbitrateKey(key); }

bool ReaderChromeHost::eventFilter(QObject* o, QEvent* e)
{
    if (o == reader_ && e->type() == QEvent::KeyPress)
    {
        auto* ke = static_cast<QKeyEvent*>(e);
        if (arbitrateKey(ke->key())) return true; // consumed: the reader does not also page/back
    }
    return QWidget::eventFilter(o, e);
}

bool ReaderChromeHost::arbitrateKey(int key)
{
    if (!themed_) return false;    // classic: the reader owns its own keys entirely

    if (key == Qt::Key_Backspace || key == Qt::Key_Escape) { handleBack(); return true; }

    if (chromeVisible_)
    {
        // The chrome zones take the keys (spike: "chrome zones activate only when chrome is VISIBLE").
        switch (key)
        {
        case Qt::Key_Up: case Qt::Key_Down: case Qt::Key_Left: case Qt::Key_Right:
            graph_->move(key); armAutoHide(); return true;
        case Qt::Key_Return: case Qt::Key_Enter:
            graph_->activate(); armAutoHide(); return true;
        default:
            return false;
        }
    }
    // Chrome hidden: Up / a menu key reveals it; every other key falls through to the reader (raw-arrow
    // paging via EbookView::keyPressEvent keeps working — Left/Right/PageUp/PageDown/Space).
    if (key == Qt::Key_Up || key == Qt::Key_Menu) { revealChrome(); return true; }
    return false;
}

void ReaderChromeHost::handleBack()
{
    if (!themed_) { emit exitRequested(); return; }
    if (chromeVisible_) { hideChrome(); return; }   // visible -> just hide the chrome
    graph_->back();                                 // hidden -> pop the reader level (onPop -> exitRequested)
}
