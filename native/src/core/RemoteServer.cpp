#include "RemoteServer.h"

#include <QFile>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkInterface>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

// #291: the stream cap is a wire figure, and it has to admit the largest body LibraryBundle will accept.
static_assert(RemoteApi::kBundleStreamCap >= LibraryBundle::kMaxV2BodyBytes,
              "RemoteApi::kBundleStreamCap must cover LibraryBundle::kMaxV2BodyBytes");

namespace
{
    // The most one read takes off a socket while a body streams to disk, and the most Qt may hold for us
    // between reads -- which is what bounds this process's memory for a 64 MiB body.
    constexpr qint64 kStreamChunk = 256 * 1024;

    // Where the header block ends (just past the blank line), or -1 while it has not all arrived. The wire is
    // CRLFCRLF; a lax client's LFLF is accepted too, exactly as RemoteApi::parseRequest accepts it.
    int headerEnd(const QByteArray& b)
    {
        const int crlf = b.indexOf("\r\n\r\n");
        const int lf   = b.indexOf("\n\n");
        if (crlf < 0 && lf < 0) return -1;
        if (crlf < 0) return lf + 2;
        if (lf < 0)   return crlf + 4;
        return qMin(crlf + 4, lf + 2);
    }

    QByteArray receiptResponse(const LibraryBundle::Receipt& r)
    {
        return RemoteApi::httpResponse(r.httpStatus, LibraryBundle::receiptJson(r), "application/json");
    }
}

// One v2 body on its way to disk.
struct RemoteServer::Spool
{
    QFile   file;
    qint64  remaining = 0;     // body bytes still to come
    QTimer* idle = nullptr;    // owned by the socket; restarted on every chunk
};

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
                // #291: a client that goes away mid-body takes its spool with it -- nothing half-sent stays
                // on disk.
                dropStream(sock);
                buffers_.remove(sock);
                answered_.remove(sock);
                sock->deleteLater();
            });
        }
    });
    return true;
}

void RemoteServer::stop()
{
    const QList<QTcpSocket*> streaming = spools_.keys();
    for (QTcpSocket* s : streaming)
    {
        dropStream(s);
        answered_.insert(s);
        s->abort();
    }
    buffers_.clear();
    answered_.clear();
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
    // One request per connection: once it is answered, anything else the client sends is drained, never
    // parsed and routed a second time.
    answered_.insert(sock);
    sock->write(responseBytes);
    sock->flush();
    sock->disconnectFromHost();   // Connection: close — one request per connection
}

