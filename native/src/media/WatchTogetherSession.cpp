#include "WatchTogetherSession.h"

#include <QDateTime>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

using namespace WatchTogether;

namespace
{
    // A peer arriving on the DIRECT port announces itself with this line first. The direct port may be
    // reachable from outside the house, so "someone connected" is not evidence that it is the person you gave
    // the code to; the code is. Netplay's rule, and its spelling, one protocol letter apart.
    QByteArray directGreeting(const QString& code) { return "EBWT1 " + code.toUtf8() + "\n"; }
    qint64 nowMs() { return QDateTime::currentMSecsSinceEpoch(); }
}

WatchTogetherSession::WatchTogetherSession(QObject* parent) : QObject(parent) {}
WatchTogetherSession::~WatchTogetherSession() { stopSocket(); }

void WatchTogetherSession::stopSocket()
{
    if (beacon_) { beacon_->stop(); beacon_->deleteLater(); beacon_ = nullptr; }
    if (sock_) { sock_->disconnect(this); sock_->abort(); sock_->deleteLater(); sock_ = nullptr; }
    if (server_) { server_->close(); server_->deleteLater(); server_ = nullptr; }
    rx_.clear();
    relayBuf_.clear();
    paired_ = false;
    awaitingPair_ = false;
}

void WatchTogetherSession::leave()
{
    hostMode_ = HostMode::None;   // a deliberate exit is not a re-arm
    if (room_.active() && paired_ && sock_) { send(room_.byeMessage()); sock_->flush(); }
    room_.close();
    stopSocket();
    emit participantsChanged();
}

void WatchTogetherSession::attach(QTcpSocket* s)
{
    sock_ = s;
    s->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    connect(s, &QAbstractSocket::disconnected, this, [this] {
        if (!room_.active()) return;
        // A HOST WHOSE GUEST LEFT STILL HAS A ROOM. Re-open it on the same code rather than reporting the
        // party over: the commonest thing a guest does after leaving is come back, and until this the host's
        // listener had been closed for good the moment the first guest proved itself.
        if (room_.isHost() && hostMode_ != HostMode::None) { rearmHost(); return; }
        emit ended(tr("The other person left."));
    });
    connect(s, &QAbstractSocket::errorOccurred, this, [this] {
        if (room_.active() && !paired_)
            emit ended(tr("Couldn't connect (%1).").arg(sock_ ? sock_->errorString() : QString()));
    });
    connect(s, &QTcpSocket::readyRead, this, &WatchTogetherSession::onReadyRead);
}

// ---- direct LAN ------------------------------------------------------------------------------------------

// Put the room back on the air after the guest went away. Everything the room KNOWS (the item, the position,
// the policy) survives — only the connection is rebuilt — so a guest that comes back is handed the film at the
// point the host has reached, which is the same join-in-progress path a first join takes.
void WatchTogetherSession::rearmHost()
{
    const HostMode mode = hostMode_;
    // Drop everyone but ourselves; the roster is rebuilt by whoever arrives next.
    for (const Participant& p : room_.participants())
        if (p.id != room_.selfId()) room_.apply([&] { Message m; m.type = MsgType::Bye; m.participantId = p.id; return m; }());
    if (beacon_) beacon_->stop();
    if (sock_) { sock_->disconnect(this); sock_->abort(); sock_->deleteLater(); sock_ = nullptr; }
    if (server_) { server_->close(); server_->deleteLater(); server_ = nullptr; }
    rx_.clear(); relayBuf_.clear(); paired_ = false; awaitingPair_ = false;
    emit participantsChanged();
    emit status(tr("They left — the room is still open on %1.").arg(room_.code()));
    if (mode == HostMode::Direct) listenDirect();
    else                          connectToRelay(relayHost_, relayPort_, "HOST " + room_.code().toUtf8() + "\n");
}

