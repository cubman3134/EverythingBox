#include "CarouselView.h"

#include <QPainter>
#include <QPaintEvent>
#include <QKeyEvent>
#include <QWheelEvent>
#include <QMouseEvent>
#include <QVariantAnimation>
#include <QEasingCurve>
#include <QFont>
#include <QPainterPath>
#include <QLinearGradient>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QUrl>
#include <cmath>

CarouselView::CarouselView(QWidget* parent) : QWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    setMinimumHeight(240);
    nam_ = new QNetworkAccessManager(this);
    anim_ = new QVariantAnimation(this);
    anim_->setDuration(260);
    anim_->setEasingCurve(QEasingCurve::OutCubic);
    connect(anim_, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
        pos_ = v.toDouble();
        update();
    });
}

int CarouselView::wrappedIndex() const
{
    if (entries_.isEmpty()) return 0;
    const int n = entries_.size();
    return ((index_ % n) + n) % n;
}

void CarouselView::queueImage(int i)
{
    if (i < 0 || i >= entries_.size() || requested_[i]) return;
    const QString u = entries_[i].imageUrl;
    if (u.isEmpty()) { requested_[i] = 1; return; }
    requested_[i] = 1;
    if (!u.startsWith(QStringLiteral("http"))) // bundled/local file
    {
        const QPixmap pm(u);
        if (!pm.isNull()) images_[i] = pm;
        return;
    }
    imgQueue_.append(i);
}

void CarouselView::pumpImages()
{
    const int kMax = 6;
    while (imgActive_ < kMax && !imgQueue_.isEmpty())
    {
        const int i = imgQueue_.takeFirst();
        if (i < 0 || i >= entries_.size()) continue;
        const int g = gen_;
        QNetworkRequest req((QUrl(entries_[i].imageUrl)));
        req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("MyMediaVault"));
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        QNetworkReply* reply = nam_->get(req);
        ++imgActive_;
        connect(reply, &QNetworkReply::finished, this, [this, reply, i, g] {
            reply->deleteLater();
            --imgActive_;
            if (g == gen_ && reply->error() == QNetworkReply::NoError && i < images_.size())
            {
                QPixmap pm;
                if (pm.loadFromData(reply->readAll())) { images_[i] = pm; update(); }
            }
            pumpImages();
        });
    }
}

void CarouselView::setEntries(const QVector<CarouselEntry>& entries, const QString& selectKey)
{
    entries_ = entries;
    images_.fill(QPixmap(), entries_.size());
    requested_.fill(0, entries_.size());
    imgQueue_.clear();
    imgActive_ = 0;
    ++gen_;
    int idx = 0;
    if (!selectKey.isEmpty())
        for (int i = 0; i < entries_.size(); ++i)
            if (entries_[i].navKey == selectKey) { idx = i; break; }
    index_ = qBound(0, idx, qMax(0, entries_.size() - 1));
    anim_->stop();
    pos_ = index_;
    for (int i = 0; i < entries_.size(); ++i) queueImage(i);
    pumpImages();
    update();
}

void CarouselView::addEntries(const QVector<CarouselEntry>& more)
{
    if (more.isEmpty()) return;
    const int oldN = entries_.size();
    const int curWrapped = wrappedIndex();
    entries_ += more;
    images_.resize(entries_.size());
    requested_.resize(entries_.size());
    for (int i = oldN; i < entries_.size(); ++i) { requested_[i] = 0; queueImage(i); }
    pumpImages();
    // Re-base so the same entry stays centred under the new modulo (seamless: drawing is modulo-based).
    anim_->stop();
    index_ = curWrapped;
    pos_ = index_;
    update();
}

QString CarouselView::currentKey() const
{
    if (entries_.isEmpty()) return QString();
    const int n = entries_.size();
    return entries_[((index_ % n) + n) % n].navKey; // wrap
}

void CarouselView::setWrap(bool wrap)
{
    wrap_ = wrap;
    if (!wrap_ && !entries_.isEmpty())
    {
        index_ = qBound(0, index_, entries_.size() - 1);
        anim_->stop();
        pos_ = index_;
        update();
    }
}

void CarouselView::goTo(int index, bool animate)
{
    if (entries_.size() <= 1) { index_ = 0; pos_ = 0; update(); return; }
    if (!wrap_) index = qBound(0, index, entries_.size() - 1); // can't go past either end
    if (index == index_ && animate) { return; }
    index_ = index; // wrapped on lookup/draw when wrap_ is on, so the carousel tiles infinitely
    if (animate)
    {
        anim_->stop();
        anim_->setStartValue(pos_);
        anim_->setEndValue(double(index_));
        anim_->start();
    }
    else { pos_ = index_; update(); }
    emit currentChanged(wrappedIndex(), entries_.size());
}

