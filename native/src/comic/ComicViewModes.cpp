// THE READING MODES, ATTACHED TO THE READER (issue #154, increment 1).
//
// ReadingModes.h holds every DECISION as a free function; this file is the reader half — the two surfaces the
// webtoon mode adds (a continuous strip and a thumbnail rail), the five per-series controls, and the plumbing
// that turns a scroll bar into a page number. It is its own TU for the reason MainWindow's features are
// (#186): ComicView.cpp is a file several branches touch at once, and a feature that can be a separate object
// should be one. Only the declarations are in ComicView.h.
//
// WHAT IT DOES NOT DO: it never opens an archive, never fetches a page, and never asks where a page came
// from. The page seam (PageSupply.h) is upstream of all of this — a local CBZ (#134), an addon's page list
// (#188) and an OPDS-PSE stream (#153) are already one list of encoded images by the time anything here runs,
// which is why the webtoon path costs those three suppliers nothing.
#include "ComicView.h"
#include "ReadingModes.h"
#include "../core/ConsumptionStats.h"
#include "../core/Settings.h"

#include <QBuffer>
#include <QColor>
#include <QCoreApplication>
#include <QFileInfo>
#include <QFont>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QPointer>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QThreadPool>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <atomic>
#include <iterator>

namespace
{
    const QColor kPageBg(0x15, 0x17, 0x1c);
    const QColor kPlaceholderBg(0x26, 0x2B, 0x33);    // #286: a page still being decoded — visibly not the gutter
    const QColor kPlaceholderText(0x7A, 0x86, 0x94);
    const QColor kRailBg(0x0E, 0x12, 0x18);
    const QColor kRailEdge(0x22, 0x30, 0x3C);
    const QColor kRailSel(0x3B, 0x71, 0xB0);

    constexpr int kRailWidth   = 116;   // the whole rail, including its padding
    constexpr int kRailThumbW  = 96;
    constexpr int kRailThumbH  = 120;   // a webtoon page is far taller than this; the thumbnail is its TOP
    constexpr int kRailSlotH   = kRailThumbH + 22;   // + the page number under it
}

// ---- The continuous strip --------------------------------------------------------------------------------
// Every page, width-fitted and stacked edge to edge, painted straight out of the reader's prepared-page cache.
// There is deliberately no gap, no border and no page number between two pages: a long strip is ONE drawing
// that happens to have been cut into files, and any furniture between the pieces is a seam its artist did not
// draw. Only the pages that intersect the exposed rectangle are asked for, so the paint cost is a screenful
// however long the chapter is.
//
// #286: AND THE PAINT NEVER DECODES. It draws what the workers have already delivered; a page that is not ready
// yet is a flat placeholder of EXACTLY its laid-out height (from the image header, like every other offset in
// the strip), so nothing below it moves when the real page lands and only that page's rectangle is repainted.
class ComicStripWidget : public QWidget
{
public:
    explicit ComicStripWidget(ComicView* owner) : QWidget(owner), owner_(owner) {}

protected:
    void paintEvent(QPaintEvent* e) override
    {
        QPainter p(this);
        p.fillRect(e->rect(), kPageBg);
        const ComicRead::Strip& s = owner_->strip_;
        const int y0 = e->rect().top(), y1 = e->rect().bottom();
        for (int i = 0; i < s.count(); ++i)
        {
            const int top = s.tops[i];
            if (top + s.heights[i] - 1 < y0) continue;
            if (top > y1) break;                       // the strip is in order, so the first miss ends it
            const QPixmap pm = owner_->stripReady(i);
            if (!pm.isNull())
            {
                p.drawPixmap((width() - pm.width()) / 2, top, pm);
                continue;
            }
            paintPlaceholder(p, e->rect(), i, QRect((width() - s.width) / 2, top, s.width, s.heights[i]));
        }
    }

private:
    // The page number is repeated once per viewport height down the placeholder, at positions fixed in STRIP
    // coordinates: a webtoon page is many screens tall, so a single centred label would usually be off screen,
    // and a label that followed the viewport would be smeared by the scroll area's blit as it moved.
    void paintPlaceholder(QPainter& p, const QRect& exposed, int index, const QRect& slot)
    {
        p.fillRect(slot & exposed, kPlaceholderBg);
        QFont f = font();
        if (f.pointSizeF() > 0) f.setPointSizeF(f.pointSizeF() * 0.9);
        p.setFont(f);
        p.setPen(kPlaceholderText);
        const int step = qMax(200, owner_->scroll_->viewport()->height());
        const QString label = QString::number(index + 1);
        for (int y = slot.top() + qMin(slot.height(), step) / 2; y < slot.top() + slot.height(); y += step)
        {
            const QRect r(slot.left(), y - 12, slot.width(), 24);
            if (r.intersects(exposed)) p.drawText(r, Qt::AlignCenter, label);
        }
    }

private:
    ComicView* owner_;
};

