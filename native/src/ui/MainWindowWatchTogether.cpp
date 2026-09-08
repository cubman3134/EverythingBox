// "Watch together" (issue #86), the MainWindow half — a SEPARATE translation unit that defines MainWindow's
// #86 members, for the reason MainWindowPlayOn.cpp gives at length: MainWindow.cpp is the busiest merge
// surface in the repository and this feature reaches the rest of the class only through members that already
// existed, so it costs that file two settings rows instead of four hundred lines.
//
// THE SHAPE OF THE FEATURE, IN ONE PARAGRAPH. One person hosts a room (a five-character code, netplay's
// alphabet, netplay's relay) and the other joins with that code. The host's transport is authoritative: play,
// pause and seek go out as messages, and either side may ASK for a pause or a seek — the host decides. What
// the room shares is an IDENTITY: a catalogue id, an addon reference, a local path — PlayOn::ItemRef, #143's
// type, reused verbatim — and each participant resolves its own stream through its own addons and its own
// accounts, by way of #143's own playOnPerformOpen. Somebody who cannot resolve it says so and stays in the
// room; nobody is dropped and nobody is blocked. A guest keeps step with a periodic beacon, correcting small
// drift with mpv's pitch-corrected rate (+-3%, imperceptible) and hard-seeking only past five seconds.
//
// NO STREAM URL EVER LEAVES THIS MACHINE. Not on the wire, not in a log, not in a room record. That is not a
// convention: WatchTogether::encode scrubs every string it writes, and probe_watchtogether byte-scans a whole
// session transcript for a token to prove it.
//
// TWO THINGS THIS FILE INFERS RATHER THAN BEING TOLD, and why:
//   * A SEEK. mpv has no "the user seeked" signal, only a position stream. A position that jumps further than
//     a tick could have carried it IS a seek, and that is what the host broadcasts on.
//   * BUFFERING. There is no stall property exposed here either. A position that stops advancing while the
//     player is not paused is a stall, which is exactly what "buffering" means to the room, and it catches a
//     throttled source as well as an empty cache.
#include "MainWindow.h"

#include <QDateTime>
#include <QLineEdit>
#include <QPointer>
#include <QStackedWidget>
#include <QRandomGenerator>
#include <QTimer>

#include <cmath>

#include "../core/AppPaths.h"
#include "../core/RecentStore.h"
#include "../core/Settings.h"
#include "../media/WatchTogetherSession.h"
#include "../video/MpvWidget.h"
#include "Notifier.h"
#include "nav/NavOverlay.h"
#include "nav/Osk.h"

using namespace WatchTogether;

namespace
{
    // The room's LAN port. Fixed, like netplay's, so "join on the same network" needs one number and not two.
    constexpr quint16 kDirectPort = 55421;
    // How many 1 Hz ticks of a frozen position count as a stall. Two, because one tick can be lost to an
    // ordinary frame hitch and telling the room about that would flap the whole party's transport.
    constexpr int kStallTicks = 2;
    // How long after this machine moves its own player the player's reports about itself are echoes rather
    // than decisions. mpv's pause flag arrives on its own thread and lands a turn or three later, so a flag
    // cleared at the end of the applying function is already false by the time the echo comes back. MEASURED
    // LIVE: a guest whose freshly opened file reported "playing" a moment after the host paused turned that
    // into a request to resume, and the room started again by itself.
    constexpr qint64 kEchoWindowMs = 1500;

    // Split "host:port" the way the netplay menu does, defaulting to the relay's own port.
    bool splitRelay(const QString& setting, QString& host, quint16& port)
    {
        const QString v = setting.trimmed();
        if (v.isEmpty()) return false;
        const int colon = v.lastIndexOf(QLatin1Char(':'));
        host = colon > 0 ? v.left(colon) : v;
        const quint16 p = colon > 0 ? quint16(v.mid(colon + 1).toUInt()) : quint16(0);
        port = p ? p : quint16(55666);
        return !host.isEmpty();
    }
}

