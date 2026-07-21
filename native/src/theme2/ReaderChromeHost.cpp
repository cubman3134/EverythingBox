#include "ReaderChromeHost.h"
#include "FormFactor.h"
#include "../ui/nav/NavGraph.h"

#include <QQuickWidget>
#include <QQuickItem>
#include <QQmlContext>
#include <QVBoxLayout>
#include <QTimer>
#include <QKeyEvent>
#include <QResizeEvent>
#include <QTouchEvent>
#include <QLineF>
#include <QColor>
#include <QUrl>
#include <algorithm>

// ---- ReaderBridge -------------------------------------------------------------------------------------------

ReaderBridge::ReaderBridge(HostedReader* reader, ReaderKind kind, QObject* parent)
    : QObject(parent), reader_(reader), kind_(kind) {}

QString ReaderBridge::readerType() const
{
    switch (kind_)
    {
    case ReaderKind::Pdf:   return QStringLiteral("pdf");
    case ReaderKind::Comic: return QStringLiteral("comic");
    case ReaderKind::Book:  default: return QStringLiteral("book");
    }
}

QString ReaderBridge::title() const { return QString(); } // reserved; label built in QML
int  ReaderBridge::page() const      { return reader_ ? reader_->currentPage() : 0; }
int  ReaderBridge::pageCount() const { return reader_ ? reader_->pageCount() : 0; }

// The page-of label the strips render. A comic showing a two-up spread reads a RANGE ("3–4 / 20"), matching
// the classic ComicView bar (which the themed chrome previously didn't — it always said "Page N / M", hiding
// that two pages were on screen). Every other reader (and a single-page comic) reads the plain "N / M".
QString ReaderBridge::pageLabel() const
{
    if (!reader_) return QString();
    const int p = reader_->currentPage(), pc = reader_->pageCount();
    if (reader_->spreadActive() && p + 1 <= pc)
        return QStringLiteral("%1–%2 / %3").arg(p).arg(p + 1).arg(pc); // N–N+1 / M (en dash, as the classic bar)
    return QStringLiteral("%1 / %2").arg(p).arg(pc);
}
int  ReaderBridge::fontSize() const  { return reader_ ? reader_->fontPt() : 14; }
bool ReaderBridge::twoUp() const     { return reader_ && reader_->twoUp(); }

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
int ReaderBridge::tocCount() const { return reader_ ? reader_->tocTitles().size() : 0; }

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

// pdf/comic settings rows: 0 = zoom out, 1 = zoom in, 2 = fit width, 3 = two-up (comic only).
void ReaderBridge::activateSetting(int index)
{
    if (!reader_) return;
    switch (index)
    {
    case 0: reader_->zoomDelta(-1); break;
    case 1: reader_->zoomDelta(+1); break;
    case 2: reader_->fitWidth();    break;
    case 3: if (kind_ == ReaderKind::Comic) reader_->setTwoUp(!reader_->twoUp()); break;
    default: return;
    }
    // No explicit refresh(): each of these reader commands emits pageInfoChanged(), which onReaderPageInfo()
    // already turns into a bridge refresh — an extra refresh() here would just double the changed() emit.
}

// Left/Right while the cursor is on the settings zone: cycle the primary setting both ways (the settings zone's
// own cross-axis Left/Right are SELF-pinned no-ops, so this reuses them for a bidirectional control cheaply).
void ReaderBridge::cycleSetting(int dir)
{
    if (!reader_ || dir == 0) return;
    if (kind_ == ReaderKind::Book)
    {
        const int n = fontOptions().size();
        if (n > 0) chooseFont(((fontIndex() + dir) % n + n) % n); // wrap both ways
    }
    else
    {
        reader_->zoomDelta(dir > 0 ? +1 : -1); // emits pageInfoChanged() -> onReaderPageInfo() refreshes; no double
    }
}

// ---- ReaderChromeHost ---------------------------------------------------------------------------------------

