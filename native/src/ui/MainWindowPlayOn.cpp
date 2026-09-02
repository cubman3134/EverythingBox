// "Play on device" (issue #143), the MainWindow half — a SEPARATE translation unit that defines
// MainWindow's #143 members.
//
// WHY THIS IS NOT IN MainWindow.cpp. That file is 25,000 lines and is the single busiest merge surface in
// the repository; several features land in it at once and every one of them pays for the others' conflicts.
// Everything below is self-contained (it reaches the rest of MainWindow only through members that already
// existed) so it costs MainWindow.cpp four short insertions instead of four hundred lines. The class is
// unchanged: these are ordinary member functions, declared in MainWindow.h beside the rest.
//
// The shape of the feature, in one paragraph. An instance with #76's server on advertises itself over mDNS;
// CastManager browses for the same service, so the cast/output picker lists peers beside Chromecast and DLNA
// (PlayOn::mergeTargets, which drops this instance from its own list). Choosing a peer pairs with it once —
// a code on the TARGET's screen, entered here, redeemed for a token stored device-local — and then hands
// off an item REFERENCE plus a position. The target resolves its own stream; a reference it cannot resolve
// comes back 409 and is shown as "not available on <device>". After the hand-off this instance becomes a
// remote over the peer's /state and /player, and "Continue on this device" is the same two endpoints run in
// the opposite direction.
//
// NO BYTES CROSS THE WIRE HERE and no credential is ever logged. Both rules are pinned by probe_playon.
#include "MainWindow.h"

#include <QFileInfo>
#include <QMenu>
#include <QPointer>
#include <QSettings>
#include <QTimer>

#include "../addons/AddonManager.h"
#include "../core/AppBrand.h"
#include "../core/AppPaths.h"
#include "../core/CastManager.h"
#include "../core/PlayOnClient.h"
#include "../core/PlayOnHost.h"
#include "../core/ProfileStore.h"
#include "../core/RecentStore.h"
#include "../core/RemoteServer.h"
#include "../core/ResumeStore.h"
#include "../core/Settings.h"
#include "../video/MpvWidget.h"
#include "nav/NavOverlay.h"
#include "nav/Osk.h"

namespace
{
    // The Recents "kind" a hand-off's item type routes to. Same closed vocabulary openRecent dispatches on;
    // an unknown type falls to "video", which is the only kind that can open an arbitrary media file.
    QString recentKindFor(const QString& type)
    {
        const QString t = type.toLower();
        if (t == QLatin1String("music") || t == QLatin1String("audio") || t == QLatin1String("song")
            || t == QLatin1String("audiobook"))
            return QStringLiteral("audio");
        if (t == QLatin1String("book") || t == QLatin1String("comic") || t == QLatin1String("manga")
            || t == QLatin1String("document"))
            return QStringLiteral("document");
        if (t == QLatin1String("game")) return QStringLiteral("game");
        return QStringLiteral("video");
    }

    QSettings& iniStore()
    {
        static QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                           QSettings::IniFormat);
        return s;
    }
}

// ---------------------------------------------------------------------------- the advertisement ----------
// Reconcile what this instance publishes with whether it can actually be handed anything. The advert is set
// ONLY while #76's server is listening: an instance with no controllable surface has nothing to advertise,
// and advertising it anyway would put a row in every peer's picker that fails the moment it is pressed.
void MainWindow::updatePlayOnAdvert()
{
    if (!castMgr_) return;
    if (!playOnHost_) playOnHost_ = new PlayOnHost(this);
    if (remoteServer_ && remoteServer_->isListening())
        castMgr_->setPlayOnAdvert(PlayOnHost::advertFor(remoteServer_->port()));
    else
        castMgr_->clearPlayOnAdvert();
}

PlayOnClient* MainWindow::playOnClient()
{
    if (!playOnClient_) playOnClient_ = new PlayOnClient(this);
    return playOnClient_;
}

