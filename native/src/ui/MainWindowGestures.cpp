// Issue #162 — the video player's touch gestures, host side.
//
// The DECISIONS all live in src/video/PlayerGestures.h, which is pure and probed headlessly
// (probe_playergestures). Nothing in this file decides what a gesture means; it translates. Touch points and
// the window geometry go in, and the events that come back are turned into the calls the transport buttons
// already make — seekRelative, setPosition, togglePause, the volume slider, mpv's brightness and aspect.
//
// It is a TU of its own rather than another few hundred lines of MainWindow.cpp deliberately (the #186
// direction): a feature that touches the player's event path should not put its hunks in the middle of every
// other branch's. Only the declarations live in MainWindow.h.
//
// The form-factor gate is not repeated here. applyGestureConfig() asks PlayerGestures::configFromSettings()
// once per press, and on a desktop or TV build that Config comes back disabled — so begin() claims nothing,
// every rule is dead, and the D-pad/nav path this file could otherwise disturb is untouched by construction.
#include "MainWindow.h"

#include "../core/Settings.h"
#include "../video/MpvWidget.h"
#include "../video/PlayerGestureConfig.h"
#include "../video/PlayerGestures.h"
#include "nav/NavOverlay.h"

#include <QApplication>
#include <QEventPoint>
#include <QPushButton>
#include <QSizeF>
#include <QSlider>
#include <QStackedWidget>
#include <QTimer>
#include <QTouchEvent>

using PlayerGestures::Event;
using PlayerGestures::Kind;
using PlayerGestures::VideoFit;

// Rebuild the recogniser's rules from what the user stored, plus the two facts only the window knows (how
// long the media is, and how big the video is right now). Called once per TouchBegin rather than cached, so a
// setting changed mid-film applies to the very next swipe with no change-notification wiring at all.
void MainWindow::applyGestureConfig()
{
    PlayerGestures::Config c = PlayerGestures::configFromSettings();
    c.durationSec = duration_;
    gestures_.setConfig(c);
    if (!gestureHoldTimer_)
    {
        // The long-press deadline. The recogniser still owns the DECISION (tick() re-checks the elapsed time
        // and the travel); this timer only makes sure the question gets asked when a finger rests perfectly
        // still and Qt therefore sends no further move frames.
        gestureHoldTimer_ = new QTimer(this);
        gestureHoldTimer_->setSingleShot(true);
        connect(gestureHoldTimer_, &QTimer::timeout, this, [this] {
            if (gestureClock_.isValid()) handleGestureEvents(gestures_.tick(gestureClock_.elapsed()));
        });
    }
}

// The player's touch filter. A touch that lands on the visible transport chrome is DEFERRED (return false) so
// it rides synthesized mouse exactly as before — a QSlider drag and a QPushButton tap need nothing new — and
// so does anything the recogniser declines to claim, which is what keeps the OS's edge swipes and a disabled
// gesture family working like the touch was never seen.
bool MainWindow::handlePlayerTouch(QTouchEvent* te)
{
    if (!player_) return false;
    if (!gestureClock_.isValid()) gestureClock_.start();
    const qint64 now = gestureClock_.elapsed();

    QVector<QPointF> pts;
    pts.reserve(te->points().size());
    for (const QEventPoint& p : te->points()) pts << p.position();

    QVector<Event> evs;
    switch (te->type())
    {
    case QEvent::TouchBegin:
    {
        if (pts.isEmpty()) return false;
        const QPoint ip = pts.first().toPoint();
        const bool overControls =
               (mediaControls_ && mediaControls_->isVisible() && mediaControls_->geometry().contains(ip))
            || (videoBack_ && videoBack_->isVisible() && videoBack_->geometry().contains(ip))
            || (streamIssueBtn_ && streamIssueBtn_->isVisible() && streamIssueBtn_->geometry().contains(ip))
            || (skipChip_ && skipChip_->isVisible() && skipChip_->geometry().contains(ip))
            || (gestureLockBtn_ && gestureLockBtn_->isVisible() && gestureLockBtn_->geometry().contains(ip));
        if (overControls) return false;
        applyGestureConfig();
        gestures_.setViewport(QSizeF(player_->size()));
        // "Never fight the system" second half: anything modal or menu-shaped on screen owns the touch.
        gestures_.setOverlayOpen(NavOverlay::topmost() != nullptr
                                 || QApplication::activePopupWidget() != nullptr
                                 || QApplication::activeModalWidget() != nullptr
                                 || escMenuVisible()
                                 || (subOverlay_ && subOverlay_->isVisible()));
        gestures_.setPosition(lastPos_);
        gestures_.setVolume(volume_ ? qBound(0, volume_->value(), 100) : 100);
        gestures_.setBrightness(player_->videoBrightness());
        evs = gestures_.begin(pts, now);
        if (gestures_.claimed() && gestureHoldTimer_)
            gestureHoldTimer_->start(gestures_.config().longPressMs);
        break;
    }
    case QEvent::TouchUpdate:
        evs = gestures_.update(pts, now);
        break;
    case QEvent::TouchEnd:
        if (gestureHoldTimer_) gestureHoldTimer_->stop();
        evs = gestures_.end(pts, now);
        break;
    default:
        return false;
    }
    handleGestureEvents(evs);
    return gestures_.claimed();
}

