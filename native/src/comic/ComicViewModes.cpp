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
#include <QFileInfo>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <iterator>

namespace
{
    const QColor kPageBg(0x15, 0x17, 0x1c);
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
            const QPixmap pm = owner_->stripPixmap(i);
            if (pm.isNull()) continue;
            p.drawPixmap((width() - pm.width()) / 2, top, pm);
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
        stripCache_.clear();
        railFocus_ = false;
    }
    if (railWidget_) railWidget_->setVisible(webtoon && railOn_);
    updateBarButtons();
}

void ComicView::rebuildStrip()
{
    const int vw = qMax(64, scroll_->viewport()->width() - 4);
    strip_ = ComicRead::stripLayout(pageSizes_, vw);
    if (stripCacheWidth_ != vw) { stripCache_.clear(); stripCacheWidth_ = vw; }
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
    if (p == current_) return;

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
// all of it. The decode itself is deferred to the next event-loop turn so a flick does not decode seven pages
// inside the scroll event that started it.
void ComicView::prefetchAround(int page)
{
    const QVector<int> want = ComicRead::prefetchWindow(page, pageTotal());
    for (auto it = stripCache_.begin(); it != stripCache_.end(); )
        it = want.contains(it.key()) ? std::next(it) : stripCache_.erase(it);

    QTimer::singleShot(0, this, [this, want] {
        if (mode_ != ComicRead::Mode::Webtoon) return;
        for (int i : want) stripPixmap(i);
        if (stripWidget_) stripWidget_->update();
    });
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

QPixmap ComicView::stripPixmap(int index)
{
    const auto it = stripCache_.constFind(index);
    if (it != stripCache_.constEnd()) return it.value();
    if (index < 0 || index >= pageTotal()) return QPixmap();
    const QImage img = preparedPage(index, -1);
    if (img.isNull()) return QPixmap();
    const QPixmap pm = QPixmap::fromImage(img.scaledToWidth(qMax(1, strip_.width), Qt::SmoothTransformation));
    stripCache_.insert(index, pm);
    return pm;
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
    stripCache_.clear();
    showPage(current_);
    updateBarButtons();
    emit pageInfoChanged();
}

void ComicView::cycleColorFilter()
{
    if (photoMode_) return;
    filter_ = ComicRead::Filter((int(filter_) + 1) % 5);
    writeOption(ComicRead::Opt::kFilter, int(filter_));
    stripCache_.clear();
    railCache_.clear();
    if (mode_ == ComicRead::Mode::Webtoon) { if (stripWidget_) stripWidget_->update(); if (railWidget_) railWidget_->update(); }
    else showPage(current_);
    updateBarButtons();
    emit pageInfoChanged();
}

void ComicView::toggleThumbnailRail()
{
    if (photoMode_) return;
    railOn_ = !railOn_;
    writeOption(ComicRead::Opt::kRail, railOn_ ? 1 : 0);
    const bool visible = railOn_ && mode_ == ComicRead::Mode::Webtoon;
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
