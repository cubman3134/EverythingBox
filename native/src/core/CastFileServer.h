// Casting a LOCAL file (issue #72): the smallest HTTP server that lets a Chromecast or a DLNA renderer fetch
// one file off this device's disk.
//
// A cast target fetches the media URL ITSELF (CastManager.h). For an addon/debrid stream that URL already
// exists; a file on this disk has none, so this unit makes one, for exactly as long as the cast lasts.
//
// WHAT IT SERVES, AND NOTHING ELSE.
//   * One file, at /cast/<token>/<url-encoded file name>. The token is 128 random bits from the system CSPRNG,
//     written as 32 lowercase hex characters. The file name rides along only so a renderer that shows or sniffs
//     it sees a real name; it must match, but it is never used to find anything on disk. The server holds the
//     path it was handed, and no byte of the request ever becomes part of a path.
//   * Every other path is 404 with a fixed body: `/`, `/cast/`, a wrong token, a wrong or traversing name, a
//     trailing segment. There is no listing because there is nothing to list.
//   * GET and HEAD only. Anything else is 405 with `Allow: GET, HEAD`. That includes OPTIONS: no CORS preflight
//     is ever answered, because a media element does not send one.
//
// HTTP. `Accept-Ranges: bytes` on every media answer. A single `Range: bytes=` range, including the open-ended
// `n-` and the suffix `-n`, is 206 with `Content-Range`. A range that starts past the end (or a zero-length
// suffix) is 416 with `Content-Range: bytes */<size>`. A MULTI-range request is answered 200 with the whole file:
// RFC 9110 allows a server to ignore Range, a multipart/byteranges body is a thing no cast receiver asks for,
// and this is the documented simplification. A malformed Range header is likewise ignored (200), as the RFC
// says. Bodies are streamed from the file in chunks under socket back-pressure; a file is never read whole.
// `Connection: close` ends every answer.
//
// CORS. A Chromecast receiver is a web page, and some receivers fetch media with XHR/fetch rather than a bare
// <video src>; those need `Access-Control-Allow-Origin` on the media response or they refuse it. So THIS server
// sends `Access-Control-Allow-Origin: *` on its media answers. #423 deliberately forbids any CORS header on the
// OTHER LAN listener (RemoteServer), and that rule still stands there: that listener has routes with effects and
// a paired-device credential. Nothing like that lives here. This listener serves one file, public to whoever
// holds the token, and nothing else, so letting a page read what the token already grants adds no reach. For
// the same reason there is no Host/Origin gate here: a DNS-rebinding page gains nothing without the token, and
// some renderers send no Host header at all.
//
// ACCESS. The token is the capability. revoke() ends it at once: any request still arriving with it is 404, and
// a body still streaming under it is cut. There is NO source-address restriction to the cast target. A
// Chromecast can fetch from an address other than the one the app talks to (a device with wired and wireless
// interfaces, a mesh node, a receiver app that delegates), and nobody has measured what a real receiver does,
// so restricting it could only break a real device in a way no test here would see. The token is not logged:
// callers log the file name or LogSafeText::url(url()), which drops the path's middle (the token).
//
// WHERE IT LISTENS. Never 0.0.0.0, never a public address. serve() refuses any bind address that is not a
// private LAN IPv4 (RFC 1918 or link-local) or loopback, so even a wrong caller cannot open it wider.
// chooseBindAddress() is the pure rule for WHICH address: the one on the interface that reaches the target.
//
// QUIT. The server lives on the thread that created it (the GUI thread in the app) and owns no thread. stop()
// closes the listener and aborts every connection synchronously; it is connected to aboutToQuit, so a download
// in flight cannot hold the exit (#442).
//
// QtCore + QtNetwork only.
#pragma once
#include <QByteArray>
#include <QHash>
#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QString>
#include <QUrl>

class QTcpServer;
class QTcpSocket;

