#include "RemoteServer.h"

#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
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

    // #143: the credential check, and it happens BEFORE the switch on purpose. /open is the one route that
    // starts playback on this screen, and a caller that has not paired must be turned away without this
    // process looking anything up on its behalf. The token itself is compared and dropped -- it is not
    // logged here, not echoed into the body, and not carried into any hook.
    if (PlayOn::routeNeedsToken(req.path))
    {
        const QSet<QString> issued = hooks_.tokens ? hooks_.tokens() : QSet<QString>();
        if (!PlayOn::authorized(req.token, issued))
        {
            finish(sock, RemoteApi::httpResponse(401, PlayOn::unauthorizedJson(), "application/json"));
            return;
        }
    }

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
        case RemoteApi::CommandKind::Open:
        {
            if (!hooks_.open)
            {
                status = 503;
                body = "{\"ok\":false,\"error\":\"no dispatcher\"}";
                break;
            }
            PlayOn::Handoff h;
            QString err;
            if (!PlayOn::parseHandoff(req.body, h, err))
            {
                status = 400;
                body = QByteArray("{\"ok\":false,\"error\":\"") + err.toUtf8() + "\"}";
                break;
            }
            const PlayOn::OpenResult r = hooks_.open(h);
            status = r.httpStatus;
            body = PlayOn::openResultJson(r);
            break;
        }
        case RemoteApi::CommandKind::PairBegin:
        {
            if (!hooks_.pairBegin)
            {
                status = 503;
                body = "{\"ok\":false,\"error\":\"no dispatcher\"}";
                break;
            }
            const bool showing = hooks_.pairBegin();
            status = showing ? 200 : 503;
            // The CODE IS NOT IN THIS RESPONSE, and that is the whole point of the #127 pattern: it is shown
            // on THIS device's screen, so pairing needs someone who can see it. A code in the reply would
            // pair anything that can reach the port.
            body = showing ? "{\"ok\":true}"
                           : "{\"ok\":false,\"reason\":\"that device could not show a code\"}";
            break;
        }
        case RemoteApi::CommandKind::PairRedeem:
        {
            if (!hooks_.pairRedeem)
            {
                status = 503;
                body = "{\"ok\":false,\"error\":\"no dispatcher\"}";
                break;
            }
            const QString token = hooks_.pairRedeem(c.pairCode);
            if (token.isEmpty())
            {
                status = 403;
                body = "{\"ok\":false,\"reason\":\"that code was not accepted\"}";
                break;
            }
            // The one response in this file that carries a credential. It goes to the caller that just proved
            // it can see this device's screen, and it is not logged on the way out.
            QJsonObject o;
            o.insert(QStringLiteral("ok"), true);
            o.insert(QStringLiteral("token"), token);
            status = 200;
            body = QJsonDocument(o).toJson(QJsonDocument::Compact);
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