// ---------------------------------------------------------------------------- what is playing ------------
// The current item as a REFERENCE. syncKey_ is already this app's stable identity for what is playing (a
// local file's path, or a stream's resume key that survives a re-resolved link), and the Recents row filed
// under it carries #224's re-mint recipe — an addon manifest id, an item id and a route. That recipe is
// EXACTLY what a peer needs to resolve its own stream, which is why this feature needs no new bookkeeping at
// play time: the reference it hands over is the one the app already had to keep in order to re-open a row.
PlayOn::Handoff MainWindow::playOnCurrentHandoff(bool* ok) const
{
    if (ok) *ok = false;
    PlayOn::Handoff h;
    if (syncKey_.isEmpty()) return h;

    const RecentItem row = RecentStore::find(syncKey_);
    if (!row.sourceItemId.isEmpty() && !row.sourceAddonId.isEmpty())
    {
        h.ref.kind   = row.sourceRoute == QLatin1String("imdb") ? QStringLiteral("catalog")
                                                                : QStringLiteral("addon");
        h.ref.id     = row.sourceItemId;
        h.ref.source = row.sourceAddonId;
        h.ref.type   = row.sourceType;
    }
    else if (QFileInfo::exists(syncKey_))
    {
        h.ref.kind = QStringLiteral("local");
        h.ref.id   = syncKey_;
        h.ref.type = row.kind.isEmpty() ? QStringLiteral("video") : row.kind;
    }
    else
    {
        // Playing something with no re-mint recipe and no file behind it — a pasted link, an IPTV channel.
        // There is nothing here a peer could resolve, and inventing a reference from the title would open the
        // wrong thing on the other device. Report it as unnameable and let the caller say so.
        return h;
    }

    h.ref.title = curPlayTitle_.isEmpty() ? row.title : curPlayTitle_;
    h.positionSec = lastPos_ > 0.0 ? lastPos_ : 0.0;
    if (player_)
    {
        for (const MpvWidget::Track& t : player_->audioTracks())
            if (t.selected) { h.audioTrack = QString::number(t.id); break; }
        for (const MpvWidget::Track& t : player_->subtitleTracks())
            if (t.selected) { h.subtitleTrack = QString::number(t.id); break; }
    }
    if (ok) *ok = true;
    return h;
}

// ---------------------------------------------------------------------------- as a TARGET ----------------
// What THIS device knows about a reference that has just arrived. Flat answers only: PlayOn::decideOpen makes
// the decision, and keeping the looking-up here is what lets the probe pin the decision without an app.
PlayOn::OpenEnv MainWindow::playOnClassify(const PlayOn::Handoff& h) const
{
    PlayOn::OpenEnv env;
    // A local reference is a path. Two boxes in one house usually mount the same library, and when they do
    // not, THIS is the honest 409 the whole contract is built around.
    env.localIdKnown = !h.ref.id.isEmpty() && QFileInfo::exists(h.ref.id);
    // A catalogue / addon reference needs a source that can resolve it. "Any source at all" is a deliberately
    // coarse test for this increment: the fine one (does THIS addon id exist here) would refuse a reference
    // that a differently-configured but equally capable peer resolves perfectly well.
    env.addonsAvailable  = addons_ != nullptr;
    env.serverConfigured = addons_ != nullptr;
    // The wall. A restricted profile refuses everything that arrives, because a hand-off enters BELOW the
    // browse surface through which the restriction is expressed — there is no shelf here to have hidden the
    // item from. Coarse, and deliberately so: the failure mode of the coarse rule is "go and press play on
    // that device yourself", and the failure mode of a clever one is a wall that a hand-off walks through.
    if (ProfileStore::current().restricted)
    {
        env.profileBlocks = true;
        env.blockReason = tr("that device is on a restricted profile");
    }
    return env;
}

PlayOn::OpenResult MainWindow::playOnOpen(const PlayOn::Handoff& h)
{
    const PlayOn::OpenResult r = PlayOn::decideOpen(h, playOnClassify(h));
    if (r.outcome != PlayOn::OpenOutcome::Accepted) return r;

    // DEFER THE OPEN PAST THIS DELIVERY. We are inside a QTcpSocket readyRead emission; opening media from
    // here runs the whole play path — nested loops, overlay teardown, widget deletion — underneath a frame
    // that is going to touch the socket again on the way out. That is the #28 / #211 crash family exactly.
    // A zero-timer hands it to the next event-loop turn, by which time the reply has been written and the
    // connection closed.
    QPointer<MainWindow> self(this);
    QTimer::singleShot(0, this, [self, h] { if (self) self->playOnPerformOpen(h); });
    return r;
}

