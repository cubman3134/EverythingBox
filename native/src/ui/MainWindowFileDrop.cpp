// LAN file drop (issue #115), the MainWindow half -- a separate translation unit for the reason
// MainWindowPlayOn.cpp gives: MainWindow.cpp is the busiest merge surface in the repository, and this feature
// reaches the rest of the class only through members that already existed.
//
// What lives here, and nothing more: the destination ROOTS as this app knows them (every ROM system folder
// from SystemCatalog + RomLibrary, and the video / music / photo library roots), handing RemoteServer the
// upload registry, the sweep of stale part files when file drop comes on, the scan after a file lands, and the
// settings toggle both builders call. Every decision -- ids, names, offsets, caps, the atomic landing -- is
// core/FileDrop's, and the routes are core/RemoteServer's.
//
// POSTURE. Off by default (Settings::fileDropEnabled). It rides #76's listener, LAN-bound as that is; when the
// remote control itself is off, turning file drop on opens the listener for file drop ALONE -- the control and
// hand-off routes answer 404 (RemoteServer::setControlSurface), so this never opens the unauthenticated remote
// -- and the toast says the listener is now open. Every /drop route except the page is token-gated
// (PlayOn::routeNeedsToken), and the token comes from the existing pairing flow: the code shows on this screen.
#include "MainWindow.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QPointer>
#include <QThreadPool>
#include <QTimer>
#include <algorithm>

#include "../core/AppPaths.h"
#include "../core/FileDrop.h"
#include "../core/RemoteServer.h"
#include "../core/RomLibrary.h"
#include "../core/Settings.h"
#include "../core/SystemCatalog.h"

namespace
{
    // The one-line append to <app>/stream_debug.log that MainWindow.cpp's mwLog does (a file-static there),
    // copied as MainWindowSendLibrary.cpp copies it. Names and counts only: never a token, never a code.
    void fdLog(const QString& msg)
    {
        QFile f(AppPaths::dataDir() + QStringLiteral("/stream_debug.log"));
        if (f.open(QIODevice::Append | QIODevice::Text))
            f.write((QDateTime::currentDateTime().toString(Qt::ISODate) + QStringLiteral("  ") + msg
                     + QLatin1String("\n")).toUtf8());
    }

    // Every folder this device offers, as it is configured right now. Asked on each request that needs it, so a
    // ROMs folder changed in Settings is honoured at once. A root whose directory does not exist is left out
    // by FileDrop::buildDestinations.
    QList<FileDrop::Destination> fileDropDestinations()
    {
        QList<FileDrop::Root> roms;
        const QString romRoot = RomLibrary::root();
        for (const GameSystem& s : SystemCatalog::systems())
            roms << FileDrop::Root{ QStringLiteral("rom"), s.id, s.name,
                                    romRoot + QLatin1Char('/') + RomLibrary::folderFor(s.id) };
        std::sort(roms.begin(), roms.end(), [](const FileDrop::Root& a, const FileDrop::Root& b) {
            return a.label.compare(b.label, Qt::CaseInsensitive) < 0;
        });
        QList<FileDrop::Root> roots;
        roots << FileDrop::Root{ QStringLiteral("video"), QStringLiteral("video"),
                                 MainWindow::tr("Videos (movies and TV)"), Settings::libraryFolder() }
              << FileDrop::Root{ QStringLiteral("music"), QStringLiteral("music"),
                                 MainWindow::tr("Music"), Settings::musicFolder() }
              << FileDrop::Root{ QStringLiteral("photo"), QStringLiteral("photo"),
                                 MainWindow::tr("Photos"), Settings::photosFolder() };
        roots << roms;
        return FileDrop::buildDestinations(roots);
    }
}

