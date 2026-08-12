// Network-free end-to-end coverage for ServerSyncBackend (self-hosted sync, Increment C). The transport is the
// REAL ServerSyncBackend; the "server" is an in-process QTcpServer on 127.0.0.1:0 implementing the versioned
// object store (GET list / GET blob / PUT with If-None-Match:* / DELETE), so nothing but loopback is touched.
//
// It asserts the six-primitive contract from the outside, the way CloudSync drives it:
//   * ensureFolder             -> the namespace id (the folder is implicit)
//   * uploadFile(new) then findFile -> the key is listed, with meta == the stateHash we sent (the CAS token)
//   * downloadFile             -> the exact bytes back
//   * uploadFile(existingId)   -> overwrites in place; findFile/download reflect the new meta + bytes
//   * uploadFile(new) on a key that exists -> If-None-Match:* is honoured (412), reported as failure ("")
//   * findFile(absent)         -> listOk=true, id="" (genuinely absent, NOT unreachable)
//   * a percent-encoded key    -> a space + a "/" round-trip upload -> findFile -> download intact
//   * a non-empty token        -> becomes the URL PATH PREFIX: requests land at /<token>/sync/<ns>/...
//   * a dead server            -> findFile listOk=false (unreachable is never "empty")
//
// Prints SERVERSYNC-OK on success; any failure prints SERVERSYNC-FAIL <what> and exits non-zero.
#include "ServerSyncBackend.h"
#include "AppPaths.h"
#include "AppBrand.h"

#include <QCoreApplication>
#include <QSettings>
#include <QTcpServer>
#include <QTcpSocket>
#include <QHostAddress>
#include <QEventLoop>
#include <QDeadlineTimer>
#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QMap>
#include <QUrl>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDateTime>
#include <cstdio>

static int failures = 0;
#define CHECK(cond, what)                                                        \
    do { if (!(cond)) { std::fprintf(stderr, "SERVERSYNC-FAIL %s\n", (what)); ++failures; } } while (0)

// ---- the in-process object store stub -------------------------------------------------------------------

class StubStore : public QObject
{
public:
    struct Obj { QByteArray data; QString meta; bool deleted = false; int version = 0; };

    QMap<QString, Obj> objs;
    QTcpServer server;
    QString nsPath = QStringLiteral("/sync/p1");   // the namespace path; a configured token prefixes this
    QString tokenPrefix;                            // e.g. "/tok123" once the token-prefix section configures one
    QStringList seenPaths;                          // every request path handled (for the token-prefix assertion)

    QString basePath() const { return tokenPrefix + nsPath; }   // "/<token>/sync/<ns>" (or just "/sync/<ns>")

    StubStore() { QObject::connect(&server, &QTcpServer::newConnection, &server, [this] { onConn(); }); }
    bool listen() { return server.listen(QHostAddress::LocalHost, 0); }
    quint16 port() const { return server.serverPort(); }
    void stop() { server.close(); }

private:
    void onConn()
    {
        while (QTcpSocket* s = server.nextPendingConnection())
        {
            auto* buf = new QByteArray();
            auto* handled = new bool(false);
            QObject::connect(s, &QTcpSocket::readyRead, s, [this, s, buf, handled] {
                if (*handled) return;
                buf->append(s->readAll());
                const int hdrEnd = buf->indexOf("\r\n\r\n");
                if (hdrEnd < 0) return;
                const QByteArray headers = buf->left(hdrEnd);
                const int contentLength = headerInt(headers, "content-length");
                if (buf->size() < hdrEnd + 4 + contentLength) return;   // body not fully arrived yet
                *handled = true;
                handle(s, headers, buf->mid(hdrEnd + 4, contentLength));
            });
            QObject::connect(s, &QTcpSocket::disconnected, s, [s, buf, handled] {
                delete buf; delete handled; s->deleteLater();
            });
        }
    }

    static int headerInt(const QByteArray& headers, const QByteArray& name)
    {
        for (const QByteArray& line : headers.split('\n')) {
            const int c = line.indexOf(':');
            if (c < 0) continue;
            if (line.left(c).trimmed().toLower() == name)
                return line.mid(c + 1).trimmed().toInt();
        }
        return 0;
    }
    static QByteArray headerVal(const QByteArray& headers, const QByteArray& name)
    {
        for (const QByteArray& line : headers.split('\n')) {
            const int c = line.indexOf(':');
            if (c < 0) continue;
            if (line.left(c).trimmed().toLower() == name)
                return line.mid(c + 1).trimmed();
        }
        return {};
    }