// ---- The thumbnail rail ----------------------------------------------------------------------------------
// Long-strip's weakness is that "page 40 of 60" stops meaning anything, so the rail is how you navigate a
// chapter you are part-way through. It is a plain painted list rather than a QListWidget because it holds one
// picture per page and needs exactly three behaviours: draw what is visible, highlight where you are, and
// jump where you press.
class ComicRailWidget : public QWidget
{
public:
    explicit ComicRailWidget(ComicView* owner) : QWidget(owner), owner_(owner)
    {
        setFixedWidth(kRailWidth);
        setCursor(Qt::PointingHandCursor);
    }

    void setCurrent(int i)
    {
        current_ = i;
        ensureVisible();
        update();
    }
    int current() const { return current_; }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.fillRect(rect(), kRailBg);
        p.setPen(QPen(kRailEdge, 1));
        p.drawLine(0, 0, 0, height());

        const int total = owner_->pageTotal();
        const int first = qMax(0, offset_ / kRailSlotH);
        const int last  = qMin(total - 1, (offset_ + height()) / kRailSlotH);
        for (int i = first; i <= last; ++i)
        {
            const int y = i * kRailSlotH - offset_;
            const QRect slot(2, y, width() - 4, kRailSlotH - 4);
            if (i == current_)
            {
                p.fillRect(slot, QColor(kRailSel.red(), kRailSel.green(), kRailSel.blue(),
                                        owner_->railFocus_ ? 150 : 80));
            }
            const QPixmap pm = owner_->railThumb(i);
            if (!pm.isNull())
                p.drawPixmap(slot.left() + (slot.width() - pm.width()) / 2, y + 4, pm);
            p.setPen(QColor(0xC7, 0xD0, 0xDA));
            p.drawText(QRect(slot.left(), y + kRailThumbH + 6, slot.width(), 16),
                       Qt::AlignHCenter | Qt::AlignVCenter, QString::number(i + 1));
        }
    }

    void mousePressEvent(QMouseEvent* e) override
    {
        const int i = (int(e->position().y()) + offset_) / kRailSlotH;
        if (i < 0 || i >= owner_->pageTotal()) return;
        owner_->railFocus_ = true;
        owner_->railIndex_ = i;
        setCurrent(i);
        owner_->scrollToPosition(i, 0.0);
        owner_->updateLabel();
        emit owner_->pageInfoChanged();
    }

    void wheelEvent(QWheelEvent* e) override
    {
        offset_ = qBound(0, offset_ - e->angleDelta().y(), maxOffset());
        update();
    }

    void resizeEvent(QResizeEvent*) override { ensureVisible(); }

private:
    int maxOffset() const { return qMax(0, owner_->pageTotal() * kRailSlotH - height()); }

    // Keep the highlighted slot on screen — scrolled to the top edge when it is above the view and to the
    // bottom when it is below, so a D-pad walk down the rail scrolls one slot at a time rather than jumping.
    void ensureVisible()
    {
        const int top = current_ * kRailSlotH;
        if (top < offset_) offset_ = top;
        else if (top + kRailSlotH > offset_ + height()) offset_ = top + kRailSlotH - height();
        offset_ = qBound(0, offset_, maxOffset());
    }

    ComicView* owner_;
    int current_ = 0;
    int offset_ = 0;
};

// ---- Off-paint decoding (#286) -----------------------------------------------------------------------------
// What a queued job may read of the view: NOTHING of ComicView itself, only this block, which the view and
// every job share by value. The view writes it on the GUI thread; a job reads it when it starts (and again
// after the decode, the expensive half) and stands down if its generation is gone or its page left the window.
// The bounds are ONE atomic, packed, so a job never reads a first from one window and a last from another.
struct ComicView::StripDecodeShared
{
    std::atomic<quint64> generation{ 1 };
    std::atomic<quint64> bounds{ pack(0, -1) };

