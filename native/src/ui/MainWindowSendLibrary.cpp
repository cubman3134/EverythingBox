// "Send library to device" (issue #127), the MainWindow half — a SEPARATE translation unit that defines
// MainWindow's #127 members, for the reason MainWindowPlayOn.cpp next door is separate: MainWindow.cpp is the
// busiest merge surface in the repository, and this feature reaches the rest of the class only through
// members that already existed.
//
// THE SHAPE, in one paragraph. A TV box that has never seen your library re-scrapes thousands of images the
// desktop already has on disk. This walks the desktop's MetaCache, asks the target what IT holds (an item id
// plus a content stamp each), and sends only what is missing or newer — one item per request, so a run that
// is interrupted simply re-diffs next time instead of restarting. Every decision is in LibraryBundle (pure,
// probe-driven); this file is the menu, the pairing detour and the progress line.
//
// WHAT IT IS BUILT ON, AND WHAT IT IS NOT. The transport is #143's, extended rather than duplicated: the same
// mDNS peers, the same code-on-the-target pairing, the same device-local token, two more routes on the one
// #76 listener. A device already paired for Play on device is already paired for this.
//
// THREE RULES, each of which is a bug this file exists to make impossible:
//
//   * IT MOVES ART, NOT STATE. MetaCache per-item folders only. Marks, favourites, resume and playlists are
//     drive sync's business; two systems owning one datum is how sync bugs are born. Nothing here opens a
//     state store — not to read it, not to write it.
//   * IT NEVER FIGHTS. An item the target has with a NEWER stamp is left alone, and the target enforces that
//     itself rather than trusting the source's diff. Landing is atomic per item.
//   * ROM AND MEDIA TRANSFER IS NOT HERE. The issue floats it; it needs chunked upload and a disk-space
//     story, and it is the part that can fill a device. The extension allowlist in LibraryBundle is what
//     keeps a trailer, a theme song, a manual and a ROM out of a bundle today.
#include "MainWindow.h"

#include <QDateTime>
#include <QFile>
#include <QLineEdit>
#include <QPointer>
#include <QTimer>

#include "../core/AppPaths.h"
#include "../core/CastManager.h"
#include "../core/LibraryBundle.h"
#include "../core/PlayOnClient.h"
#include "../core/PlayOnHost.h"
#include "../core/RemoteServer.h"
#include "../core/Settings.h"
#include "nav/NavOverlay.h"
#include "nav/Osk.h"

namespace {

// The same one-line append to <app>/stream_debug.log that MainWindow.cpp's mwLog does, copied for the reason
// MainWindowOpenFail.cpp copies it: mwLog is a file-static there, and lifting it out would touch the busiest
// file in the repository for no other reason. A refused item is the one thing a transfer needs a trace of —
// the receiving end deliberately shows no toast per item, so without this line a refusal is invisible.
void slLog(const QString& msg)
{
    QFile f(AppPaths::dataDir() + QStringLiteral("/stream_debug.log"));
    if (f.open(QIODevice::Append | QIODevice::Text))
        f.write((QDateTime::currentDateTime().toString(Qt::ISODate) + QStringLiteral("  ") + msg
                 + QLatin1String("\n")).toUtf8());
}

} // namespace

// ---------------------------------------------------------------------------- this device as a TARGET -----

QString MainWindow::libraryCacheRoot()
{
    // ONE spelling of the cache root, and it is MetaCache's own layout (<dataDir>/metadata/<sha1(key)>/).
    // Everything #127 writes is under here; LibraryBundle cannot be handed anything else.
    return AppPaths::dataDir() + QStringLiteral("/metadata");
}

QByteArray MainWindow::libraryInventoryJson() const
{
    // What this device holds, stamped. Called only after RemoteServer has checked the caller's paired token —
    // an inventory is a list of what someone owns, so it is not a public read.
    return LibraryBundle::inventoryJson(LibraryBundle::inventoryFor(libraryCacheRoot()));
}

