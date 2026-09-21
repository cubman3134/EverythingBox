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
// It also draws issue #85's TIMELINE MARKUP: a tick at every chapter boundary and a shaded band over the
// intro and the end credits. Both come in as times (setMarks) and are laid out by TimelineMarks at paint
// time, so a resize re-places them for free and nothing here caches a pixel. See paintEvent for the two
// rules that keep the markup from eating the bar it is drawn on.
//
// No Q_OBJECT: it declares no signals or slots of its own, so it needs no moc and can stay header-only.
#pragma once
#include "TimelineMarks.h"

#include <QColor>
#include <QMouseEvent>
#include <QPainter>
#include <QRegion>
#include <QSlider>
#include <QStyle>
#include <QStyleOptionSlider>

class SeekSlider : public QSlider
{
public:
    explicit SeekSlider(Qt::Orientation o, QWidget* parent = nullptr) : QSlider(o, parent) {}

    // The markup for the file that is open, in SECONDS (never pixels — see TimelineMarks.h). An empty set is
    // how "this file has nothing to mark" is spelled, and it is also what a cleared bar holds, so a caller
    // never has to special-case the transition.
    void setMarks(const TimelineMarks::Marks& m)
    {
        marks_ = m;
        update();
    }
    const TimelineMarks::Marks& marks() const { return marks_; }

protected:
    // The bar, then the markup — clipped so that the markup is BENEATH the two things the user is actually
    // steering by.
    //
    //  * THE HANDLE IS NEVER LOST. A tick painted across it turns the one control on the bar into a
    //    two-tone smudge at couch distance, and a band under it changes the handle's contrast as it
    //    travels. The handle's rect (plus a pixel of air) is cut out of the clip region, which is exactly
    //    what "drawn under the handle" means for a widget whose handle the style has already painted.
    //  * SO IS THE PLAYED PORTION. Its colour is the answer to "where am I", and markup over it competes
    //    with that. The played span is cut out of the clip region for the same reason and by the same
    //    means: the marks are under the fill, and emerge as it passes them by.
    //
    // Both cut-outs are also why nothing here needs to be translucent-over-translucent: no mark is ever
    // painted over another mark or over a styled element, so no pixel can end up doubly shaded.
    void paintEvent(QPaintEvent* e) override
    {
        QSlider::paintEvent(e);
        if (marks_.isEmpty() || marks_.duration <= 0.0) return;

        QStyleOptionSlider opt;
        initStyleOption(&opt);
        const QRect groove = style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderGroove, this);
        const QRect handle = style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderHandle, this);
        if (groove.width() <= 0 || groove.height() <= 0) return;

        QRegion clip(groove);
        clip -= QRegion(handle.adjusted(-1, -1, 1, 1));
        const int playedTo = handle.center().x();
        if (playedTo > groove.left())
            clip -= QRegion(QRect(groove.left(), groove.top(),
                                  playedTo - groove.left() + 1, groove.height()));
        if (clip.isEmpty()) return;

        QPainter p(this);
        p.setClipRegion(clip);
        for (const TimelineMarks::Band& b : marks_.bands)
        {
            QRect r = TimelineMarks::rectForRange(b.start, b.end, marks_.duration,
                                                  groove.width(), groove.height());
            if (r.isNull()) continue;
            r.translate(groove.topLeft());
            p.fillRect(r, b.kind == TimelineMarks::BandKind::Intro ? kIntroBand : kCreditsBand);
        }
        for (int x : TimelineMarks::tickPixels(marks_.ticks, marks_.duration, groove.width(), kTickWidth + 1))
            p.fillRect(QRect(groove.left() + x - kTickWidth / 2, groove.top(), kTickWidth, groove.height()),
                       kTick);
    }
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

    // The markup's three colours, and they are the TRANSPORT BAR'S OWN, not new ones: MainWindow's
    // #mediaControls stylesheet draws the row's focus state in rgba(90,140,255,…), its groove in
    // rgba(255,255,255,0.22) and its text and fill in #e8e8e8. A tick is that focus blue nearly solid, so it
    // reads against the dark groove without competing with the white fill; the two bands are washes of the
    // same blue and of the bar's own white, kept far enough apart in weight to be told apart at couch
    // distance while neither is mistakable for the played colour. They live here, next to the only code that
    // uses them, for the same reason the stylesheet's literals live next to the widgets they style.
    static constexpr int kTickWidth = 2;
    inline static const QColor kTick        { 90, 140, 255, 235 };
    inline static const QColor kIntroBand   { 90, 140, 255, 110 };
    inline static const QColor kCreditsBand { 232, 232, 232, 90 };

    TimelineMarks::Marks marks_;
};