namespace CastServe
{
// ---- the pure rules (probe_castserver drives each with no socket) --------------------------------------

// Content-Type from the file's extension, case-insensitive. Unknown -> application/octet-stream.
QByteArray contentTypeFor(const QString& fileName);

// One parsed Range header against a file of `size` bytes.
struct ByteRange
{
    enum Kind { Whole, Partial, Unsatisfiable };
    Kind kind = Whole;
    qint64 first = 0;   // inclusive
    qint64 last = -1;   // inclusive; last - first + 1 bytes
    qint64 length() const { return kind == Whole ? -1 : last - first + 1; }
};
// `present` is whether the request carried a Range header at all; `value` is its value.
ByteRange parseRange(bool present, const QByteArray& value, qint64 size);

// A fresh 128-bit token: 32 lowercase hex characters from the system CSPRNG.
QByteArray newToken();
// Constant-time equality, so the comparison's duration says nothing about how much of a guess was right.
bool tokenEquals(const QByteArray& a, const QByteArray& b);
// The one path a file is served at: "/cast/<token>/<percent-encoded name>".
QByteArray castPath(const QByteArray& token, const QString& fileName);
// Whether a request-target names the served file. The query, if any, is ignored. Exactly four segments
// ("", "cast", token, name); the name is percent-decoded and must equal `fileName` byte for byte.
bool targetMatches(const QByteArray& requestTarget, const QByteArray& token, const QString& fileName);

// Private LAN IPv4: 10/8, 172.16/12, 192.168/16, 169.254/16 (link-local). Not loopback, not 100.64/10.
bool isLanIpv4(const QHostAddress& a);
// An address serve() will bind: a LAN IPv4 or an IPv4 loopback address. Never Any/null/broadcast/public.
bool isBindable(const QHostAddress& a);

// One address on one interface, as QNetworkInterface reports it (fakeable in the probe).
struct IfaceAddr
{
    QString iface;
    bool up = true;
    bool running = true;
    bool loopback = false;
    QHostAddress ip;
    int prefixLength = -1;
};
// The address to bind so `target` can reach this device. IPv4 only (every cast target this app finds is IPv4).
//   * A loopback target gets a loopback address, and only a loopback target does.
//   * A public target gets nothing: this server never serves off the LAN.
//   * Otherwise, among the LAN addresses of up+running interfaces whose subnet holds the target: the one the OS
//     routes through (`routeHint`, from a connected UDP socket) if it is one of them, else the longest prefix,
//     then list order.
//   * No interface's subnet holds the target (a routed LAN): the route hint, if it is itself a LAN address of an
//     up interface. Else nothing.
// A null result means "don't cast": the caller says so rather than guessing an interface.
QHostAddress chooseBindAddress(const QList<IfaceAddr>& addrs, const QHostAddress& target,
                               const QHostAddress& routeHint = QHostAddress());
// This machine's interfaces in IfaceAddr form, and the OS's source address toward `target` (no packet is sent).
QList<IfaceAddr> systemInterfaces();
QHostAddress routeHintFor(const QHostAddress& target);

// The local file a playing URL/path names: a file:// URL or an absolute path to an existing regular file,
// returned as a clean absolute path. "" for http(s), for anything relative, and for a file that is not there.
QString localFileFor(const QString& urlOrPath);

// What the cast picker may offer for what is playing now. `localPath` is localFileFor(what the player loaded)
// for a VIDEO session, else "". A local file wins over a leftover remote castUrl: it is what is on screen.
enum class Offer { NothingCastable, HeaderGated, RemoteUrl, LocalFile };
Offer offerFor(const QString& castUrl, bool headerGated, const QString& localPath);
} // namespace CastServe

class CastFileServer : public QObject
{
    Q_OBJECT
public:
    static constexpr int kMaxConnections = 8;        // a renderer seeking opens a few; more is not a renderer
    static constexpr int kMaxHeaderBytes = 8192;     // a request head bigger than this is not a media fetch
    static constexpr int kHeaderTimeoutMs = 10000;   // a connection that never finishes its head is dropped
    static constexpr qint64 kChunkBytes = 64 * 1024;
    static constexpr qint64 kHighWaterBytes = 256 * 1024; // read more only when the socket has drained below this

    explicit CastFileServer(QObject* parent = nullptr);
    ~CastFileServer() override;

    // Serve `filePath` on `bindAddress`, on a random port. Mints a fresh token and revokes the previous one at
    // once. Keeps the listener when it is already bound to `bindAddress`, else (re)binds. Returns false, and
    // leaves nothing listening, for a file that cannot be read or a bind address isBindable() refuses.
    bool serve(const QString& filePath, const QHostAddress& bindAddress, QString* error = nullptr);
    void revoke();   // the token stops working now; bodies streaming under it are cut. The listener stays.
    void stop();     // revoke(), close the listener, abort every connection. Synchronous. Idempotent.

    bool isListening() const;
    bool isServing() const { return isListening() && !token_.isEmpty(); }
    QUrl url() const;                     // http://<address>:<port>/cast/<token>/<name>; empty when not serving
    QString filePath() const { return filePath_; }
    QHostAddress address() const;
    quint16 port() const;
    int connectionCount() const { return int(conns_.size()); }

private:
    struct Conn;
    void onNewConnection();
    void onReadyRead(QTcpSocket* s);
    void pump(QTcpSocket* s);
    void finish(QTcpSocket* s, const QByteArray& response);
    void drop(QTcpSocket* s);

    QTcpServer* server_ = nullptr;
    QByteArray token_;
    QString filePath_;
    QString fileName_;
    QHash<QTcpSocket*, Conn*> conns_;
};
