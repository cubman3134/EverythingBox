#include "ReaderChromeHost.h"
#include "FormFactor.h"
#include "../input/InputMode.h"   // the `input` context property (controller-aware help chips)
#include "../ui/nav/NavGraph.h"
#include "../core/BookmarkStore.h"   // issue #136: the per-book bookmark store the bridge drives
#include "../ebook/ReaderAnchor.h"   // issue #136: the one anchor model the capture/jump build
#include "../ebook/ReadAloud.h"     // issue #145: the pure settings-row count (and the divider behind it)
#include "../core/Settings.h"        // the reading look is a stored preference, not chrome state

#include <QQuickWidget>
#include <QQuickItem>
#include <QQmlContext>
#include <QVBoxLayout>
#include <QTimer>
#include <QKeyEvent>
#include <QResizeEvent>
#include <QMouseEvent>
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

// A point on the bottom strip's progress bar IS a page — that bar draws page/pageCount and nothing else. So
// the fraction is mapped back onto that scale and handed to the reader's own gotoPage, which is the one place
// that knows what landing on a page costs for its kind.
//
// The mapping is deliberately the exact INVERSE of the fill the bar draws (fill = page / pageCount, 1-based),
// not the tidier page0 = f × (pageCount − 1). Those differ by one for most of the bar, and the difference is
// visible: clicking the fill's own leading edge would land a page past it, and the bar would settle one page
// beyond where the finger was. Whatever the fill says a page is worth is what a click on it has to buy.
//
// The bar is not redrawn from here: every reader emits pageInfoChanged when it moves, and the host already
// mirrors that into the strips — so the readout cannot disagree with where the reader actually went.
void ReaderBridge::gotoFraction(qreal f)
{
    if (!reader_) return;
    const int total = pageCount();
    if (total <= 0) return;
    const qreal clamped = qBound(qreal(0), f, qreal(1));
    const int page1 = qBound(1, int(clamped * total + 0.5), total);
    reader_->gotoPage(page1 - 1);
}

// ---- The reading look ---------------------------------------------------------------------------------------
// All of this was already modelled, stored and applied; none of it was reachable. Each setter writes the stored
// preference and asks the reader to re-read it, so what a preference MEANS stays defined in exactly one place.

// Named in ReaderTypography's own order, so an index here is that enum and no second mapping can drift.
QStringList ReaderBridge::themeNames() const
{
    return { QObject::tr("Light"), QObject::tr("Sepia"), QObject::tr("Dark"), QObject::tr("True Black") };
}

int ReaderBridge::themeIndex() const { return ReaderTypography::themeToInt(Settings::readerTheme()); }

// A short list on purpose. This is a menu someone reads a book through, not a font manager: a serif, a sans, a
// monospace and "leave it alone" are the choices that change how a page reads.
static QStringList familyValues()
{
    return { QString(), QStringLiteral("Georgia"), QStringLiteral("Palatino Linotype"),
             QStringLiteral("Times New Roman"), QStringLiteral("Segoe UI"), QStringLiteral("Verdana"),
             QStringLiteral("Consolas") };
}

QStringList ReaderBridge::fontFamilies() const
{
    QStringList out;
    for (const QString& f : familyValues()) out << (f.isEmpty() ? QObject::tr("Default") : f);
    return out;
}

int ReaderBridge::fontFamilyIndex() const
{
    const QString cur = Settings::readerFont().trimmed();
    const QStringList vals = familyValues();
    const int at = vals.indexOf(cur);
    return at >= 0 ? at : 0;   // a family set elsewhere that is not on this list reads as Default
}

void ReaderBridge::stepFont(int dir)
{
    if (!reader_ || dir == 0) return;
    reader_->fontDelta(dir > 0 ? 1 : -1);   // the reader clamps; this does not second-guess the bounds
    emit changed();
}

void ReaderBridge::setTheme(int index)
{
    if (index < 0 || index >= themeNames().size()) return;
    Settings::setReaderTheme(ReaderTypography::themeFromInt(index));
    if (reader_) reader_->reloadTypography();
    emit changed();
}

void ReaderBridge::setFontFamily(int index)
{
    const QStringList vals = familyValues();
    if (index < 0 || index >= vals.size()) return;
    Settings::setReaderFont(vals[index]);
    if (reader_) reader_->reloadTypography();
    emit changed();
}