ReaderChromeHost::ReaderChromeHost(HostedReader* reader, ReaderKind kind, QWidget* parent)
    : QWidget(parent), reader_(reader), kind_(kind)
{
    // The reader fills the host; the strips are raised over it (created lazily on the first themed present).
    QWidget* rw = reader_->asWidget();
    rw->setParent(this);
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->addWidget(rw);

    graph_ = new NavGraph(this);
    buildReaderNavGraph(*graph_, kind_);
    bridge_ = new ReaderBridge(reader_, kind_, this);

    hideTimer_ = new QTimer(this);
    hideTimer_->setSingleShot(true);
    connect(hideTimer_, &QTimer::timeout, this, [this] {
        if ((topStrip_ && topStrip_->underMouse()) || (bottomStrip_ && bottomStrip_->underMouse()))
        { armAutoHide(); return; } // keep the chrome up while the pointer is on it
        hideChrome();
    });

    // The strips mirror page/font/zoom (incl. raw-key paging done by the reader itself). Connected via the
    // string-based SIGNAL so ONE host drives any reader kind (each implementer declares pageInfoChanged()).
    // String connects fail SILENTLY on signature drift — assert so a renamed signal can't desync the chrome.
    const bool pageInfoWired = connect(rw, SIGNAL(pageInfoChanged()), this, SLOT(onReaderPageInfo()));
    Q_ASSERT(pageInfoWired);
    if (!pageInfoWired)
        qWarning("ReaderChromeHost: pageInfoChanged() connect FAILED — reader %s lacks the HostedReader signal",
                 rw->metaObject()->className());
    connect(graph_, &NavGraph::activated, this, &ReaderChromeHost::onGraphActivated);
    connect(graph_, &NavGraph::selectionChanged, this, &ReaderChromeHost::onSelectionChanged);

    // Physical AND synthetic (controller) keys arrive at the focused reader; the host arbitrates them.
    rw->installEventFilter(this);

    // Touch (D1 Task 5): ONE implementation for all three readers lives HERE. The reader widget accepts touch so
    // this filter sees the QTouchEvent stream — tap-zones + horizontal swipe + centre chrome toggle, and (comic/
    // pdf) pinch-to-zoom read straight off the two-finger separation. We deliberately do NOT grabGesture(Qt::
    // PinchGesture): Qt's QPinchGesture recognition off a raw touch stream proved non-deterministic here (a run
    // would emit the gesture, the next would emit nothing and still swallow the TouchUpdate/End frames), so we
    // track the pinch ourselves — deterministic across desktop and touchscreen, and drivable by the uitest harness.
    // Mouse is FROZEN: gestures fire on QTouchEvent only, never a physical click, so text-select/desktop click are
    // unchanged. Themed-gated (arbitrateKey is too) — classic mode leaves the reader its own bar + input entirely.
    rw->setAttribute(Qt::WA_AcceptTouchEvents, true);
}

void ReaderChromeHost::onReaderPageInfo() { bridge_->refresh(); }

int  ReaderChromeHost::readerPage() const      { return reader_ ? reader_->currentPage() : 0; }
int  ReaderChromeHost::readerPageCount() const { return reader_ ? reader_->pageCount() : 0; }
bool ReaderChromeHost::readerTwoUp() const     { return reader_ && reader_->twoUp(); }

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
        // `form` (subsystem D): the strip scales its fonts/controls from the form-factor uiScale. Registered INSIDE
        // the lambda because makeStrip runs TWICE (top + bottom) — both strips must see `form` before setSource.
        qv->rootContext()->setContextProperty(QStringLiteral("form"), &FormFactor::instance());
        // region + barHeight are CONTEXT properties set BEFORE setSource so the region Loaders resolve to the
        // right sub-tree at creation. (A root property set AFTER setSource loads the QML with region defaulted
        // to "top" first, which would transiently create — then destroy — the OTHER strip's font ThemedChoice,
        // and its onDestruction would removeZone("readerSettings") out from under the real one.)
        qv->rootContext()->setContextProperty(QStringLiteral("chromeRegion"), region);
        qv->rootContext()->setContextProperty(QStringLiteral("chromeBarHeight"),
                                              reader_->chromeTopReserve());
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
    // Form-factor tokens (subsystem D): the strips grow with uiScale and pull in from the bezel by the safe-area
    // fraction of the shorter edge. Desktop is IDENTITY (uiScale 1.0, safeAreaFrac 0.0), so every qRound below is
    // a no-op and the strips keep their exact classic geometry.
    const qreal us   = FormFactor::instance().uiScale();
    const int   ins  = qRound(qMin(w, h) * FormFactor::instance().safeAreaFrac());
    const int   barH = qRound(reader_->chromeTopReserve() * us);  // the reserved top inset — align the strip to it
    const int   botH = qMax(barH, qRound(46 * us));
    const int   tocH = tocOpen_ ? qBound(qRound(120 * us), h * 2 / 5, h - barH - botH - 8) : 0;
    topStrip_->setGeometry(ins, ins, w - 2 * ins, barH + tocH);
    bottomStrip_->setGeometry(ins, h - botH - ins, w - 2 * ins, botH);
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
    // The host owns the chrome zone counts (like the detail view's syncDetailZone), so navigation never depends
    // on QML self-registration timing. Settings rows per kind: Book = 1 (the font ThemedChoice); Pdf = 3 (zoom
    // out / in / fit); Comic = 4 (+ two-up toggle). Toc = the book's chapters (0 for pdf/comic — no ToC).
    const int settingsRows = (kind_ == ReaderKind::Book)  ? 1
                           : (kind_ == ReaderKind::Comic) ? 4
                                                          : 3;
    graph_->setZoneCount(QStringLiteral("readerSettings"), settingsRows);
    graph_->setZoneCount(QStringLiteral("readerToc"), bridge_->tocCount());
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
    reader_->asWidget()->setFocus();
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
    else if (zone == QStringLiteral("readerSettings"))
    {
        // Book: activation is owned by the font ThemedChoice in the QML (its externalEdit cycles the size).
        // Pdf/Comic: there is no ThemedChoice — the host fires the indexed zoom/fit/two-up command directly.
        if (kind_ != ReaderKind::Book) { bridge_->activateSetting(index); armAutoHide(); }
    }
}