// Room news reaches whichever surface is in front. During a film the window notice is behind the video, so a
// guest going quiet, or somebody saying they could not get the film, would be reported to nobody -- which is
// precisely when it matters. The player's own notice channel is the same one the skip chip and the stream
// warnings use.
void MainWindow::watchTogetherNotice(const QString& text, int ms)
{
    if (notifier_ && stack_ && stack_->currentWidget() == playerPage_) notifier_->playerNotice(text, ms);
    else notify(text, ms);
}

// ---------------------------------------------------------------------------- wiring ---------------------
// Built once, on the first use of the feature. The session outlives any one room (leave() closes the room,
// not the object) so nothing below has to be re-wired when a second party starts.
void MainWindow::watchTogetherWire()
{
    if (watchSession_) return;
    watchSession_ = new WatchTogetherSession(this);

    connect(watchSession_, &WatchTogetherSession::status, this,
            [this](const QString& m) { watchTogetherNotice(m, 5000); });
    connect(watchSession_, &WatchTogetherSession::ended, this, [this](const QString& why) {
        watchTogetherNotice(why, 6000);
        wtNudging_ = false;
        if (player_) player_->setSpeed(1.0);
        if (wtTimer_) wtTimer_->stop();
    });
    connect(watchSession_, &WatchTogetherSession::joined, this, [this] {
        if (wtTimer_) wtTimer_->start();
        // The host puts what it is already playing into the room the moment someone arrives, so a join that
        // lands mid-film needs no second press.
        if (watchSession_->isHost()) watchTogetherShareCurrent();
    });
    connect(watchSession_, &WatchTogetherSession::participantsChanged, this, [this] {
        if (!watchSession_ || !watchSession_->active()) return;
        for (const Participant& p : watchSession_->participants())
        {
            if (p.id == Settings::deviceId()) continue;
            if (!p.resolved && !p.note.isEmpty())
                watchTogetherNotice(tr("%1 couldn't play this one (%2) — they're still in the room.").arg(p.name, p.note), 6000);
        }
    });
    connect(watchSession_, &WatchTogetherSession::itemProposed, this,
            [this](const PlayOn::ItemRef& ref, double pos, bool paused) {
                watchTogetherOnItem(ref, pos, paused);
            });
    connect(watchSession_, &WatchTogetherSession::transportChanged, this,
            [this](bool paused, double pos) { watchTogetherApplyTransport(paused, pos); });

    // The player. Both transport hooks are GUARDED by wtApplying_: applying the room's transport moves this
    // player, and without the guard that move would be reported back as a fresh local decision — a
    // request/answer loop.
    if (player_)
    {
        // A HOST THAT STARTS SOMETHING SHARES IT. Once you have deliberately opened a room, "the film I just
        // put on" is what the room is for; making the host press a second button for every item is a way to
        // sit watching different things and not know it. Nothing is broadcast when there is no room, and a
        // guest's own opens never broadcast at all. The manual row stays for the other order of events —
        // hosting a room around something that was ALREADY playing, which has no load event left to fire.
        connect(player_, &MpvWidget::fileLoaded, this, [this](bool, bool) {
            if (!watchSession_ || !watchSession_->active()) return;
            QPointer<MainWindow> self(this);
            if (!watchSession_->isHost())
            {
                // A GUEST THAT HAS JUST LOADED SOMETHING ADOPTS THE ROOM; it does not announce itself. This is
                // join-in-progress at the only moment it can be done — a fresh file starts at zero and playing
                // whatever the room is doing, and the seek/pause that fixes that has to wait for the file to
                // exist. It is also what stops the load's own "playing" report becoming a request to resume a
                // room the host has paused.
                wtQuietUntilMs_ = QDateTime::currentMSecsSinceEpoch() + kEchoWindowMs;
                QTimer::singleShot(0, this, [self] {
                    if (!self || !self->watchSession_ || !self->watchSession_->active()) return;
                    self->watchTogetherApplyTransport(self->watchSession_->hostPaused(),
                                                      self->watchSession_->hostPosition());
                });
                return;
            }
            // Deferred a turn: fileLoaded arrives while the open path is still unwinding, and
            // playOnCurrentHandoff reads syncKey_ / lastPos_, which that path is still setting.
            if (wtApplying_) return;
            QTimer::singleShot(0, this, [self] { if (self) self->watchTogetherShareCurrent(true); });
        });
        connect(player_, &MpvWidget::pausedChanged, this, [this](bool paused) {
            if (wtApplying_ || !watchSession_ || !watchSession_->active()) return;
            // THE ECHO GUARD, and the wtApplying_ flag above is NOT enough on its own. mpv reports its pause
            // flag ASYNCHRONOUSLY, so the change this machine made while applying the room's own transport
            // arrives a turn or two LATER, with the flag already cleared — and is then reported back as a
            // fresh local decision. Measured live: a paused room resumed itself seconds after the host paused
            // it. Comparing against what the room says we should be is the guard that cannot race, because a
            // press that agrees with the room needs no message and a press that disagrees is by definition
            // the local user's.
            if (paused == watchSession_->hostPaused()) return;
            // ...and the same guard as a WINDOW, because mpv's flag arrives late (see kEchoWindowMs). Without
            // it, a file this machine has just opened or just re-seated reports its own starting state after
            // the applying flag is long cleared, and that report is not a decision anybody made.
            if (QDateTime::currentMSecsSinceEpoch() < wtQuietUntilMs_) return;
            // ...and mpv unpauses at EOF. A guest whose file has ENDED must not ask the room to resume for
            // everyone still watching, so nothing is reported once there is no media here to report about.
            if (!player_->hasMedia()) return;
            // The host decides; a guest ASKS.
            if (watchSession_->isHost()) { watchSession_->setTransport(paused, lastPos_); return; }
            watchSession_->requestPause(paused);
            // SAY that it was a request. A guest pressing pause and seeing the film stop cannot tell whether
            // that was their press or the room's answer, and if the host refuses it starts again a second
            // later for no visible reason.
            watchTogetherNotice(paused ? tr("Asked the room to pause…") : tr("Asked the room to play…"), 2500);
            // The press takes effect here at once — a transport button that waits for a round trip feels
            // broken — and the host's answer confirms it a moment later. If the host REFUSES (an unresolved
            // participant does not get to steer the room), nothing would ever put this player back, so a
            // short timer re-asserts whatever the room says by then. When the answer was yes, that re-assert
            // is a no-op, because by then the room says what this press asked for.
            QPointer<MainWindow> self(this);
            QTimer::singleShot(kEchoWindowMs, this, [self] {
                if (!self || !self->watchSession_ || !self->watchSession_->active()) return;
                self->watchTogetherApplyTransport(self->watchSession_->hostPaused(),
                                                  self->watchSession_->hostPosition());
            });
        });
        connect(player_, &MpvWidget::positionChanged, this, [this](double pos) {
            if (!watchSession_ || !watchSession_->active()) return;
            if (watchSession_->isHost())
            {
                // A jump no tick could have produced is a seek, and a seek is the one position change the
                // room has to be told about immediately rather than at the next beacon.
                const bool jumped = wtSentPos_ >= 0.0 && std::abs(pos - wtSentPos_) > 2.0;
                watchSession_->notePosition(pos);
                if (jumped && !wtApplying_) watchSession_->setTransport(player_->isPaused(), pos);
                wtSentPos_ = pos;
            }
        });
    }

    if (!wtTimer_)
    {
        wtTimer_ = new QTimer(this);
        wtTimer_->setInterval(WatchTogetherSession::kBeaconMs);
        connect(wtTimer_, &QTimer::timeout, this, &MainWindow::watchTogetherTick);
    }
}

