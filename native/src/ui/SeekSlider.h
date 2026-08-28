// SeekSlider — a QSlider whose GROOVE is a seek target, not just a page-step region.
//
// Qt's default is that a left click anywhere but the handle nudges the value by one page step and emits
// neither sliderPressed nor sliderReleased. For a volume dial that is fine; for a transport bar it means the
// gesture everyone learned from YouTube/Spotify — click where you want to be — did nothing at all: the value
// crept by a tenth of a percent, the release the player listens for never came, and the next position tick
// wrote the old spot back. Clicking the bar looked broken because it WAS.
//
// So the groove is treated as the scale it draws: press = jump there and start a drag, move = follow the
// pointer, release = commit. Nothing else about the widget changes — it still emits sliderPressed /
// sliderMoved / sliderReleased in that order, so a host that already seeks on sliderReleased (and shows a
// preview time on sliderMoved) needs no new wiring, and a keyboard/controller user's arrow steps are
// untouched. Deliberately NOT a QProxyStyle SH_Slider_AbsoluteSetButtons: the transport bar is inside a
// stylesheet'd frame, where a style hint travels through QStyleSheetStyle and can be silently dropped.
//
// No Q_OBJECT: it declares no signals or slots of its own, so it needs no moc and can stay header-only.
#pragma once
#include <QMouseEvent>
#include <QSlider>
#include <QStyle>
#include <QStyleOptionSlider>

class SeekSlider : public QSlider
{
public:
    explicit SeekSlider(Qt::Orientation o, QWidget* parent = nullptr) : QSlider(o, parent) {}

protected:
    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton) { QSlider::mousePressEvent(e); return; }
        // setSliderDown BEFORE the position, so the host's sliderPressed handler has already latched "the user
        // is scrubbing" when the first sliderMoved arrives — otherwise a position tick landing between the two
        // would overwrite the value the click just chose.
        setSliderDown(true);
        setSliderPosition(valueAt(e->position().toPoint()));
        e->accept();
    }

    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (!isSliderDown()) { QSlider::mouseMoveEvent(e); return; }
        setSliderPosition(valueAt(e->position().toPoint()));
        e->accept();
    }

    void mouseReleaseEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton || !isSliderDown()) { QSlider::mouseReleaseEvent(e); return; }
        setSliderPosition(valueAt(e->position().toPoint()));
        setSliderDown(false);   // emits sliderReleased — the host's commit
        e->accept();
    }

private:
    // The value the groove draws under `p`. Measured against the groove MINUS the handle's own length (the
    // span the handle's top-left can travel) with the pointer taken as the handle's centre, which is what puts
    // the handle under the cursor rather than half a handle to its right.
    int valueAt(const QPoint& p) const
    {
        QStyleOptionSlider opt;
        initStyleOption(&opt);
        const QRect groove = style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderGroove, this);
        const QRect handle = style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderHandle, this);
        if (orientation() == Qt::Horizontal)
        {
            const int span = groove.width() - handle.width();
            const int pos  = p.x() - groove.x() - handle.width() / 2;
            return QStyle::sliderValueFromPosition(minimum(), maximum(), pos, span, opt.upsideDown);
        }
        const int span = groove.height() - handle.height();
        const int pos  = p.y() - groove.y() - handle.height() / 2;
        // Vertical sliders count downwards on screen but upwards in value — hence the inverted flag.
        return QStyle::sliderValueFromPosition(minimum(), maximum(), pos, span, !opt.upsideDown);
    }
};