    void reply(QTcpSocket* s, int status, const QByteArray& reason,
               const QByteArray& body = {}, const QList<QByteArray>& extraHeaders = {})
    {
        QByteArray out = "HTTP/1.1 " + QByteArray::number(status) + " " + reason + "\r\n";
        out += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
        for (const QByteArray& h : extraHeaders) out += h + "\r\n";
        out += "Connection: close\r\n\r\n";
        out += body;
        s->write(out);
        s->flush();
        s->disconnectFromHost();
    }

    void handle(QTcpSocket* s, const QByteArray& headers, const QByteArray& body)
    {
        const QByteArray reqLine = headers.left(headers.indexOf('\n')).trimmed();
        const QList<QByteArray> parts = reqLine.split(' ');
        if (parts.size() < 2) { reply(s, 400, "Bad Request"); return; }
        const QByteArray method = parts.at(0);
        const QString path = QString::fromUtf8(parts.at(1));
        seenPaths << path;

        if (path == basePath() && method == "GET") { replyList(s); return; }

        const QString prefix = basePath() + QStringLiteral("/");
        if (!path.startsWith(prefix)) { reply(s, 404, "Not Found"); return; }
        const QString key = QUrl::fromPercentEncoding(path.mid(prefix.size()).toUtf8());

        if (method == "GET") {
            auto it = objs.constFind(key);
            if (it == objs.constEnd() || it.value().deleted) { reply(s, 404, "Not Found"); return; }
            reply(s, 200, "OK", it.value().data,
                  { "ETag: \"" + QByteArray::number(it.value().version) + "\"",
                    "X-Sync-Meta: " + it.value().meta.toUtf8() });
        } else if (method == "PUT") {
            const QByteArray inm = headerVal(headers, "if-none-match");
            auto it = objs.constFind(key);
            if (inm == "*" && it != objs.constEnd() && !it.value().deleted) { reply(s, 412, "Precondition Failed"); return; }
            Obj o = objs.value(key);
            o.data = body;
            o.meta = QString::fromUtf8(headerVal(headers, "x-sync-meta"));
            o.deleted = false;
            o.version += 1;
            objs.insert(key, o);
            reply(s, 204, "No Content", {}, { "ETag: \"" + QByteArray::number(o.version) + "\"" });
        } else if (method == "DELETE") {
            if (objs.contains(key)) { Obj o = objs.value(key); o.deleted = true; o.version += 1; objs.insert(key, o); }
            reply(s, 204, "No Content");
        } else {
            reply(s, 405, "Method Not Allowed");
        }
    }

    void replyList(QTcpSocket* s)
    {
        QJsonArray arr;
        for (auto it = objs.constBegin(); it != objs.constEnd(); ++it) {
            const Obj& o = it.value();
            arr.append(QJsonObject{
                { QStringLiteral("key"), it.key() },
                { QStringLiteral("version"), QString::number(o.version) },
                { QStringLiteral("meta"), o.meta },
                { QStringLiteral("size"), o.data.size() },
                { QStringLiteral("deleted"), o.deleted },
                { QStringLiteral("modifiedUtc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate) },
            });
        }
        const QByteArray b = QJsonDocument(QJsonObject{ { QStringLiteral("objects"), arr } }).toJson(QJsonDocument::Compact);
        reply(s, 200, "OK", b, { "Content-Type: application/json" });
    }
};

// Spin the event loop until `done`, with a hard deadline so a stuck primitive fails loudly rather than hanging.
static void pump(bool& done, int timeoutMs = 5000)
{
    QDeadlineTimer deadline(timeoutMs);
    while (!done && !deadline.hasExpired())
        QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents, 100);
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    StubStore stub;
    if (!stub.listen()) { std::fprintf(stderr, "SERVERSYNC-FAIL stub server did not listen\n"); return 1; }

    // Point the backend at the stub through its own config file — the same isolated ini AppPaths::dataDir()
    // resolves to under EB_ISOLATED_DATA_DIR. Written BEFORE the backend first reads it.
    {
        QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                    QSettings::IniFormat);
        s.setValue(QStringLiteral("cloud/backend"), QStringLiteral("server"));
        s.setValue(QStringLiteral("cloud/server/url"),
                   QStringLiteral("http://127.0.0.1:%1").arg(stub.port()));
        s.setValue(QStringLiteral("cloud/server/token"), QString());
        s.setValue(QStringLiteral("cloud/server/namespace"), QStringLiteral("p1"));
        s.sync();
    }

