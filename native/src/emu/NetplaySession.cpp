#include "NetplaySession.h"

#include <QTcpServer>
#include <QTcpSocket>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>

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

void NetplaySession::attachSocket(QTcpSocket* s)
{
    sock_ = s;
    sock_->setSocketOption(QAbstractSocket::LowDelayOption, 1); // TCP_NODELAY: send input packets immediately
    connect(sock_, &QTcpSocket::readyRead, this, &NetplaySession::onReadyRead);
    connect(sock_, &QAbstractSocket::disconnected, this, [this] { if (active_) emit ended(tr("The other player disconnected.")); });
    connect(sock_, &QAbstractSocket::errorOccurred, this, [this] {
        if (active_ && !ready_) emit ended(tr("Couldn't connect (%1).").arg(sock_ ? sock_->errorString() : QString())); });
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
    active_ = false; ready_ = false;
    remoteInputs_.clear();
    rx_.clear();
    if (sock_) { sock_->disconnect(this); sock_->abort(); sock_->deleteLater(); sock_ = nullptr; }
    if (server_) { server_->deleteLater(); server_ = nullptr; }
}