// The listening half of hostDirect, on its own so rearmHost can run it again without re-opening the room.
void WatchTogetherSession::listenDirect()
{
    server_ = new QTcpServer(this);
    connect(server_, &QTcpServer::newConnection, this, &WatchTogetherSession::onDirectPeer);
    if (!server_->listen(QHostAddress::AnyIPv4, hostPort_))
    {
        const QString why = server_->errorString();
        const quint16 port = hostPort_;
        room_.close();
        stopSocket();
        emit ended(tr("Couldn't listen on port %1 (%2).").arg(port).arg(why));
        return;
    }
    emit status(tr("Waiting for someone to join with the code %1…").arg(room_.code()));
    emit roomOpen();
}

void WatchTogetherSession::hostDirect(quint16 port, const QString& code, const QString& selfId,
                                      const QString& selfName)
{
    stopSocket();
    room_.open(code, selfId, selfName, true);
    pendingCode_ = room_.code();
    hostMode_ = HostMode::Direct;
    hostPort_ = port;
    selfName_ = selfName;
    listenDirect();
}

void WatchTogetherSession::onDirectPeer()
{
    if (!server_) return;
    QTcpSocket* s = server_->nextPendingConnection();
    if (!s) return;
    if (sock_) { s->deleteLater(); return; }        // one guest; the room is small on purpose
    attach(s);
    // The server stays LISTENING until the greeting proves the peer. Closing it here is how a port scanner
    // gets to lock the room out: it takes the one slot, says nothing, and the person with the code can never
    // connect. onReadyRead does the vetting while paired_ is still false.
}

void WatchTogetherSession::joinDirect(const QString& hostAddr, quint16 port, const QString& code,
                                      const QString& selfId, const QString& selfName)
{
    stopSocket();
    room_.open(code, selfId, selfName, false);
    pendingCode_ = room_.code();
    QTcpSocket* s = new QTcpSocket(this);
    attach(s);
    connect(s, &QTcpSocket::connected, this, [this] {
        sock_->write(directGreeting(pendingCode_));
        beginSession();
    });
    emit status(tr("Connecting to %1…").arg(hostAddr));
    s->connectToHost(hostAddr, port);
}

// ---- the relay -------------------------------------------------------------------------------------------

void WatchTogetherSession::connectToRelay(const QString& relayHost, quint16 relayPort,
                                          const QByteArray& verbLine)
{
    sock_ = new QTcpSocket(this);
    attach(sock_);
    awaitingPair_ = true;
    // The relay handshake is line-based and comes BEFORE the byte pipe, so it gets its own reader.
    disconnect(sock_, &QTcpSocket::readyRead, this, &WatchTogetherSession::onReadyRead);
    connect(sock_, &QTcpSocket::readyRead, this, &WatchTogetherSession::onRelayHandshake);
    connect(sock_, &QTcpSocket::connected, this, [this, verbLine] {
        emit status(tr("Reached the relay — waiting for the other person…"));
        sock_->write(verbLine);
    });
    emit status(tr("Connecting to the relay…"));
    sock_->connectToHost(relayHost, relayPort);
}

void WatchTogetherSession::hostViaRelay(const QString& relayHost, quint16 relayPort, const QString& code,
                                        const QString& selfId, const QString& selfName)
{
    stopSocket();
    room_.open(code, selfId, selfName, true);
    pendingCode_ = room_.code();
    hostMode_ = HostMode::Relay;
    relayHost_ = relayHost;
    relayPort_ = relayPort;
    selfName_ = selfName;
    connectToRelay(relayHost, relayPort, "HOST " + room_.code().toUtf8() + "\n");
}

void WatchTogetherSession::joinViaRelay(const QString& relayHost, quint16 relayPort, const QString& code,
                                        const QString& selfId, const QString& selfName)
{
    stopSocket();
    room_.open(code, selfId, selfName, false);
    pendingCode_ = room_.code();
    connectToRelay(relayHost, relayPort, "JOIN " + room_.code().toUtf8() + "\n");
}

