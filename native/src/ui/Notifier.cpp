#include "Notifier.h"

#include <QLabel>
#include <QTimer>
#include <QWidget>
#include <QtGlobal>

Notifier::Notifier(QWidget* windowHost, QObject* parent)
    : QObject(parent), host_(windowHost)
{
    // Notification overlay (download/resolve progress + errors). A CHILD widget of the central area, raised
    // over the current page — NOT a separate top-level window. A top-level window is trapped behind a
    // foreground fullscreen main window (Windows' boosted fullscreen z-band), so it only appeared when you
    // alt-tabbed away. As a child it's part of the window and composites over everything: the QQuickWidget
    // themed home and the libmpv QOpenGLWidget both composite with sibling widgets. Click-through, no focus.
    notice_ = new QLabel(host_);
    notice_->setObjectName(QStringLiteral("mmvNotice"));
    notice_->setAttribute(Qt::WA_TransparentForMouseEvents);
    notice_->setFocusPolicy(Qt::NoFocus);
    notice_->setWordWrap(true);
    notice_->setAlignment(Qt::AlignCenter);
    notice_->setStyleSheet(QStringLiteral(
        "#mmvNotice { background:rgba(18,20,26,0.95); color:#f4f6f8; border:1px solid rgba(255,255,255,0.18);"
        " border-radius:10px; padding:12px 22px; font-size:12pt; font-weight:600; }"));
    notice_->hide();
    noticeTimer_ = new QTimer(this);
    noticeTimer_->setSingleShot(true);
    connect(noticeTimer_, &QTimer::timeout, this, [this] {
        if (!notice_) return;
        // Default expiry hides. The one exception is a notifyOverSticky() that covered a still-running phase
        // note: put that note back, sticky again, instead of leaving the user staring at a blank overlay while
        // the phase it described is still going.
        if (restoreSticky_)
        {
            const QString back = stickyRestore_;   // copy: notify() clears the members below us
            restoreSticky_ = false;
            stickyRestore_.clear();
            notify(back, 0);
            return;
        }
        notice_->hide();
    });
}

void Notifier::notify(const QString& text, int ms)
{
    if (!notice_) return;
    // A plain notify is the newest word on the subject and owns the label from here: drop any snapshot a
    // notifyOverSticky() was holding. Without this a flow that ENDS by replacing its sticky phase note with a
    // timed error would see the dead phase note reappear when the older restore fell due.
    restoreSticky_ = false;
    stickyRestore_.clear();
    notice_->setText(text);
    notice_->setMaximumWidth(qMax(280, int(host_->width() * 0.7)));
    notice_->adjustSize();
    notice_->show();
    notice_->raise();
    positionNotice();
    notice_->repaint(); // paint synchronously now, so a message set right before a blocking step (e.g. archive
                        // extraction) is actually visible instead of queued behind the freeze
    if (noticeTimer_) { if (ms > 0) noticeTimer_->start(ms); else noticeTimer_->stop(); } // ms<=0 => sticky
}

void Notifier::notifyOverSticky(const QString& text, int ms)
{
    if (!notice_) return;
    // Snapshot BEFORE notify() overwrites the label, and only when what is on screen is a sticky note: visible
    // with no auto-hide pending. Anything already counting down is on its way out by its own author's choice
    // and is not worth restoring. A ms <= 0 call is sticky itself and never expires, so it keeps no snapshot.
    const bool coversSticky = notice_->isVisible() && noticeTimer_ && !noticeTimer_->isActive() && ms > 0;
    const QString covered = coversSticky ? notice_->text() : QString();
    notify(text, ms);                 // clears any older snapshot, shows + arms exactly as any other message
    restoreSticky_ = coversSticky;
    stickyRestore_ = covered;
}

void Notifier::hideNotice()
{
    if (notice_) notice_->hide();
    if (noticeTimer_) noticeTimer_->stop();
    // An explicit hide is an explicit hide: a pending restore must not undo it seconds later.
    restoreSticky_ = false;
    stickyRestore_.clear();
}

void Notifier::positionNotice()
{
    if (!notice_ || !notice_->isVisible()) return;
    QWidget* area = notice_->parentWidget() ? notice_->parentWidget() : host_;
    notice_->setMaximumWidth(qMax(280, int(area->width() * 0.7)));
    notice_->adjustSize();
    // Child overlay: local coordinates over the bottom-centre of the central area.
    const int x = (area->width() - notice_->width()) / 2;
    const int y = area->height() - notice_->height() - 56; // floats just above the bottom edge
    notice_->move(qMax(8, x), qMax(8, y));
    notice_->raise(); // keep it above the current page
}

void Notifier::setPlayerHost(QWidget* player, std::function<int()> topOffsetPx)
{
    player_ = player;
    playerTop_ = std::move(topOffsetPx);

    // Single caller today; the guard keeps a future second caller from leaking the prior notice + its timer.
    if (playerNotice_) { playerNotice_->deleteLater(); playerNoticeTimer_->deleteLater(); }

    // Transient centred message over the player for next-source feedback (visible in full screen, where the
    // status bar isn't). Hidden by default.
    playerNotice_ = new QLabel(player_);
    playerNotice_->setObjectName(QStringLiteral("mmvPlayerNotice"));
    playerNotice_->setStyleSheet(QStringLiteral(
        "#mmvPlayerNotice { background: rgba(20,20,24,0.90); color:#f2f2f2; border-radius:8px; padding:10px 18px;"
        " font-size:15px; font-weight:bold; }"));
    playerNotice_->setAlignment(Qt::AlignCenter);
    playerNotice_->hide();
    playerNoticeTimer_ = new QTimer(this);
    playerNoticeTimer_->setSingleShot(true);
    connect(playerNoticeTimer_, &QTimer::timeout, this, [this] { if (playerNotice_) playerNotice_->hide(); });
}

void Notifier::playerNotice(const QString& msg, int ms)
{
    if (!playerNotice_) return;
    playerNotice_->setText(msg);
    playerNotice_->adjustSize();
    playerNotice_->move((player_->width() - playerNotice_->width()) / 2, playerTop_ ? playerTop_() : 16);
    playerNotice_->show();
    playerNotice_->raise();
    playerNoticeTimer_->start(ms);
}

void Notifier::hidePlayerNotice()
{
    if (playerNotice_) playerNotice_->hide();
    if (playerNoticeTimer_) playerNoticeTimer_->stop();
}

bool Notifier::playerNoticeVisible() const
{
    return playerNotice_ && playerNotice_->isVisible();
}

void Notifier::reposition()
{
    positionNotice();
    if (playerNotice_ && playerNotice_->isVisible())
        playerNotice_->move((player_->width() - playerNotice_->width()) / 2, playerTop_ ? playerTop_() : 16);
}
