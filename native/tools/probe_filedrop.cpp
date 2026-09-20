// Headless check of the LAN file drop (issue #115): src/core/FileDrop.{h,cpp}, and the /drop routes RemoteServer
// serves through it, on a real loopback socket.
//
// What this pins:
//   1. destinations -- ids are opaque (no path, key or label in them) and stable; the JSON carries no path; an
//      unknown id is refused at start and nothing is created;
//   2. names -- the full refusal table, and that the rule IS LibraryBundle::safePathSegment (#292's), not a copy;
//   3. start -- a bad name, an existing target, over the 64 GiB cap, over the free space (with and without
//      another upload pending) are refused with no part file; an identical triple gets the same id and its
//      received count back, in the same registry and in a fresh one (an app restart);
//   4. pieces -- the offset must equal what was received: a gap, an overlap, a piece past the end, an empty or
//      oversize piece, an unknown id and a second piece in flight are all refused before a byte; a write past
//      the piece's declared length is refused; a dropped piece keeps what arrived;
//   5. finish -- incomplete refused; fsync + no-replace rename; a target that appeared meanwhile is refused
//      and the part file KEPT; a retry under another name lands without re-sending;
//   6. sweep -- only ".eb-drop-<32 hex>.part" names, only 24 h or older, never one in flight;
//   7. nothing is written outside the destination directory (a census of the whole scratch tree, the way #292
//      asserts it for the ROM root);
//   8. routeNeedsToken covers every /drop route except GET /drop's page;
//   9. the embedded page is byte-identical to resources/filedrop/drop.html and loads nothing external;
//  10. a real socket: GET /drop serves only that page; no token -> 401 with no part file; hostile names are
//      refused; an existing name is refused; a 200 MiB upload in 8 MiB pieces lands byte-exact (SHA-256); a
//      connection killed mid-piece is resumed from /drop/status and finishes byte-exact; offsets, oversize
//      pieces, a listener up for file drop alone, and file drop off.
//
// Prints FILEDROP-OK on success; any failure prints FILEDROP-FAIL <cond> (line) and exits non-zero.
#include "FileDrop.h"
#include "LibraryBundle.h"
#include "PlayOnDevice.h"
#include "RemoteApi.h"
#include "RemoteServer.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>

#include <cstdio>
#include <functional>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "FILEDROP-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

namespace
{
    const qint64 kTiB = 1024LL * 1024 * 1024 * 1024;
    const qint64 kGiB = 1024LL * 1024 * 1024;
    const qint64 kMiB = 1024LL * 1024;

    // Everything under `dir` -- files AND directories, hidden ones included -- as relative path -> sha256 (a
    // directory maps to "<dir>"), so a created folder, a left-behind part file and a changed byte all show.
    QMap<QString, QString> census(const QString& dir)
    {
        QMap<QString, QString> out;
        QDirIterator it(dir, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                        QDirIterator::Subdirectories);
        while (it.hasNext())
        {
            const QString p = it.next();
            const QFileInfo fi(p);
            const QString rel = QDir(dir).relativeFilePath(p);
            if (fi.isDir()) { out.insert(rel, QStringLiteral("<dir>")); continue; }
            QFile f(p);
            if (!f.open(QIODevice::ReadOnly)) { out.insert(rel, QStringLiteral("<unreadable>")); continue; }
            QCryptographicHash h(QCryptographicHash::Sha256);
            h.addData(&f);
            out.insert(rel, QString::fromLatin1(h.result().toHex()));
        }
        return out;
    }

    // Every entry that is new or changed from `before` to `after`, and every entry removed.
    QStringList changed(const QMap<QString, QString>& before, const QMap<QString, QString>& after)
    {
        QStringList out;
        for (auto it = after.cbegin(); it != after.cend(); ++it)
            if (!before.contains(it.key()) || before.value(it.key()) != it.value()) out << it.key();
        for (auto it = before.cbegin(); it != before.cend(); ++it)
            if (!after.contains(it.key())) out << it.key();
        return out;
    }

    bool writeFile(const QString& path, const QByteArray& bytes)
    {
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
        return f.write(bytes) == bytes.size();
    }

