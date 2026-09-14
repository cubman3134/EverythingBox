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
#include <QFileInfo>
#include <QLineEdit>
#include <QPointer>
#include <QTimer>

#include "../core/AppPaths.h"
#include "../core/CastManager.h"
#include "../core/GamelistStore.h"
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
    // an inventory is a list of what someone owns, so it is not a public read. #292: it also says this device
    // takes gamelist entries, in a field an older source does not read.
    return LibraryBundle::inventoryJson(LibraryBundle::inventoryFor(libraryCacheRoot()), true);
}

QByteArray MainWindow::libraryGamelistsJson() const
{
    // #292. Per system folder under THIS device's ROM root: the ROM files there, and the ones its gamelist
    // already lists. Token-checked by RemoteServer before this runs. #401: and the name rule this device lands
    // by, so the source's plan resolves `snes` against a `SNES` folder exactly as the landing here will.
    return LibraryBundle::sidecarInventoryJson(LibraryBundle::sidecarInventoryFor(Settings::romsFolder()),
                                               LibraryBundle::foldsNameCase());
}

std::shared_ptr<LibraryBundle::GamelistBatcher> MainWindow::libraryMakeGamelistBatcher()
{
    // #401. ONE batcher per listener: landed gamelist entries are committed per system in batches (the source's
    // flush, a size bound, RemoteServer's idle timer, or stop()), not once per game. Every commit clears the
    // gamelist cache so the next lookup reads the new entries; a failed one is logged, and its games -- still
    // unlisted -- are simply sent again by the next run's diff.
    auto batcher = std::make_shared<LibraryBundle::GamelistBatcher>();
    batcher->setOnCommit([](const LibraryBundle::GamelistCommit& c) {
        GamelistStore::clearCache();
        // One line per BATCH, never per game: the folder name and a count, no path and nothing from the source.
        const QString folder = QFileInfo(c.systemDir).fileName();
        if (!c.ok)
            slLog(QStringLiteral("bundle: %1 gamelist entr(ies) for %2 not committed — %3").arg(c.games).arg(folder, c.error));
        else
            slLog(QStringLiteral("bundle: gamelist for %1 committed with %2 new entr(ies)%3")
                      .arg(folder).arg(c.games)
                      .arg(c.error.isEmpty() ? QString() : QStringLiteral(" — ") + c.error));
    });
    return batcher;
}

LibraryBundle::Receipt MainWindow::libraryReceiveSidecarStream(LibraryBundle::GamelistBatcher& batches, QIODevice& body)
{
    // #292. The ROM root is this device's own setting -- never a path from the source -- and the cache root is
    // not handed over at all. The batcher refuses an unsafe system or ROM name before it builds a path, and
    // writes only inside a system folder that already exists, for a ROM that is already there.
    QString error;
    const LibraryBundle::LandResult r = batches.stage(Settings::romsFolder(), body, error);
    if (r == LibraryBundle::LandResult::Refused || r == LibraryBundle::LandResult::WriteFailed)
        slLog(QStringLiteral("bundle: refused a gamelist entry — %1").arg(error));
    return LibraryBundle::receiptFor(r, error);
}