// h:mm:ss (mm:ss under an hour) for the scrub readout. The player's own bar shows the same shape.
QString MainWindow::gestureTimeText(double seconds) const
{
    if (seconds < 0.0) seconds = 0.0;
    const int t = int(seconds + 0.5);
    const int h = t / 3600, m = (t % 3600) / 60, s = t % 60;
    if (h > 0)
        return QStringLiteral("%1:%2:%3").arg(h).arg(m, 2, 10, QLatin1Char('0')).arg(s, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1:%2").arg(m).arg(s, 2, 10, QLatin1Char('0'));
}

// One switch, one line of translation per gesture. Every arm calls something that already existed: the skip
// is the EXACT seekRelative the transport buttons fire, the play/pause is the transport's togglePause, the
// volume goes through the slider and applyPlayerVolume() so the sleep-timer fade keeps scaling it, and the
// brightness and aspect are mpv properties. Nothing here is a second implementation of anything.
void MainWindow::handleGestureEvents(const QVector<Event>& evs)
{
    if (evs.isEmpty() || !player_) return;
    for (const Event& e : evs)
    {
        switch (e.kind)
        {
        case Kind::TapPending:
            // Defer: a second tap inside the window turns this into a skip instead of a chrome toggle. The
            // timer's timeout is the chrome toggle, wired in the constructor exactly as before.
            playerTapTimer_->start(gestures_.config().doubleTapMs);
            placeGestureLockButton(true);
            break;
        case Kind::DoubleTapLeft:
        case Kind::DoubleTapRight:
            playerTapTimer_->stop();                       // the deferred single tap is superseded
            player_->seekRelative(e.value);
            // The ripple: the arrow, the side and the interval, so it is obvious the double-tap registered
            // AND obvious how far it went — the interval is #140's shared setting and can be anything.
            notify(e.value < 0 ? tr("⏪  −%1s").arg(int(-e.value)) : tr("⏩  +%1s").arg(int(e.value)), 900);
            revealMediaControls();
            break;
        case Kind::DoubleTapCentre:
            playerTapTimer_->stop();
            player_->togglePause();
            revealMediaControls();
            break;
        case Kind::VolumeDelta:
            if (volume_) { volume_->setValue(int(e.target)); applyPlayerVolume(); }
            notify(tr("🔊  %1%").arg(int(e.target)), 700);
            break;
        case Kind::BrightnessDelta:
            player_->setVideoBrightness(int(e.target));
            notify(tr("☀  %1%").arg(int(e.target)), 700);
            break;
        case Kind::SeekPreview:
            // Time-only for now. #85's trickplay images are the intended upgrade of THIS readout and nothing
            // else: when a preview frame exists for `e.target` it is drawn beside these two times.
            notify(tr("%1  →  %2  (%3%4s)")
                       .arg(gestureTimeText(lastPos_), gestureTimeText(e.target),
                            e.value < 0 ? QStringLiteral("−") : QStringLiteral("+"))
                       .arg(int(qAbs(e.value))), 1200);
            break;
        case Kind::SeekCommit:
            player_->setPosition(e.target);
            notify(gestureTimeText(e.target), 900);
            revealMediaControls();
            break;
        case Kind::SeekCancel:
            notify(tr("Seek cancelled"), 700);
            break;
        case Kind::LongPressBegin:
            gestureSpeedBefore_ = player_->speed();
            player_->setSpeed(gestureSpeedBefore_ * e.value);
            notify(tr("⏩  %1× while held").arg(e.value, 0, 'g', 2), 1500);
            break;
        case Kind::LongPressEnd:
            player_->setSpeed(gestureSpeedBefore_);
            hideNotice();
            break;
        case Kind::PinchIn:
        case Kind::PinchOut:
            videoFit_ = PlayerGestures::cycleFit(videoFit_, e.kind == Kind::PinchOut);
            applyVideoFit(videoFit_);
            break;
        case Kind::LockedTap:
            // The lock swallowed the gesture; the ONLY thing it answers with is the way back out.
            placeGestureLockButton(true);
            break;
        case Kind::None:
        default:
            break;
        }
    }
}

void MainWindow::applyVideoFit(VideoFit fit)
{
    if (!player_) return;
    player_->setVideoFit(int(fit));
    notify(fit == VideoFit::Fit ? tr("Fit") : fit == VideoFit::Fill ? tr("Fill") : tr("Stretch"), 900);
}

// The lock: pocket protection for audio-only listening. While it is on, the recogniser suppresses every
// family and reports nothing but a bare tap, which is what puts this button back on screen.
void MainWindow::setGestureLocked(bool on)
{
    gestures_.setLocked(on);
    if (gestureLockBtn_) gestureLockBtn_->setText(on ? tr("🔒  Locked") : tr("🔓  Lock gestures"));
    notify(on ? tr("Gestures locked — tap to unlock") : tr("Gestures unlocked"), 1500);
    placeGestureLockButton(true);
}

// Create-on-first-use, position, show. A plain child of player_ like skipChip_ and NOT a NavOverlay: an
// overlay grabs all input, which is exactly wrong for a control that must sit over live video while the user
// ignores it. Never built at all off a touch form factor — the button has no reason to exist there, and a
// TV surface must not grow a widget the D-pad ring does not know about.
void MainWindow::placeGestureLockButton(bool show)
{
    if (!player_ || !gestures_.config().enabled) return;
    if (!gestureLockBtn_)
    {
        gestureLockBtn_ = new QPushButton(player_);
        gestureLockBtn_->setCursor(Qt::PointingHandCursor);
        gestureLockBtn_->setFocusPolicy(Qt::NoFocus);    // never joins the transport ring; touch-only by design
        gestureLockBtn_->setStyleSheet(QStringLiteral(
            "QPushButton{background:rgba(0,0,0,170);color:#fff;border:1px solid rgba(255,255,255,60);"
            "border-radius:18px;padding:8px 16px;font-size:15px;}"));
        gestureLockBtn_->setText(gestures_.locked() ? tr("🔒  Locked") : tr("🔓  Lock gestures"));
        connect(gestureLockBtn_, &QPushButton::clicked, this, [this] { setGestureLocked(!gestures_.locked()); });
        gestureLockTimer_ = new QTimer(this);
        gestureLockTimer_->setSingleShot(true);
        connect(gestureLockTimer_, &QTimer::timeout, this,
                [this] { if (gestureLockBtn_) gestureLockBtn_->hide(); });
    }
    if (!show) { gestureLockBtn_->hide(); if (gestureLockTimer_) gestureLockTimer_->stop(); return; }
    gestureLockBtn_->adjustSize();
    // Top-right of the video, clear of the ‹ Back overlay on the left and of the transport at the bottom.
    const QSize sz = gestureLockBtn_->size();
    gestureLockBtn_->move(qMax(0, player_->width() - sz.width() - 16), 16);
    gestureLockBtn_->raise();
    gestureLockBtn_->show();
    gestureLockTimer_->start(4000);      // its own life, like skipChipTimer_ — not the shared chrome timer
}