// ---------------------------------------------------------------------------- the menu -------------------
// Nav kit, so it is the same on the themed and the classic surface. Reachable from Settings on BOTH, which is
// #143's precedent and the only entry point that needs no theme to declare a new pill.
void MainWindow::showWatchTogetherMenu()
{
    watchTogetherWire();
    const bool inRoom = watchSession_ && watchSession_->active();

    QStringList rows;
    QList<int> acts;   // 0 host, 1 join, 2 who is watching, 3 share what is playing, 4 leave
    if (!inRoom)
    {
        rows << tr("Host a room");                                   acts << 0;
        rows << tr("Join with a code…");                             acts << 1;
    }
    else
    {
        rows << tr("Who's watching");                                acts << 2;
        if (watchSession_->isHost())
            { rows << tr("Play this for everyone"); acts << 3; }
        rows << tr("Leave the room (%1)").arg(watchSession_->code()); acts << 4;
    }
    const int pick = NavMenu::pick(tr("Watch together"), rows, this);
    if (pick < 0 || pick >= acts.size()) return;
    switch (acts.at(pick))
    {
        case 0:  watchTogetherHost(); break;
        case 1:  watchTogetherJoin(); break;
        case 2:  watchTogetherShowRoom(); break;
        case 3:  watchTogetherShareCurrent(); break;
        default: watchTogetherLeave(); break;
    }
}

