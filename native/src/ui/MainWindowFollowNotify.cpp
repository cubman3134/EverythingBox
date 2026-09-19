// Grouped new-item notifications for followed series (issue #155, increment 2) — the MainWindow half, in its
// own translation unit so the busiest file in the repository pays two short insertions for it.
//
// This file DELIVERS; it does not decide. Every rule — what counts as new, the per-series mute, the global
// consent and its one-time prompt, the full-screen hold and the merge of a held summary with later cycles — is
// core/FollowNotify's, where probe_follow pins it headless. What is here is the plumbing only a window has:
//
//   * the input. FollowScheduler::newItemsFound names the child ids each series grew by; they are collected
//     until cycleFinished, then resolved against the device-local snapshot and the completion marks at that
//     moment (FollowNotify::newsFor), so a child marked seen before the cycle ended never counts.
//   * the hold. "Something is playing full-screen" is this window's to answer: the window is full screen AND
//     a playback surface (the video page, a libretro game, a RetroPark game) is the current page. Music playing
//     behind the home screen does not hold anything — the screen is not owned. While held, a slow poll waits
//     for the surface to go and then releases the ONE pending summary.
//   * the delivery. Desktop: a QSystemTrayIcon message. The icon is created lazily, shown only for as long as
//     a message is up, and hidden again, so no permanent tray icon appears — nothing else in this app uses the
//     tray. Android and iOS have no notification channel yet, so the notice is logged as skipped there.
//   * the prompt. A nav-kit window notice (the Notifier — no dialog, nothing modal) offering to turn the
//     setting on, once; a click on it answers yes, and the settings row answers either way.
#include "MainWindow.h"

#include <QApplication>
#include <QDateTime>
#include <QFile>
#include <QIcon>
#include <QStackedWidget>
#include <QStyle>
#include <QTimer>
#if QT_CONFIG(systemtrayicon)
#include <QSystemTrayIcon>
#endif

#include "../core/AppPaths.h"
#include "../core/FollowNotify.h"
#include "../core/FollowScheduler.h"
#include "../core/FollowSnapshot.h"
#include "../core/FollowStore.h"
#include "../core/ItemMarks.h"
#include "../core/Settings.h"
#include "../emu/RetroParkView.h"
#include "../emu/RetroView.h"
#include "HomeView.h"
#include "Notifier.h"

namespace
{
// stream_debug.log, the same file and line shape MainWindow.cpp's mwLog writes. Series and episode titles
// only — nothing here ever holds a URL or a credential.
void fnLog(const QString& msg)
{
    QFile f(AppPaths::dataDir() + QStringLiteral("/stream_debug.log"));
    if (f.open(QIODevice::Append | QIODevice::Text))
        f.write((QDateTime::currentDateTime().toString(Qt::ISODate) + QStringLiteral("  follow-notify: ") + msg
                 + QStringLiteral("\n")).toUtf8());
}

follownotify::Consent consentNow()
{
    if (!Settings::followNotifyAnswered()) return follownotify::Consent::Unset;
    return Settings::followNotify() ? follownotify::Consent::On : follownotify::Consent::Off;
}

// The New shelf's own dealt-with test (HomeView::followUnreadCount), so a child the shelf no longer shows is
// a child the notification does not count.
bool dealtWith(const QString& childId)
{
    const ItemMarks::Marks m = ItemMarks::get(childId);
    return follow::isDealtWith(m.hidden, m.completion == ItemMarks::Completion::None);
}

bool stillNew(const QString& seriesId, const QString& childId)
{
    if (!FollowStore::isFollowed(seriesId) || dealtWith(childId)) return false;
    for (const FollowSnapshot::Pending& p : FollowSnapshot::get(seriesId).pending)
        if (p.id == childId) return true;
    return false;
}

bool isMuted(const QString& seriesId) { return FollowStore::isMuted(seriesId); }

constexpr int kHoldPollMs     = 5000;    // how soon after playback ends a held summary appears
constexpr int kTrayMessageMs  = 10000;   // how long the desktop message asks to stay up
constexpr int kTrayLingerMs   = 30000;   // then the icon goes, taking any click route with it
} // namespace

void MainWindow::setupFollowNotify()
{
    if (!followSched_ || followOutbox_) return;
    followOutbox_ = std::make_unique<follownotify::Outbox>();
    followOutbox_->setPrompted(Settings::followNotifyPrompted());
    connect(followSched_, &FollowScheduler::newItemsFound, this,
            [this](const QString& seriesId, int, const QStringList& childIds) {
                followCycleFound_[seriesId] << childIds;
            });
    connect(followSched_, &FollowScheduler::cycleFinished, this, [this](int, int) { onFollowCycleFinished(); });
    followHoldPoll_ = new QTimer(this);
    followHoldPoll_->setInterval(kHoldPollMs);
    connect(followHoldPoll_, &QTimer::timeout, this, [this] {
        if (followNoticeHeld()) return;
        followHoldPoll_->stop();
        releaseFollowNotice();
    });
}

bool MainWindow::followNoticeHeld() const
{
    if (!isFullScreen() || !stack_) return false;
    QWidget* cur = stack_->currentWidget();
    return cur && (cur == playerPage_ || cur == retro_ || cur == retroPark_);
}

