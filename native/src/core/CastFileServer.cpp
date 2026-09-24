// Casting a local file (issue #72). The rules are in CastFileServer.h; this file is their implementation.
#include "CastFileServer.h"

#include <QAbstractSocket>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QNetworkInterface>
#include <QPointer>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUdpSocket>

#include <limits>

namespace CastServe
{
QByteArray contentTypeFor(const QString& fileName)
{
    const QString ext = QFileInfo(fileName).suffix().toLower();
    struct Row { const char* ext; const char* type; };
    static const Row table[] = {
        { "mp4", "video/mp4" },        { "m4v", "video/mp4" },         { "mkv", "video/x-matroska" },
        { "webm", "video/webm" },      { "mov", "video/quicktime" },   { "avi", "video/x-msvideo" },
        { "ts", "video/mp2t" },        { "m2ts", "video/mp2t" },
        { "mp3", "audio/mpeg" },       { "flac", "audio/flac" },       { "m4a", "audio/mp4" },
        { "aac", "audio/aac" },        { "ogg", "audio/ogg" },         { "opus", "audio/ogg" },
        { "wav", "audio/wav" },
        { "jpg", "image/jpeg" },       { "jpeg", "image/jpeg" },       { "png", "image/png" },
        { "gif", "image/gif" },        { "webp", "image/webp" },
    };
    if (!ext.isEmpty())
        for (const Row& r : table)
            if (ext == QLatin1String(r.ext)) return QByteArray(r.type);
    return QByteArrayLiteral("application/octet-stream");
}

namespace
{
// Parse a run of ASCII digits. An empty run is not a number; a run too long for qint64 saturates to max, which is
// "past the end of any file" for a first-byte-pos and "the end of the file" for a last-byte-pos.
bool parseDigits(const QByteArray& s, qint64& out)
{
    if (s.isEmpty()) return false;
    qint64 v = 0;
    for (char c : s)
    {
        if (c < '0' || c > '9') return false;
        const int d = c - '0';
        if (v > (std::numeric_limits<qint64>::max() - d) / 10) v = std::numeric_limits<qint64>::max();
        else v = v * 10 + d;
    }
    out = v;
    return true;
}
} // namespace

ByteRange parseRange(bool present, const QByteArray& value, qint64 size)
{
    ByteRange whole;   // Whole: the default answer, and the answer to anything this server declines to honour
    if (!present) return whole;
    const QByteArray v = value.trimmed();
    const int eq = v.indexOf('=');
    if (eq < 0 || v.left(eq).trimmed().toLower() != "bytes") return whole;   // another unit: ignored
    const QByteArray spec = v.mid(eq + 1).trimmed();
    if (spec.contains(',')) return whole;         // multi-range: the documented simplification, 200 + whole file
    const int dash = spec.indexOf('-');
    if (dash < 0) return whole;
    const QByteArray a = spec.left(dash).trimmed(), b = spec.mid(dash + 1).trimmed();

    ByteRange unsat;
    unsat.kind = ByteRange::Unsatisfiable;
    ByteRange r;
    r.kind = ByteRange::Partial;
    if (a.isEmpty())
    {
        // Suffix: the last n bytes.
        qint64 n = 0;
        if (!parseDigits(b, n)) return whole;
        if (n == 0 || size <= 0) return unsat;
        r.first = n >= size ? 0 : size - n;
        r.last = size - 1;
        return r;
    }
    qint64 first = 0;
    if (!parseDigits(a, first)) return whole;
    qint64 last = std::numeric_limits<qint64>::max();
    if (!b.isEmpty() && !parseDigits(b, last)) return whole;
    if (last < first) return whole;               // an invalid spec: ignored, as RFC 9110 14.2 says
    if (first >= size) return unsat;
    r.first = first;
    r.last = last >= size ? size - 1 : last;
    return r;
}

QByteArray newToken()
{
    quint32 words[4];
    QRandomGenerator::system()->fillRange(words, 4);
    return QByteArray(reinterpret_cast<const char*>(words), int(sizeof(words))).toHex();
}

bool tokenEquals(const QByteArray& a, const QByteArray& b)
{
    if (a.isEmpty() || b.isEmpty() || a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (int i = 0; i < a.size(); ++i) diff |= static_cast<unsigned char>(a.at(i) ^ b.at(i));
    return diff == 0;
}

QByteArray castPath(const QByteArray& token, const QString& fileName)
{
    return QByteArrayLiteral("/cast/") + token + '/' + QUrl::toPercentEncoding(fileName);
}

bool targetMatches(const QByteArray& requestTarget, const QByteArray& token, const QString& fileName)
{
    if (token.isEmpty() || fileName.isEmpty()) return false;   // revoked, or nothing registered
    QByteArray path = requestTarget;
    const int q = path.indexOf('?');
    if (q >= 0) path.truncate(q);
    if (!path.startsWith('/')) return false;                   // origin-form only; no absolute-form, no '*'
    const QList<QByteArray> seg = path.split('/');
    if (seg.size() != 4 || !seg.at(0).isEmpty() || seg.at(1) != "cast") return false;
    if (!tokenEquals(seg.at(2), token)) return false;
    const QByteArray name = QByteArray::fromPercentEncoding(seg.at(3));
    // Belt and braces: the name is never used to find anything, but a name that decodes to a separator or a dot
    // segment is refused outright rather than compared.
    if (name.contains('/') || name.contains('\\') || name == "." || name == "..") return false;
    return name == fileName.toUtf8();
}

namespace
{
bool inNet(quint32 ip, quint32 net, int prefix)
{
    const quint32 mask = prefix <= 0 ? 0u : (prefix >= 32 ? 0xFFFFFFFFu : ~((1u << (32 - prefix)) - 1u));
    return (ip & mask) == (net & mask);
}
bool v4(const QHostAddress& a, quint32& out)
{
    bool ok = false;
    const quint32 v = a.toIPv4Address(&ok);
    if (!ok || a.protocol() != QAbstractSocket::IPv4Protocol) return false;
    out = v;
    return true;
}
} // namespace

bool isLanIpv4(const QHostAddress& a)
{
    quint32 ip = 0;
    if (!v4(a, ip)) return false;
    return inNet(ip, 0x0A000000u, 8)      // 10.0.0.0/8
        || inNet(ip, 0xAC100000u, 12)     // 172.16.0.0/12
        || inNet(ip, 0xC0A80000u, 16)     // 192.168.0.0/16
        || inNet(ip, 0xA9FE0000u, 16);    // 169.254.0.0/16, link-local
}

bool isBindable(const QHostAddress& a)
{
    quint32 ip = 0;
    if (!v4(a, ip)) return false;         // null, Any (dual-stack), every IPv6 address
    if (inNet(ip, 0x7F000000u, 8)) return true;   // loopback
    return isLanIpv4(a);                  // excludes 0.0.0.0, 255.255.255.255, and every public address
}

QHostAddress chooseBindAddress(const QList<IfaceAddr>& addrs, const QHostAddress& target, const QHostAddress& routeHint)
{
    quint32 t = 0;
    if (!v4(target, t)) return QHostAddress();
    if (inNet(t, 0x7F000000u, 8)) return QHostAddress(QHostAddress::LocalHost);   // loopback, for loopback only
    if (!isLanIpv4(target)) return QHostAddress();                                 // never serve off the LAN

    auto usable = [](const IfaceAddr& a) { return a.up && a.running && !a.loopback && isLanIpv4(a.ip); };
    const IfaceAddr* best = nullptr;
    for (const IfaceAddr& a : addrs)
    {
        quint32 ip = 0;
        if (!usable(a) || !v4(a.ip, ip) || a.prefixLength < 0 || a.prefixLength > 32) continue;
        if (!inNet(t, ip, a.prefixLength)) continue;
        if (!routeHint.isNull() && a.ip == routeHint) return a.ip;    // the OS routes through this one
        if (!best || a.prefixLength > best->prefixLength) best = &a;
    }
    if (best) return best->ip;
    // A LAN target outside every subnet here (a routed LAN): only the address the OS itself would send from,
    // and only if it is one of our own up LAN addresses.
    if (!routeHint.isNull())
        for (const IfaceAddr& a : addrs)
            if (usable(a) && a.ip == routeHint) return a.ip;
    return QHostAddress();
}

QList<IfaceAddr> systemInterfaces()
{
    QList<IfaceAddr> out;
    for (const QNetworkInterface& iface : QNetworkInterface::allInterfaces())
    {
        const auto f = iface.flags();
        for (const QNetworkAddressEntry& e : iface.addressEntries())
        {
            IfaceAddr a;
            a.iface = iface.name();
            a.up = f.testFlag(QNetworkInterface::IsUp);
            a.running = f.testFlag(QNetworkInterface::IsRunning);
            a.loopback = f.testFlag(QNetworkInterface::IsLoopBack);
            a.ip = e.ip();
            a.prefixLength = e.prefixLength();
            out << a;
        }
    }
    return out;
}

QHostAddress routeHintFor(const QHostAddress& target)
{
    // A connected UDP socket asks the OS which source address it would use toward the target. connect() on UDP
    // sends nothing; it only fixes the route.
    if (target.protocol() != QAbstractSocket::IPv4Protocol || target.isLoopback()) return QHostAddress();
    QUdpSocket s;
    s.connectToHost(target, 9);
    if (s.state() != QAbstractSocket::ConnectedState && !s.waitForConnected(200)) return QHostAddress();
    const QHostAddress local = s.localAddress();
    s.abort();
    return local;
}

QString localFileFor(const QString& urlOrPath)
{
    if (urlOrPath.isEmpty()) return QString();
    QString path;
    if (urlOrPath.startsWith(QLatin1String("file:"), Qt::CaseInsensitive))
    {
        const QUrl u(urlOrPath);
        if (!u.isLocalFile()) return QString();
        path = u.toLocalFile();
    }
    else
    {
        // A url with any other scheme is not a file. A Windows drive letter ("C:/…") is one character before the
        // colon, which no url scheme is.
        const int colon = urlOrPath.indexOf(QLatin1Char(':'));
        if (colon > 1 && QUrl(urlOrPath).scheme().size() > 1) return QString();
        path = urlOrPath;
    }
    const QFileInfo fi(path);
    if (!fi.isAbsolute() || !fi.isFile()) return QString();
    return fi.absoluteFilePath();
}

Offer offerFor(const QString& castUrl, bool headerGated, const QString& localPath)
{
    if (!localPath.isEmpty()) return Offer::LocalFile;
    if (castUrl.isEmpty() || !castUrl.startsWith(QLatin1String("http"))) return Offer::NothingCastable;
    return headerGated ? Offer::HeaderGated : Offer::RemoteUrl;
}
} // namespace CastServe

// ----------------------------------------------------------------------------------------------- the server --

struct CastFileServer::Conn
{
    QByteArray head;           // the request head as it arrives
    QTimer* headTimer = nullptr;
    QFile* file = nullptr;     // non-null while a body is streaming
    qint64 remaining = 0;
    bool answered = false;     // a response has been written; nothing more is read
};

CastFileServer::CastFileServer(QObject* parent) : QObject(parent)
{
    // #442: nothing this object owns may outlive the quit. stop() is synchronous and waits on nothing.
    if (QCoreApplication::instance())
        connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, &CastFileServer::stop);
}

