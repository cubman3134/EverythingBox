#include "RemoteServer.h"

#include <QHostAddress>
#include <QNetworkInterface>
#include <QTcpServer>
#include <QTcpSocket>

RemoteServer::RemoteServer(QObject* parent) : QObject(parent) {}

RemoteServer::~RemoteServer() { stop(); }

bool RemoteServer::isListening() const { return server_ && server_->isListening(); }

bool RemoteServer::start(quint16 port)
{
    stop();  // idempotent: a re-start (e.g. a port change) rebinds cleanly
    server_ = new QTcpServer(this);
    // Bind to ALL interfaces so a device elsewhere on the LAN can reach it — but only ever from here, which is
    // only ever reached when Settings::remoteControlEnabled() is true. A disabled install never calls start().
    if (!server_->listen(QHostAddress::Any, port))
    {
        delete server_;
        server_ = nullptr;
        return false;
    }
    port_ = server_->serverPort();
    connect(server_, &QTcpServer::newConnection, this, [this] {
        while (QTcpSocket* sock = server_->nextPendingConnection())
        {
            connect(sock, &QTcpSocket::readyRead, this, [this, sock] { onReadyRead(sock); });
            connect(sock, &QTcpSocket::disconnected, this, [this, sock] {
                buffers_.remove(sock);
                sock->deleteLater();
            });
        }
    });
    return true;
}

void RemoteServer::stop()
{
    buffers_.clear();
    if (server_)
    {
        server_->close();
        server_->deleteLater();
        server_ = nullptr;
    }
    port_ = 0;
}

void RemoteServer::finish(QTcpSocket* sock, const QByteArray& responseBytes)
{
    if (!sock) return;
    sock->write(responseBytes);
    sock->flush();
    sock->disconnectFromHost();   // Connection: close — one request per connection
}

void RemoteServer::onReadyRead(QTcpSocket* sock)
{
    QByteArray& buf = buffers_[sock];
    buf += sock->readAll();

    // Cap the buffered request. A control API's requests are tiny; anything over the cap is broken or hostile.
    if (buf.size() > kMaxRequestBytes)
    {
        finish(sock, RemoteApi::httpResponse(413, "request too large", "text/plain"));
        return;
    }

    // Wait until the header block has fully arrived (blank line). Until then we cannot even read Content-Length.
    const bool haveHeaders = buf.contains("\r\n\r\n") || buf.contains("\n\n");
    if (!haveHeaders) return;

    RemoteApi::Request req = RemoteApi::parseRequest(buf);
    // Headers are in but the declared body has not all arrived yet: wait for the rest (still under the cap).
    if (req.valid && !req.bodyComplete) return;

    const RemoteApi::Command c = RemoteApi::route(req);
    QByteArray body;
    int status = 200;
    const char* contentType = "application/json";

    switch (c.kind)
    {
        case RemoteApi::CommandKind::State:
        {
            const RemoteApi::PlayerStateView view = hooks_.state ? hooks_.state() : RemoteApi::PlayerStateView{};
            body = RemoteApi::stateJson(view);
            status = 200;
            break;
        }
        case RemoteApi::CommandKind::Player:
        case RemoteApi::CommandKind::Input:
        {
            if (!hooks_.dispatch)
            {
                status = 503;
                body = "{\"ok\":false,\"error\":\"no dispatcher\"}";
                contentType = "application/json";
                break;
            }
            const bool ok = hooks_.dispatch(c);
            status = 200;
            body = ok ? "{\"ok\":true}" : "{\"ok\":false}";
            break;
        }
        case RemoteApi::CommandKind::NotFound:
            status = 404;
            body = "{\"ok\":false,\"error\":\"not found\"}";
            break;
        case RemoteApi::CommandKind::BadRequest:
            status = 400;
            body = QByteArray("{\"ok\":false,\"error\":\"") + c.error.toUtf8() + "\"}";
            break;
    }

    finish(sock, RemoteApi::httpResponse(status, body, contentType));
}

QString RemoteServer::lanUrl(quint16 port)
{
    // First non-loopback IPv4 address — the one a phone on the same network can reach.
    for (const QHostAddress& addr : QNetworkInterface::allAddresses())
    {
        if (addr.isLoopback()) continue;
        if (addr.protocol() != QAbstractSocket::IPv4Protocol) continue;
        return QStringLiteral("http://%1:%2").arg(addr.toString()).arg(port);
    }
    return QStringLiteral("http://127.0.0.1:%1").arg(port);  // no LAN address found: at least give a usable local one
}