void WatchTogetherSession::onRelayHandshake()
{
    if (!sock_) return;
    relayBuf_ += sock_->readAll();
    for (;;)
    {
        const int nl = relayBuf_.indexOf('\n');
        if (nl < 0) return;                               // partial line: wait
        const QByteArray line = relayBuf_.left(nl).trimmed();
        const QByteArray rest = relayBuf_.mid(nl + 1);
        if (line == "PAIRED")
        {
            relayBuf_.clear();
            rx_ = rest;                                   // session bytes that arrived alongside PAIRED
            awaitingPair_ = false;
            disconnect(sock_, &QTcpSocket::readyRead, this, &WatchTogetherSession::onRelayHandshake);
            connect(sock_, &QTcpSocket::readyRead, this, &WatchTogetherSession::onReadyRead);
            beginSession();
            if (!rx_.isEmpty()) onReadyRead();
            return;
        }
        relayBuf_ = rest;
        if (line == "HOSTED")
        {
            // The room EXISTS now. Until this, a join with the same code races the registration and is told
            // NOHOST, so the code must not be handed out before it.
            emit status(tr("Room %1 is open — give that code to the other person.").arg(room_.code()));
            emit roomOpen();
            continue;
        }
        const QString err = line == "NOHOST" ? tr("No one is hosting with that code.")
                          : line == "BUSY"   ? tr("That code is already in use — pick another.")
                                             : tr("The relay gave an unexpected response.");
        room_.close();
        stopSocket();
        emit ended(err);
        return;
    }
}

// ---- the session -----------------------------------------------------------------------------------------

void WatchTogetherSession::beginSession()
{
    paired_ = true;
    send(room_.helloMessage());
    if (room_.isHost())
    {
        send(room_.rosterMessage());
        if (!beacon_)
        {
            beacon_ = new QTimer(this);
            beacon_->setInterval(kBeaconMs);
            connect(beacon_, &QTimer::timeout, this, [this] {
                if (paired_ && room_.isHost()) send(room_.beaconMessage(nowMs()));
            });
        }
        beacon_->start();
    }
    emit status(tr("Connected."));
    emit joined();
}

void WatchTogetherSession::send(const Message& m)
{
    if (!sock_ || !paired_) return;
    const QByteArray line = encode(m);
    if (!line.isEmpty()) sock_->write(line);
}

void WatchTogetherSession::send(const QList<Message>& ms)
{
    for (const Message& m : ms) send(m);
}

void WatchTogetherSession::onReadyRead()
{
    if (!sock_) return;
    rx_ += sock_->readAll();
    if (!paired_ && room_.isHost())
    {
        // A direct peer proves it holds the code before we commit to it. Anything else is hung up on: the
        // port may be reachable from outside, so a connection is not evidence of an invitation.
        const int nl = rx_.indexOf('\n');
        if (nl < 0) return;
        const QByteArray greeting = rx_.left(nl).trimmed();
        rx_.remove(0, nl + 1);
        if (greeting != directGreeting(pendingCode_).trimmed())
        {
            // Hang up on them and go on waiting. The server was never closed, so the person holding the code
            // can still arrive.
            sock_->disconnect(this);
            sock_->abort();
            sock_->deleteLater();
            sock_ = nullptr;
            rx_.clear();
            emit status(tr("Someone connected with the wrong code — still waiting for %1.").arg(room_.code()));
            return;
        }
        if (server_) server_->close();      // now, and only now, the room is taken
        beginSession();
    }
    for (const Message& m : decodeStream(rx_)) deliver(m);
}

void WatchTogetherSession::deliver(const Message& m)
{
    const bool wasPaused = room_.hostPaused();
    const double wasPos = room_.hostPosition();
    const int wasCount = room_.participants().size();

    if (m.type == MsgType::Beacon) beaconSeenMs_ = nowMs();

    const QList<Message> answers = room_.apply(m);
    send(answers);

    if (m.type == MsgType::Item)
        emit itemProposed(room_.item(), room_.hostPosition(), room_.hostPaused());
    // The transport moved — either because the host said so (guest side) or because the buffering policy
    // held/released the room (host side, a pause this machine did not ask for).
    //
    // A BEACON IS NOT A TRANSPORT CHANGE, and conflating the two cost a whole live session. A beacon carries
    // the host's position once a second, so "the position differs from last time" is true on EVERY beacon of
    // a playing room — and a listener that re-applies the room's transport on each one re-asserts the host's
    // pause flag a second at a time. MEASURED: a guest's own play press was undone within 200 ms, for ever,
    // and looked exactly like a button that did nothing. A beacon feeds evaluateDrift, which is the gentle
    // path; only a DECISION (an item, or a transport message) moves a player directly.
    const bool decision = (m.type == MsgType::Item || m.type == MsgType::Transport);
    if (room_.hostPaused() != wasPaused
        || (decision && !qFuzzyCompare(room_.hostPosition() + 1.0, wasPos + 1.0)))
        emit transportChanged(room_.hostPaused(), room_.hostPosition());
    if (room_.participants().size() != wasCount || m.type == MsgType::Roster
        || m.type == MsgType::Buffering || m.type == MsgType::Unresolved)
        emit participantsChanged();
    if (m.type == MsgType::Bye && !room_.isHost() && m.host)
        emit ended(m.reason.isEmpty() ? tr("The host left.") : m.reason);
}