    static quint64 pack(int first, int last)
    {
        return (quint64(quint32(first)) << 32) | quint64(quint32(last));
    }
    bool wanted(int page, quint64 jobGeneration) const
    {
        const quint64 b = bounds.load();
        return ComicRead::stripJobWanted(page, jobGeneration, generation.load(),
                                         int(qint32(quint32(b >> 32))), int(qint32(quint32(b))));
    }
};

// ---- Construction ----------------------------------------------------------------------------------------

void ComicView::installModeControls()
{
    auto* bar = qobject_cast<QHBoxLayout*>(bar_->layout());
    if (!bar) return;
    const auto add = [this, bar](QPushButton*& btn, void (ComicView::*slot)()) {
        btn = new QPushButton(this);
        connect(btn, &QPushButton::clicked, this, slot);
        bar->insertWidget(bar->count() - 3, btn);   // before the Prev / page label / Next group
    };
    add(modeBtn_,   &ComicView::cycleReadingMode);
    add(splitBtn_,  &ComicView::cycleSplitOverride);
    add(cropBtn_,   &ComicView::toggleBorderCrop);
    add(filterBtn_, &ComicView::cycleColorFilter);
    add(railBtn_,   &ComicView::toggleThumbnailRail);
    updateBarButtons();
}

void ComicView::installModeSurfaces(QVBoxLayout* column)
{
    stripWidget_ = new ComicStripWidget(this);
    stripWidget_->hide();   // explicitly, or showing the reader would show it over the paged page
    railWidget_ = new ComicRailWidget(this);
    railWidget_->setVisible(false);

    auto* row = new QWidget(this);
    auto* h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(0);
    h->addWidget(scroll_, 1);
    h->addWidget(railWidget_, 0);
    column->addWidget(row, 1);

    connect(scroll_->verticalScrollBar(), &QScrollBar::valueChanged, this, &ComicView::onStripScrolled);

    // #286: the strip's decode pool. DEDICATED, not QThreadPool::globalInstance() — MetaCache, the store backend
    // and others queue work there, and a chapter of 12000-px decodes must neither wait behind them nor starve
    // them. TWO threads: the landed page and the one after it decode side by side (the pair a reader sees first
    // after a jump), while the GUI thread, mpv and the rest of the app keep cores of their own; and each job
    // transiently holds a full-size decode plus its strip-width copy (~85 MB for a 900x9000 page), so two is
    // also the memory ceiling of a flick. More threads would only decode pages further from the reader sooner.
    stripShared_ = std::make_shared<StripDecodeShared>();
    stripPool_ = new QThreadPool(this);
    stripPool_->setObjectName(QStringLiteral("ComicStripDecode"));
    stripPool_->setMaxThreadCount(2);
}

ComicView::~ComicView()
{
    // No live generation is ever 0, so every job still queued stands down the moment it starts, and one that is
    // mid-decode stands down before it scales. clear() drops the ones that have not started at all; waitForDone
    // then waits only for the (at most two) running decodes. Their deliveries hold a QPointer to this view and
    // are posted to the application object, so one that lands after this destructor finds a null pointer.
    if (stripShared_) stripShared_->generation.store(0);
    if (stripPool_)
    {
        stripPool_->clear();
        stripPool_->waitForDone();
    }
}

// ---- The per-series options ------------------------------------------------------------------------------

void ComicView::readDisplayOptions()
{
    const auto opt = [this](const char* k) {
        return Settings::comicDisplayOption(seriesKey_, QString::fromLatin1(k));
    };
    const int sp = opt(ComicRead::Opt::kSplit);
    split_  = sp == 1 ? ComicRead::Split::Always : sp == 2 ? ComicRead::Split::Never : ComicRead::Split::Auto;
    const int fl = opt(ComicRead::Opt::kFilter);
    filter_ = (fl >= 1 && fl <= 4) ? ComicRead::Filter(fl) : ComicRead::Filter::None;
    crop_   = opt(ComicRead::Opt::kCrop) == 1;
    railOn_ = opt(ComicRead::Opt::kRail) == 1;
}

void ComicView::writeOption(const char* option, int value)
{
    Settings::setComicDisplayOption(seriesKey_, QString::fromLatin1(option), value);
}

