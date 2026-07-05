#include "NetplaySession.h"

#include <QTcpServer>
#include <QTcpSocket>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QPointer>
#include <QSharedPointer>

// Wire framing: [u32 length][u8 type][payload], length covers type+payload.
//   type 1 INPUT: [u32 frame][u16 buttons]
//   type 2 HELLO: JSON { gameId, core }
//   type 3 STATE: raw save-state bytes (host -> client, once)
namespace {
constexpr quint8 T_INPUT = 1, T_HELLO = 2, T_STATE = 3;
void putU32(QByteArray& b, quint32 v) { b.append(char(v >> 24)); b.append(char(v >> 16)); b.append(char(v >> 8)); b.append(char(v)); }
quint32 getU32(const char* p) { return (quint8(p[0]) << 24) | (quint8(p[1]) << 16) | (quint8(p[2]) << 8) | quint8(p[3]); }
}

NetplaySession::NetplaySession(QObject* parent) : QObject(parent) {}
NetplaySession::~NetplaySession() { stop(); }

void NetplaySession::host(quint16 port)
{
    stop();
    active_ = true; host_ = true; ready_ = false;
    server_ = new QTcpServer(this);
    connect(server_, &QTcpServer::newConnection, this, [this] {
        if (sock_) { server_->nextPendingConnection()->deleteLater(); return; } // one peer only
        attachSocket(server_->nextPendingConnection());
        server_->close();       // stop accepting further peers
        beginAsHost();
    });
    if (!server_->listen(QHostAddress::AnyIPv4, port))
    {
        emit ended(tr("Couldn't listen on port %1 (%2).").arg(port).arg(server_->errorString()));
        stop();
        return;
    }
    emit status(tr("Waiting for player 2 to join on port %1…").arg(port));
}

void NetplaySession::join(const QString& hostAddr, quint16 port)
{
    stop();
    active_ = true; host_ = false; ready_ = false;
    QTcpSocket* s = new QTcpSocket(this);
    attachSocket(s);
    connect(s, &QTcpSocket::connected, this, &NetplaySession::onConnected);
    emit status(tr("Connecting to %1…").arg(hostAddr));
    s->connectToHost(hostAddr, port);
}

void NetplaySession::wireSocketErrors(QTcpSocket* s)
{
    s->setSocketOption(QAbstractSocket::LowDelayOption, 1); // TCP_NODELAY: send input packets immediately
    connect(s, &QAbstractSocket::disconnected, this, [this] { if (active_) emit ended(tr("The other player disconnected.")); });
    connect(s, &QAbstractSocket::errorOccurred, this, [this] {
        if (active_ && !ready_) emit ended(tr("Couldn't connect (%1).").arg(sock_ ? sock_->errorString() : QString())); });
}

void NetplaySession::attachSocket(QTcpSocket* s)
{
    sock_ = s;
    wireSocketErrors(s);
    connect(sock_, &QTcpSocket::readyRead, this, &NetplaySession::onReadyRead);
}

// Connect to the relay and send our HOST/JOIN line; onRelayHandshake() takes over once the relay answers.
void NetplaySession::connectToRelay(const QString& relayHost, quint16 relayPort, const QByteArray& verbLine)
{
    stop();
    active_ = true; ready_ = false; awaitingPair_ = true;
    relayBuf_.clear();
    sock_ = new QTcpSocket(this);
    wireSocketErrors(sock_);
    connect(sock_, &QTcpSocket::connected, this, [this, verbLine] {
        emit status(tr("Reached the relay — waiting for the other player…"));
        sock_->write(verbLine);
    });
    connect(sock_, &QTcpSocket::readyRead, this, &NetplaySession::onRelayHandshake);
    emit status(tr("Connecting to the netplay relay…"));
    sock_->connectToHost(relayHost, relayPort);
}

void NetplaySession::hostViaRelay(const QString& relayHost, quint16 relayPort, const QString& code)
{
    host_ = true;
    connectToRelay(relayHost, relayPort, "HOST " + code.toUtf8() + "\n");
}

void NetplaySession::joinViaRelay(const QString& relayHost, quint16 relayPort, const QString& code)
{
    host_ = false;
    connectToRelay(relayHost, relayPort, "JOIN " + code.toUtf8() + "\n");
}