QByteArray MainWindow::libraryFlushGamelist(LibraryBundle::GamelistBatcher& batches, const QByteArray& body)
{
    // #401. The source says one system is done: commit it now. An empty answer is a body naming no system.
    QString system;
    if (!LibraryBundle::parseGamelistFlushRequest(body, system)) return QByteArray();
    return LibraryBundle::gamelistFlushResultJson(batches.flush(Settings::romsFolder(), system));
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

LibraryBundle::Receipt MainWindow::libraryReceiveBundleStream(QIODevice& body)
{
    // #291. `body` is the spool RemoteServer wrote under the cache root. landItemV2 judges the whole header --
    // id, names, sizes, and that they account for exactly the bytes in the file -- before it copies a byte.
    QString error;
    const LibraryBundle::LandResult r = LibraryBundle::landItemV2(libraryCacheRoot(), body, error);
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
    sendLibFormat_ = LibraryBundle::kFormatVersion;
    sendLibSidecars_ = false;
    sendGameQueue_.clear();
    sendGameCursor_ = 0;

    notify(tr("Comparing libraries with %1…").arg(peer.name), 4000);

    QPointer<MainWindow> self(this);
    PlayOnClient* c = playOnClient();
    connect(c, &PlayOnClient::inventoryArrived, this,
            [self, peer, token](const QString& id, const QList<LibraryBundle::Entry>& theirs,
                                bool ok, const QString& message, const QList<int>& formats, bool sidecars) {
        if (!self || id != peer.id) return;
        if (!ok) { self->notify(message, 6000); return; }
        // #291: raw bodies to a target that says it takes them; v1, exactly as before, to one that does not.
        self->sendLibFormat_ = LibraryBundle::chooseBundleFormat(formats);
        // #292: gamelist entries only to a target that says it takes them -- and those always ride v2, which
        // any target that says so also takes.
        self->sendLibSidecars_ = sidecars && self->sendLibFormat_ == LibraryBundle::kPayloadFormatV2;

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
            // Nothing in the art cache to move; the gamelists are still compared when the target takes them.
            if (self->sendLibSidecars_) { self->sendLibraryGamelists(peer, token); return; }
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
        sendLibQueue_.clear();
        sendLibCursor_ = 0;
        // #292: one run, both kinds -- the art items, then the gamelist entries.
        if (sendLibSidecars_) { sendLibraryGamelists(peer, token); return; }
        notify(LibraryBundle::describeProgress(sendLibProgress_, peer.name), 8000);
        return;
    }

    const QString itemId = sendLibQueue_.at(sendLibCursor_);
    // The item is read whole into memory -- at most the format's ceiling (64 MiB for v2), one item at a time --
    // and posted as one body, rather than streamed from the files while the request is in flight. Reading it
    // first snapshots the item: a thumb evicted or re-fetched mid-send cannot put a body on the wire whose
    // sizes no longer match its header (the target would refuse it as Malformed, every run).
    const int format = sendLibFormat_;
    LibraryBundle::Payload p;
    QString error;
    if (!LibraryBundle::readPayload(libraryCacheRoot(), itemId, p, error, LibraryBundle::maxItemBytesFor(format)))
    {
        // The item went away (an eviction, an uninstall) between the diff and the read. Not a failure of the
        // run: skip it, and let the next run's diff decide again.
        ++sendLibCursor_;
        ++sendLibProgress_.failed;
        QTimer::singleShot(0, this, [this, peer, token] { sendLibraryNextItem(peer, token); });
        return;
    }
    const QByteArray wire = format == LibraryBundle::kPayloadFormatV2 ? LibraryBundle::encodePayloadV2(p)
                                                                      : LibraryBundle::encodePayload(p);
    const qint64 fileBytes = LibraryBundle::fileBytesOf(p);
    p = LibraryBundle::Payload();   // the wire holds the bytes now; do not keep a second copy for the request's life

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

    // Honest bytes (#291): what lands on the target's disk, for both formats -- not the wire, which for v1 is
    // a third larger than the art it carries.
    sendLibProgress_.bytesSent += fileBytes;
    c->sendBundleItem(peer, token, itemId, wire, format);
}

// ---------------------------------------------------------------------------- gamelist entries (#292) -----

void MainWindow::sendLibraryGamelists(const PlayOn::Peer& peer, const QString& token)
{
    if (sendLibPeerId_ != peer.id) return;
    sendGameQueue_.clear();
    sendGameCursor_ = 0;
    sendGameFlushedAt_ = -1;

    QPointer<MainWindow> self(this);
    PlayOnClient* c = playOnClient();
    connect(c, &PlayOnClient::gamelistsArrived, this,
            [self, peer, token](const QString& id, const QList<LibraryBundle::SidecarSystem>& theirs, bool ok,
                                const QString& message, bool caseInsensitive) {
        if (!self || id != peer.id || self->sendLibPeerId_ != peer.id) return;
        if (!ok)
        {
            // The art half's result still stands; say what the gamelist half could not do.
            if (!message.isEmpty()) slLog(QStringLiteral("bundle: gamelists — %1").arg(message));
            self->notify(LibraryBundle::describeProgress(self->sendLibProgress_, peer.name) + QLatin1Char(' ')
                             + tr("Its gamelists could not be compared: %1").arg(message),
                         8000);
            return;
        }
        // THE DIFF: games whose ROM the target has and whose entry its gamelist lacks. The rest never leave. #401:
        // names are matched by the TARGET's rule, which it states (a Windows `SNES` takes a source's `snes`).
        const LibraryBundle::SidecarPlan plan = LibraryBundle::planSidecars(
            LibraryBundle::sidecarGamesFor(Settings::romsFolder()), theirs, caseInsensitive);
        self->sendLibProgress_.gamelists          = true;
        self->sendLibProgress_.gamesListed        = plan.alreadyListed;
        self->sendLibProgress_.gamesNotApplicable = plan.notApplicable;
        self->sendGameQueue_ = plan.send;
        self->sendGameCursor_ = 0;
        self->sendGameFlushedAt_ = -1;
        self->sendLibraryNextGame(peer, token);
    }, Qt::SingleShotConnection);
    c->fetchGamelists(peer, token);
}

void MainWindow::sendLibraryNextGame(const PlayOn::Peer& peer, const QString& token)
{
    if (sendLibPeerId_ != peer.id) return;

    // #401: the target commits gamelist entries per system in batches. When the queue moves past a system (the
    // plan keeps each system's games together) or ends, say so, and wait for the answer: a batch whose commit
    // failed is counted as not added, and the final sentence must not be written before that is known.
    const int n = int(sendGameQueue_.size());
    if (sendGameCursor_ > 0 && sendGameFlushedAt_ != sendGameCursor_
        && (sendGameCursor_ >= n
            || sendGameQueue_.at(sendGameCursor_).system != sendGameQueue_.at(sendGameCursor_ - 1).system))
    {
        sendGameFlushedAt_ = sendGameCursor_;
        const QString system = sendGameQueue_.at(sendGameCursor_ - 1).system;
        QPointer<MainWindow> self(this);
        PlayOnClient* c = playOnClient();
        connect(c, &PlayOnClient::gamelistFlushed, this,
                [self, peer, token, system](const QString& pid, const QString& sys, bool ok, int, int failed) {
            if (!self || pid != peer.id || sys != system) return;
            // An older target has no flush route: it committed every game as it landed, so there is nothing
            // to correct. A newer one says how many of this system's "landed" games did not commit after all.
            if (ok && failed > 0)
            {
                const int moved = qMin(failed, self->sendLibProgress_.gamesAdded);
                self->sendLibProgress_.gamesAdded  -= moved;
                self->sendLibProgress_.gamesFailed += moved;
                slLog(QStringLiteral("bundle: %1 gamelist entr(ies) for %2 were not committed by the target")
                          .arg(failed).arg(system));
            }
            // Past THIS delivery, for the same #28/#211 reason as every other leg.
            QTimer::singleShot(0, self, [self, peer, token] {
                if (self) self->sendLibraryNextGame(peer, token);
            });
        }, Qt::SingleShotConnection);
        c->flushGamelist(peer, token, system);
        return;
    }

    if (sendGameCursor_ >= sendGameQueue_.size())
    {
        notify(LibraryBundle::describeProgress(sendLibProgress_, peer.name), 8000);
        sendGameQueue_.clear();
        sendGameCursor_ = 0;
        return;
    }

    const LibraryBundle::SidecarGame game = sendGameQueue_.at(sendGameCursor_);
    LibraryBundle::SidecarPayload p;
    QString error;
    if (!LibraryBundle::readSidecarPayload(Settings::romsFolder(), game, p, error))
    {
        ++sendGameCursor_;
        ++sendLibProgress_.gamesFailed;
        QTimer::singleShot(0, this, [this, peer, token] { sendLibraryNextGame(peer, token); });
        return;
    }
    const QByteArray wire = LibraryBundle::encodeSidecarV2(p);
    // Honest bytes: the images that land, counted with the art items' bytes.
    sendLibProgress_.bytesSent += LibraryBundle::fileBytesOf(p);
    // A reply key for this one request; it names no path on either machine.
    const QString key = QStringLiteral("gamelist:") + game.system + QLatin1Char('/') + game.rom;

    QPointer<MainWindow> self(this);
    PlayOnClient* c = playOnClient();
    connect(c, &PlayOnClient::bundleItemDone, this,
            [self, peer, token, key](const QString& pid, const QString& iid, bool ok,
                                     const QString& result, const QString& message) {
        if (!self || pid != peer.id || iid != key) return;
        if (ok && result == QStringLiteral("landed"))             ++self->sendLibProgress_.gamesAdded;
        else if (ok && result == QStringLiteral("current"))       ++self->sendLibProgress_.gamesListed;
        else if (ok && result == QStringLiteral("notapplicable")) ++self->sendLibProgress_.gamesNotApplicable;
        else
        {
            ++self->sendLibProgress_.gamesFailed;
            if (!message.isEmpty()) slLog(QStringLiteral("bundle: gamelist entry — %1").arg(message));
        }
        ++self->sendGameCursor_;
        // Past THIS delivery, for the same #28/#211 reason as the art items.
        QTimer::singleShot(0, self, [self, peer, token] {
            if (self) self->sendLibraryNextGame(peer, token);
        });
    }, Qt::SingleShotConnection);
    c->sendBundleItem(peer, token, key, wire, LibraryBundle::kPayloadFormatV2);
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
