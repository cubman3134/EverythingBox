#include "XmbView.h"

#include <QPainter>
#include <QPaintEvent>
#include <QKeyEvent>
#include <QWheelEvent>
#include <QMouseEvent>
#include <QVariantAnimation>
#include <QEasingCurve>
#include <QFont>
#include <QFontMetrics>
#include <QPainterPath>
#include <QLinearGradient>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QUrl>
#include <cmath>

XmbView::XmbView(QWidget* parent) : QWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    setMinimumHeight(320);
    nam_ = new QNetworkAccessManager(this);

    catAnim_ = new QVariantAnimation(this);
    catAnim_->setDuration(220);
    catAnim_->setEasingCurve(QEasingCurve::OutCubic);
    connect(catAnim_, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
        catPos_ = v.toDouble(); update();
    });

    itemAnim_ = new QVariantAnimation(this);
    itemAnim_->setDuration(200);
    itemAnim_->setEasingCurve(QEasingCurve::OutCubic);
    connect(itemAnim_, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
        itemPos_ = v.toDouble(); update();
    });
}

// ---- categories -----------------------------------------------------------

void XmbView::setCategories(const QVector<XmbEntry>& cats, const QString& activeKey)
{
    cats_ = cats;
    int idx = 0;
    if (!activeKey.isEmpty())
        for (int i = 0; i < cats_.size(); ++i)
            if (cats_[i].navKey == activeKey) { idx = i; break; }
    catIndex_ = qBound(0, idx, qMax(0, cats_.size() - 1));
    catAnim_->stop();
    catPos_ = catIndex_;
    update();
}

void XmbView::setActiveCategory(const QString& navKey)
{
    for (int i = 0; i < cats_.size(); ++i)
        if (cats_[i].navKey == navKey) { if (i != catIndex_) animateCatTo(i); else { catIndex_ = i; } return; }
}

void XmbView::animateCatTo(int index)
{
    index = qBound(0, index, qMax(0, cats_.size() - 1));
    catIndex_ = index;
    catAnim_->stop();
    catAnim_->setStartValue(catPos_);
    catAnim_->setEndValue(double(index));
    catAnim_->start();
}

QString XmbView::activeCategoryKey() const
{
    return (catIndex_ >= 0 && catIndex_ < cats_.size()) ? cats_[catIndex_].navKey : QString();
}

// ---- items ----------------------------------------------------------------

void XmbView::setItems(const QVector<XmbEntry>& items, const QString& selectKey)
{
    items_ = items;
    itemImg_.fill(QPixmap(), items_.size());
    itemReq_.fill(0, items_.size());
    imgQueue_.clear();
    imgActive_ = 0;
    ++gen_;
    int idx = 0;
    if (!selectKey.isEmpty())
        for (int i = 0; i < items_.size(); ++i)
            if (items_[i].navKey == selectKey) { idx = i; break; }
    itemIndex_ = qBound(0, idx, qMax(0, items_.size() - 1));
    itemAnim_->stop();
    itemPos_ = itemIndex_;
    for (int i = 0; i < items_.size(); ++i) queueItemImage(i);
    pumpImages();
    update();
}

void XmbView::addItems(const QVector<XmbEntry>& more)
{
    if (more.isEmpty()) return;
    const int oldN = items_.size();
    items_ += more;
    itemImg_.resize(items_.size());
    itemReq_.resize(items_.size());
    for (int i = oldN; i < items_.size(); ++i) { itemReq_[i] = 0; queueItemImage(i); }
    pumpImages();
    update();
}

void XmbView::clearItems()
{
    items_.clear(); itemImg_.clear(); itemReq_.clear();
    imgQueue_.clear(); imgActive_ = 0; ++gen_;
    itemIndex_ = 0; itemAnim_->stop(); itemPos_ = 0.0;
    update();
}

void XmbView::animateItemTo(int index)
{
    index = qBound(0, index, qMax(0, items_.size() - 1));
    if (index == itemIndex_) return;
    itemIndex_ = index;
    itemAnim_->stop();
    itemAnim_->setStartValue(itemPos_);
    itemAnim_->setEndValue(double(index));
    itemAnim_->start();
    emit currentChanged(itemIndex_, items_.size());
}

QString XmbView::currentItemKey() const
{
    return (itemIndex_ >= 0 && itemIndex_ < items_.size()) ? items_[itemIndex_].navKey : QString();
}

void XmbView::setAtRoot(bool atRoot) { atRoot_ = atRoot; }

// ---- async box art (items only) -------------------------------------------