CastFileServer::~CastFileServer() { stop(); }

bool CastFileServer::isListening() const { return server_ && server_->isListening(); }
QHostAddress CastFileServer::address() const { return isListening() ? server_->serverAddress() : QHostAddress(); }
quint16 CastFileServer::port() const { return isListening() ? server_->serverPort() : quint16(0); }

QUrl CastFileServer::url() const
{
    if (!isServing()) return QUrl();
    QUrl u;
    u.setScheme(QStringLiteral("http"));
    u.setHost(address().toString());
    u.setPort(port());
    u.setPath(QString::fromLatin1(CastServe::castPath(token_, fileName_)), QUrl::StrictMode);
    return u;
}

bool CastFileServer::serve(const QString& filePath, const QHostAddress& bindAddress, QString* error)
{
    auto fail = [&](const QString& why) { stop(); if (error) *error = why; return false; };
    if (!CastServe::isBindable(bindAddress)) return fail(QStringLiteral("not a LAN address"));
    const QFileInfo fi(filePath);
    if (!fi.isFile() || !fi.isReadable()) return fail(QStringLiteral("file not readable"));

    revoke();   // the previous token is dead before the new one exists
    if (isListening() && server_->serverAddress() != bindAddress)
    {
        stop();
    }
    if (!isListening())
    {
        if (!server_)
        {
            server_ = new QTcpServer(this);
            connect(server_, &QTcpServer::newConnection, this, &CastFileServer::onNewConnection);
        }
        if (!server_->listen(bindAddress, 0)) return fail(server_->errorString());
    }
    filePath_ = fi.absoluteFilePath();
    fileName_ = fi.fileName();
    token_ = CastServe::newToken();
    return true;
}