void CarouselView::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    if (entries_.isEmpty()) return;

    const double W = width(), H = height();
    const double cardH = H * 0.56;
    const double cardW = cardH * 0.74;
    const double step  = cardW * 0.70;
    const double cx = W / 2.0, cy = H / 2.0 - 6;

    const int count = int(entries_.size());
    // Visible slots around the centre (wrapping), generated far-to-near so the centre draws on top.
    QVector<int> order;
    if (count == 1)
    {
        order.append(0);
    }
    else
    {
        const int mid = int(std::floor(pos_ + 0.5));
        for (int d = 4; d >= 1; --d) { order.append(mid + d); order.append(mid - d); }
        order.append(mid);
    }

    for (int slot : order)
    {
        const double off = (count == 1) ? 0.0 : (double(slot) - pos_);
        const double ad = std::fabs(off);
        if (ad > 3.6) continue;
        int i;
        if (wrap_) i = ((slot % count) + count) % count; // wrapped entry index
        else { if (slot < 0 || slot >= count) continue; i = slot; } // no wrap: skip past the ends
        const double scale = qMax(0.55, 1.0 - 0.16 * ad);
        const double w = cardW * scale, h = cardH * scale;
        const double x = cx + off * step;
        const QRectF r(x - w / 2, cy - h / 2, w, h);
        const bool centre = (ad < 0.5);
        const QPixmap art = (i < images_.size()) ? images_[i] : QPixmap();

        p.setOpacity(qMax(0.28, 1.0 - 0.24 * ad)); // depth fade
        QPainterPath clip;
        clip.addRoundedRect(r, 16, 16);

        if (!art.isNull())
        {
            // Box art: cover-fit, clipped to the rounded card, with a bottom gradient for the title.
            p.save();
            p.setClipPath(clip);
            const QPixmap sc = art.scaled(r.size().toSize(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
            p.drawPixmap(QPointF(r.center().x() - sc.width() / 2.0, r.center().y() - sc.height() / 2.0), sc);
            const QRectF band(r.left(), r.bottom() - r.height() * 0.36, r.width(), r.height() * 0.36);
            QLinearGradient g(band.topLeft(), band.bottomLeft());
            g.setColorAt(0, QColor(0, 0, 0, 0));
            g.setColorAt(1, QColor(0, 0, 0, 210));
            p.fillRect(band, g);
            QFont f = p.font(); f.setBold(true); f.setPointSizeF(qMax(8.0, 11.0 * scale)); p.setFont(f);
            p.setPen(Qt::white);
            p.drawText(band.adjusted(6, 0, -6, -4), Qt::AlignBottom | Qt::AlignHCenter | Qt::TextWordWrap,
                       entries_[i].label);
            p.restore();
        }
        else
        {
            // No art yet: a coloured tile with the centred label.
            p.setPen(Qt::NoPen);
            p.setBrush(entries_[i].color);
            p.drawPath(clip);
            QFont f = p.font(); f.setBold(true); f.setPointSizeF(qMax(9.0, 15.0 * scale)); p.setFont(f);
            p.setPen(Qt::white);
            p.drawText(r, Qt::AlignCenter | Qt::TextWordWrap, entries_[i].label);
        }

        if (centre) // highlight ring, full opacity
        {
            p.setOpacity(1.0);
            QPen pen(QColor(255, 255, 255, 235));
            pen.setWidth(3);
            p.setPen(pen);
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(r.adjusted(2, 2, -2, -2), 14, 14);
        }
    }
    p.setOpacity(1.0);

    p.setPen(QColor(120, 122, 130));
    QFont hf = p.font();
    hf.setBold(false);
    hf.setPointSizeF(9);
    p.setFont(hf);
    p.drawText(QRectF(0, H - 26, W, 22), Qt::AlignCenter, tr("←  →  spin    ·    Enter to open"));
}

int CarouselView::indexNearestX(int mx) const
{
    if (entries_.size() <= 1) return index_;
    const double W = width(), H = height();
    const double cardH = H * 0.56, cardW = cardH * 0.74, step = cardW * 0.70, cx = W / 2.0;
    const int center = int(std::floor(pos_ + 0.5));
    int best = center;
    double bestd = 1e9;
    for (int s = center - 4; s <= center + 4; ++s) // slot index (unbounded), wrapped on lookup
    {
        const double x = cx + (s - pos_) * step;
        const double d = std::abs(mx - x);
        if (d < bestd) { bestd = d; best = s; }
    }
    return best;
}

void CarouselView::keyPressEvent(QKeyEvent* e)
{
    switch (e->key())
    {
    case Qt::Key_Left:  goTo(index_ - 1, true); return;
    case Qt::Key_Right: goTo(index_ + 1, true); return;
    case Qt::Key_Up:    emit navUp(); return; // hand off to the top chrome row
    case Qt::Key_Return: case Qt::Key_Enter: case Qt::Key_Space:
        if (!entries_.isEmpty()) emit activated(currentKey());
        return;
    case Qt::Key_Backspace: case Qt::Key_Escape:
        emit backRequested();
        return;
    default: QWidget::keyPressEvent(e);
    }
}

void CarouselView::wheelEvent(QWheelEvent* e)
{
    const int d = e->angleDelta().y() != 0 ? e->angleDelta().y() : e->angleDelta().x();
    if (d > 0) goTo(index_ - 1, true);
    else if (d < 0) goTo(index_ + 1, true);
}

void CarouselView::mousePressEvent(QMouseEvent* e)
{
    if (entries_.isEmpty()) return;
    const int n = entries_.size();
    const int s = indexNearestX(int(e->position().x()));
    if (((s % n) + n) % n == ((index_ % n) + n) % n) emit activated(currentKey()); // clicked the centre
    else goTo(s, true);
}

void CarouselView::resizeEvent(QResizeEvent*) { update(); }