void MainWindow::watchTogetherHost()
{
    watchTogetherWire();
    const QString code = makeRoomCode(QRandomGenerator::global()->generate());
    watchSession_->setBufferPolicy(policyFromId(Settings::watchTogetherPolicy()));

    QString relayHost; quint16 relayPort = 0;
    const bool haveRelay = splitRelay(Settings::netplayRelay(), relayHost, relayPort);

    QStringList rows;
    rows << tr("On this network (no relay needed)");
    if (haveRelay) rows << tr("Over the internet (via the relay)");
    else           rows << tr("Over the internet — set a relay first, in Settings ▸ General");
    const int how = NavMenu::pick(tr("Host a watch-together room"), rows, this);
    if (how < 0) return;
    if (how == 1 && !haveRelay)
    {
        notify(tr("Set a relay server first — Settings ▸ General ▸ the netplay relay. Watch together uses "
                  "the same one."), 8000);
        return;
    }
    if (how == 1) watchSession_->hostViaRelay(relayHost, relayPort, code, Settings::deviceId(), Settings::deviceName());
    else          watchSession_->hostDirect(kDirectPort, code, Settings::deviceId(), Settings::deviceName());
    notify(tr("Room %1 — give that code to the other person.").arg(code), 15000);
}

void MainWindow::watchTogetherJoin()
{
    watchTogetherWire();
    const QString typed = Osk::getText(tr("Room code from the host"), QString(), QLineEdit::Normal, this);
    if (typed.isNull()) return;
    const QString code = normalizeCode(typed);
    if (!codeValid(code))
    {
        notify(tr("That doesn't look like a room code — five letters and digits."), 5000);
        return;
    }
    QString relayHost; quint16 relayPort = 0;
    const bool haveRelay = splitRelay(Settings::netplayRelay(), relayHost, relayPort);

    QStringList rows;
    rows << tr("They're on this network");
    if (haveRelay) rows << tr("They're somewhere else (via the relay)");
    const int how = NavMenu::pick(tr("Where is the host?"), rows, this);
    if (how < 0) return;
    if (how == 1)
    {
        watchSession_->joinViaRelay(relayHost, relayPort, code, Settings::deviceId(), Settings::deviceName());
        return;
    }
    const QString addr = Osk::getText(tr("Host's address on this network"), QStringLiteral("192.168."),
                                      QLineEdit::Normal, this);
    if (addr.isNull() || addr.trimmed().isEmpty()) return;
    watchSession_->joinDirect(addr.trimmed(), kDirectPort, code, Settings::deviceId(), Settings::deviceName());
}

// Both settings builders call this after writing the key. A policy change has to reach a LIVE room, not just
// the next one: a host switching to "keep going" while the room is waiting for a stalled guest expects it to
// start moving, and the next stall report -- which is what would otherwise carry the change -- never comes if
// everyone has already recovered.
void MainWindow::applyWatchTogetherPolicy()
{
    if (!watchSession_ || !watchSession_->active()) return;
    watchSession_->setBufferPolicy(policyFromId(Settings::watchTogetherPolicy()));
}