    ServerSyncBackend backend;

    // ---- auth surface: a configured URL reads as signed in, the URL stands in for the account ----
    CHECK(backend.isSignedIn(), "a configured server URL reads as signed in");
    CHECK(backend.accountEmail().startsWith(QStringLiteral("http://127.0.0.1:")),
          "accountEmail reports the server URL");

    // ---- ensureFolder -> the namespace id ----
    {
        bool done = false; QString folder;
        backend.ensureFolder([&](const QString& id) { folder = id; done = true; });
        pump(done);
        CHECK(done && folder == QStringLiteral("p1"), "ensureFolder returns the namespace id");
    }

    const QString KEY = QStringLiteral("state.bin");
    const QByteArray V1 = "SERVER-STATE-BYTES-V1";
    const QByteArray V2 = "SERVER-STATE-BYTES-VERSION-2-LONGER";
    const QString META1 = QStringLiteral("hash-v1");
    const QString META2 = QStringLiteral("hash-v2");

    // ---- uploadFile(new) -> the key back as its id ----
    {
        bool done = false; QString id;
        backend.uploadFile(QStringLiteral("p1"), QString(), KEY, QStringLiteral("application/octet-stream"),
                           V1, META1, [&](const QString& i) { id = i; done = true; });
        pump(done);
        CHECK(done && id == KEY, "a new upload returns the key as its id");
    }

    // ---- findFile -> listOk, id, and meta == the stateHash we sent ----
    {
        bool done = false; bool listOk = false; QString id, modIso, meta;
        backend.findFile(QStringLiteral("p1"), KEY, [&](bool ok, const QString& i, const QString& m, const QString& h) {
            listOk = ok; id = i; modIso = m; meta = h; done = true;
        });
        pump(done);
        CHECK(done && listOk && id == KEY, "findFile lists the uploaded key");
        CHECK(meta == META1, "findFile echoes the stateHash back as meta (the CAS token round-trips)");
        CHECK(!modIso.isEmpty(), "findFile reports a modifiedUtc");
    }

    // ---- downloadFile -> the exact bytes ----
    {
        bool done = false; bool ok = false; QByteArray got;
        backend.downloadFile(KEY, [&](bool o, const QByteArray& d) { ok = o; got = d; done = true; });
        pump(done);
        CHECK(done && ok && got == V1, "downloadFile returns the exact bytes uploaded");
    }

    // ---- uploadFile(existingId) -> overwrites in place ----
    {
        bool done = false; QString id;
        backend.uploadFile(QStringLiteral("p1"), KEY, KEY, QStringLiteral("application/octet-stream"),
                           V2, META2, [&](const QString& i) { id = i; done = true; });
        pump(done);
        CHECK(done && id == KEY, "an update with a non-empty existingId succeeds");

        bool d2 = false; QString meta; bool ok2 = false; QByteArray got;
        backend.findFile(QStringLiteral("p1"), KEY, [&](bool, const QString&, const QString&, const QString& h) {
            meta = h; ok2 = true; d2 = true;
        });
        pump(d2);
        CHECK(ok2 && meta == META2, "…and the new stateHash is what the list now reports");
        bool d3 = false; bool ok3 = false;
        backend.downloadFile(KEY, [&](bool o, const QByteArray& dd) { ok3 = o; got = dd; d3 = true; });
        pump(d3);
        CHECK(ok3 && got == V2, "…and the new bytes are what the download returns");
    }

    // ---- create-only conflict: a NEW upload onto an existing key is rejected (If-None-Match:* -> 412 -> "") ----
    {
        bool done = false; QString id = QStringLiteral("sentinel");
        backend.uploadFile(QStringLiteral("p1"), QString(), KEY, QStringLiteral("application/octet-stream"),
                           QByteArray("SHOULD-NOT-LAND"), QStringLiteral("hash-x"),
                           [&](const QString& i) { id = i; done = true; });
        pump(done);
        CHECK(done && id.isEmpty(), "a create-only upload onto an existing key is a 412, reported as failure");

        bool d2 = false; QByteArray got;
        backend.downloadFile(KEY, [&](bool, const QByteArray& dd) { got = dd; d2 = true; });
        pump(d2);
        CHECK(got == V2, "…and the existing object is untouched by the rejected create");
    }