void CastFileServer::revoke()
{
    token_.clear();
    // A body streaming under the old token is cut now; a connection still sending its head will be answered 404.
    const QList<QTcpSocket*> socks = conns_.keys();
    for (QTcpSocket* s : socks)
    {
        Conn* c = conns_.value(s);
        if (c && c->file) drop(s);
    }
}

void CastFileServer::stop()
{
    token_.clear();
    if (server_) server_->close();
    const QList<QTcpSocket*> socks = conns_.keys();
    for (QTcpSocket* s : socks) drop(s);
}

void CastFileServer::drop(QTcpSocket* s)
{
    Conn* c = conns_.take(s);
    if (!c) return;
    s->disconnect(this);        // no more deliveries to us from this socket, abort() included
    if (c->headTimer) { c->headTimer->stop(); c->headTimer->deleteLater(); }
    delete c->file;
    delete c;
    s->abort();
    s->deleteLater();           // never delete a socket that may be inside one of its own emissions
}

void CastFileServer::onNewConnection()
{
    while (QTcpSocket* s = server_->nextPendingConnection())
    {
        if (conns_.size() >= kMaxConnections)
        {
            s->write("HTTP/1.1 503 Service Unavailable\r\nContent-Type: text/plain\r\nContent-Length: 5\r\n"
                     "Connection: close\r\n\r\nbusy\n");
            connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
            s->disconnectFromHost();
            continue;
        }
        Conn* c = new Conn;
        conns_.insert(s, c);
        c->headTimer = new QTimer(this);
        c->headTimer->setSingleShot(true);
        connect(c->headTimer, &QTimer::timeout, this, [this, s] { drop(s); });
        c->headTimer->start(kHeaderTimeoutMs);
        connect(s, &QTcpSocket::readyRead, this, [this, s] { onReadyRead(s); });
        connect(s, &QTcpSocket::bytesWritten, this, [this, s] { pump(s); });
        connect(s, &QTcpSocket::disconnected, this, [this, s] {
            // The peer left (a renderer seeking closes its old connection). Deferred: this is the socket's own
            // emission, and drop() aborts and releases it.
            const QPointer<QTcpSocket> p(s);
            QTimer::singleShot(0, this, [this, p] { if (p && conns_.contains(p.data())) drop(p.data()); });
        });
        if (s->bytesAvailable() > 0) onReadyRead(s);
    }
}