void MainWindow::playOnPerformOpen(const PlayOn::Handoff& h)
{
    // Plant the resume position first, so the ordinary resume path seeks there as part of the normal open.
    // Handing the position to the open call instead would need a second seek after load, which is a visible
    // jump and a race against the first frames.
    if (h.positionSec > 1.0 && !h.ref.id.isEmpty())
        iniStore().setValue(ResumeStore::groupFor(h.ref.id) + QStringLiteral("/pos"), h.positionSec);

    const QString kind = recentKindFor(h.ref.type);
    notify(tr("Playing “%1” from another device…").arg(h.ref.title), 4000);

    if (h.ref.kind == QLatin1String("local"))
    {
        openRecent(h.ref.id, kind, h.ref.id, h.ref.title, QString());
        return;
    }
    // A catalogue / addon reference goes through #224's re-mint: the recipe resolves a link on THIS device,
    // with THIS device's credentials. Nothing about the source's link travels.
    RecentItem row;
    row.key           = h.ref.id;
    row.title         = h.ref.title;
    row.kind          = kind;
    row.sourceAddonId = h.ref.source;
    row.sourceItemId  = h.ref.id;
    row.sourceRoute   = h.ref.kind == QLatin1String("catalog") ? QStringLiteral("imdb")
                                                               : QStringLiteral("direct");
    row.sourceType    = h.ref.type.isEmpty() ? QStringLiteral("movie") : h.ref.type;
    remintAndOpen(row, h.ref.id);
}

bool MainWindow::playOnPairBegin()
{
    if (!playOnHost_) playOnHost_ = new PlayOnHost(this);
    const QString code = playOnHost_->beginPairing();
    if (code.isEmpty()) return false;
    // The code goes on THIS device's screen and nowhere else — that is the whole of what pairing proves.
    // A minute is long enough to walk across a room and short enough that a code does not sit on a TV.
    notify(tr("Pairing code: %1  —  enter it on the other device").arg(code), 60000);
    return true;
}

QString MainWindow::playOnPairRedeem(const QString& code)
{
    if (!playOnHost_) return QString();
    const QString token = playOnHost_->redeemPairing(code);
    // Never log either side of this. The RESULT is reported; the code and the token are not.
    notify(token.isEmpty() ? tr("A device tried to pair and got the code wrong.")
                           : tr("Paired with another device."), 5000);
    return token;
}

QSet<QString> MainWindow::playOnIssuedTokens() const
{
    return playOnHost_ ? playOnHost_->issuedTokens() : QSet<QString>();
}

// ---------------------------------------------------------------------------- the picker -----------------
QList<PlayOn::Target> MainWindow::playOnTargets() const
{
    QList<PlayOn::Target> cast;
    if (castMgr_)
        for (const CastDevice& d : castMgr_->devices())
        {
            PlayOn::Target t;
            t.kind  = d.type == CastDevice::Chromecast ? PlayOn::TargetKind::Chromecast
                                                       : PlayOn::TargetKind::Dlna;
            t.id    = d.id;
            t.name  = d.name;
            t.label = d.name;
            t.host  = d.host;
            t.port  = d.port;
            cast << t;
        }
    const QList<PlayOn::Peer> peers = castMgr_ ? castMgr_->peers() : QList<PlayOn::Peer>();
    const QSet<QString> paired = playOnHost_ ? playOnHost_->pairedPeerIds() : QSet<QString>();
    return PlayOn::mergeTargets(cast, peers, Settings::deviceId(), paired);
}

PlayOn::Peer MainWindow::playOnPeerById(const QString& instanceId) const
{
    if (castMgr_)
        for (const PlayOn::Peer& p : castMgr_->peers())
            if (p.id == instanceId) return p;
    return PlayOn::Peer();
}