// ---- Mode application ------------------------------------------------------------------------------------

void ComicView::applyMode()
{
    // The direction IS the mode now: #152's rtl_ (which arrow advances, which side of a spread a page is
    // drawn on) is derived from it rather than kept beside it, so the two can never disagree.
    rtl_ = ComicRead::isRtl(mode_);
    const bool webtoon = (mode_ == ComicRead::Mode::Webtoon) && !photoMode_;

    QWidget* want = webtoon ? static_cast<QWidget*>(stripWidget_) : static_cast<QWidget*>(imageLabel_);
    if (scroll_->widget() != want)
    {
        // takeWidget() hands ownership back and clears the parent, so the widget we are NOT showing is
        // re-adopted here — otherwise it would be a top-level window the moment anything showed it.
        if (QWidget* old = scroll_->takeWidget()) { old->setParent(this); old->hide(); }
        scroll_->setWidget(want);
        want->show();
    }

    if (webtoon)
    {
        half_ = -1;        // a long strip has no spreads, so it has no halves
        twoUp_ = false;    // ... and no open book either
        relayoutStrip();
    }
    else
    {
        // #286: leaving the strip empties the window too, so every decode still queued for it stands down.
        clearStripCache();
        stripWindow_.clear();
        stripLastVisible_ = -1;
        publishStripWindow();
        railFocus_ = false;
    }
    if (railWidget_) railWidget_->setVisible(ComicRead::railShown(webtoon, railOn_));
    updateBarButtons();
}

// #397: which of this reader's pixels are its own controls, for the themed host's pointer filter. The geometry
// is read live and mapped into this widget's coordinates; the decision is ComicRead::ownsPointerAt, which the
// probe drives. The rail's "shown" is the same rule applyMode shows it by, not the widget's visibility, so the
// answer does not depend on whether the reader itself is on screen yet.
bool ComicView::ownsPointerAt(const QPoint& readerPos) const
{
    const auto rectIn = [this](const QWidget* w) {
        return w ? QRect(w->mapTo(this, QPoint(0, 0)), w->size()) : QRect();
    };
    ComicRead::PointerControls c;
    c.railShown = railWidget_ && ComicRead::railShown(mode_ == ComicRead::Mode::Webtoon && !photoMode_, railOn_);
    c.rail = rectIn(railWidget_);
    if (scroll_)
    {
        const QScrollBar* vb = scroll_->verticalScrollBar();
        const QScrollBar* hb = scroll_->horizontalScrollBar();
        c.vBarShown = vb && vb->isVisibleTo(this);
        c.vBar = rectIn(vb);
        c.hBarShown = hb && hb->isVisibleTo(this);
        c.hBar = rectIn(hb);
    }
    return ComicRead::ownsPointerAt(readerPos, c);
}

void ComicView::rebuildStrip()
{
    const int vw = qMax(64, scroll_->viewport()->width() - 4);
    strip_ = ComicRead::stripLayout(pageSizes_, vw);
    if (stripCacheWidth_ != vw) { clearStripCache(); stripCacheWidth_ = vw; }   // #286: a new generation too
    if (stripWidget_)
    {
        stripWidget_->resize(qMax(vw, scroll_->viewport()->width()), qMax(1, strip_.totalHeight));
        stripWidget_->update();
    }
}

// A relayout must not move the reader. current_ + resumeFraction_ are kept in READING terms (a page and how
// far into it), never in pixels, so a rotation, a window resize or the themed host settling on its real width
// lands on the same drawing at a different scale — which is exactly the property the stored resume needs.
void ComicView::relayoutStrip()
{
    const int page = current_;
    const double frac = resumeFraction_;
    rebuildStrip();
    scrollToPosition(page, frac);
}

void ComicView::scrollToPosition(int page, double fraction)
{
    if (mode_ != ComicRead::Mode::Webtoon) return;
    current_ = qBound(0, page, qMax(0, pageTotal() - 1));
    resumeFraction_ = qBound(0.0, fraction, 1.0);
    inScrollUpdate_ = true;
    scroll_->verticalScrollBar()->setValue(ComicRead::stripOffset(strip_, current_, resumeFraction_));
    inScrollUpdate_ = false;
    prefetchAround(current_);
    if (railWidget_ && !railFocus_) railWidget_->setCurrent(current_);
}