// ---- what the app tells us -------------------------------------------------------------------------------

void WatchTogetherSession::setBufferPolicy(BufferPolicy p)
{
    room_.setBufferPolicy(p);
    if (!room_.isHost()) return;
    // Switching to "keep going" WHILE the room is waiting has to release it here and now. Waiting for the next
    // stall report would never work: if everyone has recovered, that report never comes and the room sits
    // paused for a stall the user has just said they do not care about.
    if (decideBuffering(p, room_.anyoneBuffering(), !room_.hostPaused(), room_.heldForBuffering())
        == BufferAction::ResumeAfterBuffering)
    {
        room_.setHostTransport(false, room_.hostPosition());
        send(room_.transportMessage());
        emit transportChanged(false, room_.hostPosition());
    }
}

void WatchTogetherSession::shareItem(const PlayOn::ItemRef& ref, double positionSec, bool paused)
{
    if (!room_.isHost()) return;
    room_.setItem(ref, positionSec, paused);
    send(room_.itemMessage());
}

void WatchTogetherSession::setTransport(bool paused, double positionSec)
{
    if (!room_.isHost()) return;
    room_.setHostTransport(paused, positionSec);
    send(room_.transportMessage());
}

void WatchTogetherSession::notePosition(double positionSec)
{
    if (room_.isHost()) room_.noteHostPosition(positionSec);
}

void WatchTogetherSession::requestPause(bool paused)
{
    if (room_.isHost()) { setTransport(paused, room_.hostPosition()); return; }
    send(room_.requestPauseMessage(paused));
}

void WatchTogetherSession::requestSeek(double positionSec)
{
    if (room_.isHost()) { setTransport(room_.hostPaused(), positionSec); return; }
    send(room_.requestSeekMessage(positionSec));
}

void WatchTogetherSession::reportBuffering(bool buffering, double positionSec)
{
    send(room_.bufferingMessage(buffering, positionSec));
}

void WatchTogetherSession::reportUnresolved(const QString& why)
{
    if (!room_.setSelfUnresolved(why)) return;   // nothing changed: saying it again is chatter
    send(room_.unresolvedMessage(why));
    emit participantsChanged();
}

void WatchTogetherSession::reportResolved()
{
    // ONLY ON CHANGE. A resolved report is a hello, the host answers a hello with the room's item, and the
    // item is what made the caller report resolution — so an unconditional send is an infinite loop that
    // re-opens the film every few seconds. It ran live before this line existed.
    if (!room_.setSelfResolved()) return;
    send(room_.helloMessage());
    emit participantsChanged();
}

DriftDecision WatchTogetherSession::evaluateDrift(double localPositionSec, bool nudging) const
{
    DriftDecision none;
    if (!room_.active() || room_.isHost() || beaconSeenMs_ <= 0) return none;
    // The age is measured on THIS machine's clock, from when the beacon landed. The host's own clock reading
    // rides along in the message and is deliberately never differenced against ours: two machines' clocks are
    // not comparable, and pretending they are is how a controller ends up chasing a skew instead of a drift.
    const double ageSec = double(nowMs() - beaconSeenMs_) / 1000.0;
    return decideDrift(room_.hostPosition(), room_.hostPaused(), localPositionSec, ageSec, nudging);
}