LibraryBundle::Receipt MainWindow::libraryReceiveBundle(const QByteArray& body)
{
    LibraryBundle::Payload p;
    LibraryBundle::Refusal why = LibraryBundle::Refusal::None;
    QString message;
    if (!LibraryBundle::decodePayload(body, p, why, message))
        return LibraryBundle::receiptFor(LibraryBundle::LandResult::Refused, message);

    QString error;
    const LibraryBundle::LandResult r = LibraryBundle::landItem(libraryCacheRoot(), p, error);
    // Deliberately silent on success: a library transfer is thousands of items, and a toast per item would
    // bury the screen. The SOURCE reports the run; this end reports only what it refused.
    if (r == LibraryBundle::LandResult::Refused || r == LibraryBundle::LandResult::WriteFailed)
        slLog(QStringLiteral("bundle: refused an item — %1").arg(error));
    return LibraryBundle::receiptFor(r, error);
}

// ---------------------------------------------------------------------------- this device as a SOURCE -----

void MainWindow::sendLibraryTo(const PlayOn::Peer& peer)
{
    if (!playOnHost_) playOnHost_ = new PlayOnHost(this);

    const QString token = playOnHost_->tokenFor(peer.id);
    if (!token.isEmpty()) { sendLibraryWithToken(peer, token); return; }

    // Not paired yet: the SAME code-on-the-target flow #143 uses. The code appears on the target's screen,
    // which is what makes a pairing a statement about being in the same house.
    notify(tr("Asking %1 to show a pairing code…").arg(peer.name), 4000);
    QPointer<MainWindow> self(this);
    PlayOnClient* c = playOnClient();
    connect(c, &PlayOnClient::pairingOffered, this, [self, peer](const QString& id) {
        if (!self || id != peer.id) return;
        // DEFER PAST THIS EMISSION. The OSK below is a nested event loop and we are inside a QNetworkReply
        // finished() delivery; opening it here would run that loop under a frame that still owns the reply.
        QTimer::singleShot(0, self, [self, peer] {
            if (!self) return;
            const QString code = Osk::getText(tr("Enter the code shown on %1").arg(peer.name),
                                              QString(), QLineEdit::Normal, self);
            if (code.isNull()) return;
            QPointer<MainWindow> me(self);
            PlayOnClient* cc = self->playOnClient();
            connect(cc, &PlayOnClient::paired, self, [me, peer](const QString& pid, const QString& tok) {
                if (!me || pid != peer.id) return;
                me->playOnHost_->storeToken(peer.id, tok);      // the one place a token is written
                me->notify(tr("Paired with %1.").arg(peer.name), 4000);
                me->sendLibraryWithToken(peer, tok);
            }, Qt::SingleShotConnection);
            connect(cc, &PlayOnClient::pairingFailed, self, [me](const QString&, const QString& msg) {
                if (me) me->notify(msg, 6000);
            }, Qt::SingleShotConnection);
            cc->redeemPairing(peer, code);
        });
    }, Qt::SingleShotConnection);
    connect(c, &PlayOnClient::pairingFailed, this, [self](const QString&, const QString& msg) {
        if (self) self->notify(msg, 6000);
    }, Qt::SingleShotConnection);
    c->requestPairing(peer);
}

void MainWindow::sendLibraryWithToken(const PlayOn::Peer& peer, const QString& token)
{
    sendLibQueue_.clear();
    sendLibCursor_ = 0;
    sendLibProgress_ = LibraryBundle::Progress();
    sendLibPeerId_ = peer.id;

    notify(tr("Comparing libraries with %1…").arg(peer.name), 4000);

    QPointer<MainWindow> self(this);
    PlayOnClient* c = playOnClient();
    connect(c, &PlayOnClient::inventoryArrived, this,
            [self, peer, token](const QString& id, const QList<LibraryBundle::Entry>& theirs,
                                bool ok, const QString& message) {
        if (!self || id != peer.id) return;
        if (!ok) { self->notify(message, 6000); return; }

        // THE DIFF, and it is the whole feature: only what is missing or newer leaves this machine.
        const QList<LibraryBundle::Entry> mine =
            LibraryBundle::inventoryFor(MainWindow::libraryCacheRoot());
        const LibraryBundle::Plan plan = LibraryBundle::planTransfer(mine, theirs);

        self->sendLibQueue_ = plan.send;
        self->sendLibCursor_ = 0;
        self->sendLibProgress_ = LibraryBundle::Progress();
        self->sendLibProgress_.itemsTotal = int(plan.send.size());
        self->sendLibProgress_.unchanged  = int(plan.unchanged.size());
        self->sendLibProgress_.keptNewer  = int(plan.targetNewer.size());

        if (plan.send.isEmpty())
        {
            // The second run, and the sentence that says the feature worked.
            self->notify(LibraryBundle::describeProgress(self->sendLibProgress_, peer.name), 6000);
            return;
        }
        self->notify(tr("Sending %n item(s) to %1…", "", int(plan.send.size())).arg(peer.name), 5000);
        self->sendLibraryNextItem(peer, token);
    }, Qt::SingleShotConnection);

    c->fetchInventory(peer, token);
}