// ---------------------------------------------------------------------------- hand-off -------------------
void MainWindow::playOnHandOffTo(const PlayOn::Peer& peer)
{
    if (!playOnHost_) playOnHost_ = new PlayOnHost(this);
    bool ok = false;
    const PlayOn::Handoff h = playOnCurrentHandoff(&ok);
    if (!ok)
    {
        notify(tr("There's nothing playing here that another device could pick up."), 5000);
        return;
    }

    const QString token = playOnHost_->tokenFor(peer.id);
    if (!token.isEmpty()) { playOnSendHandoff(peer, token, h); return; }

    // First hand-off to this target: pair. The code appears on the TARGET, which is what makes a pairing a
    // statement about being in the same house rather than about being on the same subnet.
    notify(tr("Asking %1 to show a pairing code…").arg(peer.name), 4000);
    QPointer<MainWindow> self(this);
    PlayOnClient* c = playOnClient();
    connect(c, &PlayOnClient::pairingOffered, this, [self, peer, h](const QString& id) {
        if (!self || id != peer.id) return;
        // DEFER PAST THIS EMISSION. The OSK below is a nested event loop and we are inside a QNetworkReply
        // finished() delivery; opening it here would run the loop under a frame that still owns the reply.
        QTimer::singleShot(0, self, [self, peer, h] {
            if (!self) return;
            const QString code = Osk::getText(tr("Enter the code shown on %1").arg(peer.name),
                                              QString(), QLineEdit::Normal, self);
            if (code.isNull()) return;                       // backed out; nothing is stored, nothing is sent
            self->playOnRedeemAndHandOff(peer, code, h);
        });
    }, Qt::SingleShotConnection);
    connect(c, &PlayOnClient::pairingFailed, this, [self](const QString&, const QString& msg) {
        if (self) self->notify(msg, 6000);
    }, Qt::SingleShotConnection);
    c->requestPairing(peer);
}

void MainWindow::playOnRedeemAndHandOff(const PlayOn::Peer& peer, const QString& code,
                                        const PlayOn::Handoff& h)
{
    QPointer<MainWindow> self(this);
    PlayOnClient* c = playOnClient();
    connect(c, &PlayOnClient::paired, this, [self, peer, h](const QString& id, const QString& token) {
        if (!self || id != peer.id) return;
        // The one place a token is written. It is not logged here or anywhere downstream.
        self->playOnHost_->storeToken(peer.id, token);
        self->notify(tr("Paired with %1.").arg(peer.name), 4000);
        self->playOnSendHandoff(peer, token, h);
    }, Qt::SingleShotConnection);
    connect(c, &PlayOnClient::pairingFailed, this, [self](const QString&, const QString& msg) {
        if (self) self->notify(msg, 6000);
    }, Qt::SingleShotConnection);
    c->redeemPairing(peer, code);
}

void MainWindow::playOnSendHandoff(const PlayOn::Peer& peer, const QString& token, const PlayOn::Handoff& h)
{
    QPointer<MainWindow> self(this);
    PlayOnClient* c = playOnClient();
    connect(c, &PlayOnClient::handedOff, this, [self, peer](const QString& id) {
        if (!self || id != peer.id) return;
        // The hand-off worked: give up the local decoder and become a remote. Stopping is what makes this a
        // TRANSFER rather than the same thing playing twice in two rooms.
        if (self->player_) self->player_->stop();
        self->playOnRemotePeer_ = peer;
        self->playOnRemoteActive_ = true;
        self->notify(tr("Playing on %1.").arg(peer.name), 4000);
        QTimer::singleShot(0, self, [self, peer] { if (self) self->playOnShowRemote(peer); });
    }, Qt::SingleShotConnection);
    connect(c, &PlayOnClient::handOffRefused, this, [self, peer](const QString& id, const QString& msg) {
        if (!self || id != peer.id) return;
        // A 401 means the token this device holds is no longer one the peer issued (it was re-installed, or
        // the pairing was revoked there). Drop it so the next attempt pairs again instead of failing forever.
        if (msg.contains(tr("needs pairing again"))) self->playOnHost_->forgetPeer(peer.id);
        self->notify(msg, 7000);
    }, Qt::SingleShotConnection);
    c->handOff(peer, token, h);
}

