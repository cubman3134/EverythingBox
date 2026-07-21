#include "VirtualPad.h"
#include "../theme2/FormFactor.h"
#include "../libretro/libretro.h"   // RETRO_DEVICE_ID_JOYPAD_*

#include <QPainter>
#include <QPen>
#include <QTouchEvent>
#include <QMouseEvent>
#include <QEvent>

VirtualPad::VirtualPad(QWidget* parent) : QWidget(parent)
{
    // Touch is the primary input; the widget must opt in to receive QTouchEvents (else they arrive as
    // synthesised mouse events with no multi-touch). Painted zones sit over the game; the rest is transparent
    // (a child QWidget shows the parent through any pixel it doesn't paint — no WA_OpaquePaintEvent).
    setAttribute(Qt::WA_AcceptTouchEvents, true);
}

void VirtualPad::reset()
{
    mouseDown_ = false;
    mouseMask_ = 0;
    setTouchMask(0);
}

void VirtualPad::setOpacity(int pct)
{
    opacityPct_ = qBound(0, pct, 100);
    update();
}

void VirtualPad::resizeEvent(QResizeEvent*)
{
    layoutZones();
}

// Anchor the controls to the edges: D-pad bottom-left, A/B/X/Y bottom-right, L/R along the top corners,
// Start/Select as a pair at the bottom centre. Every zone is sized through FormFactor::hitClamp() so it is
// never smaller than the mode's minimum hit target (mobile 44px), then insets from a safe-area margin.
void VirtualPad::layoutZones()
{
    zones_.clear();
    const FormFactor& ff = FormFactor::instance();
    const int W = width(), H = height();
    if (W <= 0 || H <= 0) return;

    const int margin = qMax(12, int(qMin(W, H) * qMax(ff.safeAreaFrac(), 0.03)));
    const int btn  = ff.hitClamp(58);     // face button / shoulder diameter
    const int cell = ff.hitClamp(52);     // one d-pad direction cell (3x3 grid => 3*cell across)
    const int sys  = ff.hitClamp(46);     // start/select height

    // D-pad: a 3x3 grid, bottom-left.
    const int dsz = cell * 3;
    dpad_ = QRect(margin, H - margin - dsz, dsz, dsz);

    // Face cluster (diamond), bottom-right. B bottom, A right, Y left, X top — matches the RetroPad layout.
    const int gap = btn / 2 + 8;
    const int cx = W - margin - btn - gap;      // centre of the diamond
    const int cy = H - margin - btn - gap;
    auto face = [&](int dx, int dy, int bit, const QString& lbl) {
        zones_.push_back({ QRect(cx + dx - btn / 2, cy + dy - btn / 2, btn, btn), bit, lbl, Circle });
    };
    face(-gap, 0, RETRO_DEVICE_ID_JOYPAD_Y, QStringLiteral("Y"));
    face(+gap, 0, RETRO_DEVICE_ID_JOYPAD_A, QStringLiteral("A"));
    face(0, -gap, RETRO_DEVICE_ID_JOYPAD_X, QStringLiteral("X"));
    face(0, +gap, RETRO_DEVICE_ID_JOYPAD_B, QStringLiteral("B"));

    // Shoulders L/R along the top corners.
    const int shW = btn * 3 / 2;
    zones_.push_back({ QRect(margin, margin, shW, btn), RETRO_DEVICE_ID_JOYPAD_L, QStringLiteral("L"), RoundRect });
    zones_.push_back({ QRect(W - margin - shW, margin, shW, btn), RETRO_DEVICE_ID_JOYPAD_R, QStringLiteral("R"), RoundRect });

    // Start/Select pair, bottom centre.
    const int pw = ff.hitClamp(74);
    const int mid = W / 2;
    zones_.push_back({ QRect(mid - pw - 8, H - margin - sys, pw, sys), RETRO_DEVICE_ID_JOYPAD_SELECT, QStringLiteral("Select"), RoundRect });
    zones_.push_back({ QRect(mid + 8, H - margin - sys, pw, sys), RETRO_DEVICE_ID_JOYPAD_START, QStringLiteral("Start"), RoundRect });
}