namespace
{
QByteArray statusLine(int code)
{
    switch (code)
    {
    case 200: return "HTTP/1.1 200 OK\r\n";
    case 206: return "HTTP/1.1 206 Partial Content\r\n";
    case 400: return "HTTP/1.1 400 Bad Request\r\n";
    case 404: return "HTTP/1.1 404 Not Found\r\n";
    case 405: return "HTTP/1.1 405 Method Not Allowed\r\n";
    case 416: return "HTTP/1.1 416 Range Not Satisfiable\r\n";
    case 431: return "HTTP/1.1 431 Request Header Fields Too Large\r\n";
    default:  return "HTTP/1.1 500 Internal Server Error\r\n";
    }
}
// A short plain-text refusal. Names nothing: not the file, not the token, not the app.
QByteArray refusal(int code, const QByteArray& extraHeaders = QByteArray())
{
    const QByteArray body = QByteArray::number(code) + '\n';
    return statusLine(code) + "Content-Type: text/plain\r\nContent-Length: " + QByteArray::number(body.size())
         + "\r\n" + extraHeaders + "Connection: close\r\n\r\n" + body;
}
} // namespace

void CastFileServer::finish(QTcpSocket* s, const QByteArray& response)
{
    Conn* c = conns_.value(s);
    if (c) { c->answered = true; if (c->headTimer) c->headTimer->stop(); }
    s->readAll();              // unread request bytes at close would turn the close into a reset that eats the answer
    s->write(response);
    s->disconnectFromHost();   // flushes, then closes; `disconnected` then releases the connection
}