void XmbView::queueItemImage(int i)
{
    if (i < 0 || i >= items_.size() || itemReq_[i]) return;
    const QString u = items_[i].imageUrl;
    if (u.isEmpty()) { itemReq_[i] = 1; return; }
    itemReq_[i] = 1;
    if (!u.startsWith(QStringLiteral("http"))) { const QPixmap pm(u); if (!pm.isNull()) itemImg_[i] = pm; return; }
    imgQueue_.append(i);
}

void XmbView::pumpImages()
{
    const int kMax = 6;
    while (imgActive_ < kMax && !imgQueue_.isEmpty())
    {
        const int i = imgQueue_.takeFirst();
        if (i < 0 || i >= items_.size()) continue;
        const int g = gen_;
        QNetworkRequest req((QUrl(items_[i].imageUrl)));
        req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("MyMediaVault"));
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        QNetworkReply* reply = nam_->get(req);
        ++imgActive_;
        connect(reply, &QNetworkReply::finished, this, [this, reply, i, g] {
            reply->deleteLater();
            --imgActive_;
            if (g == gen_ && reply->error() == QNetworkReply::NoError && i < itemImg_.size())
            {
                QPixmap pm;
                if (pm.loadFromData(reply->readAll())) { itemImg_[i] = pm; update(); }
            }
            pumpImages();
        });
    }
}

// ---- painting -------------------------------------------------------------