void RemoteServer::onReadyRead(QTcpSocket* sock)
{
    if (answered_.contains(sock)) { sock->readAll(); return; }
    if (spools_.contains(sock))   { pumpStream(sock); return; }

    QByteArray& buf = buffers_[sock];

    // #291 -- THE HEADER PHASE, read without taking a body byte. Until the blank line is in, bytes are PEEKED
    // and only the header block is consumed, so that when the headers turn out to announce a streamed bundle
    // not one byte of its body is sitting in this buffer. Every other request then continues below exactly as
    // before.
    if (headerEnd(buf) < 0)
    {
        while (sock->bytesAvailable() > 0)
        {
            const QByteArray peeked = sock->peek(RemoteApi::kDefaultRequestCap);
            if (peeked.isEmpty()) break;
            const int end = headerEnd(buf + peeked);
            if (end >= 0)
            {
                buf += sock->read(end - buf.size());
                break;
            }
            buf += sock->read(peeked.size());
            noteBuffered(buf.size());
            if (buf.size() > RemoteApi::requestCapBytes(buf))
            {
                finish(sock, RemoteApi::httpResponse(413, "request too large", "text/plain"));
                return;
            }
        }
        noteBuffered(buf.size());
        if (headerEnd(buf) < 0) return;

        const RemoteApi::Request head = RemoteApi::parseRequest(buf);
        const RemoteApi::BodyPlan plan = RemoteApi::bodyPlanFor(head);
        if (plan != RemoteApi::BodyPlan::Buffer)
        {
            beginStream(sock, head, plan);
            return;
        }
    }

    buf += sock->readAll();
    noteBuffered(buf.size());

    // Cap the buffered request. #76's routes are tiny, so anything over that cap is broken or hostile; #127's
    // POST /bundle is the one route that legitimately carries a payload and is capped at a bundle's size.
    // The decision is RemoteApi's, read off the request line, so the exception is one testable function.
    if (buf.size() > RemoteApi::requestCapBytes(buf))
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
        case RemoteApi::CommandKind::Inventory:
        {
            if (!hooks_.inventory)
            {
                status = 503;
                body = "{\"ok\":false,\"error\":\"no dispatcher\"}";
                break;
            }
            body = hooks_.inventory();
            status = 200;
            break;
        }
        case RemoteApi::CommandKind::Bundle:
        {
            if (!hooks_.bundle)
            {
                status = 503;
                body = "{\"ok\":false,\"error\":\"no dispatcher\"}";
                break;
            }
            // The body is handed over whole. Decoding it -- and refusing an unsafe id, an unsafe file name or
            // a future format -- is LibraryBundle's job, on the far side of this hook, so the socket code
            // never learns the bundle vocabulary and cannot get the safety rules subtly different.
            const LibraryBundle::Receipt r = hooks_.bundle(req.body);
            status = r.httpStatus;
            body = LibraryBundle::receiptJson(r);
            break;
        }
        case RemoteApi::CommandKind::Gamelists:
        {
            // #292. What this device's ROM folders hold, for the source's gamelist diff. Token-checked above.
            if (!hooks_.gamelists)
            {
                status = 503;
                body = "{\"ok\":false,\"error\":\"no dispatcher\"}";
                break;
            }
            body = hooks_.gamelists();
            status = 200;
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

void RemoteServer::beginStream(QTcpSocket* sock, const RemoteApi::Request& head, RemoteApi::BodyPlan plan)
{
    buffers_.remove(sock);   // the header block is spent; the body never enters a buffer

    // THE TOKEN FIRST. Before the length is even considered, before a spool exists, before a body byte is
    // accepted: an unpaired caller learns nothing about the ceiling and costs this device nothing on disk.
    {
        const QSet<QString> issued = hooks_.tokens ? hooks_.tokens() : QSet<QString>();
        if (!PlayOn::authorized(head.token, issued))
        {
            finish(sock, RemoteApi::httpResponse(401, PlayOn::unauthorizedJson(), "application/json"));
            return;
        }
    }
    // THEN THE DECLARED LENGTH, still with no body read: a 413 up front, not after buffering.
    if (plan == RemoteApi::BodyPlan::TooLarge)
    {
        LibraryBundle::Receipt r;
        r.httpStatus = 413;
        r.result = QStringLiteral("refused");
        r.reason = QStringLiteral("that bundle was too large for an art transfer");
        finish(sock, receiptResponse(r));
        return;
    }
    if (plan == RemoteApi::BodyPlan::LengthRequired)
    {
        finish(sock, RemoteApi::httpResponse(411, "{\"ok\":false,\"error\":\"length required\"}", "application/json"));
        return;
    }
    if (!hooks_.bundleRoot || !hooks_.bundleStream)
    {
        finish(sock, RemoteApi::httpResponse(503, "{\"ok\":false,\"error\":\"no dispatcher\"}", "application/json"));
        return;
    }

    auto spool = std::make_shared<Spool>();
    if (LibraryBundle::openSpool(hooks_.bundleRoot(), spool->file).isEmpty())
    {
        finish(sock, receiptResponse(LibraryBundle::receiptFor(
                         LibraryBundle::LandResult::WriteFailed,
                         QStringLiteral("this device could not open its cache for writing"))));
        return;
    }
    spool->remaining = head.declaredLength;
    // Bound what Qt holds for us between reads, so a fast sender waits on TCP rather than on this process's
    // memory.
    sock->setReadBufferSize(kStreamChunk);
    spool->idle = new QTimer(sock);
    spool->idle->setSingleShot(true);
    spool->idle->setInterval(bodyIdleTimeoutMs_);
    connect(spool->idle, &QTimer::timeout, this, [this, sock] {
        // A body that stopped arriving. Its spool goes now, not when the peer eventually hangs up.
        dropStream(sock);
        answered_.insert(sock);
        sock->abort();
    });
    spools_.insert(sock, spool);
    pumpStream(sock);
}

void RemoteServer::pumpStream(QTcpSocket* sock)
{
    const std::shared_ptr<Spool> spool = spools_.value(sock);
    if (!spool) return;

    while (spool->remaining > 0 && sock->bytesAvailable() > 0)
    {
        const QByteArray chunk = sock->read(qMin(spool->remaining, kStreamChunk));
        if (chunk.isEmpty()) break;
        if (spool->file.write(chunk) != qint64(chunk.size()))
        {
            dropStream(sock);
            finish(sock, receiptResponse(LibraryBundle::receiptFor(LibraryBundle::LandResult::WriteFailed,
                                                                   QStringLiteral("the cache ran out of room"))));
            return;
        }
        spool->remaining -= chunk.size();
    }
    if (spool->remaining > 0)
    {
        spool->idle->start();
        return;
    }

    // The whole declared body is on disk. Decode and land it from the FILE -- LibraryBundle judges the header
    // before it reads a file byte -- then remove the spool whatever the answer was.
    spool->idle->stop();
    spool->file.close();
    LibraryBundle::Receipt receipt;
    {
        QFile body(spool->file.fileName());
        if (body.open(QIODevice::ReadOnly))
        {
            // #292: the header says which kind of body this is, and each kind has its own landing -- the art
            // cache's, or the ROM folder's. A device without the gamelist hook refuses that kind in words.
            if (LibraryBundle::bodyKindV2(body) == LibraryBundle::BodyKind::Gamelist)
                receipt = hooks_.sidecarStream
                              ? hooks_.sidecarStream(body)
                              : LibraryBundle::receiptFor(LibraryBundle::LandResult::Refused,
                                                          QStringLiteral("this device does not take gamelist entries"));
            else
                receipt = hooks_.bundleStream(body);
        }
        else
            receipt = LibraryBundle::receiptFor(LibraryBundle::LandResult::WriteFailed,
                                                QStringLiteral("this device could not read what it received"));
    }
    dropStream(sock);
    finish(sock, receiptResponse(receipt));
}

void RemoteServer::dropStream(QTcpSocket* sock)
{
    const std::shared_ptr<Spool> spool = spools_.take(sock);
    if (!spool) return;
    if (spool->idle)
    {
        spool->idle->stop();
        spool->idle->deleteLater();
        spool->idle = nullptr;
    }
    LibraryBundle::discardSpool(spool->file);
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