void ComicView::scrollByViewport(int dir)
{
    QScrollBar* sb = scroll_->verticalScrollBar();
    const int step = qMax(1, int(scroll_->viewport()->height() * ComicRead::kScrollFraction));
    sb->setValue(sb->value() + dir * step);   // valueChanged -> onStripScrolled() does the bookkeeping
}

void ComicView::onStripScrolled()
{
    if (inScrollUpdate_ || mode_ != ComicRead::Mode::Webtoon || photoMode_) return;
    int p = 0; double f = 0.0;
    ComicRead::stripPositionAt(strip_, scroll_->verticalScrollBar()->value(), &p, &f);
    resumeFraction_ = f;          // every scroll, not just every page: this IS the resume position
    if (p == current_)
    {
        // #286: the page at the top has not changed, but the bottom edge may have crossed onto a page the
        // window does not reach yet (only possible with pages shorter than a third of the viewport).
        if (ComicRead::stripLastVisible(strip_, scroll_->verticalScrollBar()->value(),
                                        scroll_->viewport()->height()) != stripLastVisible_)
            prefetchAround(p);
        return;
    }

    current_ = p;
    prefetchAround(p);
    if (railWidget_ && !railFocus_) railWidget_->setCurrent(p);
    updateLabel();
    ConsumptionStats::addPagesRead(path_, current_ + 1, QFileInfo(path_).fileName());
    emit readingProgress(QFileInfo(path_).completeBaseName(),
                         tr("Reading · p. %1 of %2").arg(current_ + 1).arg(pageTotal()));
    emit pageInfoChanged();
    if (comicPastEnd(current_, pageTotal())) emit reachedLastPage();
}

// The +/-3 window, and the eviction that makes it a window rather than a leak: a chapter of full-width
// pixmaps is tens of megabytes, and a reader who scrolls to the end of a 200-page strip would otherwise hold
// all of it. #286: the decode is no longer done here or on the next event-loop turn — the window is handed to
// the workers (requestStripPages), and whatever they were still going to do for the previous window stands down.
void ComicView::prefetchAround(int page)
{
    stripLastVisible_ = ComicRead::stripLastVisible(strip_, scroll_->verticalScrollBar()->value(),
                                                    scroll_->viewport()->height());
    stripWindow_ = ComicRead::stripWindow(strip_, page, stripLastVisible_);
    for (auto it = stripCache_.begin(); it != stripCache_.end(); )
        it = stripWindow_.contains(it.key()) ? std::next(it) : stripCache_.erase(it);
    publishStripWindow();
    requestStripPages();
}

// ---- The strip's workers (#286) --------------------------------------------------------------------------

// Every clear of the strip cache is a new generation: whatever a worker is still preparing was prepared for
// pages (or a width, or a filter) that are gone, and ComicRead::acceptStripResult drops it when it lands.
void ComicView::clearStripCache()
{
    stripCache_.clear();
    ++stripGen_;
    publishStripWindow();
}

void ComicView::publishStripWindow()
{
    if (!stripShared_) return;
    int first = 0, last = -1;
    if (!stripWindow_.isEmpty())
    {
        first = *std::min_element(stripWindow_.cbegin(), stripWindow_.cend());
        last  = *std::max_element(stripWindow_.cbegin(), stripWindow_.cend());
    }
    stripShared_->bounds.store(StripDecodeShared::pack(first, last));
    stripShared_->generation.store(stripGen_);
}