void MainWindow::watchTogetherLeave()
{
    if (!watchSession_) return;
    watchSession_->leave();
    if (wtTimer_) wtTimer_->stop();
    wtNudging_ = false;
    wtBuffering_ = false;
    if (player_) player_->setSpeed(1.0);
    watchTogetherNotice(tr("Left the room."), 4000);
}

void MainWindow::watchTogetherShowRoom()
{
    if (!watchSession_ || !watchSession_->active()) return;
    QStringList rows;
    for (const Participant& p : watchSession_->participants())
    {
        const QString who = p.name.isEmpty() ? tr("Someone") : p.name;
        QString state = tr("watching");
        if (!p.resolved) state = p.note.isEmpty() ? tr("couldn't play this") : p.note;
        else if (p.buffering) state = tr("buffering…");
        rows << (p.host ? tr("★  %1 — %2 (host)").arg(who, state) : tr("•  %1 — %2").arg(who, state));
    }
    rows << tr("Room code: %1").arg(watchSession_->code());
    rows << (watchSession_->bufferPolicy() == BufferPolicy::WaitForEveryone
                 ? tr("When someone stalls: everyone waits")
                 : tr("When someone stalls: keep going"));
    NavMenu::pick(tr("Who's watching"), rows, this);
}

// ---------------------------------------------------------------------------- host: what to play ---------
// The host puts what it is ALREADY playing into the room, as a reference. #143 already had to work out how to
// name what is playing in a way another install can resolve — a Recents re-mint recipe — so this asks that
// question through the same member rather than inventing a second answer that could drift from it.
void MainWindow::watchTogetherShareCurrent(bool automatic)
{
    if (!watchSession_ || !watchSession_->active() || !watchSession_->isHost()) return;
    bool nameable = false;
    const PlayOn::Handoff h = playOnCurrentHandoff(&nameable);
    if (!nameable || !refShareable(h.ref))
    {
        // Silent when this was the AUTOMATIC share off a load: the room simply goes on showing what it had,
        // and telling the host off for opening a trailer would be noise. A press of the row says so, because
        // then somebody asked.
        if (!automatic)
            notify(tr("There's nothing here the other person could open on their own copy — a pasted link or a "
                      "live channel has no reference to share. Start something from your library or a "
                      "catalogue."),
                   8000);
        return;
    }
    watchSession_->shareItem(h.ref, h.positionSec, player_ ? player_->isPaused() : true);
    wtSentPos_ = h.positionSec;
    watchTogetherNotice(tr("Sharing “%1” with the room.").arg(h.ref.title), 5000);
}

// ---------------------------------------------------------------------------- guest: resolve it here -----
void MainWindow::watchTogetherOnItem(const PlayOn::ItemRef& ref, double positionSec, bool paused)
{
    Q_UNUSED(paused);
    if (!watchSession_) return;
    PlayOn::Handoff h;
    h.ref = ref;
    h.positionSec = positionSec;

    // The SAME classification #143 uses for an arriving hand-off, so "can this device play that reference"
    // has one answer in the app and not two.
    const PlayOn::OpenResult r = PlayOn::decideOpen(h, playOnClassify(h));
    if (r.outcome != PlayOn::OpenOutcome::Accepted)
    {
        // Not dropped, not blocking anyone: the room is told, and this screen says why.
        watchSession_->reportUnresolved(r.reason.isEmpty() ? tr("nothing here can open it") : r.reason);
        notify(tr("The room is watching “%1”, but this device can't open it (%2). You're still in the room.")
                   .arg(ref.title, r.reason),
               9000);
        return;
    }
    watchSession_->reportResolved();
    // DEFER PAST THIS DELIVERY. We are inside a QTcpSocket readyRead emission; opening media from here runs
    // the whole play path — nested loops, overlay teardown, widget deletion — underneath a frame that is going
    // to touch the socket again on the way out. That is the #28 / #211 crash family exactly.
    QPointer<MainWindow> self(this);
    QTimer::singleShot(0, this, [self, h] { if (self) self->playOnPerformOpen(h); });
}