void XmbView::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    if (cats_.isEmpty()) return;

    const double W = width(), H = height();
    const double barY    = H * 0.40;   // horizontal category bar (the cross row)
    const double crossX  = W * 0.24;   // active category / item column (the cross column)
    const double catGap  = 120.0;      // spacing between category tiles
    const double itemX   = crossX;     // item icons left-aligned under the active category
    const double itemTop = barY + 74;  // y of the active (highlighted) item
    const double itemGap = 52.0;

    // ----- the active category's item column (drawn under the bar) -----
    if (!items_.isEmpty())
    {
        for (int j = 0; j < items_.size(); ++j)
        {
            const double iy = itemTop + (double(j) - itemPos_) * itemGap;
            if (iy < barY + 6 || iy > H - 8) continue;        // cull above the bar / off the bottom
            const double dist = std::fabs(double(j) - itemPos_);
            const bool active = dist < 0.5;
            double op = qBound(0.18, 1.0 - 0.26 * dist, 1.0);
            if (iy < itemTop)                                  // fade as items slide up toward the bar
                op *= qBound(0.0, (iy - barY) / (itemTop - barY), 1.0);
            p.setOpacity(op);

            const double sz = active ? 46.0 : 34.0;
            const QRectF icon(itemX, iy - sz / 2, sz, sz);

            if (active) // a translucent highlight bar behind the focused row
            {
                p.setOpacity(op * 0.85);
                QPainterPath hp; hp.addRoundedRect(QRectF(itemX - 10, iy - 26, W - itemX - 24, 52), 10, 10);
                p.fillPath(hp, QColor(255, 255, 255, 28));
                p.setOpacity(op);
            }

            const QPixmap art = (j < itemImg_.size()) ? itemImg_[j] : QPixmap();
            QPainterPath clip; clip.addRoundedRect(icon, 8, 8);
            if (!art.isNull())
            {
                p.save(); p.setClipPath(clip);
                const QPixmap sc = art.scaled(icon.size().toSize(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
                p.drawPixmap(QPointF(icon.center().x() - sc.width() / 2.0, icon.center().y() - sc.height() / 2.0), sc);
                p.restore();
            }
            else
            {
                p.setPen(Qt::NoPen);
                p.setBrush(items_[j].color);
                p.drawPath(clip);
            }

            QFont f = p.font();
            f.setBold(active);
            f.setPointSizeF(active ? 13.0 : 11.0);
            p.setFont(f);
            p.setPen(active ? QColor(255, 255, 255) : QColor(225, 230, 238));
            const QRectF tr(itemX + sz + 14, iy - itemGap / 2, W - (itemX + sz + 14) - 20, itemGap);
            const QString elided = QFontMetrics(f).elidedText(items_[j].label, Qt::ElideRight, int(tr.width()));
            p.drawText(tr, Qt::AlignVCenter | Qt::AlignLeft, elided);
        }
    }
    p.setOpacity(1.0);

    // ----- the horizontal category bar (drawn on top) -----
    for (int i = 0; i < cats_.size(); ++i)
    {
        const double cx = crossX + (double(i) - catPos_) * catGap;
        if (cx < -90 || cx > W + 90) continue;
        const double dist = std::fabs(double(i) - catPos_);
        const bool active = dist < 0.5;
        p.setOpacity(qBound(0.32, 1.0 - 0.30 * dist, 1.0));

        const double sz = active ? 58.0 : 42.0;
        const QRectF tile(cx - sz / 2, barY - sz / 2, sz, sz);
        QPainterPath tp; tp.addRoundedRect(tile, 12, 12);
        p.setPen(Qt::NoPen);
        p.setBrush(cats_[i].color);
        p.drawPath(tp);

        // The initial of the category, centred on the tile, as a simple glyph.
        QFont gf = p.font(); gf.setBold(true); gf.setPointSizeF(active ? 20.0 : 15.0); p.setFont(gf);
        p.setPen(QColor(255, 255, 255, 235));
        p.drawText(tile, Qt::AlignCenter, cats_[i].label.left(1).toUpper());

        if (active)
        {
            QPen ring(QColor(255, 255, 255, 240)); ring.setWidth(3);
            p.setPen(ring); p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(tile.adjusted(2, 2, -2, -2), 10, 10);

            QFont nf = p.font(); nf.setBold(true); nf.setPointSizeF(14.0); p.setFont(nf);
            p.setPen(QColor(255, 255, 255));
            p.drawText(QRectF(cx - 130, barY - sz / 2 - 34, 260, 26), Qt::AlignHCenter | Qt::AlignBottom, cats_[i].label);
        }
    }
    p.setOpacity(1.0);

    // ----- hint line -----
    p.setPen(QColor(170, 178, 190));
    QFont hf = p.font(); hf.setBold(false); hf.setPointSizeF(9); p.setFont(hf);
    const QString hint = atRoot_ ? tr("←  →  category     ↑  ↓  browse     Enter  open")
                                 : tr("↑  ↓  browse     ←  back     Enter  open");
    p.drawText(QRectF(0, H - 26, W, 22), Qt::AlignCenter, hint);
}

// ---- input ----------------------------------------------------------------

void XmbView::keyPressEvent(QKeyEvent* e)
{
    switch (e->key())
    {
    case Qt::Key_Up:
        if (itemIndex_ > 0) animateItemTo(itemIndex_ - 1);
        else emit navUpOffTop(); // already at the top item -> hand off to the top chrome row
        return;
    case Qt::Key_Down: animateItemTo(itemIndex_ + 1); return;
    case Qt::Key_Left:
        if (atRoot_)
        {
            if (catIndex_ > 0) { animateCatTo(catIndex_ - 1); emit categoryChanged(activeCategoryKey()); }
        }
        else emit backRequested(); // drilled into a sub-column: Left backs out
        return;
    case Qt::Key_Right:
        if (atRoot_ && catIndex_ < cats_.size() - 1) { animateCatTo(catIndex_ + 1); emit categoryChanged(activeCategoryKey()); }
        return;
    case Qt::Key_Return: case Qt::Key_Enter: case Qt::Key_Space:
        if (!items_.isEmpty()) emit activated(currentItemKey());
        return;
    case Qt::Key_Backspace: case Qt::Key_Escape:
        emit backRequested();
        return;
    default: QWidget::keyPressEvent(e);
    }
}

void XmbView::wheelEvent(QWheelEvent* e)
{
    const int d = e->angleDelta().y() != 0 ? e->angleDelta().y() : e->angleDelta().x();
    if (d > 0) animateItemTo(itemIndex_ - 1);
    else if (d < 0) animateItemTo(itemIndex_ + 1);
}

void XmbView::mousePressEvent(QMouseEvent* e)
{
    setFocus(Qt::MouseFocusReason); // so the keyboard works after a click
    if (items_.isEmpty()) { QWidget::mousePressEvent(e); return; }

    // Map the click's y to an item using the same column geometry paintEvent draws with: the focused row sits
    // at itemTop, each row itemGap apart, sliding by itemPos_. A click below the category bar selects that row
    // (scrolling it to the cross); clicking the already-focused row opens it.
    const double H = height();
    const double barY    = H * 0.40;
    const double itemTop = barY + 74.0;
    const double itemGap = 52.0;
    const double y = e->position().y();
    if (y <= barY + 6.0) { QWidget::mousePressEvent(e); return; } // not in the item column

    int j = int(std::lround(itemPos_ + (y - itemTop) / itemGap));
    j = qBound(0, j, items_.size() - 1);
    if (j == itemIndex_) emit activated(currentItemKey()); // click the focused item -> open it
    else                 animateItemTo(j);                 // otherwise slide it to the cross (scroll to it)
}

void XmbView::resizeEvent(QResizeEvent*) { update(); }
