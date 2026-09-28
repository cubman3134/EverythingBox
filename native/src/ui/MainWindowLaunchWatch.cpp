// Store-launch play time (issue #61), the MainWindow half — a SEPARATE translation unit that defines
// MainWindow::handOffStoreLaunch, off the busiest TU (the #143 / #186 direction).
//
// A steam:// or com.epicgames.launcher:// launch used to be fire-and-forget: the store client owns the process,
// so nothing learned when the game exited and no play time was ever banked for it. Now the hand-off itself is
// unchanged — the same QDesktopServices::openUrl — and, for a RUN, a LaunchWatcher samples "is the game running?"
// every few seconds (LaunchWatch.h has the rules, LaunchSamplers.h the Windows reads). The session is added to
// PlayStats when it exits, under the per-launcher id the Recent records ("steam:<appid>" / "epic:<AppName>") —
// PlayStats::identity(id, path), exactly as launchPcExe banks a GOG session under its item id.
//
// What is deliberately NOT recorded: a launch that never showed a running game within 3 minutes (an update, a
// login prompt, a cancel), a session under 30 s, and anything still running when the app quits. Off Windows
// there are no samplers and the launch stays fire-and-forget, as before.
#include "MainWindow.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QFile>
#include <QUrl>

#include "../core/AppPaths.h"
#include "../core/EpicLibrary.h"
#include "../core/LaunchSamplers.h"
#include "../core/LaunchWatch.h"
#include "../core/LogSafeText.h"
#include "../core/PlayStats.h"
#include "../core/SteamLibrary.h"

namespace {
void watchLog(const QString& msg)
{
    QFile f(AppPaths::dataDir() + QStringLiteral("/stream_debug.log"));
    if (f.open(QIODevice::Append | QIODevice::Text))
        f.write((QDateTime::currentDateTime().toString(Qt::ISODate) + QStringLiteral("  ") + msg
                 + QStringLiteral("\n")).toUtf8());
}

// EB_UITEST_STORE_URI_SINK: a UI-test drive exercises the watch without handing the URI to a real Steam or Epic
// client (which would start the user's real game, or an install). The URI is logged as held back instead; the
// drive starts and stops its own fixture process. Honoured only with the test channel on (the #80 / #98 shape).
bool storeUriSinkForTest()
{
    return qEnvironmentVariableIsSet("EB_UITEST") && qEnvironmentVariableIsSet("EB_UITEST_STORE_URI_SINK");
}

QString epicInstallLocation(const QString& appName)
{
    for (const EpicGame& g : EpicLibrary::installedGames())
        if (g.appName == appName) return g.installLocation;
    return QString();
}

const char* endName(LaunchWatch::State s)
{
    switch (s)
    {
        case LaunchWatch::State::Exited:   return "exited";
        case LaunchWatch::State::TimedOut: return "timed out";
        default:                           return "ended";
    }
}
} // namespace

void MainWindow::handOffStoreLaunch(const QString& url, const QString& id)
{
    if (storeUriSinkForTest())
        watchLog(QStringLiteral("uitest: store launch URI held back (EB_UITEST_STORE_URI_SINK): %1")
                     .arg(LogSafeText::url(url)));
    else
        QDesktopServices::openUrl(QUrl(url));

    using K = LaunchWatch::Target::Kind;
    const LaunchWatch::Target t = LaunchWatch::targetForLaunchUrl(url);
    if (t.kind == K::None) return;   // steam://install/<id> opens Steam's install dialog; nothing plays
    const bool steam = t.kind == K::Steam;
    const QString prefix = steam ? QStringLiteral("steam:") : QStringLiteral("epic:");
    const QString key = id.startsWith(prefix) ? id : prefix + t.storeId;   // the Recent's key, built the same way
    const QString playId = PlayStats::identity(key, url);

    if (!LaunchSamplers::supported())
    {
        watchLog(QStringLiteral("launchwatch: %1 - no watch on this platform; the launch stays fire-and-forget").arg(key));
        return;
    }
    if (!launchWatcher_)
    {
        launchWatcher_ = std::make_shared<LaunchWatcher>(
            [](const QString& gameKey, const QString& pid, LaunchWatch::State end, qint64 secs) {
                if (secs > 0) PlayStats::addSession(pid, secs);
                watchLog(QStringLiteral("launchwatch: %1 %2, %3")
                             .arg(gameKey, QLatin1String(endName(end)),
                                  secs > 0 ? QStringLiteral("recorded %1s of play").arg(secs)
                                           : QStringLiteral("nothing recorded")));
            });
        launchWatcher_->setOnRunning([](const QString& gameKey) {
            watchLog(QStringLiteral("launchwatch: %1 running").arg(gameKey));
        });
        // Quitting ends every watch WITHOUT recording: a session still running has no end to measure, and a
        // guessed one would be worse than none. stopAll only clears a list and stops a timer — nothing here can
        // hold the quit (#442's budget).
        std::weak_ptr<LaunchWatcher> weak = launchWatcher_;
        connect(qApp, &QCoreApplication::aboutToQuit, this, [weak] {
            const auto w = weak.lock();
            if (!w || w->count() == 0) return;
            watchLog(QStringLiteral("launchwatch: quitting - %1 watch(es) dropped, nothing recorded").arg(w->count()));
            w->stopAll();
        });
    }
    if (launchWatcher_->isWatching(key))
    {
        watchLog(QStringLiteral("launchwatch: %1 is already being watched; this launch adds no second watch").arg(key));
        return;
    }
    const QString dir = steam ? SteamLibrary::installDirFor(t.storeId) : epicInstallLocation(t.storeId);
    const LaunchWatcher::Sampler sampler = steam ? LaunchSamplers::steam(t.storeId, dir) : LaunchSamplers::epic(dir);
    if (!launchWatcher_->watch(key, playId, sampler))
    {
        watchLog(QStringLiteral("launchwatch: %1 - no install location known; not watched").arg(key));
        return;
    }
    watchLog(QStringLiteral("launchwatch: %1 pending (%2%3)")
                 .arg(key, steam ? QStringLiteral("RunningAppID, else processes under ") : QStringLiteral("processes under "),
                      dir.isEmpty() ? QStringLiteral("<no install dir>") : dir));
}
