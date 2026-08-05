#include "AttractOverlay.h"

#include <QApplication>
#include <QEvent>
#include <QFileInfo>
#include <QLinearGradient>
#include <QPainter>
#include <QTimer>
#include <QtMath>

AttractOverlay::AttractOverlay(QWidget* parent) : QWidget(parent)
{
    // Passive presentation: never a focus/click target, so a controller/keyboard press is handled by
    // MainWindow's input path (which dismisses attract) and is never swallowed here. Mouse events fall through
    // to whatever is behind, but the overlay covers the window while active so it reads as a screensaver.
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setFocusPolicy(Qt::NoFocus);
    hide();

    frame_ = new QTimer(this);
    frame_->setInterval(33);   // ~30fps repaint pump
    connect(frame_, &QTimer::timeout, this, [this] { update(); });

    dwell_ = new QTimer(this);
    dwell_->setSingleShot(true);
    connect(dwell_, &QTimer::timeout, this, [this] { emit advanceRequested(); });
}

void AttractOverlay::loadPixmap(const AttractSlide& slide, QPixmap& into) const
{
    into = QPixmap();
    if (slide.art.isEmpty()) return;
    // Offline-first: buildSlides resolves cached art to local file paths. A local file loads; a bare remote
    // url that was never cached leaves the pixmap null and we fall back to the dark backdrop.
    if (QFileInfo::exists(slide.art)) into.load(slide.art);
}

void AttractOverlay::showSlide(const AttractSlide& slide)
{
    // The outgoing slide becomes the fade-from layer.
    prevPix_ = curPix_;
    prevSeed_ = curSeed_;
    loadPixmap(slide, curPix_);
    curTitle_ = slide.title;
    curSeed_ = ++slideOrdinal_;
    since_.restart();
    if (dwellMs_ > 0) dwell_->start(dwellMs_);
    if (!frame_->isActive()) frame_->start();
    update();
}

void AttractOverlay::start(const AttractSlide& first)
{
    if (parentWidget()) setGeometry(parentWidget()->rect());
    prevPix_ = QPixmap();
    curPix_ = QPixmap();
    slideOrdinal_ = 0;
    show();
    raise();
    // Engage the physical-input catch ONLY now; stop() removes it. qApp so it beats a focused QML scene.
    qApp->installEventFilter(this);
    showSlide(first);
}

void AttractOverlay::stop()
{
    qApp->removeEventFilter(this);   // never leave the catch installed once the slideshow is gone
    frame_->stop();
    dwell_->stop();
    prevPix_ = curPix_ = QPixmap();
    hide();
}

bool AttractOverlay::eventFilter(QObject* obj, QEvent* ev)
{
    if (isVisible())
    {
        switch (ev->type())
        {
        case QEvent::KeyPress:
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonDblClick:
        case QEvent::Wheel:
            // A physical press while showing: ask MainWindow to dismiss (it routes through the same
            // noteInput authority the pad path uses) and swallow THIS event — the wake press only wakes.
            emit dismissRequested();
            return true;
        default:
            break;
        }
    }
    return QWidget::eventFilter(obj, ev);
}

void AttractOverlay::kenBurns(qreal p, const QSize& widget, const QSize& image, int seed,
                              qreal& scaleOut, QPointF& offsetOut) const
{
    // The envelope from Video.qml's Ken-Burns: zoom 1.0 -> 1.12 and pan +/- 4% of width, eased. `p` runs 0..1
    // across the dwell; a smoothstep gives the InOutSine feel without trig per frame.
    const qreal eased = p * p * (3.0 - 2.0 * p);
    const qreal zoom = 1.0 + 0.12 * eased;

    // Cover the widget (fill, cropping overflow), then apply the zoom on top.
    qreal cover = 1.0;
    if (image.width() > 0 && image.height() > 0)
        cover = qMax(qreal(widget.width()) / image.width(), qreal(widget.height()) / image.height());
    scaleOut = cover * zoom;

    const qreal drawW = image.width() * scaleOut;
    const qreal drawH = image.height() * scaleOut;
    // Pan across ~4% of the width, direction alternating per slide so consecutive stills do not all drift the
    // same way.
    const qreal panRange = 0.04 * widget.width();
    const int dir = (seed % 2 == 0) ? 1 : -1;
    const qreal panX = dir * panRange * (eased - 0.5) * 2.0;
    const qreal panY = ((seed / 2) % 2 == 0 ? 1 : -1) * (0.03 * widget.height()) * (eased - 0.5) * 2.0;
    offsetOut = QPointF((widget.width() - drawW) / 2.0 + panX, (widget.height() - drawH) / 2.0 + panY);
}

void AttractOverlay::paintEvent(QPaintEvent*)
{
    QPainter pr(this);
    pr.fillRect(rect(), Qt::black);   // the backdrop behind everything (and the whole surface when no art)
    pr.setRenderHint(QPainter::SmoothPixmapTransform, true);

    const qint64 t = since_.isValid() ? since_.elapsed() : 0;
    const qreal p = dwellMs_ > 0 ? qBound(0.0, qreal(t) / dwellMs_, 1.0) : 0.0;

    auto drawPix = [&](const QPixmap& pix, int seed, qreal prog, qreal opacity) {
        if (pix.isNull() || opacity <= 0.0) return;
        qreal scale; QPointF off;
        kenBurns(prog, size(), pix.size(), seed, scale, off);
        pr.save();
        pr.setOpacity(opacity);
        const QRectF target(off, QSizeF(pix.width() * scale, pix.height() * scale));
        pr.drawPixmap(target, pix, QRectF(pix.rect()));
        pr.restore();
    };

    // Cross-fade: the previous slide fades out over fadeMs_ while the current fades in. The previous slide keeps
    // Ken-Burns'ing near the end of its own motion so the transition is not a static image sliding under a
    // moving one.
    const qreal fadeIn = fadeMs_ > 0 ? qBound(0.0, qreal(t) / fadeMs_, 1.0) : 1.0;
    if (fadeIn < 1.0) drawPix(prevPix_, prevSeed_, qMin(1.0, p + 0.85), 1.0 - fadeIn);
    drawPix(curPix_, curSeed_, p, fadeIn);

    // A soft bottom gradient so the title stays legible over bright art, then the title.
    if (!curTitle_.isEmpty() && !curPix_.isNull())
    {
        QLinearGradient g(0, height() * 0.6, 0, height());
        g.setColorAt(0.0, QColor(0, 0, 0, 0));
        g.setColorAt(1.0, QColor(0, 0, 0, 180));
        pr.fillRect(QRect(0, int(height() * 0.6), width(), int(height() * 0.4)), g);

        QFont f = pr.font();
        f.setPointSize(qMax(20, height() / 22));
        f.setBold(true);
        pr.setFont(f);
        pr.setPen(QColor(255, 255, 255, 235));
        const QRect tr(40, height() - 120, width() - 80, 90);
        pr.drawText(tr, Qt::AlignLeft | Qt::AlignVCenter, curTitle_);
    }
}