// ---------------------------------------------------------------------------- remote mode ----------------
// #76's phone-remote page, rendered natively with the nav kit so it works on both layouts and from a pad.
// The menu is re-shown after each transport press with a freshly polled state, which is what makes the
// position line live without a timer that keeps running after the user has walked away.
//
// LEAVING NEVER STOPS THE TARGET. Backing out of this menu drops the remote and nothing else; the only row
// that stops playback is the one that says so.
void MainWindow::playOnShowRemote(const PlayOn::Peer& peer)
{
    QPointer<MainWindow> self(this);
    PlayOnClient* c = playOnClient();
    connect(c, &PlayOnClient::stateArrived, this, [self, peer](const QString& id, const PlayOn::RemoteView& v) {
        if (!self || id != peer.id) return;
        QTimer::singleShot(0, self, [self, peer, v] { if (self) self->playOnRemoteMenu(peer, v); });
    }, Qt::SingleShotConnection);
    c->pollState(peer);
}

void MainWindow::playOnRemoteMenu(const PlayOn::Peer& peer, const PlayOn::RemoteView& v)
{
    if (!v.reachable)
    {
        playOnRemoteActive_ = false;
        notify(tr("%1 isn't answering any more.").arg(peer.name), 6000);
        return;
    }

    auto mmss = [](double s) {
        const int t = int(s < 0 ? 0 : s);
        return QStringLiteral("%1:%2").arg(t / 60).arg(t % 60, 2, 10, QLatin1Char('0'));
    };

    QStringList rows;
    QList<int> acts;                                   // parallel: which action each row performs
    rows << (v.hasMedia ? tr("▶ %1   %2 / %3").arg(v.title, mmss(v.positionSec), mmss(v.durationSec))
                        : tr("Nothing is playing on %1").arg(peer.name));
    acts << -1;                                        // the status line is not an action
    if (v.hasMedia)
    {
        rows << (v.playing ? tr("❚❚  Pause") : tr("▶  Play"));           acts << 0;
        rows << tr("◀◀  Back 10 seconds");                                acts << 1;
        rows << tr("▶▶  Forward 30 seconds");                             acts << 2;
        rows << tr("⏭  Next");                                            acts << 3;
        rows << tr("⏮  Previous");                                        acts << 4;
        if (v.volumeControllable)
        {
            rows << tr("🔉  Volume down  (%1)").arg(v.volume);            acts << 5;
            rows << tr("🔊  Volume up  (%1)").arg(v.volume);              acts << 6;
        }
        rows << tr("■  Stop on %1").arg(peer.name);                       acts << 7;
        rows << tr("⤓  Continue on this device");                         acts << 8;
    }
    rows << tr("Leave the remote  (keeps playing there)");                acts << 9;

    const int pick = NavMenu::pick(tr("Remote: %1").arg(peer.name), rows, this);
    if (pick < 0 || pick >= acts.size()) { playOnRemoteActive_ = false; return; }
    const int a = acts.at(pick);
    PlayOnClient* c = playOnClient();
    switch (a)
    {
        case -1: break;                                                    // the status row: just refresh
        case 0:  c->sendPlayerCommand(peer, PlayOn::playerCommandBody(QStringLiteral("playpause"))); break;
        case 1:  c->sendPlayerCommand(peer, PlayOn::seekCommandBody(v.positionSec - 10.0)); break;
        case 2:  c->sendPlayerCommand(peer, PlayOn::seekCommandBody(v.positionSec + 30.0)); break;
        case 3:  c->sendPlayerCommand(peer, PlayOn::playerCommandBody(QStringLiteral("next"))); break;
        case 4:  c->sendPlayerCommand(peer, PlayOn::playerCommandBody(QStringLiteral("prev"))); break;
        case 5:  c->sendPlayerCommand(peer, PlayOn::volumeCommandBody(v.volume - 5)); break;
        case 6:  c->sendPlayerCommand(peer, PlayOn::volumeCommandBody(v.volume + 5)); break;
        case 7:
            c->sendPlayerCommand(peer, PlayOn::playerCommandBody(QStringLiteral("stop")));
            playOnRemoteActive_ = false;
            return;
        case 8:
            playOnContinueHere(peer);
            return;
        case 9:
        default:
            playOnRemoteActive_ = false;                                   // the target keeps playing
            return;
    }
    // Re-poll and re-show, so the position line and the play/pause caption reflect the press just made.
    QPointer<MainWindow> self(this);
    QTimer::singleShot(250, this, [self, peer] { if (self) self->playOnShowRemote(peer); });
}