// "Both" host: listen for a DIRECT (UPnP-forwarded) connection on localPort AND register on the relay at once.
// Whichever peer arrives first wins; the loser path is torn down. If UPnP mapping succeeded the caller already
// forwarded localPort so a low-latency direct connection is possible; if not, the relay still pairs us. The relay
// runs on its OWN socket during the race so a stray direct attempt can't disturb the verified fallback.
void NetplaySession::hostOnline(quint16 localPort, const QString& relayHost, quint16 relayPort, const QString& code)
{
    stop();
    active_ = true; host_ = true; ready_ = false; awaitingPair_ = true;
    relayBuf_.clear();

    // Path A — direct server.
    server_ = new QTcpServer(this);
    connect(server_, &QTcpServer::newConnection, this, [this] {
        QTcpSocket* s = server_ ? server_->nextPendingConnection() : nullptr;
        if (!awaitingPair_ || !s) { if (s) s->deleteLater(); return; }  // relay already won
        awaitingPair_ = false;
        server_->close();
        if (relaySock_) { relaySock_->disconnect(this); relaySock_->abort(); relaySock_->deleteLater(); relaySock_ = nullptr; }
        attachSocket(s);
        emit status(tr("Player 2 connected directly."));
        beginAsHost();
    });
    server_->listen(QHostAddress::AnyIPv4, localPort);  // if this fails, the relay path still carries us

    // Path B — relay, on its own socket until it wins.
    relaySock_ = new QTcpSocket(this);
    relaySock_->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    connect(relaySock_, &QTcpSocket::connected, this, [this, code] {
        if (relaySock_) relaySock_->write("HOST " + code.toUtf8() + "\n"); });
    connect(relaySock_, &QTcpSocket::readyRead, this, [this] {
        if (!awaitingPair_ || !relaySock_) return;
        relayBuf_ += relaySock_->readAll();
        const int nl = relayBuf_.indexOf('\n');
        if (nl < 0) return;
        const QByteArray lineB = relayBuf_.left(nl).trimmed();
        const QByteArray leftover = relayBuf_.mid(nl + 1);
        relayBuf_.clear();
        if (lineB != "PAIRED") {
            emit ended(lineB == "BUSY" ? tr("That code is already in use — pick another.")
                                       : tr("The relay gave an unexpected response."));
            stop();
            return;
        }
        // Relay won: adopt it as the session socket, drop the direct server.
        awaitingPair_ = false;
        if (server_) { server_->close(); server_->deleteLater(); server_ = nullptr; }
        QTcpSocket* s = relaySock_; relaySock_ = nullptr;
        s->disconnect(this);          // clear the temp handshake handlers
        attachSocket(s);              // re-installs wireSocketErrors + onReadyRead
        rx_ = leftover;
        emit status(tr("Player 2 joined via the relay."));
        beginAsHost();
    });
    connect(relaySock_, &QAbstractSocket::errorOccurred, this, [this] {
        // Relay unreachable. Only fatal if the direct server also isn't listening — otherwise keep waiting on direct.
        if (awaitingPair_ && (!server_ || !server_->isListening())) {
            emit ended(tr("Couldn't reach the relay and no direct route is available."));
            stop();
        }
    });
    emit status(tr("Opening the room — waiting for player 2…"));
    relaySock_->connectToHost(relayHost, relayPort);
}

// "Both" join: if the host advertised a direct endpoint (UPnP worked), try it first for lowest latency; on ANY
// failure (refused, unreachable, or timeout) fall back to the verified relay path. No endpoint -> relay directly.
void NetplaySession::joinOnline(const QString& relayHost, quint16 relayPort, const QString& code,
                                const QString& directIp, quint16 directPort)
{
    if (directIp.isEmpty() || directPort == 0) { joinViaRelay(relayHost, relayPort, code); return; }

    stop();
    active_ = true; host_ = false; ready_ = false;
    QTcpSocket* d = new QTcpSocket(this);
    d->setSocketOption(QAbstractSocket::LowDelayOption, 1);

    auto resolved = QSharedPointer<bool>::create(false);   // committed to direct, or fell back — happens once
    QPointer<QTcpSocket> dp = d;
    auto toRelay = [this, relayHost, relayPort, code, resolved, dp]() {
        if (*resolved) return;
        *resolved = true;
        if (dp) { dp->disconnect(this); dp->abort(); dp->deleteLater(); }
        emit status(tr("Direct connection failed — using the relay…"));
        joinViaRelay(relayHost, relayPort, code);
    };
    connect(d, &QTcpSocket::connected, this, [this, resolved, dp] {
        if (*resolved || !dp) return;
        *resolved = true;
        attachSocket(dp);   // adopt the direct socket; wait for the host's HELLO + STATE
        emit status(tr("Connected directly — syncing game state…"));
    });
    connect(d, &QAbstractSocket::errorOccurred, this, [toRelay] { toRelay(); });
    emit status(tr("Trying a direct connection to the host…"));
    d->connectToHost(directIp, directPort);
    QTimer::singleShot(4000, this, [toRelay] { toRelay(); });   // direct didn't connect in time
}