void ComicView::requestStripPages()
{
    if (!stripPool_ || !stripShared_ || mode_ != ComicRead::Mode::Webtoon || photoMode_) return;
    QSet<int> cached;
    for (auto it = stripCache_.cbegin(); it != stripCache_.cend(); ++it) cached.insert(it.key());
    const QVector<int> ask = ComicRead::stripRequests(stripWindow_, cached, stripInFlight_, stripGen_);
    if (ask.isEmpty()) return;

    // Everything a job needs, BY VALUE: the encoded bytes (implicitly shared, so this is a reference count and
    // not a copy), the options, the width, the page, the generation, the shared block and a guarded pointer
    // back. A job never dereferences the view; it only hands its result to the application object's queue.
    const ComicRead::PageOptions opts = optionsFor(-1);
    const int width = qMax(1, strip_.width);
    const quint64 gen = stripGen_;
    const std::shared_ptr<StripDecodeShared> shared = stripShared_;
    const QPointer<ComicView> self(this);
    for (int page : ask)
    {
        if (page < 0 || page >= pages_.size()) continue;
        stripInFlight_.insert(page, gen);
        const QByteArray bytes = pages_[page];
        stripPool_->start([bytes, opts, width, page, gen, shared, self] {
            QImage out;
            bool skipped = true;
            if (shared->wanted(page, gen))
            {
                QImage img;
                img.loadFromData(bytes);
                if (shared->wanted(page, gen))      // the decode is the long half: look again before scaling
                {
                    skipped = false;
                    if (!img.isNull()) img = ComicRead::preparePage(img, opts);
                    if (!img.isNull())
                    {
                        out = img.scaledToWidth(width, Qt::SmoothTransformation);
                        // In the format a raster QPixmap wraps as-is, so the GUI thread's fromImage is not a copy.
                        const QImage::Format fmt = out.hasAlphaChannel() ? QImage::Format_ARGB32_Premultiplied
                                                                         : QImage::Format_RGB32;
                        if (out.format() != fmt) out = out.convertToFormat(fmt);
                    }
                }
            }
            // A posted call, not a signal: it runs on the GUI thread on a later event-loop turn of its own, never
            // inside some other emission, and the view it names may have been closed and destroyed by then.
            QMetaObject::invokeMethod(QCoreApplication::instance(), [self, page, gen, width, out, skipped] {
                if (ComicView* v = self.data()) v->onStripPageReady(page, gen, width, out, skipped);
            }, Qt::QueuedConnection);
        });
    }
}

void ComicView::onStripPageReady(int page, quint64 generation, int width, const QImage& image, bool skipped)
{
    // The in-flight mark is cleared only by the request that set it: an answer from an older generation must not
    // unmark the newer request for the same page that is still running.
    const auto it = stripInFlight_.find(page);
    if (it != stripInFlight_.end() && it.value() == generation) stripInFlight_.erase(it);
    if (mode_ != ComicRead::Mode::Webtoon || photoMode_) return;

    const ComicRead::StripResult r{ page, generation, width };
    if (skipped || !ComicRead::acceptStripResult(r, stripGen_, qMax(1, strip_.width), stripWindow_))
    {
        // Dropped. If the page is still wanted under the CURRENT state (a job that stood down against a window
        // that has since come back, say), this asks for it again; otherwise it asks for nothing, because every
        // page it could name is already cached or in flight.
        requestStripPages();
        return;
    }
    // A page that would not decode is cached as a null pixmap: it stays a placeholder and is not asked for again.
    stripCache_.insert(page, image.isNull() ? QPixmap() : QPixmap::fromImage(image));
    if (stripWidget_ && page < strip_.count())
        stripWidget_->update(QRect(0, strip_.tops[page], stripWidget_->width(), strip_.heights[page]));
}

void ComicView::currentPosition(int* page, double* fraction) const
{
    if (page) *page = current_;
    if (fraction) *fraction = 0.0;
    if (mode_ != ComicRead::Mode::Webtoon || strip_.count() == 0) return;
    ComicRead::stripPositionAt(strip_, scroll_->verticalScrollBar()->value(), page, fraction);
}

// ---- One page, prepared ----------------------------------------------------------------------------------

QSize ComicView::rawPageSize(int index) const
{
    return (index >= 0 && index < pageSizes_.size()) ? pageSizes_[index] : QSize();
}

// THE SPLIT DECISION IS MADE ON THE PAGE'S OWN PROPORTIONS, not on the cropped ones — the size comes out of
// the image header, so asking costs no decode and a 300-page comic can be asked about any page at any time.
// A scan margin is roughly even on all four sides, so it moves the ratio by a few percent and never carries a
// real double spread (1.4:1 and wider) back across 1.0. The CROP still happens first when the page is
// actually prepared, so the halves are halves of the art rather than halves of the paper.
bool ComicView::pageSplits(int index) const
{
    if (photoMode_ || !ComicRead::isPaged(mode_)) return false;
    return ComicRead::shouldSplit(split_, rawPageSize(index), scroll_->viewport()->size());
}

ComicRead::PageOptions ComicView::optionsFor(int half) const
{
    ComicRead::PageOptions o;
    // Crop is a PAGED-mode correction: see ReadingModes.h on why a webtoon's geometry has to stay fixed.
    o.crop = crop_ && !photoMode_ && ComicRead::isPaged(mode_);
    o.half = half;
    o.rtl  = rtl_;
    o.adjust = ComicRead::adjustFor(filter_);
    return o;
}