void MainWindow::onFollowCycleFinished()
{
    if (!followOutbox_) return;
    const QHash<QString, QStringList> found = followCycleFound_;
    followCycleFound_.clear();

    QVector<follownotify::SeriesNews> news;
    if (!found.isEmpty())
    {
        const QVector<FollowItem> follows = FollowStore::list();
        for (const FollowItem& f : follows)   // the store's order, so equal-time series rank stably
        {
            const auto it = found.constFind(f.itemId);
            if (it == found.constEnd()) continue;
            news << follownotify::newsFor(f.itemId, f.title, FollowSnapshot::get(f.itemId).pending, it.value(),
                                          dealtWith);
        }
    }
    const bool held = followNoticeHeld();
    const follownotify::Consent consent = consentNow();
    const follownotify::Decision d = followOutbox_->onCycle(news, consent, held, isMuted);
    if (followOutbox_->prompted() && !Settings::followNotifyPrompted()) Settings::setFollowNotifyPrompted(true);
    if (!news.isEmpty() || d.held)
        fnLog(QStringLiteral("cycle: %1 series with news, consent=%2, held=%3 -> notices=%4 prompt=%5")
                  .arg(news.size())
                  .arg(consent == follownotify::Consent::On ? QStringLiteral("on")
                       : consent == follownotify::Consent::Off ? QStringLiteral("off") : QStringLiteral("unset"))
                  .arg(held ? 1 : 0).arg(d.notices.size()).arg(d.prompt ? 1 : 0));
    if (d.held && followHoldPoll_ && !followHoldPoll_->isActive()) followHoldPoll_->start();
    applyFollowDecision(d);
}

void MainWindow::releaseFollowNotice()
{
    if (!followOutbox_ || !followOutbox_->hasPending()) return;
    const follownotify::Decision d = followOutbox_->release(consentNow(), isMuted, stillNew);
    fnLog(QStringLiteral("released after playback -> notices=%1 prompt=%2").arg(d.notices.size()).arg(d.prompt ? 1 : 0));
    applyFollowDecision(d);
}

void MainWindow::applyFollowDecision(const follownotify::Decision& d)
{
    if (d.prompt) showFollowNotifyPrompt();
    for (const follownotify::Notice& n : d.notices) showFollowSystemNotice(n.title, n.body);
}

void MainWindow::showFollowSystemNotice(const QString& title, const QString& body)
{
#if defined(Q_OS_ANDROID) || defined(Q_OS_IOS) || !QT_CONFIG(systemtrayicon)
    // No notification channel exists on these platforms yet (a follow-up; the New shelf still shows it all).
    fnLog(QStringLiteral("skipped (no notification channel on this platform): \"%1\"").arg(title));
    Q_UNUSED(body);
#else
    if (!QSystemTrayIcon::isSystemTrayAvailable())
    {
        fnLog(QStringLiteral("skipped (no system tray): \"%1\"").arg(title));
        return;
    }
    if (!followTray_)
    {
        QIcon icon = QApplication::windowIcon();
        if (icon.isNull()) icon = style()->standardIcon(QStyle::SP_MessageBoxInformation);
        followTray_ = new QSystemTrayIcon(icon, this);
        followTray_->setToolTip(QApplication::applicationDisplayName());
        connect(followTray_, &QSystemTrayIcon::messageClicked, this, &MainWindow::onFollowNoticeClicked);
        // A click on the icon itself (the message has gone to the action centre) lands in the same place.
        connect(followTray_, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason r) {
            if (r == QSystemTrayIcon::Trigger || r == QSystemTrayIcon::DoubleClick) onFollowNoticeClicked();
        });
        followTrayHide_ = new QTimer(this);
        followTrayHide_->setSingleShot(true);
        connect(followTrayHide_, &QTimer::timeout, this, [this] {
            if (followTray_) followTray_->hide();
            fnLog(QStringLiteral("tray icon hidden"));
        });
    }
    followTray_->show();
    followTray_->showMessage(title, body, QSystemTrayIcon::Information, kTrayMessageMs);
    fnLog(QStringLiteral("showMessage title=\"%1\" body=\"%2\"").arg(title, body));
    followTrayHide_->start(kTrayLingerMs);
#endif
}

void MainWindow::onFollowNoticeClicked()
{
    fnLog(QStringLiteral("notification clicked -> home (New shelf)"));
#if QT_CONFIG(systemtrayicon)
    if (followTrayHide_) followTrayHide_->stop();
    if (followTray_) followTray_->hide();
#endif
    if (isMinimized()) setWindowState(windowState() & ~Qt::WindowMinimized);
    show();
    raise();
    activateWindow();
    // Never pull the user out of something they are playing; otherwise land on the home screen, whose New
    // shelf is where every announced child is. Deferred past the tray's own signal delivery (#28/#211 rule).
    QTimer::singleShot(0, this, [this] {
        QWidget* cur = stack_ ? stack_->currentWidget() : nullptr;
        if (cur && (cur == playerPage_ || cur == retro_ || cur == retroPark_)) return;
        if (home_) home_->reloadForFilterChange();
        showHomeScreen();
    });
}

void MainWindow::showFollowNotifyPrompt()
{
    fnLog(QStringLiteral("one-time prompt shown"));
    if (!notifier_) return;
    notifier_->notifyWithAction(
        tr("Something new arrived in a series you follow — it is on the New shelf. Click here to get a desktop "
           "notification next time, or choose in Settings ▸ Following."),
        12000, [this] {
            setFollowNotifyFromUi(true);
            notify(tr("Notifications for new episodes are on. Mute a series from its Follow menu."), 4500);
        });
}

void MainWindow::setFollowNotifyFromUi(bool on)
{
    Settings::setFollowNotify(on);
    // Answered either way: the offer is spent even if the settings row got there first.
    if (!Settings::followNotifyPrompted()) Settings::setFollowNotifyPrompted(true);
    if (followOutbox_) followOutbox_->setPrompted(true);
    fnLog(QStringLiteral("setting -> %1").arg(on ? QStringLiteral("on") : QStringLiteral("off")));
}