    QByteArray readAll(const QString& path)
    {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) return QByteArray();
        return f.readAll();
    }

    bool setMtime(const QString& path, const QDateTime& t)
    {
        QFile f(path);
        if (!f.open(QIODevice::ReadWrite)) return false;
        return f.setFileTime(t, QFileDevice::FileModificationTime);
    }

    // Deterministic, incompressible-looking bytes for piece `index` of a generated file (xorshift).
    QByteArray pieceBytes(quint64 seed, qint64 index, qint64 n)
    {
        QByteArray b(int(n), Qt::Uninitialized);
        quint64 x = seed ^ (quint64(index + 1) * 0x9E3779B97F4A7C15ULL);
        char* p = b.data();
        for (qint64 i = 0; i < n; i += 8)
        {
            x ^= x << 13; x ^= x >> 7; x ^= x << 17;
            for (int k = 0; k < 8 && i + k < n; ++k) p[i + k] = char((x >> (8 * k)) & 0xff);
        }
        return b;
    }

    QByteArray fileSha256(const QString& path)
    {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) return QByteArray();
        QCryptographicHash h(QCryptographicHash::Sha256);
        h.addData(&f);
        return h.result().toHex();
    }

    static bool spinUntil(const std::function<bool()>& pred, int ms)
    {
        QElapsedTimer t;
        t.start();
        while (!pred() && t.elapsed() < ms)
        {
            QEventLoop loop;
            QTimer::singleShot(5, &loop, &QEventLoop::quit);
            loop.exec();
        }
        return pred();
    }

    struct HttpResult { int status = 0; QByteArray head; QByteArray body; QJsonObject json; };

    // One request over a real loopback socket; server and client share this thread, so every wait spins the
    // event loop. `sendBodyBytes` < 0 sends the whole body; `abortAfter` drops the connection once that much
    // has been flushed, without waiting for an answer.
    HttpResult httpRequest(quint16 port, const QByteArray& head, const QByteArray& body = QByteArray(),
                           qint64 sendBodyBytes = -1, bool abortAfter = false)
    {
        HttpResult r;
        QTcpSocket c;
        c.connectToHost(QHostAddress(QHostAddress::LocalHost), port);
        if (!spinUntil([&] { return c.state() == QAbstractSocket::ConnectedState; }, 5000)) return r;
        QByteArray response;
        QObject::connect(&c, &QTcpSocket::readyRead, [&] { response += c.readAll(); });
        c.write(head);
        const qint64 n = sendBodyBytes < 0 ? qint64(body.size()) : sendBodyBytes;
        const qint64 chunk = kMiB;
        for (qint64 at = 0; at < n && c.state() == QAbstractSocket::ConnectedState; at += chunk)
        {
            c.write(body.constData() + at, qMin(chunk, n - at));
            spinUntil([&] { return c.bytesToWrite() < 4 * chunk || c.state() != QAbstractSocket::ConnectedState; }, 20000);
        }
        spinUntil([&] { return c.bytesToWrite() == 0 || c.state() != QAbstractSocket::ConnectedState; }, 20000);
        if (abortAfter)
        {
            // Let the server take what was flushed before the line is cut.
            spinUntil([] { return false; }, 150);
            c.abort();
            return r;
        }
        spinUntil([&] { return c.state() == QAbstractSocket::UnconnectedState; }, 30000);
        response += c.readAll();
        if (response.startsWith("HTTP/1.1 ")) r.status = response.mid(9, 3).toInt();
        const int sep = response.indexOf("\r\n\r\n");
        if (sep >= 0) { r.head = response.left(sep); r.body = response.mid(sep + 4); }
        r.json = QJsonDocument::fromJson(r.body).object();
        return r;
    }

    QByteArray jsonBody(const QJsonObject& o) { return QJsonDocument(o).toJson(QJsonDocument::Compact); }

    QByteArray post(const QString& path, const QByteArray& body, const QString& token)
    {
        QByteArray h = "POST " + path.toUtf8() + " HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\n";
        if (!token.isEmpty()) h += "X-EB-Token: " + token.toUtf8() + "\r\n";
        h += "Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n";
        return h + body;
    }

    QByteArray get(const QString& target, const QString& token)
    {
        QByteArray h = "GET " + target.toUtf8() + " HTTP/1.1\r\nHost: 127.0.0.1\r\n";
        if (!token.isEmpty()) h += "X-EB-Token: " + token.toUtf8() + "\r\n";
        return h + "\r\n";
    }

    QByteArray putHead(const QString& id, qint64 offset, qint64 length, const QString& token)
    {
        QByteArray h = "PUT /drop/chunk?id=" + id.toUtf8() + "&offset=" + QByteArray::number(offset)
                       + " HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/octet-stream\r\n";
        if (!token.isEmpty()) h += "X-EB-Token: " + token.toUtf8() + "\r\n";
        h += "Content-Length: " + QByteArray::number(length) + "\r\n\r\n";
        return h;
    }

    QStringList partFilesIn(const QString& dir)
    {
        return QDir(dir).entryList({ QStringLiteral(".eb-drop-*") }, QDir::Files | QDir::Hidden | QDir::System);
    }
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    using namespace FileDrop;

    QTemporaryDir scratch;
    CHECK(scratch.isValid());
    const QString root = QDir::cleanPath(scratch.path());
    const QString roms = root + QStringLiteral("/roms");
    const QString snes = roms + QStringLiteral("/snes");
    const QString nes  = roms + QStringLiteral("/nes");
    const QString videos = root + QStringLiteral("/library");
    const QString outside = root + QStringLiteral("/state");   // a sibling that must never change
    for (const QString& d : { snes, nes, videos, outside }) QDir().mkpath(d);
    CHECK(writeFile(outside + QStringLiteral("/marks.json"), "{\"keep\":true}"));
    CHECK(writeFile(roms + QStringLiteral("/README.txt"), "roms"));

    const QList<Root> roots = {
        { QStringLiteral("rom"), QStringLiteral("snes"), QStringLiteral("Super Nintendo"), snes },
        { QStringLiteral("rom"), QStringLiteral("nes"), QStringLiteral("Nintendo Entertainment System"), nes },
        { QStringLiteral("rom"), QStringLiteral("n64"), QStringLiteral("Nintendo 64"), roms + QStringLiteral("/n64") },  // missing
        { QStringLiteral("video"), QStringLiteral("video"), QStringLiteral("Videos"), videos },
        { QStringLiteral("music"), QStringLiteral("music"), QStringLiteral("Music"), QString() },                          // unset
    };

    // ---- 1. destinations ---------------------------------------------------------------------------------------
    const QList<Destination> dests = buildDestinations(roots);
    CHECK(dests.size() == 3);
    const Destination* dSnes = nullptr;
    const Destination* dVideo = nullptr;
    for (const Destination& d : dests)
    {
        CHECK(validUploadId(d.id));                                   // 32 hex digits: opaque
        CHECK(!d.id.contains(QStringLiteral("snes")) && !d.id.contains(QStringLiteral("/")));
        if (d.key == QLatin1String("snes")) dSnes = &d;
        if (d.kind == QLatin1String("video")) dVideo = &d;
    }
    CHECK(dSnes && dVideo);
    if (!dSnes || !dVideo) { std::fprintf(stderr, "FILEDROP had %d failure(s)\n", failures + 1); return 1; }
    CHECK(dSnes->dir == snes);
    CHECK(dSnes->label == QLatin1String("Super Nintendo"));
    {
        // Stable: built again, the same ids. Distinct: every destination its own.
        const QList<Destination> again = buildDestinations(roots);
        CHECK(again.size() == dests.size());
        for (int i = 0; i < qMin(again.size(), dests.size()); ++i) CHECK(again[i].id == dests[i].id);
        CHECK(dests[0].id != dests[1].id && dests[1].id != dests[2].id && dests[0].id != dests[2].id);
        CHECK(destinationId(QStringLiteral("rom"), QStringLiteral("snes"), snes) == dSnes->id);
        CHECK(destinationId(QStringLiteral("rom"), QStringLiteral("snes"), nes) != dSnes->id);
        // The JSON names ids, labels and kinds -- never a directory.
        const QByteArray j = destinationsJson(dests);
        CHECK(!j.contains(root.toUtf8()));
        CHECK(!j.contains("dir"));
        CHECK(!j.contains("roms/"));
        const QJsonArray arr = QJsonDocument::fromJson(j).object().value(QStringLiteral("destinations")).toArray();
        CHECK(arr.size() == 3);
        CHECK(arr.at(0).toObject().value(QStringLiteral("id")).toString() == dests[0].id);
        CHECK(arr.at(0).toObject().value(QStringLiteral("label")).toString() == QLatin1String("Super Nintendo"));
        CHECK(arr.at(0).toObject().keys().size() == 3);
        CHECK(findDestination(dests, dSnes->id) == dSnes);
        CHECK(findDestination(dests, QString()) == nullptr);
        CHECK(findDestination(dests, dSnes->id.toUpper()) == nullptr);
        CHECK(findDestination(dests, snes) == nullptr);                // a path is not an id
        CHECK(findDestination(dests, QStringLiteral("snes")) == nullptr);
    }

    auto plenty = [](const QString&) { return kTiB; };

    // ---- 2. names: the shared rule, and its refusal table ----------------------------------------------------
    {
        const QStringList refused = {
            QString(), QStringLiteral("."), QStringLiteral(".."), QStringLiteral("../evil.sfc"),
            QStringLiteral("..\\evil.sfc"), QStringLiteral("a/b.sfc"), QStringLiteral("a\\b.sfc"),
            QStringLiteral("/etc/passwd"), QStringLiteral("C:\\Windows\\evil.sfc"), QStringLiteral("C:evil.sfc"),
            QStringLiteral("C:"), QStringLiteral("\\\\host\\share\\evil.sfc"), QStringLiteral("//host/share/evil.sfc"),
            QStringLiteral("con"), QStringLiteral("CON.sfc"), QStringLiteral("nul"), QStringLiteral("aux.txt"),
            QStringLiteral("lpt1.gba"), QStringLiteral("COM9.zip"), QStringLiteral(".hidden.sfc"),
            QStringLiteral(".eb-drop-0123456789abcdef0123456789abcdef.part"), QStringLiteral("game.sfc."),
            QStringLiteral("game.sfc "), QStringLiteral("a") + QChar(0x01) + QStringLiteral("b.sfc"),
            QStringLiteral("tab\there.sfc"), QStringLiteral("a*b.sfc"), QStringLiteral("a?b.sfc"),
            QStringLiteral("a|b.sfc"), QStringLiteral("a<b.sfc"), QStringLiteral("a>b.sfc"),
            QStringLiteral("a\"b.sfc"), QStringLiteral("file.sfc:stream"), QString(300, QLatin1Char('x')),
        };
        const QStringList accepted = {
            QStringLiteral("Super Mario World (USA).sfc"), QStringLiteral("Zelda & Link's, Adventure [!].nes"),
            QString::fromUtf8("Pok\xC3\xA9mon - Edici\xC3\xB3n Roja (Spain).gb"),
            QString::fromUtf8("\xE3\x83\x9D\xE3\x82\xB1\xE3\x83\xA2\xE3\x83\xB3.gb"), QStringLiteral("x"),
            QStringLiteral("Movie (2020).mkv"), QString(255, QLatin1Char('y')),
        };
        for (const QString& n : refused)
        {
            CHECK(!safeName(n));
            CHECK(safeName(n) == LibraryBundle::safePathSegment(n));   // reused, not copied
        }
        for (const QString& n : accepted)
        {
            CHECK(safeName(n));
            CHECK(safeName(n) == LibraryBundle::safePathSegment(n));
        }

        // 7 (part 1). Every refused name at start creates nothing anywhere under the scratch root.
        Uploads up(plenty);
        const QMap<QString, QString> before = census(root);
        for (const QString& n : refused)
        {
            const Answer a = up.start(dests, dSnes->id, n, 10);
            CHECK(!a.ok());
            CHECK(a.code == QLatin1String("badname"));
            CHECK(a.http == 400);
            CHECK(a.uploadId.isEmpty());
        }
        CHECK(up.inProgress() == 0);
        CHECK(census(root) == before);
    }

    // ---- 3. start ----------------------------------------------------------------------------------------------
    {
        const QMap<QString, QString> before = census(root);
        Uploads up(plenty);
        // An unknown destination id, a path, a key: refused, nothing created.
        for (const QString& bad : { QStringLiteral("0123456789abcdef0123456789abcdef"), snes, QStringLiteral("snes"),
                                    QString(), QStringLiteral("../roms/snes") })
        {
            const Answer a = up.start(dests, bad, QStringLiteral("A.sfc"), 10);
            CHECK(a.code == QLatin1String("nodest"));
            CHECK(a.http == 400);
        }
        CHECK(census(root) == before);

        // A good start: an id, nothing received, a hidden part file IN the destination directory.
        const Answer a = up.start(dests, dSnes->id, QStringLiteral("Alpha (USA).sfc"), 100);
        CHECK(a.ok());
        CHECK(a.http == 200);
        CHECK(validUploadId(a.uploadId));
        CHECK(a.received == 0);
        CHECK(a.size == 100);
        CHECK(a.uploadId == uploadIdFor(dSnes->id, QStringLiteral("Alpha (USA).sfc"), 100));
        CHECK(QFileInfo(snes + QLatin1Char('/') + partFileName(a.uploadId)).isFile());
        CHECK(up.partPathFor(a.uploadId) == snes + QLatin1Char('/') + partFileName(a.uploadId));
        CHECK(isPartFileName(partFileName(a.uploadId)));
        CHECK(!QFileInfo::exists(snes + QStringLiteral("/Alpha (USA).sfc")));   // never visible under its name
        CHECK(!answerJson(a).contains(root.toUtf8()));                           // no path in any answer

        // The same triple again: the same id, the bytes received so far.
        CHECK(up.beginChunk(a.uploadId, 0, 40).ok());
        const QByteArray first40 = pieceBytes(1, 0, 40);
        CHECK(up.writeChunk(a.uploadId, first40.constData(), 40));
        CHECK(up.endChunk(a.uploadId).received == 40);
        const Answer again = up.start(dests, dSnes->id, QStringLiteral("Alpha (USA).sfc"), 100);
        CHECK(again.ok());
        CHECK(again.uploadId == a.uploadId);
        CHECK(again.received == 40);
        CHECK(up.inProgress() == 1);
        CHECK(partFilesIn(snes).size() == 1);
        // A different size is a different upload.
        const Answer other = up.start(dests, dSnes->id, QStringLiteral("Alpha (USA).sfc"), 101);
        CHECK(other.ok() && other.uploadId != a.uploadId);

        // An app restart forgets the registry, not the disk.
        Uploads fresh(plenty);
        const Answer adopted = fresh.start(dests, dSnes->id, QStringLiteral("Alpha (USA).sfc"), 100);
        CHECK(adopted.ok());
        CHECK(adopted.uploadId == a.uploadId);
        CHECK(adopted.received == 40);
        CHECK(fresh.status(a.uploadId).received == 40);

        // An existing name is "already there", and no part file is made for it.
        CHECK(writeFile(snes + QStringLiteral("/Owned.sfc"), "USER-ROM"));
        const int partsBefore = partFilesIn(snes).size();
        const Answer ex = up.start(dests, dSnes->id, QStringLiteral("Owned.sfc"), 10);
        CHECK(ex.code == QLatin1String("exists"));
        CHECK(ex.http == 409);
        CHECK(partFilesIn(snes).size() == partsBefore);
        CHECK(readAll(snes + QStringLiteral("/Owned.sfc")) == "USER-ROM");
#ifdef Q_OS_WIN
        // NTFS names are case-insensitive: another spelling of the same name is the same file.
        CHECK(up.start(dests, dSnes->id, QStringLiteral("OWNED.SFC"), 10).code == QLatin1String("exists"));
#endif

        // The size cap: 64 GiB exactly is allowed through to the space check; one byte more is not.
        const Answer big = up.start(dests, dSnes->id, QStringLiteral("Big.iso"), kMaxFileBytes + 1);
        CHECK(big.code == QLatin1String("toolarge"));
        CHECK(big.http == 413);
        CHECK(up.start(dests, dSnes->id, QStringLiteral("Neg.iso"), -1).code == QLatin1String("badrequest"));
        {
            const QString capName = QStringLiteral("Cap.iso");
            const Answer cap = up.start(dests, dSnes->id, capName, kMaxFileBytes);
            CHECK(cap.ok());   // a TiB free
            CHECK(QFileInfo(snes + QLatin1Char('/') + partFileName(cap.uploadId)).size() == 0);   // reserved, not allocated
        }
    }
    {
        // Free space: refuse when the declared size would leave under 1 GiB free.
        QTemporaryDir d2;
        const QString dir = d2.path() + QStringLiteral("/gba");
        QDir().mkpath(dir);
        const QList<Destination> ds = buildDestinations({ { QStringLiteral("rom"), QStringLiteral("gba"), QStringLiteral("GBA"), dir } });
        const qint64 free = 5 * kGiB;
        Uploads up([free](const QString&) { return free; });
        const Answer over = up.start(ds, ds[0].id, QStringLiteral("Over.gba"), free - kFreeSpaceReserve + 1);
        CHECK(over.code == QLatin1String("nospace"));
        CHECK(over.http == 507);
        CHECK(partFilesIn(dir).isEmpty());
        const Answer fits = up.start(ds, ds[0].id, QStringLiteral("Fits.gba"), free - kFreeSpaceReserve);
        CHECK(fits.ok());
        // With that upload pending, a second one must fit in what is left after it.
        const Answer second = up.start(ds, ds[0].id, QStringLiteral("Second.gba"), 1);
        CHECK(second.code == QLatin1String("nospace"));
        // Unknown free space is no free space.
        Uploads blind([](const QString&) { return qint64(-1); });
        CHECK(blind.start(ds, ds[0].id, QStringLiteral("Any.gba"), 1).code == QLatin1String("nospace"));
        // At most kMaxUploads in progress.
        Uploads many(plenty);
        int okCount = 0;
        for (int i = 0; i < kMaxUploads; ++i)
            okCount += many.start(ds, ds[0].id, QStringLiteral("m%1.gba").arg(i), 1).ok() ? 1 : 0;
        CHECK(okCount == kMaxUploads);
        const Answer tooMany = many.start(ds, ds[0].id, QStringLiteral("one-more.gba"), 1);
        CHECK(tooMany.code == QLatin1String("toomany"));
        CHECK(many.start(ds, ds[0].id, QStringLiteral("m0.gba"), 1).ok());   // an existing one is still found
    }

    // ---- 4. pieces: the offset rule ------------------------------------------------------------------------------
    {
        QTemporaryDir d4;
        const QString dir = d4.path() + QStringLiteral("/gb");
        QDir().mkpath(dir);
        const QList<Destination> ds = buildDestinations({ { QStringLiteral("rom"), QStringLiteral("gb"), QStringLiteral("GB"), dir } });
        Uploads up(plenty);
        const qint64 size = 3 * 1000;
        const QByteArray whole = pieceBytes(4, 0, size);
        const Answer s = up.start(ds, ds[0].id, QStringLiteral("Piece.gb"), size);
        CHECK(s.ok());
        const QString id = s.uploadId;

        CHECK(up.beginChunk(QStringLiteral("0123456789abcdef0123456789abcdef"), 0, 10).code == QLatin1String("unknown"));
        CHECK(up.beginChunk(QStringLiteral("0123456789abcdef0123456789abcdef"), 0, 10).http == 404);
        const Answer gap = up.beginChunk(id, 10, 10);                // a gap
        CHECK(gap.code == QLatin1String("offset"));
        CHECK(gap.http == 409);
        CHECK(gap.received == 0);
        CHECK(up.beginChunk(id, 0, 0).code == QLatin1String("badrequest"));
        CHECK(up.beginChunk(id, 0, kMaxChunkBytes + 1).code == QLatin1String("toolarge"));
        CHECK(up.beginChunk(id, 0, size + 1).code == QLatin1String("badrequest"));   // past the end
        CHECK(up.busyIds().isEmpty());

        CHECK(up.beginChunk(id, 0, 1000).ok());
        CHECK(up.beginChunk(id, 0, 1000).code == QLatin1String("busy"));            // one piece at a time
        CHECK(up.busyIds().contains(id));
        CHECK(up.writeChunk(id, whole.constData(), 600));
        CHECK(!up.writeChunk(id, whole.constData() + 600, 401));                     // past the piece's length
        CHECK(up.writeChunk(id, whole.constData() + 600, 400));
        const Answer e1 = up.endChunk(id);
        CHECK(e1.ok() && e1.received == 1000);

        const Answer overlap = up.beginChunk(id, 500, 500);          // an overlap
        CHECK(overlap.code == QLatin1String("offset"));
        CHECK(overlap.received == 1000);
        CHECK(up.beginChunk(id, 1001, 10).code == QLatin1String("offset"));
        CHECK(up.beginChunk(id, 1000, 2001).code == QLatin1String("badrequest"));  // runs past the declared size

        // A piece cut off half way keeps what arrived, and the next piece continues from there.
        CHECK(up.beginChunk(id, 1000, 1000).ok());
        CHECK(up.writeChunk(id, whole.constData() + 1000, 300));
        up.abortChunk(id);
        CHECK(up.busyIds().isEmpty());
        CHECK(up.status(id).received == 1300);
        CHECK(up.beginChunk(id, 1000, 1000).code == QLatin1String("offset"));
        CHECK(up.beginChunk(id, 1300, 1700).ok());
        CHECK(up.writeChunk(id, whole.constData() + 1300, 1700));
        CHECK(up.endChunk(id).received == size);
        CHECK(up.beginChunk(id, size, 1).code == QLatin1String("badrequest"));      // nothing past the end

        // ---- 5. finish ----
        const QString part = up.partPathFor(id);
        const Answer done = up.finish(id);
        CHECK(done.ok());
        CHECK(done.landed);
        CHECK(done.landedPath == dir + QStringLiteral("/Piece.gb"));
        CHECK(readAll(dir + QStringLiteral("/Piece.gb")) == whole);
        CHECK(!QFileInfo::exists(part));
        CHECK(partFilesIn(dir).isEmpty());
        CHECK(up.status(id).code == QLatin1String("unknown"));
        CHECK(!answerJson(done).contains(dir.toUtf8()));

        // Incomplete is refused.
        const Answer s2 = up.start(ds, ds[0].id, QStringLiteral("Half.gb"), 10);
        CHECK(up.beginChunk(s2.uploadId, 0, 5).ok());
        CHECK(up.finish(s2.uploadId).code == QLatin1String("busy"));
        CHECK(up.writeChunk(s2.uploadId, "12345", 5));
        CHECK(up.endChunk(s2.uploadId).received == 5);
        const Answer inc = up.finish(s2.uploadId);
        CHECK(inc.code == QLatin1String("incomplete"));
        CHECK(!QFileInfo::exists(dir + QStringLiteral("/Half.gb")));

        // A target that appears between start and finish: refused, the user's file untouched, the part KEPT.
        const Answer s3 = up.start(ds, ds[0].id, QStringLiteral("Race.gb"), 4);
        CHECK(up.beginChunk(s3.uploadId, 0, 4).ok() && up.writeChunk(s3.uploadId, "DROP", 4));
        CHECK(up.endChunk(s3.uploadId).ok());
        CHECK(writeFile(dir + QStringLiteral("/Race.gb"), "USER"));
        const Answer raced = up.finish(s3.uploadId);
        CHECK(raced.code == QLatin1String("exists"));
        CHECK(raced.http == 409);
        CHECK(readAll(dir + QStringLiteral("/Race.gb")) == "USER");
        CHECK(QFileInfo(up.partPathFor(s3.uploadId)).size() == 4);
        // A bad new name is refused and still keeps it; a good one lands without re-sending a byte.
        CHECK(up.finish(s3.uploadId, QStringLiteral("../Race.gb")).code == QLatin1String("badname"));
        CHECK(QFileInfo(up.partPathFor(s3.uploadId)).size() == 4);
        const Answer renamed = up.finish(s3.uploadId, QStringLiteral("Race (2).gb"));
        CHECK(renamed.ok() && renamed.landed);
        CHECK(readAll(dir + QStringLiteral("/Race (2).gb")) == "DROP");
        CHECK(readAll(dir + QStringLiteral("/Race.gb")) == "USER");

        // The primitives themselves.
        CHECK(writeFile(dir + QStringLiteral("/src.bin"), "S"));
        CHECK(writeFile(dir + QStringLiteral("/dst.bin"), "D"));
        CHECK(renameNoReplace(dir + QStringLiteral("/src.bin"), dir + QStringLiteral("/dst.bin")) == RenameResult::Exists);
        CHECK(readAll(dir + QStringLiteral("/dst.bin")) == "D");
        CHECK(renameNoReplace(dir + QStringLiteral("/src.bin"), dir + QStringLiteral("/new.bin")) == RenameResult::Renamed);
        CHECK(!QFileInfo::exists(dir + QStringLiteral("/src.bin")) && readAll(dir + QStringLiteral("/new.bin")) == "S");
        {
            QFile f(dir + QStringLiteral("/new.bin"));
            CHECK(f.open(QIODevice::ReadWrite) && syncFile(f));
        }
#ifndef Q_OS_WIN
        // A dangling symlink at the final name: the landing must not write through it (QFile::rename's copy
        // fallback would), and nothing appears where it points.
        const QString target = d4.path() + QStringLiteral("/escaped.gb");
        CHECK(QFile::link(target, dir + QStringLiteral("/Link.gb")));
        const Answer s4 = up.start(ds, ds[0].id, QStringLiteral("Link.gb"), 2);
        CHECK(s4.code == QLatin1String("exists"));
        CHECK(renameNoReplace(dir + QStringLiteral("/new.bin"), dir + QStringLiteral("/Link.gb")) == RenameResult::Exists);
        CHECK(!QFileInfo::exists(target));
#endif
    }

    // ---- 6. sweep ----------------------------------------------------------------------------------------------
    {
        const QDateTime now = QDateTime::currentDateTimeUtc();
        const QString p = partFileName(QStringLiteral("0123456789abcdef0123456789abcdef"));
        CHECK(shouldSweep(p, now.addSecs(-kStaleAfterSecs), now));
        CHECK(shouldSweep(p, now.addDays(-3), now));
        CHECK(!shouldSweep(p, now.addSecs(-kStaleAfterSecs + 60), now));
        CHECK(!shouldSweep(p, now, now));
        CHECK(!shouldSweep(QStringLiteral("Game.sfc"), now.addDays(-30), now));
        CHECK(!shouldSweep(QStringLiteral("foo.part"), now.addDays(-30), now));
        CHECK(!shouldSweep(QStringLiteral(".eb-drop-xyz.part"), now.addDays(-30), now));
        CHECK(!shouldSweep(QStringLiteral("x") + p, now.addDays(-30), now));
        CHECK(!shouldSweep(p + QStringLiteral(".bak"), now.addDays(-30), now));
        CHECK(!shouldSweep(QStringLiteral(".eb-drop-0123456789ABCDEF0123456789ABCDEF.part"), now.addDays(-30), now));
        CHECK(!shouldSweep(p, QDateTime(), now));

        QTemporaryDir d6;
        const QString dir = d6.path() + QStringLiteral("/psx");
        const QString sub = dir + QStringLiteral("/sub");
        QDir().mkpath(sub);
        const QList<Destination> ds = buildDestinations({ { QStringLiteral("rom"), QStringLiteral("psx"), QStringLiteral("PS"), dir } });
        Uploads up(plenty);
        const Answer live = up.start(ds, ds[0].id, QStringLiteral("Live.bin"), 10);
        const Answer idle = up.start(ds, ds[0].id, QStringLiteral("Idle.bin"), 10);
        CHECK(live.ok() && idle.ok());
        const QString stale = dir + QLatin1Char('/') + p;
        const QString fresh = dir + QLatin1Char('/') + partFileName(QStringLiteral("fedcba9876543210fedcba9876543210"));
        CHECK(writeFile(stale, "old"));
        CHECK(writeFile(fresh, "new"));
        CHECK(writeFile(dir + QStringLiteral("/Old Game.bin"), "user"));
        CHECK(writeFile(dir + QStringLiteral("/old.part"), "user"));
        CHECK(writeFile(sub + QLatin1Char('/') + p, "nested"));
        const QDateTime old = now.addDays(-2);
        for (const QString& f : { stale, dir + QStringLiteral("/Old Game.bin"), dir + QStringLiteral("/old.part"),
                                  sub + QLatin1Char('/') + p, up.partPathFor(live.uploadId), up.partPathFor(idle.uploadId) })
            CHECK(setMtime(f, old));
        CHECK(setMtime(fresh, now.addSecs(-3600)));
        CHECK(up.beginChunk(live.uploadId, 0, 10).ok());              // in flight: never swept
        const int n = up.sweep({ dir }, now);
        CHECK(n == 2);                                                // the stale orphan and the idle upload
        CHECK(!QFileInfo::exists(stale));
        CHECK(!QFileInfo::exists(dir + QLatin1Char('/') + partFileName(idle.uploadId)));
        CHECK(QFileInfo::exists(fresh));
        CHECK(QFileInfo::exists(up.partPathFor(live.uploadId)));
        CHECK(QFileInfo::exists(dir + QStringLiteral("/Old Game.bin")));
        CHECK(QFileInfo::exists(dir + QStringLiteral("/old.part")));
        CHECK(QFileInfo::exists(sub + QLatin1Char('/') + p));        // not recursive
        CHECK(up.status(idle.uploadId).code == QLatin1String("unknown"));   // a swept upload is forgotten
        up.abortChunk(live.uploadId);
        CHECK(sweepDirs({ dir }, now.addDays(5), {}) == 2);           // later: the live one and the fresh one
    }

    // ---- 7. nothing written outside the destination ------------------------------------------------------------
    {
        const QMap<QString, QString> before = census(root);
        Uploads up(plenty);
        const QByteArray bytes = pieceBytes(7, 0, 5000);
        const Answer s = up.start(dests, dSnes->id, QStringLiteral("Census (Europe).sfc"), bytes.size());
        CHECK(s.ok());
        CHECK(up.beginChunk(s.uploadId, 0, bytes.size()).ok());
        CHECK(up.writeChunk(s.uploadId, bytes.constData(), bytes.size()));
        CHECK(up.endChunk(s.uploadId).ok());
        // Hostile attempts in between change nothing.
        for (const QString& n : { QStringLiteral("../../state/marks.json"), QStringLiteral("..\\..\\state\\marks.json"),
                                  outside + QStringLiteral("/marks.json"), QStringLiteral("\\\\?\\") + outside })
            CHECK(up.start(dests, dSnes->id, n, 3).code == QLatin1String("badname"));
        CHECK(up.finish(s.uploadId, QStringLiteral("../../state/marks.json")).code == QLatin1String("badname"));
        CHECK(up.finish(s.uploadId).ok());
        const QStringList diff = changed(before, census(root));
        CHECK(diff == QStringList{ QStringLiteral("roms/snes/Census (Europe).sfc") });
        for (const QString& c : diff) CHECK(c.startsWith(QStringLiteral("roms/snes/")));
        CHECK(readAll(outside + QStringLiteral("/marks.json")) == "{\"keep\":true}");
    }

    // ---- 8. the token rule ---------------------------------------------------------------------------------------
    {
        for (const char* r : { "/drop/destinations", "/drop/start", "/drop/status", "/drop/chunk", "/drop/finish",
                               "/drop/", "/drop/anything", "/drop/../open", "/dropx" })
            CHECK(PlayOn::routeNeedsToken(QString::fromLatin1(r)));
        CHECK(!PlayOn::routeNeedsToken(QStringLiteral("/drop")));
        CHECK(!PlayOn::routeNeedsToken(QStringLiteral("/pair")));
        CHECK(PlayOn::routeNeedsToken(QStringLiteral("/bundle")));   // unchanged
    }

    // ---- 9. the page -------------------------------------------------------------------------------------------
    const QByteArray page = pageHtml();
    {
        CHECK(!page.isEmpty());
        QFile src(QString::fromUtf8(EB_FILEDROP_PAGE_SOURCE));
        CHECK(src.open(QIODevice::ReadOnly));
        CHECK(page == src.readAll());                                 // the embedded copy IS the source file
        CHECK(page.startsWith("<!DOCTYPE html>"));
        CHECK(!page.contains("src=\"http"));
        CHECK(!page.contains("href=\"http"));
        CHECK(!page.contains("url(http"));
        CHECK(!page.contains("@import"));
        CHECK(!page.contains("localStorage"));
        CHECK(!page.contains("sessionStorage"));
        CHECK(!page.contains("document.cookie"));
        CHECK(page.contains("X-EB-Token"));
        CHECK(page.contains("/drop/start"));
        CHECK(page.contains("8 * 1024 * 1024"));                     // the page's piece size is FileDrop's
        CHECK(kChunkBytes == 8 * kMiB);
    }

    // ---- 10. the real socket -----------------------------------------------------------------------------------
    {
        QTemporaryDir sd;
        const QString sroot = QDir::cleanPath(sd.path());
        const QString sdir = sroot + QStringLiteral("/roms/psx");
        const QString sside = sroot + QStringLiteral("/state");
        QDir().mkpath(sdir);
        QDir().mkpath(sside);
        CHECK(writeFile(sdir + QStringLiteral("/Existing (USA).bin"), "USER-ROM"));
        CHECK(writeFile(sside + QStringLiteral("/keep.txt"), "keep"));
        const QList<Destination> sds = buildDestinations({ { QStringLiteral("rom"), QStringLiteral("psx"),
                                                             QStringLiteral("PlayStation"), sdir } });
        const QString destId = sds.value(0).id;
        const QString token = QStringLiteral("fixture-token-115-0123456789");

        auto uploads = std::make_shared<Uploads>(plenty);
        QList<Answer> landedCalls;
        RemoteServer server;
        RemoteServer::Hooks hooks;
        int pairBegins = 0;
        hooks.tokens = [token] { return QSet<QString>{ token }; };
        hooks.pairBegin = [&pairBegins] { ++pairBegins; return true; };
        hooks.state = [] { return RemoteApi::PlayerStateView{}; };
        server.setHooks(hooks);
        RemoteServer::DropHooks dh;
        dh.uploads = uploads;
        dh.destinations = [sds] { return sds; };
        dh.landed = [&landedCalls](const Answer& a) { landedCalls << a; };
        server.setFileDrop(dh);
        server.setBodyIdleTimeoutMs(2000);
        CHECK(server.start(0));
        const quint16 port = server.port();

        // (a) GET /drop is the page, whatever the query; nothing under /drop/ is served without a token.
        const HttpResult pg = httpRequest(port, get(QStringLiteral("/drop"), QString()));
        CHECK(pg.status == 200);
        CHECK(pg.body == page);
        CHECK(pg.head.contains("Content-Type: text/html; charset=utf-8"));
        CHECK(pg.head.contains("Content-Security-Policy: default-src 'none'"));
        CHECK(pg.head.contains("frame-ancestors 'none'"));
        CHECK(httpRequest(port, get(QStringLiteral("/drop?file=../../state/keep.txt"), QString())).body == page);
        CHECK(httpRequest(port, get(QStringLiteral("/drop?file=") + sside + QStringLiteral("/keep.txt"), token)).body == page);
        CHECK(httpRequest(port, get(QStringLiteral("/drop/../state/keep.txt"), QString())).status == 401);
        CHECK(httpRequest(port, get(QStringLiteral("/drop/../state/keep.txt"), token)).status == 404);
        CHECK(httpRequest(port, get(QStringLiteral("/drop/destinations/../../state/keep.txt"), token)).status == 404);
        CHECK(httpRequest(port, post(QStringLiteral("/drop"), "{}", token)).status == 400);

        // (b) No token: 401, and no part file anywhere.
        const QMap<QString, QString> sBefore = census(sroot);
        CHECK(httpRequest(port, get(QStringLiteral("/drop/destinations"), QString())).status == 401);
        CHECK(httpRequest(port, get(QStringLiteral("/drop/destinations"), QStringLiteral("wrong"))).status == 401);
        QJsonObject st;
        st.insert(QStringLiteral("dest"), destId);
        st.insert(QStringLiteral("name"), QStringLiteral("NoToken.bin"));
        st.insert(QStringLiteral("size"), 1000);
        const HttpResult nt = httpRequest(port, post(QStringLiteral("/drop/start"), jsonBody(st), QString()));
        CHECK(nt.status == 401);
        CHECK(nt.json.value(QStringLiteral("reason")).toString() == QLatin1String("pair this device first"));
        const QByteArray tiny = pieceBytes(9, 0, 1000);
        CHECK(httpRequest(port, putHead(uploadIdFor(destId, QStringLiteral("NoToken.bin"), 1000), 0, 1000, QString()), tiny).status == 401);
        CHECK(httpRequest(port, get(QStringLiteral("/drop/status?id=") + uploadIdFor(destId, QStringLiteral("NoToken.bin"), 1000), QString())).status == 401);
        QJsonObject fin0; fin0.insert(QStringLiteral("id"), uploadIdFor(destId, QStringLiteral("NoToken.bin"), 1000));
        CHECK(httpRequest(port, post(QStringLiteral("/drop/finish"), jsonBody(fin0), QString())).status == 401);
        CHECK(census(sroot) == sBefore);
        CHECK(partFilesIn(sdir).isEmpty());

        // (c) With the token: the list, by id and label, and no path.
        const HttpResult dl = httpRequest(port, get(QStringLiteral("/drop/destinations"), token));
        CHECK(dl.status == 200);
        CHECK(dl.json.value(QStringLiteral("destinations")).toArray().at(0).toObject().value(QStringLiteral("id")).toString() == destId);
        CHECK(!dl.body.contains(sroot.toUtf8()));

        // (d) Hostile names: "..", absolute, UNC -- 400, nothing on disk.
        for (const QString& n : { QStringLiteral(".."), QStringLiteral("../evil.bin"), QStringLiteral("../../state/keep.txt"),
                                  sside + QStringLiteral("/evil.bin"), QStringLiteral("C:\\evil.bin"),
                                  QStringLiteral("\\\\host\\share\\evil.bin"), QStringLiteral("//host/share/evil.bin") })
        {
            QJsonObject o;
            o.insert(QStringLiteral("dest"), destId);
            o.insert(QStringLiteral("name"), n);
            o.insert(QStringLiteral("size"), 10);
            const HttpResult h = httpRequest(port, post(QStringLiteral("/drop/start"), jsonBody(o), token));
            CHECK(h.status == 400);
            CHECK(h.json.value(QStringLiteral("code")).toString() == QLatin1String("badname"));
        }
        {
            QJsonObject o;                                            // a path in place of the destination id
            o.insert(QStringLiteral("dest"), sside);
            o.insert(QStringLiteral("name"), QStringLiteral("x.bin"));
            o.insert(QStringLiteral("size"), 10);
            CHECK(httpRequest(port, post(QStringLiteral("/drop/start"), jsonBody(o), token)).json.value(QStringLiteral("code")).toString() == QLatin1String("nodest"));
            o.insert(QStringLiteral("dest"), destId);
            o.insert(QStringLiteral("size"), 1.5);                    // not a whole number of bytes
            CHECK(httpRequest(port, post(QStringLiteral("/drop/start"), jsonBody(o), token)).status == 400);
            o.insert(QStringLiteral("size"), QStringLiteral("10"));
            CHECK(httpRequest(port, post(QStringLiteral("/drop/start"), jsonBody(o), token)).status == 400);
        }
        CHECK(census(sroot) == sBefore);

        // (e) An existing name: "already there".
        {
            QJsonObject o;
            o.insert(QStringLiteral("dest"), destId);
            o.insert(QStringLiteral("name"), QStringLiteral("Existing (USA).bin"));
            o.insert(QStringLiteral("size"), 10);
            const HttpResult h = httpRequest(port, post(QStringLiteral("/drop/start"), jsonBody(o), token));
            CHECK(h.status == 409);
            CHECK(h.json.value(QStringLiteral("code")).toString() == QLatin1String("exists"));
            CHECK(readAll(sdir + QStringLiteral("/Existing (USA).bin")) == "USER-ROM");
            CHECK(partFilesIn(sdir).isEmpty());
        }

        auto startOver = [&](const QString& name, qint64 size) {
            QJsonObject o;
            o.insert(QStringLiteral("dest"), destId);
            o.insert(QStringLiteral("name"), name);
            o.insert(QStringLiteral("size"), double(size));
            return httpRequest(port, post(QStringLiteral("/drop/start"), jsonBody(o), token));
        };
        auto finishOver = [&](const QString& id) {
            QJsonObject o;
            o.insert(QStringLiteral("id"), id);
            return httpRequest(port, post(QStringLiteral("/drop/finish"), jsonBody(o), token));
        };

        // (f) 200 MiB in 8 MiB pieces, landed byte-exact.
        {
            const qint64 size = 200 * kMiB;
            const quint64 seed = 115;
            const HttpResult s = startOver(QStringLiteral("Big Game (USA).bin"), size);
            CHECK(s.status == 200);
            const QString id = s.json.value(QStringLiteral("uploadId")).toString();
            CHECK(validUploadId(id));
            QCryptographicHash sent(QCryptographicHash::Sha256);
            qint64 received = 0;
            int pieces = 0;
            bool allOk = true;
            for (qint64 off = 0; off < size; off += kChunkBytes)
            {
                const qint64 n = qMin(kChunkBytes, size - off);
                const QByteArray b = pieceBytes(seed, off / kChunkBytes, n);
                sent.addData(b);
                const HttpResult r = httpRequest(port, putHead(id, off, n, token), b);
                allOk = allOk && r.status == 200;
                received = qint64(r.json.value(QStringLiteral("received")).toDouble());
                allOk = allOk && received == off + n;
                ++pieces;
            }
            CHECK(allOk);
            CHECK(pieces == 25);
            CHECK(received == size);
            CHECK(!QFileInfo::exists(sdir + QStringLiteral("/Big Game (USA).bin")));   // not before finish
            const HttpResult f = finishOver(id);
            CHECK(f.status == 200);
            CHECK(f.json.value(QStringLiteral("landed")).toBool());
            CHECK(!f.body.contains(sroot.toUtf8()));
            const QString landed = sdir + QStringLiteral("/Big Game (USA).bin");
            CHECK(QFileInfo(landed).size() == size);
            const bool exact = QFileInfo(landed).size() == size && fileSha256(landed) == sent.result().toHex();
            CHECK(exact);
            CHECK(partFilesIn(sdir).isEmpty());
            CHECK(landedCalls.size() == 1);
            CHECK(landedCalls.value(0).destinationId == destId);
            CHECK(landedCalls.value(0).landedPath == landed);
            CHECK(server.bufferedHighWater() < 64 * 1024);           // no piece ever entered a request buffer
            std::printf("FILEDROP-INFO 200 MiB in %d pieces: %s (sha256 %s...); largest request buffer %lld bytes\n",
                        pieces, exact ? "landed byte-exact" : "NOT landed byte-exact",
                        sent.result().toHex().left(16).constData(), (long long)server.bufferedHighWater());
            QFile::remove(landed);
        }

        // (g) A connection killed mid-piece; /drop/status says where to resume; it finishes byte-exact.
        {
            const qint64 size = 20 * kMiB;
            const quint64 seed = 7115;
            const HttpResult s = startOver(QStringLiteral("Resume (Japan).bin"), size);
            const QString id = s.json.value(QStringLiteral("uploadId")).toString();
            CHECK(s.status == 200);
            const QByteArray p0 = pieceBytes(seed, 0, kChunkBytes);
            CHECK(httpRequest(port, putHead(id, 0, kChunkBytes, token), p0).status == 200);
            const QByteArray p1 = pieceBytes(seed, 1, kChunkBytes);
            httpRequest(port, putHead(id, kChunkBytes, kChunkBytes, token), p1, 3 * kMiB, true);   // cut at ~3 MiB
            CHECK(spinUntil([&] { return server.streamsInFlight() == 0; }, 10000));
            const HttpResult stat = httpRequest(port, get(QStringLiteral("/drop/status?id=") + id, token));
            CHECK(stat.status == 200);
            const qint64 have = qint64(stat.json.value(QStringLiteral("received")).toDouble());
            CHECK(have > kChunkBytes);                                // part of the cut piece was kept
            CHECK(have <= kChunkBytes + 3 * kMiB);
            // A retry of the whole piece is refused (overlap) -- before its body is read.
            const HttpResult again = httpRequest(port, putHead(id, kChunkBytes, kChunkBytes, token));
            CHECK(again.status == 409);
            CHECK(again.json.value(QStringLiteral("code")).toString() == QLatin1String("offset"));
            CHECK(qint64(again.json.value(QStringLiteral("received")).toDouble()) == have);
            // Finishing now is "incomplete".
            CHECK(finishOver(id).json.value(QStringLiteral("code")).toString() == QLatin1String("incomplete"));
            // Resume from exactly `have`, in pieces of at most 8 MiB, as the page does.
            QByteArray whole;
            for (qint64 i = 0; i * kChunkBytes < size; ++i)
                whole += pieceBytes(seed, i, qMin(kChunkBytes, size - i * kChunkBytes));
            qint64 at = have;
            bool ok = true;
            while (at < size)
            {
                const qint64 n = qMin(kChunkBytes, size - at);
                const HttpResult r = httpRequest(port, putHead(id, at, n, token), whole.mid(int(at), int(n)));
                ok = ok && r.status == 200;
                at = qint64(r.json.value(QStringLiteral("received")).toDouble());
                if (r.status != 200) break;
            }
            CHECK(ok);
            CHECK(at == size);
            CHECK(finishOver(id).status == 200);
            const QString landed = sdir + QStringLiteral("/Resume (Japan).bin");
            const bool exact = fileSha256(landed) == QCryptographicHash::hash(whole, QCryptographicHash::Sha256).toHex();
            CHECK(exact);
            std::printf("FILEDROP-INFO cut at %lld of %lld bytes, resumed from /drop/status: %s\n",
                        (long long)have, (long long)size, exact ? "landed byte-exact" : "NOT landed byte-exact");
            QFile::remove(landed);
        }

        // (h) The piece rules over the wire, all answered before a body byte.
        {
            const HttpResult s = startOver(QStringLiteral("Rules.bin"), 100);
            const QString id = s.json.value(QStringLiteral("uploadId")).toString();
            const HttpResult gap = httpRequest(port, putHead(id, 50, 10, token));
            CHECK(gap.status == 409);
            CHECK(gap.json.value(QStringLiteral("code")).toString() == QLatin1String("offset"));
            const HttpResult big = httpRequest(port, putHead(id, 0, kChunkBytes + 1, token));
            CHECK(big.status == 413);
            const HttpResult past = httpRequest(port, putHead(id, 0, 101, token));
            CHECK(past.status == 400);
            QByteArray noLen = "PUT /drop/chunk?id=" + id.toUtf8() + "&offset=0 HTTP/1.1\r\nHost: 127.0.0.1\r\nX-EB-Token: " + token.toUtf8() + "\r\n\r\n";
            CHECK(httpRequest(port, noLen).status == 411);
            CHECK(httpRequest(port, putHead(QStringLiteral("0123456789abcdef0123456789abcdef"), 0, 10, token)).status == 404);
            QByteArray noOffset = "PUT /drop/chunk?id=" + id.toUtf8() + " HTTP/1.1\r\nHost: 127.0.0.1\r\nX-EB-Token: " + token.toUtf8() + "\r\nContent-Length: 10\r\n\r\n";
            CHECK(httpRequest(port, noOffset).status == 400);
            CHECK(uploads->status(id).received == 0);
            CHECK(server.streamsInFlight() == 0);
        }

        // (h2) #423, over the wire: a REBINDING Host -- a name a hostile site has pointed at this device's
        // LAN address -- is refused 403 before the request is routed, before the token is weighed, before a
        // body byte is read and before any part file exists. The page and /pair are the two places a code is
        // read off the screen and typed, so they matter most; the gate covers every route on the listener.
        {
            server.setLocalHostName(QStringLiteral("a1b2c3d4e5f6.local"));
            const QMap<QString, QString> gateBefore = census(sroot);
            const QStringList partsBefore = partFilesIn(sdir);
            const int pairsBefore = pairBegins;

            // One request with an arbitrary Host / Origin. A null pointer means the header is not sent at all.
            auto raw = [&](const char* method, const QByteArray& target, const char* host, const char* origin,
                           const QByteArray& body, bool hasBody, bool withToken) {
                QByteArray h = QByteArray(method) + " " + target + " HTTP/1.1\r\n";
                if (host)   h += QByteArray("Host: ") + host + "\r\n";
                if (origin) h += QByteArray("Origin: ") + origin + "\r\n";
                if (withToken) h += "X-EB-Token: " + token.toUtf8() + "\r\n";
                if (hasBody)
                    h += "Content-Type: application/json\r\nContent-Length: "
                         + QByteArray::number(body.size()) + "\r\n";
                return h + "\r\n" + body;
            };
            const QByteArray none;

            for (const char* host : { "evil.example.com", "127.0.0.1.evil.com", "evilocalhost",
                                      "deadbeefcafe.local", "a1b2c3d4e5f6.local.evil.com",
                                      "evil-a1b2c3d4e5f6.local", "", static_cast<const char*>(nullptr) })
            {
                const HttpResult page403 = httpRequest(port, raw("GET", "/drop", host, nullptr, none, false, false));
                CHECK(page403.status == 403);
                CHECK(page403.body.trimmed() == "forbidden");            // nothing about what is served here
                CHECK(!page403.body.contains("<"));
                CHECK(!page403.head.contains("Access-Control"));
                CHECK(page403.head.contains("Content-Type: text/plain"));
                CHECK(httpRequest(port, raw("GET", "/state", host, nullptr, none, false, false)).status == 403);
                CHECK(httpRequest(port, raw("GET", "/drop/destinations", host, nullptr, none, false, true)).status == 403);
                CHECK(httpRequest(port, raw("POST", "/pair", host, nullptr, none, true, false)).status == 403);
            }
            CHECK(pairBegins == pairsBefore);                            // no code was ever put on the screen

            // The addresses a browser on this LAN really uses still work, Origin or no Origin.
            CHECK(httpRequest(port, raw("GET", "/drop", "127.0.0.1", nullptr, none, false, false)).status == 200);
            CHECK(httpRequest(port, raw("GET", "/drop", "127.0.0.1:8080", nullptr, none, false, false)).status == 200);
            CHECK(httpRequest(port, raw("GET", "/drop", "192.168.1.5:8080", nullptr, none, false, false)).status == 200);
            CHECK(httpRequest(port, raw("GET", "/drop", "[::1]:8080", nullptr, none, false, false)).status == 200);
            CHECK(httpRequest(port, raw("GET", "/drop", "localhost:8080", nullptr, none, false, false)).status == 200);
            CHECK(httpRequest(port, raw("GET", "/drop", "a1b2c3d4e5f6.local", nullptr, none, false, false)).status == 200);
            CHECK(httpRequest(port, raw("GET", "/drop", "127.0.0.1", "http://127.0.0.1", none, false, false)).status == 200);
            CHECK(httpRequest(port, raw("GET", "/drop", "127.0.0.1", "http://evil.example.com", none, false, false)).status == 403);
            CHECK(httpRequest(port, raw("GET", "/drop", "127.0.0.1", "null", none, false, false)).status == 403);
            CHECK(httpRequest(port, raw("GET", "/drop/destinations", "127.0.0.1", "http://127.0.0.1", none, false, true)).status == 200);
            CHECK(httpRequest(port, raw("GET", "/drop/destinations", "127.0.0.1", "http://evil.example.com", none, false, true)).status == 403);
            // A CORS preflight is never answered -- with or without a token, on any route.
            CHECK(httpRequest(port, raw("OPTIONS", "/drop", "127.0.0.1", nullptr, none, false, false)).status == 403);
            CHECK(httpRequest(port, raw("OPTIONS", "/drop/start", "127.0.0.1", nullptr, none, false, true)).status == 403);
            CHECK(httpRequest(port, raw("OPTIONS", "/pair", "127.0.0.1", "http://evil.example.com", none, false, false)).status == 403);

            // A refused /drop/start writes nothing, and a refused piece reads no body and keeps no bytes.
            QJsonObject gs;
            gs.insert(QStringLiteral("dest"), destId);
            gs.insert(QStringLiteral("name"), QStringLiteral("Rebound (USA).bin"));
            gs.insert(QStringLiteral("size"), 4096);
            CHECK(httpRequest(port, raw("POST", "/drop/start", "evil.example.com", nullptr, jsonBody(gs), true, true)).status == 403);
            CHECK(partFilesIn(sdir) == partsBefore);
            CHECK(census(sroot) == gateBefore);
            CHECK(!QFileInfo::exists(sdir + QStringLiteral("/Rebound (USA).bin")));

            const HttpResult okStart = httpRequest(port, post(QStringLiteral("/drop/start"), jsonBody(gs), token));
            CHECK(okStart.status == 200);
            const QString gateId = okStart.json.value(QStringLiteral("uploadId")).toString();
            const QByteArray piece = pieceBytes(423, 0, 4096);
            const QByteArray reboundHead = "PUT /drop/chunk?id=" + gateId.toUtf8()
                + "&offset=0 HTTP/1.1\r\nHost: evil.example.com\r\nX-EB-Token: " + token.toUtf8()
                + "\r\nContent-Type: application/octet-stream\r\nContent-Length: 4096\r\n\r\n";
            CHECK(httpRequest(port, reboundHead, piece).status == 403);
            CHECK(uploads->status(gateId).received == 0);                // not one byte of the body was taken
            CHECK(server.streamsInFlight() == 0);                        // and no spool or part file was opened
            CHECK(server.bufferedHighWater() < 64 * 1024);

            // The same upload, from the device's own address, still goes through end to end.
            CHECK(httpRequest(port, putHead(gateId, 0, 4096, token), piece).status == 200);
            CHECK(uploads->status(gateId).received == 4096);
            QJsonObject gf;
            gf.insert(QStringLiteral("id"), gateId);
            CHECK(httpRequest(port, post(QStringLiteral("/drop/finish"), jsonBody(gf), token)).status == 200);
            CHECK(QFileInfo(sdir + QStringLiteral("/Rebound (USA).bin")).size() == 4096);
            CHECK(QFile::remove(sdir + QStringLiteral("/Rebound (USA).bin")));
            CHECK(census(sroot) == gateBefore);                          // the tree is exactly as it was
        }

        // (i) A listener up for file drop alone: no remote control, no hand-off; pairing and the drop still work.
        {
            server.setControlSurface(false);
            CHECK(httpRequest(port, get(QStringLiteral("/state"), QString())).status == 404);
            CHECK(httpRequest(port, post(QStringLiteral("/player"), "{\"action\":\"pause\"}", QString())).status == 404);
            CHECK(httpRequest(port, get(QStringLiteral("/inventory"), token)).status == 404);
            // A MALFORMED control request is 404 too: a 400 would say the route is there after all.
            CHECK(httpRequest(port, post(QStringLiteral("/player"), "{}", QString())).status == 404);
            CHECK(httpRequest(port, post(QStringLiteral("/input"), "{\"dir\":\"sideways\"}", QString())).status == 404);
            CHECK(httpRequest(port, post(QStringLiteral("/state"), QByteArray(), QString())).status == 404);
            CHECK(httpRequest(port, post(QStringLiteral("/bundle"), QByteArray(), token)).status == 404);
            const int before = pairBegins;
            CHECK(httpRequest(port, post(QStringLiteral("/pair"), QByteArray(), QString())).status == 200);
            CHECK(pairBegins == before + 1);
            CHECK(httpRequest(port, get(QStringLiteral("/drop"), QString())).status == 200);
            CHECK(httpRequest(port, get(QStringLiteral("/drop/destinations"), token)).status == 200);
            server.setControlSurface(true);
            CHECK(httpRequest(port, get(QStringLiteral("/state"), QString())).status == 200);
        }

        // (j) File drop off: every /drop route is a 404 (after the token check), the page included.
        {
            server.setFileDrop(RemoteServer::DropHooks());
            CHECK(httpRequest(port, get(QStringLiteral("/drop"), QString())).status == 404);
            CHECK(httpRequest(port, get(QStringLiteral("/drop/destinations"), token)).status == 404);
            CHECK(httpRequest(port, get(QStringLiteral("/drop/destinations"), QString())).status == 401);
            CHECK(httpRequest(port, putHead(QStringLiteral("0123456789abcdef0123456789abcdef"), 0, 10, token)).status == 404);
        }

        // Nothing outside the destination changed through all of it.
        CHECK(readAll(sside + QStringLiteral("/keep.txt")) == "keep");
        for (const QString& c : changed(sBefore, census(sroot)))
            CHECK(c.startsWith(QStringLiteral("roms/psx/")));
        server.stop();
    }

    if (failures == 0) std::printf("FILEDROP-OK\n");
    else               std::fprintf(stderr, "FILEDROP had %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