QImage ComicView::preparedPage(int index, int half) const
{
    const QImage src = decodeAt(index);
    if (src.isNull()) return src;
    return ComicRead::preparePage(src, optionsFor(half));
}

// #286: a cache read and nothing else. The paint path calls this for every page on screen, so it must never be
// the thing that decodes — a page that is not here yet is a placeholder, and the workers are already on it.
QPixmap ComicView::stripReady(int index) const
{
    return stripCache_.value(index);
}

// A rail thumbnail is decoded SCALED (QImageReader::setScaledSize), so a 800x12000 webtoon page costs a
// thumbnail rather than a full decode, and only its top is kept: a strip page reduced to 120px tall would be
// a smear, and the top of it is the part you recognise.
QPixmap ComicView::railThumb(int index)
{
    const auto it = railCache_.constFind(index);
    if (it != railCache_.constEnd()) return it.value();
    if (index < 0 || index >= pages_.size()) return QPixmap();

    QBuffer buf;
    buf.setData(pages_[index]);
    if (!buf.open(QIODevice::ReadOnly)) return QPixmap();
    QImageReader reader(&buf);
    const QSize sz = reader.size();
    if (sz.width() > 0 && sz.height() > 0)
        reader.setScaledSize(QSize(kRailThumbW, qMax(1, sz.height() * kRailThumbW / sz.width())));
    QImage img = reader.read();
    if (img.isNull()) return QPixmap();
    if (img.height() > kRailThumbH) img = img.copy(0, 0, img.width(), kRailThumbH);
    img = ComicRead::applyAdjust(img, ComicRead::adjustFor(filter_));
    const QPixmap pm = QPixmap::fromImage(img);
    railCache_.insert(index, pm);
    return pm;
}

// ---- The five controls -----------------------------------------------------------------------------------

void ComicView::cycleReadingMode()
{
    if (photoMode_) return;   // a folder of photographs has no reading mode (#102)
    mode_ = mode_ == ComicRead::Mode::PagedLtr ? ComicRead::Mode::PagedRtl
          : mode_ == ComicRead::Mode::PagedRtl ? ComicRead::Mode::Webtoon
                                               : ComicRead::Mode::PagedLtr;
    writeOption(ComicRead::Opt::kMode, int(mode_));
    // Entering the strip lands on the TOP of the page you were reading rather than at a fraction carried over
    // from a different geometry; leaving it keeps the page you had scrolled to.
    resumeFraction_ = 0.0;
    applyMode();
    if (ComicRead::isPaged(mode_)) showPage(current_);
    updateLabel();
    emit pageInfoChanged();
}

void ComicView::cycleSplitOverride()
{
    if (photoMode_) return;
    split_ = split_ == ComicRead::Split::Auto   ? ComicRead::Split::Always
           : split_ == ComicRead::Split::Always ? ComicRead::Split::Never
                                                : ComicRead::Split::Auto;
    writeOption(ComicRead::Opt::kSplit, int(split_));
    showPage(current_);        // re-enters the page, which re-asks whether it splits
    updateBarButtons();
    emit pageInfoChanged();
}

void ComicView::toggleBorderCrop()
{
    if (photoMode_) return;
    crop_ = !crop_;
    writeOption(ComicRead::Opt::kCrop, crop_ ? 1 : 0);
    clearStripCache();   // #286: a new generation (showPage below re-requests the window in webtoon mode)
    showPage(current_);
    updateBarButtons();
    emit pageInfoChanged();
}

void ComicView::cycleColorFilter()
{
    if (photoMode_) return;
    filter_ = ComicRead::Filter((int(filter_) + 1) % 5);
    writeOption(ComicRead::Opt::kFilter, int(filter_));
    clearStripCache();   // #286: a new generation, so a page still being tinted the old way is dropped
    railCache_.clear();
    if (mode_ == ComicRead::Mode::Webtoon)
    {
        requestStripPages();   // the paint no longer decodes, so the re-tinted window has to be asked for
        if (stripWidget_) stripWidget_->update();
        if (railWidget_) railWidget_->update();
    }
    else showPage(current_);
    updateBarButtons();
    emit pageInfoChanged();
}