// ---------------------------------------------------------------------------- continue here --------------
void MainWindow::playOnContinueHere(const PlayOn::Peer& peer)
{
    QPointer<MainWindow> self(this);
    PlayOnClient* c = playOnClient();
    connect(c, &PlayOnClient::pullArrived, this, [self, peer](const QString& id, const PlayOn::Pull& p) {
        if (!self || id != peer.id) return;
        if (!p.valid) { self->notify(p.reason, 6000); return; }
        // The hand-off the peer would itself have sent — the same struct, the same decision, the opposite
        // direction. Classified against THIS device before anything opens, so a reference we cannot resolve
        // says so instead of failing halfway through a load.
        const PlayOn::Handoff h = PlayOn::handoffFromPull(p);
        const PlayOn::OpenResult r = PlayOn::decideOpen(h, self->playOnClassify(h));
        if (r.outcome != PlayOn::OpenOutcome::Accepted)
        {
            self->notify(PlayOn::describeRefusal(r, tr("this device")), 7000);
            return;
        }
        // Take over: stop it there first, so the two devices are never both playing the same thing.
        self->playOnClient()->sendPlayerCommand(peer, PlayOn::playerCommandBody(QStringLiteral("stop")));
        self->playOnRemoteActive_ = false;
        QTimer::singleShot(0, self, [self, h] { if (self) self->playOnPerformOpen(h); });
    }, Qt::SingleShotConnection);
    c->pullState(peer);
}

// ---------------------------------------------------------------------------- the menus ------------------
// The classic cast picker's #143 section. Called from showCastMenu so there is ONE output picker with three
// target kinds in it, which is the decision this issue turns on.
void MainWindow::playOnAddCastMenuRows(QMenu* menu)
{
    if (!menu || !castMgr_) return;
    const QList<PlayOn::Target> targets = playOnTargets();
    bool any = false;
    for (const PlayOn::Target& t : targets)
    {
        if (t.kind != PlayOn::TargetKind::EverythingBox) continue;
        any = true;
        const PlayOn::Peer peer = playOnPeerById(t.id.mid(3));   // strip the "eb:" namespace
        QAction* a = menu->addAction(QStringLiteral("📦  ") + t.label
                                     + (t.paired ? QString() : tr("  (pair)")));
        // Unlike a cast target, a hand-off does not need a castable URL: it sends a reference. It needs
        // something whose reference this device can name, which is a different and broader test.
        bool nameable = false;
        (void)playOnCurrentHandoff(&nameable);
        a->setEnabled(nameable);
        connect(a, &QAction::triggered, this, [this, peer] { playOnHandOffTo(peer); });
    }
    if (any) menu->addSeparator();
}

// The picker reachable when NOTHING is playing — the "Continue on this device" direction, and the settings
// row both layouts hang off. Nav kit, so it is identical on the themed and classic surfaces.
void MainWindow::showPlayOnMenu()
{
    if (castMgr_) castMgr_->startDiscovery();
    if (!playOnHost_) playOnHost_ = new PlayOnHost(this);

    const QList<PlayOn::Target> targets = playOnTargets();
    QList<PlayOn::Peer> peers;
    for (const PlayOn::Target& t : targets)
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

    bool nameable = false;
    (void)playOnCurrentHandoff(&nameable);

    QStringList rows;
    QList<QPair<int, int>> acts;      // (peer index, action: 0 = play there, 1 = continue here, 2 = remote)
    for (int i = 0; i < peers.size(); ++i)
    {
        const PlayOn::Peer& p = peers.at(i);
        if (nameable) { rows << tr("▶  Play on %1").arg(p.name);            acts << qMakePair(i, 0); }
        rows << tr("⤓  Continue on this device from %1").arg(p.name);       acts << qMakePair(i, 1);
        rows << tr("🎛  Remote for %1").arg(p.name);                        acts << qMakePair(i, 2);
    }
    const int pick = NavMenu::pick(tr("Play on device"), rows, this);
    if (pick < 0 || pick >= acts.size()) return;
    const PlayOn::Peer peer = peers.at(acts.at(pick).first);
    switch (acts.at(pick).second)
    {
        case 0: playOnHandOffTo(peer);    break;
        case 1: playOnContinueHere(peer); break;
        default: playOnShowRemote(peer);  break;
    }
}
