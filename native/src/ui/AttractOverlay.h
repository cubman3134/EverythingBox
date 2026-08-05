// The full-screen visual surface of attract mode (idle screensaver, issue #54). A plain child QWidget of the
// main window — NOT a top-level window and NOT a nav-kit menu/confirm: it is a passive presentation layer, the
// same shape as the subtitle overlay, so it never fights the desktop for focus and a D-pad press is handled by
// MainWindow's input path (which dismisses it) rather than by the overlay stealing the key.
//
// It renders one AttractSlide at a time with a Ken-Burns pan/zoom over the still, cross-fading to the next. The
// pan/zoom envelope (scale 1.0 -> 1.12, pan +/- 4% of width) is taken verbatim from the theme `video` element's
// Ken-Burns (native/src/theme2/qml/elements/Video.qml) so the effect matches the themed surface rather than
// being a second, different one.
//
// It owns ONLY the visuals and the dwell timer: when the dwell elapses it emits advanceRequested(), and
// MainWindow answers by stepping the (tested) AttractController rotation and handing back the next slide via
// showSlide(). The overlay decides nothing about WHICH slide is next or WHEN attract ends — those live in the
// controller and MainWindow, where they are testable.
#pragma once
#include <QElapsedTimer>
#include <QPixmap>
#include <QString>
#include <QWidget>

#include "../core/AttractController.h"

class QTimer;

class AttractOverlay : public QWidget
{
    Q_OBJECT
public:
    explicit AttractOverlay(QWidget* parent = nullptr);

    // Show `slide` (cross-fading from whatever is currently shown) and (re)start its dwell timer. An empty /
    // invalid slide clears to the plain dark backdrop. Loading is offline-first: a local file path renders; a
    // bare remote url that is not cached simply shows the backdrop (buildSlides feeds local paths where cached).
    void showSlide(const AttractSlide& slide);

    // Start/stop the presentation. start() shows `first` and begins the dwell cycle; stop() halts the timers
    // and hides. Idempotent.
    void start(const AttractSlide& first);
    void stop();

    int dwellMs() const { return dwellMs_; }
    void setDwellMs(int ms) { dwellMs_ = ms > 0 ? ms : dwellMs_; }

signals:
    void advanceRequested();   // the dwell for the current slide elapsed — MainWindow steps the rotation
    void dismissRequested();    // a physical key/mouse press arrived while showing — MainWindow dismisses

protected:
    void paintEvent(QPaintEvent*) override;
    // While the slideshow is up, an application-level filter (installed only for that window of time) catches a
    // physical keyboard/mouse press that a focused QML scene would otherwise consume, and asks MainWindow to
    // dismiss. It is installed by start() and removed by stop(), so it is completely inert whenever attract is
    // not showing — it can never become a standing input trap.
    bool eventFilter(QObject* obj, QEvent* ev) override;

private:
    void loadPixmap(const AttractSlide& slide, QPixmap& into) const;
    // The Ken-Burns transform for a slide at animation progress p (0..1), given the widget and image sizes.
    // Returns the scale to apply and the top-left offset to draw at. `seed` varies pan direction per slide.
    void kenBurns(qreal p, const QSize& widget, const QSize& image, int seed,
                  qreal& scaleOut, QPointF& offsetOut) const;

    QPixmap curPix_;
    QPixmap prevPix_;
    QString curTitle_;
    int curSeed_ = 0;
    int prevSeed_ = 0;
    int slideOrdinal_ = 0;         // increments per slide; seeds the pan direction

    QElapsedTimer since_;          // ms since the current slide appeared (drives Ken-Burns + fade)
    QTimer* frame_ = nullptr;      // ~30fps repaint pump while showing
    QTimer* dwell_ = nullptr;      // fires advanceRequested() once per slide

    int dwellMs_ = 8000;           // time each slide is held before advancing
    int fadeMs_ = 900;             // cross-fade duration at the start of each slide
};