void CastFileServer::onReadyRead(QTcpSocket* s)
{
    Conn* c = conns_.value(s);
    if (!c) return;
    if (c->answered) { s->readAll(); return; }   // a body is going out; anything more the client sends is ignored
    c->head += s->read(kMaxHeaderBytes + 1 - c->head.size());
    int end = c->head.indexOf("\r\n\r\n");
    if (end < 0) end = c->head.indexOf("\n\n");
    if (end < 0)
    {
        if (c->head.size() > kMaxHeaderBytes) finish(s, refusal(431));
        return;
    }
    c->headTimer->stop();
    c->answered = true;
    const QByteArray head = c->head.left(end);
    c->head.clear();

    QList<QByteArray> lines = head.split('\n');
    for (QByteArray& l : lines) if (l.endsWith('\r')) l.chop(1);
    const QList<QByteArray> rl = lines.isEmpty() ? QList<QByteArray>() : lines.first().split(' ');
    if (rl.size() != 3 || !rl.at(2).startsWith("HTTP/1.")) { finish(s, refusal(400)); return; }
    const QByteArray method = rl.at(0), target = rl.at(1);
    const bool isHead = method == "HEAD";
    if (method != "GET" && !isHead) { finish(s, refusal(405, "Allow: GET, HEAD\r\n")); return; }
    if (!CastServe::targetMatches(target, token_, fileName_)) { finish(s, refusal(404)); return; }

    bool rangePresent = false;
    int rangeCount = 0;
    QByteArray rangeValue;
    for (int i = 1; i < lines.size(); ++i)
    {
        const QByteArray& l = lines.at(i);
        const int colon = l.indexOf(':');
        if (colon <= 0) continue;
        if (l.left(colon).trimmed().toLower() == "range")
        {
            rangePresent = true;
            ++rangeCount;
            rangeValue = l.mid(colon + 1).trimmed();
        }
    }
    if (rangeCount > 1) rangeValue = "bytes=0-0,0-0";   // two Range headers is a multi-range: the whole file

    auto* f = new QFile(filePath_);
    if (!f->open(QIODevice::ReadOnly)) { delete f; finish(s, refusal(404)); return; }
    const qint64 size = f->size();
    const CastServe::ByteRange r = CastServe::parseRange(rangePresent, rangeValue, size);

    QByteArray h;
    qint64 first = 0, len = size;
    if (r.kind == CastServe::ByteRange::Unsatisfiable)
    {
        delete f;
        finish(s, statusLine(416) + "Content-Range: bytes */" + QByteArray::number(size)
                      + "\r\nContent-Length: 0\r\nAccept-Ranges: bytes\r\nAccess-Control-Allow-Origin: *\r\n"
                        "Connection: close\r\n\r\n");
        return;
    }
    if (r.kind == CastServe::ByteRange::Partial)
    {
        first = r.first;
        len = r.length();
        h = statusLine(206) + "Content-Range: bytes " + QByteArray::number(r.first) + '-'
          + QByteArray::number(r.last) + '/' + QByteArray::number(size) + "\r\n";
    }
    else
    {
        h = statusLine(200);
    }
    h += "Content-Type: " + CastServe::contentTypeFor(fileName_) + "\r\n"
         "Content-Length: " + QByteArray::number(len) + "\r\n"
         "Accept-Ranges: bytes\r\n"
         // CORS on THIS server only; the header comment says why, and why #423 still forbids it elsewhere.
         "Access-Control-Allow-Origin: *\r\n"
         "Connection: close\r\n\r\n";
    s->write(h);
    if (isHead || len == 0 || !f->seek(first))
    {
        delete f;
        s->disconnectFromHost();
        return;
    }
    c->file = f;
    c->remaining = len;
    pump(s);
}

void CastFileServer::pump(QTcpSocket* s)
{
    Conn* c = conns_.value(s);
    if (!c || !c->file) return;
    // Read only while the socket has drained below the high-water mark: a renderer that stops reading (paused,
    // buffer full) stops the reads here too, so memory stays at a few chunks whatever the file's size.
    while (c->remaining > 0 && s->bytesToWrite() < kHighWaterBytes)
    {
        const QByteArray chunk = c->file->read(qMin(kChunkBytes, c->remaining));
        if (chunk.isEmpty()) { drop(s); return; }   // the file shrank or failed under us: cut, never pad
        c->remaining -= chunk.size();
        s->write(chunk);
    }
    if (c->remaining == 0)
    {
        delete c->file;
        c->file = nullptr;
        s->disconnectFromHost();
    }
}