void MainWindow::applyFileDrop()
{
    if (!remoteServer_) return;
    if (!Settings::fileDropEnabled())
    {
        remoteServer_->setFileDrop(RemoteServer::DropHooks());
        if (fileDropOn_) fdLog(QStringLiteral("filedrop: off"));
        fileDropOn_ = false;
        return;
    }
    if (!fileDrop_) fileDrop_ = std::make_shared<FileDrop::Uploads>();

    RemoteServer::DropHooks d;
    d.uploads = fileDrop_;
    d.destinations = [] { return fileDropDestinations(); };
    QPointer<MainWindow> self(this);
    d.landed = [self](const FileDrop::Answer& a) {
        // Called inside the socket's readyRead delivery. Everything that follows -- a scan, a home refresh, a
        // toast -- runs on the next turn, never under that emission (the #28 / #211 family).
        const QString destId = a.destinationId;
        const QString name = a.name;
        QTimer::singleShot(0, self, [self, destId, name] {
            if (!self) return;
            QString kind, label;
            for (const FileDrop::Destination& dd : fileDropDestinations())
                if (dd.id == destId) { kind = dd.kind; label = dd.label; break; }
            fdLog(QStringLiteral("filedrop: landed \"%1\" in %2").arg(name, label.isEmpty() ? QStringLiteral("?") : label));
            // The incremental scan of the root it landed in, so it appears within seconds.
            if (kind == QLatin1String("rom"))
            {
                const int added = RomLibrary::syncToDownloads();
                fdLog(QStringLiteral("filedrop: ROM scan added %1 game(s)").arg(added));
            }
            else if (kind == QLatin1String("video")) self->rescanLocalLibrary();
            else if (kind == QLatin1String("music")) self->rescanMusicLibrary();
            // Photos are read from the folder when the Photos shelf opens: nothing to rescan.
            self->notify(tr("Received “%1” into %2").arg(name, label), 5000);
        });
    };
    remoteServer_->setFileDrop(d);

    if (fileDropOn_) return;
    fileDropOn_ = true;
    // The per-system folders are what the page offers, so make sure they exist -- exactly what opening the
    // Library's Local ROMs does.
    RomLibrary::ensureStructure();
    fdLog(QStringLiteral("filedrop: on, %1 destination(s) offered at %2/drop")
              .arg(fileDropDestinations().size())
              .arg(RemoteServer::lanUrl(remoteServer_->port())));
    // Sweep part files untouched for 24 h, off the GUI thread (it lists every destination folder). Uploads in
    // flight right now are skipped; one whose part goes is told to start again by FileDrop.
    QStringList dirs;
    for (const FileDrop::Destination& dd : fileDropDestinations()) dirs << dd.dir;
    const QSet<QString> busy = fileDrop_->busyIds();
    QThreadPool::globalInstance()->start([dirs, busy] {
        const int n = FileDrop::sweepDirs(dirs, QDateTime::currentDateTimeUtc(), busy);
        if (n > 0) fdLog(QStringLiteral("filedrop: swept %1 stale partial upload(s)").arg(n));
    });
}

QString MainWindow::fileDropStatusText() const
{
    if (!Settings::fileDropEnabled()) return tr("Turn on to get a URL");
    const quint16 port = remoteServer_ && remoteServer_->isListening()
                             ? remoteServer_->port()
                             : static_cast<quint16>(Settings::remoteControlPort());
    return RemoteServer::lanUrl(port) + QStringLiteral("/drop");
}

void MainWindow::setFileDropFromUi(bool on)
{
    const bool listenerWasUp = remoteServer_ && remoteServer_->isListening();
    Settings::setFileDropEnabled(on);
    updateRemoteServer();
    if (!on)
    {
        notify(Settings::remoteControlEnabled()
                   ? tr("File drop is off. Remote control is still on.")
                   : tr("File drop is off. The network listener is closed."), 5000);
        return;
    }
    if (!remoteServer_ || !remoteServer_->isListening()) return;   // updateRemoteServer said why
    // It says so: turning file drop on can open a listening port that was not open before.
    notify(listenerWasUp
               ? tr("File drop is on. Open %1 in a browser on your network and pair with the code shown here.")
                     .arg(fileDropStatusText())
               : tr("File drop is on, so this device is now listening on your network (for file drop only; remote "
                    "control stays off). Open %1 in a browser and pair with the code shown here.")
                     .arg(fileDropStatusText()),
           9000);
}