bool ReaderChromeHost::handleNavKey(int key) { return arbitrateKey(key); }

bool ReaderChromeHost::eventFilter(QObject* o, QEvent* e)
{
    if (o == reader_->asWidget())
    {
        switch (e->type())
        {
        case QEvent::KeyPress:
            if (arbitrateKey(static_cast<QKeyEvent*>(e)->key())) return true; // reader does not also page/back
            break;
        case QEvent::TouchBegin:
        case QEvent::TouchUpdate:
        case QEvent::TouchEnd:
            if (handleReaderTouch(static_cast<QTouchEvent*>(e))) return true;
            break;
        default:
            break;
        }
    }
    return QWidget::eventFilter(o, e);
}

// Touch on the reader page. TWO fingers = pinch-to-zoom (pdf/comic): step zoomDelta(±1) — the EXACT ± the zoom
// buttons fire — for each 15% change in the finger separation, re-baselining after each step so a slow pinch keeps
// stepping. ONE finger = tap-zones (left ⅓ = prev, right ⅓ = next, centre = chrome toggle) or a horizontal swipe
// (≥80 px, leftward = next / rightward = prev — the page-flip convention). Consuming the stream stops synthesized-
// mouse, keeping the reader's mouse behavior frozen. Themed-only (classic mode owns its own input, like
// arbitrateKey). sawMulti_ latches a pinch so a lifted-to-one-finger tail is never mistaken for a tap/swipe.
bool ReaderChromeHost::handleReaderTouch(QTouchEvent* te)
{
    if (!themed_) return false;
    const auto pts = te->points();

    if (pts.size() >= 2)   // pinch: read the zoom straight off the two-finger separation
    {
        sawMulti_ = true;
        if (kind_ != ReaderKind::Book && te->type() != QEvent::TouchEnd) // zoomDelta() is pdf/comic only
        {
            const qreal d = QLineF(pts[0].position(), pts[1].position()).length();
            if (pinchBaseDist_ <= 0.0) pinchBaseDist_ = d;              // first pinch frame: set the baseline
            else if (d > 0.0)
            {
                const qreal r = d / pinchBaseDist_;
                if (r >= 1.15)          { reader_->zoomDelta(+1); pinchBaseDist_ = d; }
                else if (r <= 1.0 / 1.15) { reader_->zoomDelta(-1); pinchBaseDist_ = d; }
            }
        }
        return true;
    }

    switch (te->type())
    {
    case QEvent::TouchBegin:
        sawMulti_ = false;
        pinchBaseDist_ = 0.0;
        touchStart_ = pts.isEmpty() ? QPointF() : pts.first().position();
        return true;
    case QEvent::TouchUpdate:
        return true;   // single-finger drag: consumed (swipe is resolved on release), no synthesized mouse
    case QEvent::TouchEnd:
    {
        pinchBaseDist_ = 0.0;
        if (sawMulti_) { sawMulti_ = false; return true; } // the pinch's final frame — not a tap/swipe
        const QPointF end = pts.isEmpty() ? touchStart_ : pts.first().position();
        const qreal dx = end.x() - touchStart_.x(), dy = end.y() - touchStart_.y();
        if (qAbs(dx) >= 80.0 && qAbs(dx) > qAbs(dy))
        {
            if (dx < 0) reader_->nextPage(); else reader_->prevPage(); // leftward swipe = next
        }
        else if (qAbs(dx) < 24.0 && qAbs(dy) < 24.0)                   // a tap (no meaningful travel)
        {
            const int w = reader_->asWidget()->width();
            const qreal x = end.x();
            if (x < w / 3.0)             reader_->prevPage();
            else if (x > 2.0 * w / 3.0)  reader_->nextPage();
            else if (chromeVisible_)     hideChrome();                 // centre: toggle the themed chrome
            else                         revealChrome();
        }
        return true;
    }
    default:
        return true;
    }
}

bool ReaderChromeHost::arbitrateKey(int key)
{
    if (!themed_) return false;    // classic: the reader owns its own keys entirely

    if (key == Qt::Key_Backspace || key == Qt::Key_Escape) { handleBack(); return true; }

    if (chromeVisible_)
    {
        // The chrome zones take the keys (spike: "chrome zones activate only when chrome is VISIBLE").
        // On the settings zone, Left/Right cycle the primary setting both ways (font for a book, zoom for a
        // pdf/comic) — the zone's cross-axis Left/Right are SELF-pinned no-ops, so this reuses them cheaply.
        if ((key == Qt::Key_Left || key == Qt::Key_Right)
            && graph_->zone() == QStringLiteral("readerSettings"))
        {
            bridge_->cycleSetting(key == Qt::Key_Right ? +1 : -1);
            armAutoHide();
            return true;
        }
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
