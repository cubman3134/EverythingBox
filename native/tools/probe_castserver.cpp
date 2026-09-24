// Headless check of core/CastFileServer and CastManager::castLocalFile (issue #72): casting a LOCAL file through
// a minimal, on-demand HTTP server that a Chromecast or DLNA renderer can fetch from.
//
// Everything runs in this process against loopback: a generated fixture file, an in-process HTTP client written
// on a raw QTcpSocket (so the bytes on the wire are exactly what a renderer would see), and a fake DLNA
// AVTransport endpoint that records the URL it is handed. No device and no LAN traffic.
//
// What this probe pins:
//   A. The Content-Type table.
//   B. Range arithmetic, pure: single, open-ended, suffix, clamped, unsatisfiable, multi, malformed.
//   C. The capability: the token's shape, constant-time equality, the one served path, and every path that must
//      not match (wrong token, "/", "/cast/", traversal in the name part, extra segments, absolute-form).
//   D. Where it listens: the bind predicate and the interface choice, a pure function over FAKE interface lists.
//   E. What the cast picker may offer (a local file, a remote url, a header-gated one, nothing).
//   F. The server over a real socket: full GET and HEAD, the three range shapes, 416, multi-range, 404/405, the
//      head cap, the connection cap, bind refusals, revoke (404 at once, a streaming body cut) and stop (port
//      closed).
//   G. CastManager::castLocalFile against a fake DLNA renderer: the URL it is handed serves the file byte-exact,
//      and stopCasting closes it. A public target is refused before anything listens.
//   H. Quit: with a client mid-download under back-pressure, aboutToQuit stops the server inside the #442 budget.
//
// The token never appears in this probe's output. Prints CASTSERVER-OK on success; any failure prints
// CASTSERVER-FAIL <cond> (line) and exits non-zero.
#include "CastFileServer.h"
#include "CastManager.h"
#include "QuitBudget.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QHostAddress>
#include <QNetworkProxy>
#include <QPointer>
#include <QRegularExpression>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>

#include <cstdio>
#include <memory>
#include <vector>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "CASTSERVER-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

using namespace CastServe;

namespace
{
// Deterministic, non-repeating-looking bytes, so a wrong offset can never compare equal by accident.
QByteArray makeFixture(qint64 n)
{
    QByteArray b(int(n), Qt::Uninitialized);
    quint32 x = 0x9E3779B9u;
    for (qint64 i = 0; i < n; ++i) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; b[int(i)] = char(x & 0xFF); }
    return b;
}

struct Resp
{
    bool connected = false;
    int status = 0;
    QHash<QByteArray, QList<QByteArray>> h;   // lower-cased name -> values
    QByteArray body;
    QByteArray raw;
    QByteArray one(const char* name) const
    {
        const auto it = h.constFind(QByteArray(name).toLower());
        return it == h.constEnd() || it->isEmpty() ? QByteArray() : it->first();
    }
    bool has(const char* name) const { return h.contains(QByteArray(name).toLower()); }
};

Resp parse(const QByteArray& all)
{
    Resp r;
    r.raw = all;
    const int end = all.indexOf("\r\n\r\n");
    if (end < 0) return r;
    const QList<QByteArray> lines = all.left(end).split('\n');
    if (lines.isEmpty()) return r;
    const QList<QByteArray> sl = lines.first().trimmed().split(' ');
    if (sl.size() >= 2) r.status = sl.at(1).toInt();
    for (int i = 1; i < lines.size(); ++i)
    {
        const QByteArray l = lines.at(i).trimmed();
        const int c = l.indexOf(':');
        if (c <= 0) continue;
        r.h[l.left(c).trimmed().toLower()].append(l.mid(c + 1).trimmed());
    }
    r.body = all.mid(end + 4);
    return r;
}

// One request, one answer, read until the server closes. Event-loop driven: the server is in this thread.
Resp roundTrip(quint16 port, const QByteArray& raw, int timeoutMs = 8000)
{
    QTcpSocket s;
    QByteArray all;
    QEventLoop loop;
    QTimer t;
    t.setSingleShot(true);
    bool connected = false;
    QObject::connect(&s, &QTcpSocket::connected, &loop, [&] { connected = true; s.write(raw); });
    QObject::connect(&s, &QTcpSocket::readyRead, &loop, [&] { all += s.readAll(); });
    QObject::connect(&s, &QTcpSocket::disconnected, &loop, [&] { all += s.readAll(); loop.quit(); });
    QObject::connect(&s, &QAbstractSocket::errorOccurred, &loop, [&](QAbstractSocket::SocketError) {
        all += s.readAll(); loop.quit(); });
    QObject::connect(&t, &QTimer::timeout, &loop, &QEventLoop::quit);
    t.start(timeoutMs);
    s.connectToHost(QHostAddress(QHostAddress::LocalHost), port);
    loop.exec();
    Resp r = parse(all);
    r.connected = connected;
    return r;
}

QByteArray get(const QByteArray& path, const QByteArray& extra = QByteArray(), const char* method = "GET")
{
    return QByteArray(method) + ' ' + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\n" + extra + "\r\n";
}

// Whether anything accepts a connection on this port now.
bool portOpen(quint16 port)
{
    QTcpSocket s;
    QEventLoop loop;
    bool ok = false;
    QObject::connect(&s, &QTcpSocket::connected, &loop, [&] { ok = true; loop.quit(); });
    QObject::connect(&s, &QAbstractSocket::errorOccurred, &loop, [&](QAbstractSocket::SocketError) { loop.quit(); });
    QTimer::singleShot(3000, &loop, &QEventLoop::quit);
    s.connectToHost(QHostAddress(QHostAddress::LocalHost), port);
    loop.exec();
    s.abort();
    return ok;
}