void MainWindow::sendLibraryNextItem(const PlayOn::Peer& peer, const QString& token)
{
    if (sendLibPeerId_ != peer.id) return;                     // a newer run took over; this one is stale
    if (sendLibCursor_ >= sendLibQueue_.size())
    {
        notify(LibraryBundle::describeProgress(sendLibProgress_, peer.name), 8000);
        sendLibQueue_.clear();
        sendLibCursor_ = 0;
        return;
    }

    const QString itemId = sendLibQueue_.at(sendLibCursor_);
    LibraryBundle::Payload p;
    QString error;
    if (!LibraryBundle::readPayload(libraryCacheRoot(), itemId, p, error))
    {
        // The item went away (an eviction, an uninstall) between the diff and the read. Not a failure of the
        // run: skip it, and let the next run's diff decide again.
        ++sendLibCursor_;
        ++sendLibProgress_.failed;
        QTimer::singleShot(0, this, [this, peer, token] { sendLibraryNextItem(peer, token); });
        return;
    }
    const QByteArray wire = LibraryBundle::encodePayload(p);

    QPointer<MainWindow> self(this);
    PlayOnClient* c = playOnClient();
    connect(c, &PlayOnClient::bundleItemDone, this,
            [self, peer, token, itemId](const QString& pid, const QString& iid, bool ok,
                                        const QString& result, const QString& message) {
        if (!self || pid != peer.id || iid != itemId) return;
        if (ok && result == QStringLiteral("landed"))            ++self->sendLibProgress_.itemsSent;
        else if (ok && result == QStringLiteral("kept"))         ++self->sendLibProgress_.keptNewer;
        else if (ok && result == QStringLiteral("current"))      ++self->sendLibProgress_.unchanged;
        else
        {
            ++self->sendLibProgress_.failed;
            if (!message.isEmpty()) slLog(QStringLiteral("bundle: %1").arg(message));
        }
        ++self->sendLibCursor_;
        // One item per request, and the next is queued past THIS delivery: we are inside a QNetworkReply
        // finished() emission, and the #28/#211 family is exactly what starting the next leg here would be.
        QTimer::singleShot(0, self, [self, peer, token] {
            if (self) self->sendLibraryNextItem(peer, token);
        });
    }, Qt::SingleShotConnection);

    sendLibProgress_.bytesSent += qint64(wire.size());
    c->sendBundleItem(peer, token, itemId, wire);
}

// ---------------------------------------------------------------------------- the way in ------------------

void MainWindow::showSendLibraryMenu()
{
    if (castMgr_) castMgr_->startDiscovery();
    if (!playOnHost_) playOnHost_ = new PlayOnHost(this);

    QList<PlayOn::Peer> peers;
    for (const PlayOn::Target& t : playOnTargets())
        if (t.kind == PlayOn::TargetKind::EverythingBox) peers << playOnPeerById(t.id.mid(3));

    if (peers.isEmpty())
    {
        notify(Settings::remoteControlEnabled()
                   ? tr("No other EverythingBox found on this network yet — give it a moment, and make sure "
                        "remote control is on there too.")
                   : tr("Turn on Settings ▸ General ▸ Remote control so other devices can find this one."),
               8000);
        return;
    }

    const int cached = int(LibraryBundle::inventoryFor(libraryCacheRoot()).size());
    QStringList rows;
    for (const PlayOn::Peer& p : peers)
        rows << tr("📚  Send library to %1").arg(p.name);

    const int pick = NavMenu::pick(tr("Send library to device (%n item(s) cached here)", "", cached),
                                   rows, this);
    if (pick < 0 || pick >= peers.size()) return;
    sendLibraryTo(peers.at(pick));
}
