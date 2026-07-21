// On-screen virtual gamepad overlay for touch form factors. A translucent child QWidget of RetroView that
// paints a D-pad, A/B/X/Y face cluster, L/R shoulders and Start/Select, tracks multi-touch (and single
// mouse for desktop testing), and emits a held RetroPad button bitmask (bit per RETRO_DEVICE_ID_JOYPAD id).
// RetroView ORs that mask into resolveInput() so in-process libretro cores see the presses.
#pragma once
#include <QWidget>
#include <QRect>
#include <QVector>
#include <QString>
#include <cstdint>

class VirtualPad : public QWidget
{
    Q_OBJECT
public:
    explicit VirtualPad(QWidget* parent = nullptr);

    // 0..100; drives the paint alpha of the zones. Repaints.
    void setOpacity(int pct);
    int  opacity() const { return opacityPct_; }

    quint32 mask() const { return mask_; }
    void reset();                   // clear all held bits (e.g. when the pad hides) and emit maskChanged(0)

signals:
    void maskChanged(quint32 mask); // the union of every currently-held zone's RetroPad bit

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    bool event(QEvent*) override;                 // routes QTouchEvent (multi-touch)
    void mousePressEvent(QMouseEvent*) override;   // single-button, desktop testing
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;

private:
    // A tappable face/shoulder/system zone. The D-pad is handled separately (a 3x3 grid so corners fire two
    // bits and a finger can slide between directions).
    enum Shape { Circle, RoundRect };
    struct Zone { QRect rect; int bit; QString label; Shape shape; };

    void layoutZones();                    // recompute every zone rect for the current size (all >= minHitPx)
    quint32 bitsAt(const QPointF& p) const; // the RetroPad bits a point at p is holding (0..multiple for d-pad)
    void setTouchMask(quint32 touch);       // store the touch bits, fold in the mouse, emit maskChanged on change

    QVector<Zone> zones_;   // face buttons, shoulders, start/select
    QRect dpad_;            // the d-pad bounding square (split into a 3x3 grid in bitsAt)
    quint32 mask_ = 0;      // last emitted union mask (touch | mouse)
    quint32 touchMask_ = 0; // bits held by live touch points
    quint32 mouseMask_ = 0; // bits held by the (single) mouse button, when down
    bool    mouseDown_ = false;
    int     opacityPct_ = 45;
};