void ReaderBridge::exitReader() { emit exitRequested(); }

// ---- Bookmarks (issue #136) ---------------------------------------------------------------------------------
// The bridge owns the reader<->store glue: it builds a ReaderAnchor for the current spot, and jumps to a stored
// one. The store (BookmarkStore) is per-profile and syncs; the anchor (ReaderAnchor) is the build-it-once model.

// The anchor for where the reader is RIGHT NOW: a book is spine (chapter) + character offset; a pdf/comic is a
// 0-based page (currentPage() is 1-based). endOffset stays -1 — a bookmark is a point anchor, never a range.
static ReaderAnchor anchorForReader(HostedReader* reader, ReaderKind kind)
{
    ReaderAnchor a;
    if (kind == ReaderKind::Book)
    {
        a.kind   = ReaderAnchor::Book;
        a.spine  = reader ? reader->spineIndex() : 0;
        a.offset = reader ? reader->textOffset() : 0;
    }
    else
    {
        a.kind = (kind == ReaderKind::Pdf) ? ReaderAnchor::Pdf : ReaderAnchor::Comic;
        a.page = reader ? qMax(0, reader->currentPage() - 1) : 0; // currentPage() is 1-based
    }
    return a;
}

// A human-readable default label for a fresh bookmark: a book uses its chapter title when the spine indexes the
// toc, else a page read-out; a pdf/comic uses the page read-out. The store keeps whatever we pass (empty is
// allowed), so this is where the "default to a chapter/percent name" the issue asks for is computed.
static QString defaultLabelForReader(HostedReader* reader, ReaderKind kind, const ReaderAnchor& a)
{
    if (!reader) return QString();
    if (kind == ReaderKind::Book)
    {
        const QStringList toc = reader->tocTitles();
        if (a.spine >= 0 && a.spine < toc.size() && !toc.at(a.spine).isEmpty()) return toc.at(a.spine);
    }
    const int pc = reader->pageCount();
    return pc > 0 ? QStringLiteral("Page %1 / %2").arg(reader->currentPage()).arg(pc)
                  : QStringLiteral("Page %1").arg(reader->currentPage());
}

QStringList ReaderBridge::bookmarkLabels() const
{
    QStringList out;
    if (!reader_) return out;
    const QString key = reader_->itemKey();
    if (key.isEmpty()) return out;
    int n = 0;
    for (const BookmarkStore::Bookmark& b : BookmarkStore::list(key))
    {
        ++n;
        out << (b.label.isEmpty() ? QStringLiteral("Bookmark %1").arg(n) : b.label);
    }
    return out;
}

int ReaderBridge::bookmarkCount() const
{
    if (!reader_) return 0;
    const QString key = reader_->itemKey();
    return key.isEmpty() ? 0 : BookmarkStore::list(key).size();
}

void ReaderBridge::refreshBookmarks() { emit bookmarksChanged(); }

void ReaderBridge::addBookmark()
{
    if (!reader_) return;
    const QString key = reader_->itemKey();
    if (key.isEmpty()) return;
    const ReaderAnchor a = anchorForReader(reader_, kind_);
    BookmarkStore::add(key, a, defaultLabelForReader(reader_, kind_, a));
    emit bookmarksChanged();
}

void ReaderBridge::gotoBookmark(int i)
{
    if (!reader_) return;
    const QString key = reader_->itemKey();
    if (key.isEmpty()) return;
    const QVector<BookmarkStore::Bookmark> items = BookmarkStore::list(key); // reading order
    if (i < 0 || i >= items.size()) return;
    const ReaderAnchor& a = items.at(i).anchor;
    if (kind_ == ReaderKind::Book) reader_->gotoSpineOffset(a.spine, a.offset);
    else                           reader_->gotoPage(a.page);
}

void ReaderBridge::removeBookmark(int i)
{
    if (!reader_) return;
    const QString key = reader_->itemKey();
    if (key.isEmpty()) return;
    const QVector<BookmarkStore::Bookmark> items = BookmarkStore::list(key);
    if (i < 0 || i >= items.size()) return;
    BookmarkStore::remove(items.at(i).id);
    emit bookmarksChanged();
}

// ---- Read aloud (issue #145) ---------------------------------------------------------------------------------
// Thin, like everything else on this bridge: each of these forwards to the HostedReader, whose default answers
// are inert. So a pdf or a comic reports "unavailable" without either of them knowing read-aloud exists, and a
// build without the Qt TextToSpeech module reports it through the book too.