void ComicView::toggleThumbnailRail()
{
    if (photoMode_) return;
    railOn_ = !railOn_;
    writeOption(ComicRead::Opt::kRail, railOn_ ? 1 : 0);
    const bool visible = ComicRead::railShown(mode_ == ComicRead::Mode::Webtoon, railOn_);   // photoMode_ returned above
    if (railWidget_)
    {
        railWidget_->setVisible(visible);
        railWidget_->setCurrent(current_);
    }
    // Turning it on hands it the key cursor, which is the only way a D-pad can reach it: the rail is inside
    // the reader (see ComicView.h), so it is not a themed NavGraph zone and the chrome cannot walk into it.
    railFocus_ = visible;
    railIndex_ = current_;
    if (mode_ == ComicRead::Mode::Webtoon) relayoutStrip();   // the strip just got narrower (or wider)
    updateBarButtons();
    emit pageInfoChanged();
}

void ComicView::setRailFocus(bool on)
{
    railFocus_ = on;
    if (railWidget_) railWidget_->update();   // the highlight is drawn brighter while the rail has the cursor
}

void ComicView::railStep(int delta)
{
    if (pageTotal() <= 0) return;
    railIndex_ = qBound(0, railIndex_ + delta, pageTotal() - 1);
    if (railWidget_) railWidget_->setCurrent(railIndex_);
}

// ---- The labels, once, for both layouts ------------------------------------------------------------------

QStringList ComicView::comicControlLabels() const
{
    if (photoMode_) return {};   // a photo folder has no series and nothing to remember
    const QStringList filterNames{ tr("None"), tr("Grey"), tr("Sepia"), tr("Night"), tr("Contrast") };
    const QString modeText = mode_ == ComicRead::Mode::Webtoon ? tr("Webtoon")
                           : mode_ == ComicRead::Mode::PagedRtl ? tr("Paged R→L")
                                                                : tr("Paged L→R");
    const QString splitText = split_ == ComicRead::Split::Always ? tr("Split: on")
                            : split_ == ComicRead::Split::Never  ? tr("Split: off")
                                                                 : tr("Split: auto");
    return QStringList{ modeText,
                        splitText,
                        tr("Crop"),
                        tr("Filter: %1").arg(filterNames[qBound(0, int(filter_), 4)]),
                        tr("Rail") };
}

QVector<bool> ComicView::comicControlActive() const
{
    if (photoMode_) return {};
    return QVector<bool>{ mode_ != ComicRead::Mode::PagedLtr,
                          split_ != ComicRead::Split::Auto,
                          crop_,
                          filter_ != ComicRead::Filter::None,
                          railOn_ };
}

// The half of the page label the number cannot carry. The classic bar builds its own sentence around the
// same two facts (updateLabel), and the themed chrome appends this to the "N / M" the bridge draws — one
// answer, both layouts, rather than a label that reads differently depending on which chrome is up.
QString ComicView::pageLabelNote() const
{
    if (photoMode_) return QString();
    if (mode_ == ComicRead::Mode::Webtoon) return tr("(approx)");
    if (half_ >= 0) return tr("· half %1 of 2").arg(half_ + 1);
    return QString();
}

void ComicView::comicActivateControl(int index)
{
    switch (index)
    {
    case 0: cycleReadingMode();   break;
    case 1: cycleSplitOverride(); break;
    case 2: toggleBorderCrop();   break;
    case 3: cycleColorFilter();   break;
    case 4: toggleThumbnailRail(); break;
    default: break;
    }
}

void ComicView::updateBarButtons()
{
    if (!modeBtn_) return;
    const QStringList labels = comicControlLabels();
    QPushButton* const btns[5] = { modeBtn_, splitBtn_, cropBtn_, filterBtn_, railBtn_ };
    const QVector<bool> active = comicControlActive();
    for (int i = 0; i < 5; ++i)
    {
        if (!btns[i]) continue;
        const bool have = i < labels.size();
        btns[i]->setVisible(have);
        if (!have) continue;
        btns[i]->setText(labels[i]);
        btns[i]->setCheckable(true);
        btns[i]->setChecked(i < active.size() && active[i]);
    }
    // The rail belongs to the strip: outside webtoon mode there is nothing for it to index.
    if (railBtn_) railBtn_->setEnabled(mode_ == ComicRead::Mode::Webtoon && !photoMode_);
}