// The RetroPad bits a point holds. The d-pad maps to a 3x3 grid: edge cells = one direction, corner cells =
// two (diagonals), the centre cell = none — so a finger sliding across the pad transitions cleanly and holds
// two directions at the corners. Face/shoulder/system zones are simple hit tests.
quint32 VirtualPad::bitsAt(const QPointF& p) const
{
    quint32 b = 0;
    const QPoint ip = p.toPoint();
    if (dpad_.contains(ip))
    {
        const int third = dpad_.width() / 3;
        int col = int((p.x() - dpad_.left()) / third); col = qBound(0, col, 2);
        int row = int((p.y() - dpad_.top())  / third); row = qBound(0, row, 2);
        if (row == 0) b |= (1u << RETRO_DEVICE_ID_JOYPAD_UP);
        if (row == 2) b |= (1u << RETRO_DEVICE_ID_JOYPAD_DOWN);
        if (col == 0) b |= (1u << RETRO_DEVICE_ID_JOYPAD_LEFT);
        if (col == 2) b |= (1u << RETRO_DEVICE_ID_JOYPAD_RIGHT);
    }
    for (const Zone& z : zones_)
        if (z.rect.contains(ip)) b |= (1u << z.bit);
    return b;
}

bool VirtualPad::event(QEvent* e)
{
    switch (e->type())
    {
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TouchEnd:
    case QEvent::TouchCancel:
    {
        auto* te = static_cast<QTouchEvent*>(e);
        quint32 touch = 0;
        for (const QEventPoint& tp : te->points())
        {
            if (tp.state() == QEventPoint::State::Released) continue; // finger lifted: its bits clear
            touch |= bitsAt(tp.position());
        }
        setTouchMask(touch);
        e->accept();
        return true;
    }
    default:
        break;
    }
    return QWidget::event(e);
}

void VirtualPad::setTouchMask(quint32 touch)
{
    touchMask_ = touch;
    const quint32 combined = touchMask_ | mouseMask_;
    if (combined != mask_) { mask_ = combined; emit maskChanged(mask_); }
    update();
}

void VirtualPad::mousePressEvent(QMouseEvent* e)
{
    const quint32 b = bitsAt(e->position());
    if (b) { mouseDown_ = true; mouseMask_ = b; setTouchMask(touchMask_); e->accept(); }
    else { e->ignore(); } // let non-zone clicks fall through to the game view (keeps its keyboard focus)
}

void VirtualPad::mouseMoveEvent(QMouseEvent* e)
{
    if (!mouseDown_) { e->ignore(); return; }
    mouseMask_ = bitsAt(e->position());
    setTouchMask(touchMask_);
    e->accept();
}

void VirtualPad::mouseReleaseEvent(QMouseEvent* e)
{
    if (!mouseDown_) { e->ignore(); return; }
    mouseDown_ = false;
    mouseMask_ = 0;
    setTouchMask(touchMask_);
    e->accept();
}

void VirtualPad::paintEvent(QPaintEvent*)
{
    if (width() <= 0 || height() <= 0) return;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const qreal a = opacityPct_ / 100.0;
    const QColor fill(235, 238, 245, int(a * 90));
    const QColor edge(255, 255, 255, int(a * 150));
    const QColor hot(120, 170, 255, int(qMin(1.0, a + 0.25) * 200)); // pressed tint
    const QColor text(255, 255, 255, int(qMin(1.0, a + 0.35) * 230));

    auto pressed = [&](int bit) { return (mask_ >> bit) & 1u; };

    // D-pad: the four direction cells of the 3x3 grid; centre left blank.
    if (dpad_.isValid())
    {
        const int third = dpad_.width() / 3;
        auto cellRect = [&](int col, int row) {
            return QRect(dpad_.left() + col * third, dpad_.top() + row * third, third, third).adjusted(3, 3, -3, -3);
        };
        struct DC { int col, row, bit; };
        const DC dirs[4] = {
            { 1, 0, RETRO_DEVICE_ID_JOYPAD_UP }, { 1, 2, RETRO_DEVICE_ID_JOYPAD_DOWN },
            { 0, 1, RETRO_DEVICE_ID_JOYPAD_LEFT }, { 2, 1, RETRO_DEVICE_ID_JOYPAD_RIGHT } };
        for (const DC& d : dirs)
        {
            p.setBrush(pressed(d.bit) ? hot : fill);
            p.setPen(QPen(edge, 1.5));
            p.drawRoundedRect(cellRect(d.col, d.row), 6, 6);
        }
    }

    // Face / shoulder / system zones.
    for (const Zone& z : zones_)
    {
        p.setPen(QPen(edge, 1.5));
        p.setBrush(pressed(z.bit) ? hot : fill);
        if (z.shape == Circle) p.drawEllipse(z.rect);
        else                   p.drawRoundedRect(z.rect, 10, 10);
        QFont f = p.font(); f.setBold(true); f.setPixelSize(qMax(10, z.rect.height() * 4 / 10)); p.setFont(f);
        p.setPen(QPen(text));
        p.drawText(z.rect, Qt::AlignCenter, z.label);
    }
}