    // ---- findFile(absent) -> listOk=true, id="" (genuinely absent, not unreachable) ----
    {
        bool done = false; bool listOk = false; QString id = QStringLiteral("sentinel");
        backend.findFile(QStringLiteral("p1"), QStringLiteral("nope.bin"),
                         [&](bool ok, const QString& i, const QString&, const QString&) { listOk = ok; id = i; done = true; });
        pump(done);
        CHECK(done && listOk && id.isEmpty(), "an absent key is listOk=true with an empty id");
    }

    // ---- a key needing percent-encoding (a space and a '/') round-trips upload -> findFile -> download ----
    {
        const QString NASTY = QStringLiteral("sub dir/save 1.bin");   // both chars MUST be percent-encoded on the wire
        const QByteArray NV = "NASTY-KEY-BYTES";
        const QString NMETA = QStringLiteral("hash-nasty");

        bool done = false; QString id;
        backend.uploadFile(QStringLiteral("p1"), QString(), NASTY, QStringLiteral("application/octet-stream"),
                           NV, NMETA, [&](const QString& i) { id = i; done = true; });
        pump(done);
        CHECK(done && id == NASTY, "a key with a space and a '/' uploads (percent-encoded on the wire)");

        bool d2 = false; bool listOk = false; QString fid, meta;
        backend.findFile(QStringLiteral("p1"), NASTY, [&](bool ok, const QString& i, const QString&, const QString& h) {
            listOk = ok; fid = i; meta = h; d2 = true;
        });
        pump(d2);
        CHECK(d2 && listOk && fid == NASTY && meta == NMETA, "findFile lists the percent-encoded key intact");

        bool d3 = false; bool ok3 = false; QByteArray got;
        backend.downloadFile(NASTY, [&](bool o, const QByteArray& dd) { ok3 = o; got = dd; d3 = true; });
        pump(d3);
        CHECK(d3 && ok3 && got == NV, "downloadFile returns the exact bytes for the percent-encoded key");
    }

    // ---- a NON-EMPTY token is a URL PATH PREFIX: requests land at /<token>/sync/<ns>/... (never a header) ----
    {
        stub.tokenPrefix = QStringLiteral("/tok123");   // teach the stub to expect the prefixed path
        stub.seenPaths.clear();
        {
            // Reconfigure the token in the shared ini; the backend reads it lazily on the next call.
            QSettings s(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                        QSettings::IniFormat);
            s.setValue(QStringLiteral("cloud/server/token"), QStringLiteral("tok123"));
            s.sync();
        }

        const QString TKEY = QStringLiteral("token-scoped.bin");
        const QByteArray TV = "TOKEN-SCOPED-BYTES";
        bool done = false; QString id;
        backend.uploadFile(QStringLiteral("p1"), QString(), TKEY, QStringLiteral("application/octet-stream"),
                           TV, QStringLiteral("hash-tok"), [&](const QString& i) { id = i; done = true; });
        pump(done);
        CHECK(done && id == TKEY, "an upload with a token configured succeeds through the token path prefix");

        bool d2 = false; bool listOk = false; QString fid;
        backend.findFile(QStringLiteral("p1"), TKEY, [&](bool ok, const QString& i, const QString&, const QString&) {
            listOk = ok; fid = i; d2 = true;
        });
        pump(d2);
        CHECK(d2 && listOk && fid == TKEY, "findFile works through the token path prefix");

        bool sawPrefixed = false;
        for (const QString& p : stub.seenPaths)
            if (p.startsWith(QStringLiteral("/tok123/sync/p1"))) { sawPrefixed = true; break; }
        CHECK(sawPrefixed, "the stub received requests under the /<token>/sync/<ns> path prefix");
    }

    // ---- a dead server -> findFile listOk=false (unreachable is never 'empty') ----
    {
        stub.stop();   // same URL, no listener: connections are now refused
        bool done = false; bool listOk = true; QString id = QStringLiteral("sentinel");
        backend.findFile(QStringLiteral("p1"), KEY,
                         [&](bool ok, const QString& i, const QString&, const QString&) { listOk = ok; id = i; done = true; });
        pump(done);
        CHECK(done && !listOk, "a connection error surfaces as listOk=false, not as an empty namespace");
    }

    if (failures) { std::fprintf(stderr, "SERVERSYNC-FAIL %d check(s) failed\n", failures); return 1; }
    std::printf("SERVERSYNC-OK\n");
    return 0;
}