// ---------------------------------------------------------------------------- the room's transport -------
// A guest with NOTHING LOADED adopts the room's item. Two ways to get here and both are ordinary: the file
// this machine was playing reached its end while the room went on (a host seeking backwards then finds a guest
// that cannot follow, because there is no player left to seek), and a rejoin that arrives between the room's
// Item and this machine opening anything. Throttled, so a burst of transports is one open and not twenty.
void MainWindow::watchTogetherOpenRoomItem()
{
    if (!watchSession_ || !watchSession_->active() || watchSession_->isHost()) return;
    const PlayOn::ItemRef ref = watchSession_->room().item();
    if (!refShareable(ref)) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now < wtReopenAtMs_) return;
    wtReopenAtMs_ = now + 5000;
    watchTogetherOnItem(ref, watchSession_->hostPosition(), watchSession_->hostPaused());
}

void MainWindow::watchTogetherApplyTransport(bool paused, double positionSec)
{
    if (!player_ || !watchSession_ || !watchSession_->active()) return;
    // Nothing is loaded here, so there is no transport to apply — only an item to go and get. Ahead of the
    // seek below because setPosition on an idle player is not an error, it is a silent no-op, and a guest
    // whose file had ended then sat at its last frame for the rest of the party.
    if (!watchSession_->isHost() && !player_->hasMedia()) { watchTogetherOpenRoomItem(); return; }
    // The host follows this too: an automatic buffering hold is a pause this machine did not ask for.
    wtApplying_ = true;
    wtQuietUntilMs_ = QDateTime::currentMSecsSinceEpoch() + kEchoWindowMs;
    if (!watchSession_->isHost() && std::abs(lastPos_ - positionSec) > 2.0)
        player_->setPosition(positionSec);
    if (player_->isPaused() != paused) player_->setPaused(paused);
    if (wtNudging_) { player_->setSpeed(1.0); wtNudging_ = false; }
    wtSentPos_ = positionSec;
    wtApplying_ = false;
}

// ---------------------------------------------------------------------------- the 1 Hz tick --------------
void MainWindow::watchTogetherTick()
{
    if (!watchSession_ || !watchSession_->active() || !player_) return;

    const double pos = lastPos_;
    const bool paused = player_->isPaused();

    // STALL DETECTION, both sides. A position that has stopped advancing while the player is not paused is a
    // stall — which is what buffering means to the room, and it catches a throttled source as well as an
    // empty cache. Reported on the EDGE only, so a long stall is one message and not one a second.
    const bool frozen = !paused && player_->hasMedia() && wtSeenPos_ >= 0.0 && std::abs(pos - wtSeenPos_) < 0.01;
    wtStalledTicks_ = frozen ? wtStalledTicks_ + 1 : 0;
    const bool stalled = wtStalledTicks_ >= kStallTicks;
    if (stalled != wtBuffering_)
    {
        wtBuffering_ = stalled;
        watchSession_->reportBuffering(stalled, pos);
    }
    wtSeenPos_ = pos;

    if (watchSession_->isHost()) { watchSession_->notePosition(pos); return; }

    // GUEST: keep step. Nothing to correct against a paused room, and nothing to correct while WE are the one
    // that stalled — the room's own policy decides what happens then, and nudging a player that is not
    // decoding would only mean a jolt when it resumes.
    if (paused || wtBuffering_ || !player_->hasMedia()) return;
    const DriftDecision d = watchSession_->evaluateDrift(pos, wtNudging_);
    switch (d.action)
    {
        case DriftAction::HardSeek:
            wtApplying_ = true;
            player_->setSpeed(1.0);
            player_->setPosition(d.seekTo);
            wtApplying_ = false;
            wtNudging_ = false;
            break;
        case DriftAction::Nudge:
            player_->setSpeed(d.speed);
            wtNudging_ = true;
            break;
        default:
            if (wtNudging_) { player_->setSpeed(1.0); wtNudging_ = false; }
            break;
    }
}