// The relay speaks one line (PAIRED / NOHOST / BUSY), then pipes raw bytes. Consume just that line, hand any
// trailing bytes to the session's rx_, then run the normal handshake over the now-transparent pipe.
void NetplaySession::onRelayHandshake()
{
    if (!sock_) return;
    relayBuf_ += sock_->readAll();
    const int nl = relayBuf_.indexOf('\n');
    if (nl < 0) return; // wait for the full line
    const QByteArray line = relayBuf_.left(nl).trimmed();
    const QByteArray leftover = relayBuf_.mid(nl + 1); // session bytes that arrived alongside PAIRED
    relayBuf_.clear();
    disconnect(sock_, &QTcpSocket::readyRead, this, &NetplaySession::onRelayHandshake);

    if (line != "PAIRED")
    {
        emit ended(line == "NOHOST" ? tr("No one is hosting with that code.")
                 : line == "BUSY"   ? tr("That code is already in use — pick another.")
                                    : tr("The relay gave an unexpected response."));
        stop();
        return;
    }
    awaitingPair_ = false;
    connect(sock_, &QTcpSocket::readyRead, this, &NetplaySession::onReadyRead);
    rx_ = leftover;
    if (host_) beginAsHost();                 // host now sends HELLO + STATE
    else { emit status(tr("Connected — syncing game state…")); if (!rx_.isEmpty()) onReadyRead(); }
}

void NetplaySession::onConnected() // client
{
    emit status(tr("Connected — syncing game state…"));
}

void NetplaySession::beginAsHost()
{
    // Host: announce identity + hand the client our current state, then start the lockstep at frame 0.
    const QJsonObject hello{ { QStringLiteral("gameId"), gameId }, { QStringLiteral("core"), coreName } };
    sendFrame(T_HELLO, QJsonDocument(hello).toJson(QJsonDocument::Compact));
    sendFrame(T_STATE, serializeState ? serializeState() : QByteArray());
    ready_ = true;
    emit status(tr("Player 2 joined — starting."));
    emit started();
}

void NetplaySession::sendFrame(quint8 type, const QByteArray& payload)
{
    if (!sock_) return;
    QByteArray frame;
    putU32(frame, quint32(1 + payload.size()));
    frame.append(char(type));
    frame += payload;
    sock_->write(frame);
}

void NetplaySession::onReadyRead()
{
    rx_ += sock_->readAll();
    while (rx_.size() >= 4)
    {
        const quint32 len = getU32(rx_.constData());
        if (quint32(rx_.size()) < 4 + len) break;
        const quint8 type = quint8(rx_[4]);
        const QByteArray payload = rx_.mid(5, int(len) - 1);
        rx_.remove(0, int(4 + len));

        if (type == T_INPUT && payload.size() >= 6)
        {
            const quint32 frame = getU32(payload.constData());
            const quint16 buttons = (quint8(payload[4]) << 8) | quint8(payload[5]);
            remoteInputs_.insert(frame, buttons);
        }
        else if (type == T_HELLO) // client validates the host's game matches
        {
            const QJsonObject o = QJsonDocument::fromJson(payload).object();
            if (o.value(QStringLiteral("gameId")).toString() != gameId
                || o.value(QStringLiteral("core")).toString() != coreName)
            {
                emit ended(tr("The other player is running a different game or core."));
                stop();
                return;
            }
        }
        else if (type == T_STATE) // client adopts the host's state, then both start at frame 0
        {
            if (applyState) applyState(payload);
            ready_ = true;
            emit status(tr("In sync — starting."));
            emit started();
        }
    }
}

void NetplaySession::sendLocalInput(quint32 frame, quint16 buttons)
{
    QByteArray p;
    putU32(p, frame);
    p.append(char(buttons >> 8)); p.append(char(buttons & 0xFF));
    sendFrame(T_INPUT, p);
}

bool NetplaySession::remoteInput(quint32 frame, quint16& out) const
{
    auto it = remoteInputs_.constFind(frame);
    if (it == remoteInputs_.constEnd()) return false;
    out = it.value();
    return true;
}

void NetplaySession::pruneBefore(quint32 frame)
{
    for (auto it = remoteInputs_.begin(); it != remoteInputs_.end();)
    {
        if (it.key() < frame) it = remoteInputs_.erase(it);
        else ++it;
    }
}

void NetplaySession::stop()
{
    active_ = false; ready_ = false; awaitingPair_ = false;
    remoteInputs_.clear();
    rx_.clear(); relayBuf_.clear();
    if (sock_) { sock_->disconnect(this); sock_->abort(); sock_->deleteLater(); sock_ = nullptr; }
    if (relaySock_) { relaySock_->disconnect(this); relaySock_->abort(); relaySock_->deleteLater(); relaySock_ = nullptr; }
    if (server_) { server_->deleteLater(); server_ = nullptr; }
}