void spin(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

IfaceAddr ia(const char* name, const char* ip, int prefix, bool up = true, bool loop = false, bool running = true)
{
    IfaceAddr a;
    a.iface = QString::fromLatin1(name);
    a.ip = QHostAddress(QString::fromLatin1(ip));
    a.prefixLength = prefix;
    a.up = up;
    a.running = running;
    a.loopback = loop;
    return a;
}

// The fake DLNA renderer: an AVTransport control endpoint on loopback that answers 200 to every SOAP call and
// records each action and its body.
struct FakeRenderer
{
    QTcpServer srv;
    QList<QPair<QByteArray, QByteArray>> calls;   // (SOAPACTION, body)
    std::vector<std::unique_ptr<QByteArray>> bufs;
    bool listen()
    {
        if (!srv.listen(QHostAddress(QHostAddress::LocalHost), 0)) return false;
        QObject::connect(&srv, &QTcpServer::newConnection, &srv, [this] {
            while (QTcpSocket* s = srv.nextPendingConnection())
            {
                bufs.emplace_back(new QByteArray);
                QByteArray* buf = bufs.back().get();
                QObject::connect(s, &QTcpSocket::readyRead, s, [this, s, buf] {
                    *buf += s->readAll();
                    const int end = buf->indexOf("\r\n\r\n");
                    if (end < 0) return;
                    const QRegularExpression cl(QStringLiteral("(?im)^content-length:\\s*(\\d+)"));
                    const auto m = cl.match(QString::fromLatin1(buf->left(end)));
                    const int len = m.hasMatch() ? m.captured(1).toInt() : 0;
                    if (buf->size() < end + 4 + len) return;
                    const QRegularExpression sa(QStringLiteral("(?im)^soapaction:\\s*(.*)$"));
                    const auto a = sa.match(QString::fromLatin1(buf->left(end)));
                    calls.append({ a.hasMatch() ? a.captured(1).trimmed().toLatin1() : QByteArray(),
                                   buf->mid(end + 4, len) });
                    s->write("HTTP/1.1 200 OK\r\nContent-Type: text/xml\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                    s->disconnectFromHost();
                    buf->clear();
                });
                QObject::connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
            }
        });
        return true;
    }
    QByteArray bodyOf(const char* action) const
    {
        for (const auto& c : calls)
            if (c.first.contains(action)) return c.second;
        return QByteArray();
    }
};

QString xmlUnescape(QString s)
{
    s.replace(QStringLiteral("&lt;"), QStringLiteral("<")).replace(QStringLiteral("&gt;"), QStringLiteral(">"))
     .replace(QStringLiteral("&quot;"), QStringLiteral("\"")).replace(QStringLiteral("&amp;"), QStringLiteral("&"));
    return s;
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);   // loopback only; never a system proxy

    // ------------------------------------------------------------------------ A. Content-Type table ------
    {
        const struct { const char* name; const char* type; } table[] = {
            { "a.mp4", "video/mp4" }, { "A.MP4", "video/mp4" }, { "a.m4v", "video/mp4" },
            { "a.mkv", "video/x-matroska" }, { "a.webm", "video/webm" }, { "a.mov", "video/quicktime" },
            { "a.avi", "video/x-msvideo" }, { "a.ts", "video/mp2t" }, { "a.m2ts", "video/mp2t" },
            { "a.mp3", "audio/mpeg" }, { "a.flac", "audio/flac" }, { "a.m4a", "audio/mp4" },
            { "a.aac", "audio/aac" }, { "a.ogg", "audio/ogg" }, { "a.opus", "audio/ogg" },
            { "a.wav", "audio/wav" }, { "a.jpg", "image/jpeg" }, { "a.JPEG", "image/jpeg" },
            { "a.png", "image/png" }, { "a.gif", "image/gif" }, { "a.webp", "image/webp" },
            { "a.exe", "application/octet-stream" }, { "noext", "application/octet-stream" },
            { "a.mp4.txt", "application/octet-stream" }, { "", "application/octet-stream" },
            { "dir.mkv/film", "application/octet-stream" },
        };
        for (const auto& row : table)
        {
            const QByteArray got = contentTypeFor(QString::fromLatin1(row.name));
            if (got != QByteArray(row.type))
                std::fprintf(stderr, "  type %s -> '%s' (want %s)\n", row.name, got.constData(), row.type);
            CHECK(got == QByteArray(row.type));
        }
    }

    // ------------------------------------------------------------------------ B. Range arithmetic --------
    {
        const qint64 N = 1000;
        struct Row { bool present; const char* v; ByteRange::Kind k; qint64 first, last; };
        const Row rows[] = {
            { false, "",              ByteRange::Whole,         0, -1 },
            { true,  "bytes=0-99",    ByteRange::Partial,       0, 99 },
            { true,  "bytes=100-",    ByteRange::Partial,     100, 999 },
            { true,  "bytes=-100",    ByteRange::Partial,     900, 999 },
            { true,  "bytes=0-",      ByteRange::Partial,       0, 999 },
            { true,  "bytes=999-999", ByteRange::Partial,     999, 999 },
            { true,  "bytes=990-5000",ByteRange::Partial,     990, 999 },   // last clamped to the end
            { true,  "bytes=-5000",   ByteRange::Partial,       0, 999 },   // suffix longer than the file
            { true,  "BYTES = 5-9",   ByteRange::Partial,       5, 9 },     // unit is case-insensitive, OWS ok
            { true,  "bytes=1000-",   ByteRange::Unsatisfiable, 0, -1 },
            { true,  "bytes=1000-1001",ByteRange::Unsatisfiable,0, -1 },
            { true,  "bytes=-0",      ByteRange::Unsatisfiable, 0, -1 },
            { true,  "bytes=99999999999999999999-", ByteRange::Unsatisfiable, 0, -1 }, // overflow is past the end
            { true,  "bytes=0-9,20-29", ByteRange::Whole,       0, -1 },    // multi-range: the whole file
            { true,  "bytes=5-4",     ByteRange::Whole,         0, -1 },    // invalid spec: ignored
            { true,  "bytes=abc",     ByteRange::Whole,         0, -1 },
            { true,  "bytes=",        ByteRange::Whole,         0, -1 },
            { true,  "bytes=-",       ByteRange::Whole,         0, -1 },
            { true,  "items=0-9",     ByteRange::Whole,         0, -1 },    // another unit: ignored
            { true,  "bytes=+5-9",    ByteRange::Whole,         0, -1 },
            { true,  "bytes=5-9x",    ByteRange::Whole,         0, -1 },
        };
        for (const Row& r : rows)
        {
            const ByteRange g = parseRange(r.present, QByteArray(r.v), N);
            const bool ok = g.kind == r.k
                         && (r.k != ByteRange::Partial || (g.first == r.first && g.last == r.last));
            if (!ok)
                std::fprintf(stderr, "  range '%s' -> kind %d %lld-%lld\n", r.v, int(g.kind),
                             (long long)g.first, (long long)g.last);
            CHECK(ok);
        }
        CHECK(parseRange(true, "bytes=0-99", N).length() == 100);
        CHECK(parseRange(true, "bytes=-100", N).length() == 100);
        CHECK(parseRange(true, "bytes=0-0", N).length() == 1);
        // An empty file: nothing is satisfiable, and no Range is a plain 200 of zero bytes.
        CHECK(parseRange(true, "bytes=0-", 0).kind == ByteRange::Unsatisfiable);
        CHECK(parseRange(true, "bytes=-1", 0).kind == ByteRange::Unsatisfiable);
        CHECK(parseRange(false, QByteArray(), 0).kind == ByteRange::Whole);
    }

    // ------------------------------------------------------------------------ C. The capability ----------
    {
        const QByteArray t1 = newToken(), t2 = newToken();
        CHECK(t1.size() == 32);
        CHECK(QRegularExpression(QStringLiteral("^[0-9a-f]{32}$")).match(QString::fromLatin1(t1)).hasMatch());
        CHECK(t1 != t2);
        CHECK(tokenEquals(t1, t1));
        CHECK(!tokenEquals(t1, t2));
        CHECK(!tokenEquals(t1, t1.left(31)));
        CHECK(!tokenEquals(t1, QByteArray()));
        CHECK(!tokenEquals(QByteArray(), QByteArray()));   // no token is not a match for no token

        const QString name = QStringLiteral("My Film (2020) ü#1.mkv");
        const QByteArray p = castPath(t1, name);
        CHECK(p.startsWith("/cast/" + t1 + "/"));
        CHECK(!p.contains(' '));
        CHECK(!p.contains('#'));
        CHECK(targetMatches(p, t1, name));
        CHECK(targetMatches(p + "?x=1", t1, name));                         // a query is ignored
        CHECK(targetMatches("/cast/" + t1 + "/My%20Film%20(2020)%20%C3%BC%231.mkv", t1, name));
        CHECK(!targetMatches(p, t2, name));                                 // wrong token
        CHECK(!targetMatches(castPath(t2, name), t1, name));
        CHECK(!targetMatches("/cast/" + t1.toUpper() + "/" + p.mid(p.lastIndexOf('/') + 1), t1, name));
        CHECK(!targetMatches(p, QByteArray(), name));                       // revoked: nothing matches
        CHECK(!targetMatches("/cast//" + p.mid(p.lastIndexOf('/') + 1), QByteArray(), name));
        CHECK(!targetMatches("/", t1, name));
        CHECK(!targetMatches("", t1, name));
        CHECK(!targetMatches("/cast", t1, name));
        CHECK(!targetMatches("/cast/", t1, name));
        CHECK(!targetMatches("/cast/" + t1, t1, name));
        CHECK(!targetMatches("/cast/" + t1 + "/", t1, name));
        CHECK(!targetMatches("/cast/" + t1 + "/other.mkv", t1, name));
        CHECK(!targetMatches("/cast/" + t1 + "/../" + p.mid(p.lastIndexOf('/') + 1), t1, name));
        CHECK(!targetMatches("/cast/" + t1 + "/..%2F..%2Fsecret.txt", t1, name));
        CHECK(!targetMatches("/cast/" + t1 + "/..%5C..%5Csecret.txt", t1, name));
        CHECK(!targetMatches(p + "/..", t1, name));
        CHECK(!targetMatches(p + "/", t1, name));
        CHECK(!targetMatches("/x" + p, t1, name));
        CHECK(!targetMatches("//cast/" + t1 + "/" + p.mid(p.lastIndexOf('/') + 1), t1, name));
        CHECK(!targetMatches("http://127.0.0.1" + p, t1, name));            // absolute-form: not ours
        CHECK(!targetMatches("/CAST/" + t1 + "/" + p.mid(p.lastIndexOf('/') + 1), t1, name));
        // A name that decodes to a separator can never equal a real file name.
        CHECK(!targetMatches("/cast/" + t1 + "/a%2Fb.mkv", t1, QStringLiteral("a/b.mkv")));
    }

    // ------------------------------------------------------------------------ D. Where it listens --------
    {
        CHECK(isLanIpv4(QHostAddress(QStringLiteral("192.168.1.10"))));
        CHECK(isLanIpv4(QHostAddress(QStringLiteral("10.4.5.6"))));
        CHECK(isLanIpv4(QHostAddress(QStringLiteral("172.16.0.1"))));
        CHECK(isLanIpv4(QHostAddress(QStringLiteral("172.31.255.254"))));
        CHECK(isLanIpv4(QHostAddress(QStringLiteral("169.254.3.4"))));
        CHECK(!isLanIpv4(QHostAddress(QStringLiteral("172.32.0.1"))));
        CHECK(!isLanIpv4(QHostAddress(QStringLiteral("172.15.255.255"))));
        CHECK(!isLanIpv4(QHostAddress(QStringLiteral("8.8.8.8"))));
        CHECK(!isLanIpv4(QHostAddress(QStringLiteral("100.64.0.5"))));
        CHECK(!isLanIpv4(QHostAddress(QStringLiteral("127.0.0.1"))));
        CHECK(!isLanIpv4(QHostAddress(QStringLiteral("0.0.0.0"))));
        CHECK(!isLanIpv4(QHostAddress(QStringLiteral("fe80::1"))));
        CHECK(isBindable(QHostAddress(QStringLiteral("192.168.1.10"))));
        CHECK(isBindable(QHostAddress(QHostAddress::LocalHost)));
        CHECK(!isBindable(QHostAddress(QHostAddress::AnyIPv4)));
        CHECK(!isBindable(QHostAddress(QHostAddress::Any)));
        CHECK(!isBindable(QHostAddress(QHostAddress::AnyIPv6)));
        CHECK(!isBindable(QHostAddress()));
        CHECK(!isBindable(QHostAddress(QHostAddress::Broadcast)));
        CHECK(!isBindable(QHostAddress(QStringLiteral("203.0.113.7"))));
        CHECK(!isBindable(QHostAddress(QHostAddress::LocalHostIPv6)));

        const QHostAddress none;
        auto H = [](const char* s) { return QHostAddress(QString::fromLatin1(s)); };
        const QList<IfaceAddr> home = {
            ia("lo", "127.0.0.1", 8, true, true),
            ia("public", "203.0.113.7", 24),
            ia("eth", "192.168.1.10", 24),
            ia("wlan", "10.0.0.5", 8),
            ia("v6", "fe80::1", 64),
        };
        CHECK(chooseBindAddress(home, H("192.168.1.50")) == H("192.168.1.10"));
        CHECK(chooseBindAddress(home, H("10.9.8.7")) == H("10.0.0.5"));
        CHECK(chooseBindAddress(home, H("203.0.113.50")).isNull());        // public target: never
        CHECK(chooseBindAddress(home, H("8.8.8.8"), H("203.0.113.7")).isNull());
        CHECK(chooseBindAddress(home, H("127.0.0.1")) == H("127.0.0.1"));  // loopback only for loopback
        CHECK(chooseBindAddress(home, H("fe80::2")).isNull());             // IPv6 targets: not supported
        CHECK(chooseBindAddress(home, QHostAddress()).isNull());
        // A LAN target outside every subnet (a routed LAN): the OS route, if it is one of our LAN addresses.
        CHECK(chooseBindAddress(home, H("172.20.1.1"), H("192.168.1.10")) == H("192.168.1.10"));
        CHECK(chooseBindAddress(home, H("172.20.1.1"), H("203.0.113.7")).isNull());
        CHECK(chooseBindAddress(home, H("172.20.1.1"), H("192.168.1.99")).isNull());   // not ours
        CHECK(chooseBindAddress(home, H("172.20.1.1")).isNull());
        // A down interface, a not-running one, and a loopback-flagged one never serve a LAN target.
        const QList<IfaceAddr> down = {
            ia("eth", "192.168.1.10", 24, /*up=*/false),
            ia("wlan", "192.168.1.11", 24, true, false, /*running=*/false),
            ia("odd", "192.168.1.12", 24, true, /*loopback=*/true),
        };
        CHECK(chooseBindAddress(down, H("192.168.1.50")).isNull());
        CHECK(chooseBindAddress(down, H("192.168.1.50"), H("192.168.1.10")).isNull());
        // Two interfaces on one subnet: the OS route wins; without one, the list order.
        const QList<IfaceAddr> twin = { ia("eth", "192.168.1.10", 24), ia("wlan", "192.168.1.20", 24) };
        CHECK(chooseBindAddress(twin, H("192.168.1.50"), H("192.168.1.20")) == H("192.168.1.20"));
        CHECK(chooseBindAddress(twin, H("192.168.1.50")) == H("192.168.1.10"));
        CHECK(chooseBindAddress(twin, H("192.168.1.50"), H("10.1.1.1")) == H("192.168.1.10"));
        // Nested subnets: the longest prefix is the more specific route.
        const QList<IfaceAddr> nest = { ia("vpn", "10.0.0.2", 8), ia("lab", "10.1.2.3", 24) };
        CHECK(chooseBindAddress(nest, H("10.1.2.50")) == H("10.1.2.3"));
        CHECK(chooseBindAddress(nest, H("10.7.7.7")) == H("10.0.0.2"));
        // Link-local.
        CHECK(chooseBindAddress({ ia("ll", "169.254.10.1", 16) }, H("169.254.20.2")) == H("169.254.10.1"));
        // A public address whose subnet "holds" a private target is still never chosen; nor is 0.0.0.0, nor
        // an interface address outside the private ranges, nor a CGNAT one.
        CHECK(chooseBindAddress({ ia("pub", "203.0.113.7", 0) }, H("192.168.1.50")).isNull());
        CHECK(chooseBindAddress({ ia("any", "0.0.0.0", 0) }, H("192.168.1.50")).isNull());
        CHECK(chooseBindAddress({ ia("cg", "100.64.0.2", 10) }, H("100.64.0.9")).isNull());
        // Whatever the list, the answer is bindable or null — never Any.
        for (const char* t : { "192.168.1.50", "10.0.0.9", "8.8.8.8", "127.0.0.1", "0.0.0.0", "255.255.255.255" })
        {
            const QHostAddress got = chooseBindAddress(home, H(t), H("192.168.1.10"));
            CHECK(got.isNull() || isBindable(got));
            CHECK(got != QHostAddress(QHostAddress::AnyIPv4));
        }
    }

    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    const QString dir = tmp.path();

    // ------------------------------------------------------------------------ E. What may be offered ------
    {
        const QString real = dir + QStringLiteral("/Offer Fixture.mp4");
        { QFile f(real); CHECK(f.open(QIODevice::WriteOnly)); f.write("x"); }
        CHECK(localFileFor(real) == QFileInfo(real).absoluteFilePath());
        CHECK(localFileFor(QUrl::fromLocalFile(real).toString()) == QFileInfo(real).absoluteFilePath());
        CHECK(localFileFor(dir + QStringLiteral("/missing.mp4")).isEmpty());
        CHECK(localFileFor(dir).isEmpty());                                 // a directory is not a file
        CHECK(localFileFor(QStringLiteral("Offer Fixture.mp4")).isEmpty());   // relative
        CHECK(localFileFor(QStringLiteral("https://cdn.example/film.mp4")).isEmpty());
        CHECK(localFileFor(QStringLiteral("http://127.0.0.1:8080/x.mp4")).isEmpty());
        CHECK(localFileFor(QString()).isEmpty());

        const QString web = QStringLiteral("https://cdn.example/film.mp4");
        CHECK(offerFor(web, false, QString()) == Offer::RemoteUrl);
        CHECK(offerFor(web, true, QString()) == Offer::HeaderGated);
        CHECK(offerFor(QString(), false, QString()) == Offer::NothingCastable);
        CHECK(offerFor(QStringLiteral("magnet:?xt=urn:btih:00"), false, QString()) == Offer::NothingCastable);
        CHECK(offerFor(QString(), false, real) == Offer::LocalFile);
        CHECK(offerFor(web, false, real) == Offer::LocalFile);   // what is playing beats a leftover url
        CHECK(offerFor(web, true, real) == Offer::LocalFile);
        CHECK(offerFor(real, false, real) == Offer::LocalFile);
    }

    // ------------------------------------------------------------------------ F. The server, over a socket -
    const QString fname = QStringLiteral("Cast Fixture ü (2020).mp4");
    const QString fpath = dir + QStringLiteral("/") + fname;
    const qint64 N = 3 * 1024 * 1024 + 12345;   // several chunks, not a multiple of one
    const QByteArray fixture = makeFixture(N);
    { QFile f(fpath); CHECK(f.open(QIODevice::WriteOnly)); CHECK(f.write(fixture) == N); }

    {
        CastFileServer srv;
        QString err;
        // Bind refusals: nothing listens after any of them.
        CHECK(!srv.serve(fpath, QHostAddress(QHostAddress::AnyIPv4), &err));
        CHECK(!srv.isListening());
        CHECK(!srv.serve(fpath, QHostAddress(QHostAddress::Any), &err));
        CHECK(!srv.serve(fpath, QHostAddress(QStringLiteral("203.0.113.7")), &err));
        CHECK(!srv.serve(fpath, QHostAddress(), &err));
        CHECK(!srv.isListening());
        CHECK(!srv.serve(dir + QStringLiteral("/missing.mp4"), QHostAddress(QHostAddress::LocalHost), &err));
        CHECK(!srv.serve(dir, QHostAddress(QHostAddress::LocalHost), &err));
        CHECK(!srv.isListening());
        CHECK(srv.url().isEmpty());

        CHECK(srv.serve(fpath, QHostAddress(QHostAddress::LocalHost), &err));
        CHECK(srv.isServing());
        const quint16 port = srv.port();
        CHECK(port != 0);
        CHECK(srv.address() == QHostAddress(QHostAddress::LocalHost));
        CHECK(srv.address() != QHostAddress(QHostAddress::AnyIPv4));
        const QUrl u = srv.url();
        CHECK(u.scheme() == QStringLiteral("http"));
        CHECK(u.host() == QStringLiteral("127.0.0.1"));
        CHECK(u.port() == port);
        const QByteArray path = u.path(QUrl::FullyEncoded).toLatin1();
        CHECK(path.startsWith("/cast/"));
        CHECK(path.split('/').size() == 4);

        // Full GET.
        Resp r = roundTrip(port, get(path));
        CHECK(r.connected);
        CHECK(r.status == 200);
        CHECK(r.one("content-length") == QByteArray::number(N));
        CHECK(r.one("content-type") == "video/mp4");
        CHECK(r.one("accept-ranges") == "bytes");
        CHECK(r.one("access-control-allow-origin") == "*");
        CHECK(r.one("connection").toLower() == "close");
        CHECK(!r.has("content-range"));
        CHECK(r.body.size() == N);
        CHECK(r.body == fixture);
        // HEAD: the same headers, no body.
        r = roundTrip(port, get(path, QByteArray(), "HEAD"));
        CHECK(r.status == 200);
        CHECK(r.one("content-length") == QByteArray::number(N));
        CHECK(r.one("content-type") == "video/mp4");
        CHECK(r.one("accept-ranges") == "bytes");
        CHECK(r.body.isEmpty());
        // bytes=0-99
        r = roundTrip(port, get(path, "Range: bytes=0-99\r\n"));
        CHECK(r.status == 206);
        CHECK(r.one("content-range") == "bytes 0-99/" + QByteArray::number(N));
        CHECK(r.one("content-length") == "100");
        CHECK(r.body == fixture.left(100));
        // bytes=100-
        r = roundTrip(port, get(path, "Range: bytes=100-\r\n"));
        CHECK(r.status == 206);
        CHECK(r.one("content-range") == "bytes 100-" + QByteArray::number(N - 1) + "/" + QByteArray::number(N));
        CHECK(r.one("content-length") == QByteArray::number(N - 100));
        CHECK(r.body == fixture.mid(100));
        // bytes=-100
        r = roundTrip(port, get(path, "Range: bytes=-100\r\n"));
        CHECK(r.status == 206);
        CHECK(r.one("content-range") == "bytes " + QByteArray::number(N - 100) + "-" + QByteArray::number(N - 1)
                                        + "/" + QByteArray::number(N));
        CHECK(r.one("content-length") == "100");
        CHECK(r.body == fixture.right(100));
        // A range across a chunk boundary, deep in the file.
        r = roundTrip(port, get(path, "Range: bytes=2000000-2100000\r\n"));
        CHECK(r.status == 206);
        CHECK(r.body == fixture.mid(2000000, 100001));
        // HEAD with a range: 206 headers, no body.
        r = roundTrip(port, get(path, "Range: bytes=0-99\r\n", "HEAD"));
        CHECK(r.status == 206);
        CHECK(r.one("content-length") == "100");
        CHECK(r.body.isEmpty());
        // Unsatisfiable.
        r = roundTrip(port, get(path, "Range: bytes=" + QByteArray::number(N) + "-\r\n"));
        CHECK(r.status == 416);
        CHECK(r.one("content-range") == "bytes */" + QByteArray::number(N));
        CHECK(r.body.isEmpty());
        // Multi-range: the whole file, 200.
        r = roundTrip(port, get(path, "Range: bytes=0-9,20-29\r\n"));
        CHECK(r.status == 200);
        CHECK(r.one("content-length") == QByteArray::number(N));
        CHECK(r.body == fixture);

        // 404: wrong token, "/", "/cast/", traversal in the name part. Never the file, never a listing.
        const QByteArray wrong = "/cast/" + newToken() + "/" + path.mid(path.lastIndexOf('/') + 1);
        const QList<QByteArray> segs = path.split('/');
        const QByteArray tok = segs.size() > 2 ? segs.at(2) : QByteArray("<none>");
        for (const QByteArray& p : { wrong, QByteArray("/"), QByteArray("/cast/"), QByteArray("/cast"),
                                     QByteArray("/cast/" + tok), QByteArray("/cast/" + tok + "/"),
                                     QByteArray("/cast/" + tok + "/..%2F..%2Fsecret.txt"),
                                     QByteArray("/cast/" + tok + "/../../secret.txt"),
                                     QByteArray("/cast/" + tok + "/x/../" + path.mid(path.lastIndexOf('/') + 1)),
                                     QByteArray(path + "/.."), QByteArray("/favicon.ico"), QByteArray("/cast/x/y") })
        {
            r = roundTrip(port, get(p));
            CHECK(r.status == 404);
            CHECK(r.body.size() < 64);
            CHECK(!r.has("access-control-allow-origin"));
            CHECK(!r.body.contains(fname.toUtf8()));
            CHECK(!r.raw.contains(tok));
        }
        // 405: every method but GET and HEAD, including a CORS preflight.
        for (const char* m : { "POST", "PUT", "DELETE", "OPTIONS", "PATCH", "TRACE", "PROPFIND" })
        {
            r = roundTrip(port, get(path, QByteArray(), m));
            CHECK(r.status == 405);
            CHECK(r.one("allow") == "GET, HEAD");
            CHECK(!r.has("access-control-allow-origin"));
        }
        // Garbage and an HTTP/1.0 request with no Host header (some renderers send exactly that).
        r = roundTrip(port, "NONSENSE\r\n\r\n");
        CHECK(r.status == 400);
        r = roundTrip(port, "GET " + path + " HTTP/1.0\r\nRange: bytes=0-9\r\n\r\n");
        CHECK(r.status == 206);
        CHECK(r.body == fixture.left(10));
        // An oversized request head is refused, not buffered.
        r = roundTrip(port, "GET " + path + " HTTP/1.1\r\nX-Pad: " + QByteArray(CastFileServer::kMaxHeaderBytes, 'a')
                                + "\r\n\r\n");
        CHECK(r.status == 431);

        // The connection cap: kMaxConnections idle connections are held; the next is refused with 503.
        {
            std::vector<std::unique_ptr<QTcpSocket>> idle;
            for (int i = 0; i < CastFileServer::kMaxConnections; ++i)
            {
                idle.emplace_back(new QTcpSocket);
                idle.back()->connectToHost(QHostAddress(QHostAddress::LocalHost), port);
            }
            spin(300);
            CHECK(srv.connectionCount() == CastFileServer::kMaxConnections);
            r = roundTrip(port, get(path));
            CHECK(r.status == 503);
            for (auto& s : idle) s->abort();
            spin(300);
            CHECK(srv.connectionCount() == 0);
            r = roundTrip(port, get(path, "Range: bytes=0-0\r\n"));
            CHECK(r.status == 206);
        }

        // Revoke: the old path is 404 at once and the listener stays; a new serve mints a new token on the
        // same port, and only the new path works.
        srv.revoke();
        CHECK(srv.isListening());
        CHECK(!srv.isServing());
        CHECK(srv.url().isEmpty());
        r = roundTrip(port, get(path, "Range: bytes=0-99\r\n"));
        CHECK(r.status == 404);
        CHECK(srv.serve(fpath, QHostAddress(QHostAddress::LocalHost), &err));
        CHECK(srv.port() == port);
        const QByteArray path2 = srv.url().path(QUrl::FullyEncoded).toLatin1();
        CHECK(path2 != path);
        r = roundTrip(port, get(path, "Range: bytes=0-99\r\n"));
        CHECK(r.status == 404);
        r = roundTrip(port, get(path2, "Range: bytes=0-99\r\n"));
        CHECK(r.status == 206);
        CHECK(r.body == fixture.left(100));

        // Revoke with a body streaming: the transfer is cut, not finished. A file far bigger than any socket
        // buffer, so "not finished" cannot be the kernel having swallowed it all already.
        {
            const QString bigPath = dir + QStringLiteral("/stream.mkv");
            const qint64 bigN = 48LL * 1024 * 1024;
            { QFile f(bigPath); CHECK(f.open(QIODevice::WriteOnly)); const QByteArray mb = makeFixture(1024 * 1024);
              for (int i = 0; i < 48; ++i) f.write(mb); }
            CHECK(srv.serve(bigPath, QHostAddress(QHostAddress::LocalHost), &err));
            CHECK(srv.port() == port);
            const QByteArray pathBig = srv.url().path(QUrl::FullyEncoded).toLatin1();
            QTcpSocket c;
            c.setReadBufferSize(64 * 1024);   // read slowly: the server's writes back up behind this
            QByteArray got;
            QEventLoop loop;
            bool gone = false;
            QObject::connect(&c, &QTcpSocket::connected, &loop, [&] { c.write(get(pathBig)); });
            QObject::connect(&c, &QTcpSocket::readyRead, &loop, [&] { if (got.isEmpty()) { got += c.read(4096); loop.quit(); } });
            QObject::connect(&c, &QTcpSocket::disconnected, &loop, [&] { gone = true; loop.quit(); });
            QTimer::singleShot(5000, &loop, &QEventLoop::quit);
            c.connectToHost(QHostAddress(QHostAddress::LocalHost), port);
            loop.exec();
            CHECK(!got.isEmpty());
            CHECK(!gone);
            spin(200);
            CHECK(srv.connectionCount() == 1);
            srv.revoke();
            CHECK(srv.connectionCount() == 0);
            QEventLoop drain;
            qint64 total = got.size();
            QObject::connect(&c, &QTcpSocket::readyRead, &drain, [&] { total += c.readAll().size(); });
            QObject::connect(&c, &QTcpSocket::disconnected, &drain, [&] { gone = true; drain.quit(); });
            QObject::connect(&c, &QAbstractSocket::errorOccurred, &drain, [&](QAbstractSocket::SocketError) { gone = true; drain.quit(); });
            QTimer::singleShot(5000, &drain, &QEventLoop::quit);
            total += c.readAll().size();   // what sat in the client's full buffer; reading it re-arms the socket
            if (!gone && c.state() != QAbstractSocket::UnconnectedState) drain.exec();
            CHECK(gone || c.state() == QAbstractSocket::UnconnectedState);
            CHECK(total < bigN);
            std::printf("F: revoke mid-stream cut the body at %lld of %lld bytes\n", (long long)total, (long long)bigN);
        }

        // Stop: the port is closed.
        CHECK(srv.serve(fpath, QHostAddress(QHostAddress::LocalHost), &err));
        const quint16 port3 = srv.port();
        CHECK(portOpen(port3));
        QElapsedTimer st; st.start();
        srv.stop();
        CHECK(st.elapsed() < 200);
        CHECK(!srv.isListening());
        CHECK(!srv.isServing());
        CHECK(srv.url().isEmpty());
        CHECK(!portOpen(port3));
        srv.stop();   // idempotent
    }

    // ------------------------------------------------------------------------ G. CastManager + fake DLNA --
    {
        FakeRenderer fake;
        CHECK(fake.listen());
        CastManager cm;
        QStringList errors;
        int started = 0, stopped = 0;
        QObject::connect(&cm, &CastManager::castError, &cm, [&](const QString& m) { errors << m; });
        QObject::connect(&cm, &CastManager::castStarted, &cm, [&](const QString&) { ++started; });
        QObject::connect(&cm, &CastManager::castStopped, &cm, [&] { ++stopped; });

        CastDevice dev;
        dev.type = CastDevice::Dlna;
        dev.host = QStringLiteral("127.0.0.1");
        dev.id = QStringLiteral("dlna:127.0.0.1");
        dev.name = QStringLiteral("Fake Renderer");
        dev.controlUrl = QStringLiteral("http://127.0.0.1:%1/ctl").arg(fake.srv.serverPort());

        CHECK(cm.castLocalFile(dev, fpath, QStringLiteral("Cast Fixture")));
        CHECK(cm.isServingFile());
        CHECK(cm.servedFilePath() == QFileInfo(fpath).absoluteFilePath());
        for (int i = 0; i < 50 && (fake.bodyOf("SetAVTransportURI").isEmpty() || fake.bodyOf("#Play").isEmpty()); ++i)
            spin(100);
        const QString set = QString::fromUtf8(fake.bodyOf("SetAVTransportURI"));
        CHECK(!set.isEmpty());
        CHECK(!fake.bodyOf("#Play").isEmpty());
        const auto m = QRegularExpression(QStringLiteral("<CurrentURI>(.*?)</CurrentURI>")).match(set);
        CHECK(m.hasMatch());
        const QUrl handed(xmlUnescape(m.captured(1)));
        CHECK(handed.scheme() == QStringLiteral("http"));
        CHECK(handed.host() == QStringLiteral("127.0.0.1"));        // the interface that reaches the target
        CHECK(handed.path(QUrl::FullyEncoded).startsWith(QStringLiteral("/cast/")));
        CHECK(handed == cm.servedUrl());
        CHECK(set.contains(QStringLiteral("video/mp4")));           // the DIDL protocolInfo carries the type
        const quint16 hp = quint16(handed.port());
        Resp r = roundTrip(hp, get(handed.path(QUrl::FullyEncoded).toLatin1(), "Range: bytes=1000-1999\r\n"));
        CHECK(r.status == 206);
        CHECK(r.body == fixture.mid(1000, 1000));
        CHECK(errors.isEmpty());
        CHECK(started == 1);

        // Stopping the cast stops the server: the port is closed.
        cm.stopCasting();
        CHECK(!cm.isServingFile());
        CHECK(!portOpen(hp));
        CHECK(stopped == 1);

        // A public target is refused before anything listens, and says so.
        CastDevice pub = dev;
        pub.host = QStringLiteral("203.0.113.9");
        pub.id = QStringLiteral("dlna:203.0.113.9");
        errors.clear();
        CHECK(!cm.castLocalFile(pub, fpath, QStringLiteral("Cast Fixture")));
        CHECK(!cm.isServingFile());
        CHECK(errors.size() == 1);
        // A file that is not there is refused too.
        errors.clear();
        CHECK(!cm.castLocalFile(dev, dir + QStringLiteral("/missing.mp4"), QString()));
        CHECK(!cm.isServingFile());
        CHECK(errors.size() == 1);
        // Casting an http url after a local file stops serving the file.
        CHECK(cm.castLocalFile(dev, fpath, QString()));
        const quint16 hp2 = quint16(cm.servedUrl().port());
        CHECK(portOpen(hp2));
        cm.cast(dev, QStringLiteral("http://127.0.0.1:9/remote.mp4"), QString(), QString());
        CHECK(!cm.isServingFile());
        CHECK(!portOpen(hp2));
        cm.stopCasting();
    }

    // ------------------------------------------------------------------------ H. Quit mid-download --------
    {
        const QString big = dir + QStringLiteral("/big.mkv");
        { QFile f(big); CHECK(f.open(QIODevice::WriteOnly)); const QByteArray mb = makeFixture(1024 * 1024);
          for (int i = 0; i < 24; ++i) f.write(mb); }
        CastFileServer srv;
        CHECK(srv.serve(big, QHostAddress(QHostAddress::LocalHost)));
        const quint16 port = srv.port();
        const QByteArray path = srv.url().path(QUrl::FullyEncoded).toLatin1();

        QTcpSocket c;
        c.setReadBufferSize(64 * 1024);
        qint64 got = 0;
        bool firstByte = false;
        QObject::connect(&c, &QTcpSocket::connected, &c, [&] { c.write(get(path)); });
        QObject::connect(&c, &QTcpSocket::readyRead, &c, [&] {
            if (!firstByte) { firstByte = true; got += c.read(8192).size(); }   // then stop reading: back-pressure
        });
        c.connectToHost(QHostAddress(QHostAddress::LocalHost), port);

        QElapsedTimer quitClock;
        qint64 quitMs = -1;
        bool listeningAtQuit = false;
        QTimer::singleShot(1500, &app, [&] {
            listeningAtQuit = srv.isListening() && srv.connectionCount() == 1;
            quitClock.start();
            QCoreApplication::quit();
        });
        // The app's quit path: aboutToQuit is emitted as exec() returns. The server must already be down by
        // the time this handler (connected after the server's own) runs.
        bool downInAboutToQuit = false;
        QObject::connect(&app, &QCoreApplication::aboutToQuit, &app, [&] {
            downInAboutToQuit = !srv.isListening() && srv.connectionCount() == 0;
        });
        app.exec();
        quitMs = quitClock.isValid() ? quitClock.elapsed() : -1;
        CHECK(firstByte);
        CHECK(listeningAtQuit);          // the download really was in flight
        CHECK(downInAboutToQuit);
        CHECK(!srv.isListening());
        CHECK(srv.connectionCount() == 0);
        CHECK(quitMs >= 0 && quitMs < QuitBudget::kNetworkMs);
        // No event loop runs after exec(), so the client is read synchronously: lift its read limit, and the
        // server's abort reaches it as the end of the stream.
        c.setReadBufferSize(0);
        c.readAll();
        const bool clientSawEnd = c.waitForDisconnected(2000) || c.state() == QAbstractSocket::UnconnectedState;
        CHECK(clientSawEnd);
        std::printf("H: quit with a download in flight: server down in %lld ms (budget %d ms)\n",
                    (long long)quitMs, QuitBudget::kNetworkMs);
    }

    if (failures)
    {
        std::fprintf(stderr, "CASTSERVER: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("CASTSERVER-OK\n");
    return 0;
}