bool ReaderBridge::readAloudAvailable() const { return reader_ && reader_->readAloudAvailable(); }
bool ReaderBridge::readAloudActive() const    { return reader_ && reader_->readAloudActive(); }
bool ReaderBridge::readAloudPaused() const    { return reader_ && reader_->readAloudPaused(); }

QString ReaderBridge::readAloudSpeedLabel() const
{
    if (!reader_) return QString();
    return QString::number(reader_->readAloudSpeed(), 'g', 3) + QStringLiteral("x");
}

QString ReaderBridge::readAloudVoiceLabel() const
{
    const QString v = reader_ ? reader_->readAloudVoiceName() : QString();
    return v.isEmpty() ? tr("Voice") : v;
}

// pdf/comic settings rows: 0 = zoom out, 1 = zoom in, 2 = fit width, 3 = two-up (comic only).
void ReaderBridge::activateSetting(int index)
{
    if (!reader_) return;

    // Index 0 is Exit for every kind, and returns immediately: leaving is not a reader command and must not
    // fall through to one.
    if (index == 0) { exitReader(); return; }

    if (kind_ == ReaderKind::Book)
    {
        switch (index)
        {
        case 1: stepFont(-1); break;
        case 2: stepFont(+1); break;
        // The two cycling controls advance and wrap: a short, closed list is quicker to step through than to
        // open a sub-menu for, and every value is one press away from every other.
        case 3: setTheme((themeIndex() + 1) % themeNames().size()); break;
        case 4: setFontFamily((fontFamilyIndex() + 1) % fontFamilies().size()); break;
        // Read aloud (issue #145), indices 5..8 - present in the row ONLY when readAloudAvailable(), which is
        // what ReadAloud::bookSettingsRowCount() counts and what the QML model gates its entries on. Guarded
        // here as well so a stale index (a count that arrived before the reader did) can never fire a command
        // the row is not drawing.
        case 5: if (readAloudAvailable()) reader_->toggleReadAloud();      break;
        case 6: if (readAloudAvailable()) reader_->readAloudTogglePause(); break;
        case 7: if (readAloudAvailable()) reader_->readAloudCycleSpeed();  break;
        case 8: if (readAloudAvailable()) reader_->readAloudCycleVoice();  break;
        default: return;
        }
        emit changed();   // narration's labels (Stop/Resume/speed/voice) are read straight off this bridge
        return;   // these already emit changed(); a reader page command did not happen
    }

    switch (index)
    {
    case 1: reader_->zoomDelta(-1); break;
    case 2: reader_->zoomDelta(+1); break;
    case 3: reader_->fitWidth();    break;
    case 4: if (kind_ == ReaderKind::Comic) reader_->setTwoUp(!reader_->twoUp()); break;
    default: return;
    }
    // No explicit refresh(): each of these reader commands emits pageInfoChanged(), which onReaderPageInfo()
    // already turns into a bridge refresh — an extra refresh() here would just double the changed() emit.
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
    // The Exit control is the visible twin of Back, so it leaves by the same door rather than a second one.
    connect(bridge_, &ReaderBridge::exitRequested, this, [this] { hideChrome(); emit exitRequested(); });

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

    // Bookmarks (issue #136): keep the readerBookmarks zone count in lockstep with the store as bookmarks are
    // added ('B') or removed (the panel's × affordance) WHILE the reader is open, so the list zone appears the
    // moment the first bookmark lands and gates off again when the last one goes (reassigning focus off it).
    connect(bridge_, &ReaderBridge::bookmarksChanged, this, [this] {
        if (themed_) graph_->setZoneCount(QStringLiteral("readerBookmarks"), bridge_->bookmarkCount());
    });

    // Physical AND synthetic (controller) keys arrive at the focused reader; the host arbitrates them.
    watchReaderTree();

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
        // `input` (controller-aware UI): registered INSIDE the lambda for the same reason `form` is -- makeStrip
        // runs TWICE (top + bottom) and each strip is its own QQuickWidget with its own root context.
        qv->rootContext()->setContextProperty(QStringLiteral("input"), &InputMode::instance());
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
    bridge_->refreshBookmarks();   // a (re)opened book has its own bookmark list (issue #136)
    // The host owns the chrome zone counts (like the detail view's syncDetailZone), so navigation never depends
    // on QML self-registration timing. The row is Exit first for EVERY kind — the way out is the one control
    // that means the same thing everywhere, so it sits in the same place everywhere — then the kind's own:
    // Book = font smaller / larger / theme / typeface (5); Pdf = zoom out / in / fit (4); Comic = + two-up (5).
    // Toc = the book's chapters (0 for pdf/comic — no ToC).
    // A BOOK's count is ReadAloud::bookSettingsRowCount(): 5 without read-aloud (the row it has always been)
    // and 9 with it. The count comes from that pure function rather than a literal here so the number the nav
    // cursor can reach and the number probe_readaloud pins are the same statement, not two that agree today.
    const int settingsRows = (kind_ == ReaderKind::Book)
                                 ? ReadAloud::bookSettingsRowCount(bridge_->readAloudAvailable())
                           : (kind_ == ReaderKind::Comic) ? 5
                                                          : 4;
    graph_->setZoneCount(QStringLiteral("readerSettings"), settingsRows);
    graph_->setZoneCount(QStringLiteral("readerToc"), bridge_->tocCount());
    // The bookmark list zone (issue #136): fed from the bridge's live bookmark count, so an empty list gates
    // the zone off (never a crossing target — focus can't strand on it) and the first 'B' brings it live.
    graph_->setZoneCount(QStringLiteral("readerBookmarks"), bridge_->bookmarkCount());
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
    // The chapter list OR the bookmark list (issue #136) expands the top strip when the cursor is on it (a
    // temporary panel over the page, like the classic contents overlay). Re-geometry only on a real open/close
    // transition — either sibling panel occupies the same expanded area.
    const bool open = (zone == QStringLiteral("readerToc") || zone == QStringLiteral("readerBookmarks"));
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
    else if (zone == QStringLiteral("readerBookmarks"))
    {
        bridge_->gotoBookmark(index);   // issue #136: jump to the i-th bookmark (reading order)
        hideChrome();                   // and dismiss the chrome, exactly like a chapter jump
    }
    else if (zone == QStringLiteral("readerSettings"))
    {
        // Every kind now, Book included: the row is plain indexed controls and the host fires the one under the
        // cursor. Book used to be the exception because its single control was a ThemedChoice that owned its own
        // activation; with a real row there is nothing left for that exception to mean.
        bridge_->activateSetting(index);
        armAutoHide();
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
        // A child added later (a reader rebuilding its page) must be watched too, or clicks on it are unseen.
        case QEvent::ChildAdded:
            if (auto* c = qobject_cast<QWidget*>(static_cast<QChildEvent*>(e)->child()))
                c->installEventFilter(this);
            break;
        default:
            break;
        }
    }

    // Mouse events come from whichever widget is under the cursor, and inside a reader that is normally a
    // CHILD — so this deliberately does not require `o` to be the reader itself. Only the top band is claimed:
    // everything below it belongs to the reader, which already turns a page on a click and follows an in-book
    // footnote link, and swallowing those to run a zone map would break both.
    if (themed_ && reader_)
    {
        QWidget* rw = reader_->asWidget();
        QWidget* w = qobject_cast<QWidget*>(o);
        if (rw && w && (w == rw || rw->isAncestorOf(w)))
        {
            switch (e->type())
            {
            case QEvent::MouseButtonPress:
            {
                auto* me = static_cast<QMouseEvent*>(e);
                if (me->button() != Qt::LeftButton) break;
                mouseStart_ = toReaderPos(w, me->position());
                mouseDown_ = claimsClickAt(mouseStart_);
                return mouseDown_;              // consumed ONLY when the press is ours
            }
            case QEvent::MouseButtonRelease:
            {
                auto* me = static_cast<QMouseEvent*>(e);
                if (me->button() != Qt::LeftButton || !mouseDown_) break;
                mouseDown_ = false;
                const QPointF p = toReaderPos(w, me->position());
                if (claimsClickAt(p)
                    && qAbs(p.x() - mouseStart_.x()) < 24.0 && qAbs(p.y() - mouseStart_.y()) < 24.0)
                    tapAt(p);                   // the same zone map a tap runs
                return true;
            }
            case QEvent::MouseMove:
                if (mouseDown_) return true;    // our drag, not the reader's
                break;
            default:
                break;
            }
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
// Where a tap lands and what it means. A BAND ACROSS THE TOP opens the chrome whatever column it falls in —
// that is the gesture every e-reader trains people to expect, and "tap the top of the page for the menu" is not
// a thing anyone qualifies by column. Below the band the page is thirds: outer thirds turn pages, and the
// middle still toggles the chrome, so the older centre-tap habit keeps working.
// The band across the top that opens the menu. Proportional with a floor: set in pixels alone it is a thick
// stripe on a phone and a hairline on a television, and this host runs on both.
qreal ReaderChromeHost::topBandHeight() const
{
    if (!reader_) return 56.0;
    // Exactly the reader's OWN declared inset: the strip where its bar sits and where a book reserves space
    // rather than drawing text. Matching it means the zone is the bar you can see — "click the top bar" — and
    // that it cannot cover a line of text, so an in-book footnote link near the top of a page stays clickable
    // instead of vanishing under an invisible menu zone. A reader that declares nothing gets a usable default.
    const int reserve = reader_->chromeTopReserve();
    return reserve > 0 ? qreal(reserve) : 56.0;
}

// A click arrives in the coordinates of whichever widget received it, and that is usually a CHILD of the
// reader. The zone map is expressed in the reader's own coordinates, so everything is brought there first —
// otherwise the band would be measured from the wrong origin.
QPointF ReaderChromeHost::toReaderPos(QWidget* from, const QPointF& p) const
{
    QWidget* rw = reader_ ? reader_->asWidget() : nullptr;
    if (!rw || !from || from == rw) return p;
    return QPointF(rw->mapFromGlobal(from->mapToGlobal(p.toPoint())));
}

// Filter the reader AND every widget inside it. Qt delivers a mouse event to the child under the cursor, so a
// filter on the parent alone never sees a click at all — which is exactly why the top band answered a finger
// and ignored a mouse. Touch is different (the reader itself accepts it), which is how the difference hid.
void ReaderChromeHost::watchReaderTree()
{
    QWidget* rw = reader_ ? reader_->asWidget() : nullptr;
    if (!rw) return;
    rw->installEventFilter(this);
    for (QWidget* child : rw->findChildren<QWidget*>()) child->installEventFilter(this);
}

// Which clicks are the HOST's. The band always is — that is the menu, and no reader draws content there.
//
// Below it depends on whether the reader handles a click itself. A book does: it turns pages and, before that,
// follows an in-book footnote link, and swallowing those to run a zone map would break both. A pdf and a comic
// do not handle the mouse at all, so without this a finger could page them and a mouse could not — the same
// split that left the menu band answering only touch.
bool ReaderChromeHost::claimsClickAt(const QPointF& pos) const
{
    if (pos.y() <= topBandHeight()) return true;
    return kind_ != ReaderKind::Book;
}

void ReaderChromeHost::tapAt(const QPointF& pos)
{
    QWidget* rw = reader_->asWidget();
    const int w = rw->width(), h = rw->height();
    const qreal band = topBandHeight();

    if (pos.y() <= band)
    {
        if (chromeVisible_) hideChrome(); else revealChrome();
        return;
    }
    if (pos.x() < w / 3.0)            reader_->prevPage();
    else if (pos.x() > 2.0 * w / 3.0) reader_->nextPage();
    else if (chromeVisible_)          hideChrome();
    else                              revealChrome();
}

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
            tapAt(end);
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

    // Add-bookmark affordance (issue #136): 'B' drops a bookmark at the current spot, from anywhere in the
    // reader (chrome shown or hidden). A controller-independent way to invoke the add while the on-screen list
    // panel — the readerBookmarks nav zone + its QML — is still the deferred follow-up. Revealing the chrome
    // afterwards surfaces the confirmation implicitly (the refreshed list) without stealing the page.
    if (key == Qt::Key_B) { bridge_->addBookmark(); return true; }

    if (chromeVisible_)
    {
        // The chrome zones take the keys (spike: "chrome zones activate only when chrome is VISIBLE").
        // Left/Right on the settings zone used to be intercepted to cycle one setting, because the zone was a
        // single-item column whose horizontal arrows were self-pinned no-ops. The zone is a ROW of controls
        // now, so those arrows are its own axis: they step between the controls, and intercepting them would
        // make everything past the first unreachable.
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
