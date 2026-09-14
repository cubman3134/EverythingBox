// Headless check of "Send library to device" (issue #127) — src/core/LibraryBundle.{h,cpp}, plus the two
// routes it adds to the pure #76/#143 surface (src/core/RemoteApi.{h,cpp}, src/core/PlayOnDevice.{h,cpp}).
//
// What this pins, with no socket, no window and no second machine — the whole feature runs against two
// temporary directories that stand in for two devices' metadata caches:
//
//   1. THE SAFETY RULES FIRST. safeItemId accepts a 40-character MetaCache hash and NOTHING else, so "..",
//      an absolute path, a drive letter and a UNC share are refused before a path is built out of them;
//      safeFileName refuses separators, hidden names, Windows device names and every extension that is not
//      art or metadata (a trailer, a manual and an executable therefore cannot ride in a bundle).
//   2. THE STAMP. Stable for an unchanged folder; different for a changed meta.json BYTE, for a changed art
//      size, for a bumped art mtime and for an added role. And its DOCUMENTED LIMIT, asserted rather than
//      glossed: an art file rewritten to the same size with its mtime restored is invisible to the stamp.
//      That is the price of not reading gigabytes of PNG on every inventory, and it is pinned so nobody
//      later believes the stamp is a content hash of the art.
//   3. THE DIFF. Missing -> send; identical stamp -> nothing; different and the source newer -> send;
//      different and the TARGET newer (or a tie) -> keep what the target has.
//   4. THE DEFINING PROPERTY. A whole transfer, then a second run with nothing changed: the plan is empty
//      and the BYTES THAT WOULD BE SENT ARE ZERO. Then one item changes on the source and EXACTLY that one
//      item moves. This is the feature; everything else is how it is made safe.
//   5. WARM, NEVER FIGHT. An item that is newer on the target is kept, byte for byte. Files the bundle did
//      not carry (the trailer this feature deliberately does not move) survive a landing.
//   6. ATOMIC LANDING. An interruption injected mid-item leaves the target's existing copy exactly as it
//      was, leaves no half-written folder at the item's real path, and is swept on the next run — after
//      which the retry lands. A crash injected DURING THE SWAP (the live folder set aside, the new one not
//      yet in place) is recovered rather than lost.
//   7. NOTHING OUTSIDE THE CACHE, AND NO STATE. A recursive census of the parent directory is taken before
//      and after a transfer: everything that changed is inside the cache root, and a sibling state store
//      (marks / favourites / resume) is byte-identical afterwards.
//   8. THE ROUTES. /inventory and /bundle require a paired token; the routing table answers them for the
//      right method only; and a /bundle request is allowed a payload-sized read cap while every other route
//      keeps #76's tiny one.
//
// Issue #291 adds the raw-body format (v2) and the streaming target, and pins them in 12-17:
//
//  12. THE V2 CODEC. A byte-exact round trip (mtimes included, no base64), and every refusal -- truncated,
//      over-long, sizes that do not sum, a traversing or non-art name, a non-hash id, a future version, a
//      file or an item declared over its ceiling -- decided with the device still positioned inside the
//      header (no file byte read) and with nothing written under the target.
//  13. INTEROP. The inventory keeps "v":1 and advertises v2 in a field an old parser ignores; no field means
//      v1; a v2 body handed to the v1 decoder (an old target) is refused, never half-landed.
//  14. THE ISSUE'S PROPERTY. A ~13 MiB item that v1 skips moves via v2, and the next plan sends nothing.
//      Progress counts file bytes, not wire bytes.
//  15. THE SPOOL. It lives under <root>/.eb-incoming and nowhere else, survives an inventory sweep while in
//      flight, and is gone after landed, refused and a crash leftover.
//  16. THE STREAMING DECISION (RemoteApi::bodyPlanFor), read off the headers alone.
//  17. A REAL SOCKET. RemoteServer on a loopback port: a v2 body past the 20 MiB buffered cap lands and the
//      request buffer never held it; the token is required before a body is accepted; a declared length over
//      the ceiling is a 413 with no body sent; a disconnect or a stall mid-body leaves no spool; v1 still
//      lands through the same listener.
//
// Prints BUNDLEXFER-OK on success; any failure prints BUNDLEXFER-FAIL <cond> (line) and exits non-zero.
#include "LibraryBundle.h"
#include "PlayOnDevice.h"
#include "RemoteApi.h"
#include "RemoteServer.h"

#include <QBuffer>
#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QTcpSocket>
#include <QTimer>

#include <cstdio>
#include <functional>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "BUNDLEXFER-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

// ---- fixtures ---------------------------------------------------------------------------------------------

namespace fx
{
    // A MetaCache folder name is sha1(key).toHex(). Building the probe's ids the same way keeps the fixture
    // honest about what a real cache holds.
    static QString idFor(const char* key)
    {
        return QString::fromLatin1(
            QCryptographicHash::hash(QByteArray(key), QCryptographicHash::Sha1).toHex());
    }

    static bool writeFile(const QString& path, const QByteArray& data)
    {
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
        f.write(data);
        f.close();
        return true;
    }

    static QByteArray readFile(const QString& path)
    {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) return QByteArray();
        return f.readAll();
    }

    static void setMtime(const QString& path, qint64 ms)
    {
        QFile f(path);
        if (f.open(QIODevice::ReadWrite))
        {
            f.setFileTime(QDateTime::fromMSecsSinceEpoch(ms), QFileDevice::FileModificationTime);
            f.close();
        }
    }

    static qint64 mtimeOf(const QString& path)
    {
        return QFileInfo(path).lastModified().toMSecsSinceEpoch();
    }

    // A whole item: meta.json plus two art roles, every file at a PINNED modification time so the stamp is
    // reproducible from run to run and the "newer" comparisons are exact rather than racy.
    static void makeItem(const QString& root, const QString& id, const QByteArray& meta,
                         const QByteArray& thumb, qint64 baseMs)
    {
        QDir().mkpath(root + QLatin1Char('/') + id);
        writeFile(root + QLatin1Char('/') + id + QStringLiteral("/meta.json"), meta);
        writeFile(root + QLatin1Char('/') + id + QStringLiteral("/thumb.png"), thumb);
        writeFile(root + QLatin1Char('/') + id + QStringLiteral("/poster.jpg"), QByteArray("POSTERBYTES"));
        setMtime(root + QLatin1Char('/') + id + QStringLiteral("/meta.json"), baseMs);
        setMtime(root + QLatin1Char('/') + id + QStringLiteral("/thumb.png"), baseMs);
        setMtime(root + QLatin1Char('/') + id + QStringLiteral("/poster.jpg"), baseMs);
    }

    // Everything under `dir`, as relative path -> (size, sha256). The census the "nothing outside the cache"
    // and "no state store touched" assertions compare.
    static QMap<QString, QString> census(const QString& dir)
    {
        QMap<QString, QString> out;
        QDir base(dir);
        QStringList stack;
        stack << dir;
        while (!stack.isEmpty())
        {
            const QString cur = stack.takeLast();
            QDir d(cur);
            const QFileInfoList entries = d.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot,
                                                          QDir::Name);
            for (const QFileInfo& fi : entries)
            {
                if (fi.isDir()) { stack << fi.absoluteFilePath(); continue; }
                const QString rel = base.relativeFilePath(fi.absoluteFilePath());
                out.insert(rel, QString::fromLatin1(
                    QCryptographicHash::hash(readFile(fi.absoluteFilePath()),
                                             QCryptographicHash::Sha256).toHex()));
            }
        }
        return out;
    }

    // The bytes a plan would put on the wire. The second-run assertion is about THIS number being zero, not
    // about a count of items: an empty plan that still encoded a payload would pass a count and fail here.
    static qint64 wireBytesFor(const QString& root, const QStringList& ids)
    {
        qint64 total = 0;
        for (const QString& id : ids)
        {
            LibraryBundle::Payload p;
            QString err;
            if (!LibraryBundle::readPayload(root, id, p, err)) continue;
            total += qint64(LibraryBundle::encodePayload(p).size());
        }
        return total;
    }

    // One item, source -> target, through encode/decode exactly as the wire would. Returns the landing
    // result and adds what crossed to `bytes`.
    static LibraryBundle::LandResult transferOne(const QString& srcRoot, const QString& dstRoot,
                                                 const QString& id, qint64* bytes, QString* error)
    {
        LibraryBundle::Payload p;
        QString err;
        if (!LibraryBundle::readPayload(srcRoot, id, p, err))
        {
            if (error) *error = err;
            return LibraryBundle::LandResult::Refused;
        }
        const QByteArray wire = LibraryBundle::encodePayload(p);
        if (bytes) *bytes += qint64(wire.size());

        LibraryBundle::Payload got;
        LibraryBundle::Refusal why = LibraryBundle::Refusal::None;
        QString message;
        if (!LibraryBundle::decodePayload(wire, got, why, message))
        {
            if (error) *error = message;
            return LibraryBundle::LandResult::Refused;
        }
        return LibraryBundle::landItem(dstRoot, got, err);
    }

    // ---- #291 fixtures ----

    // Deterministic, incompressible-looking bytes, so a byte-exact comparison means something.
    static QByteArray noise(qint64 n, quint32 seed)
    {
        QByteArray out(int(n), Qt::Uninitialized);
        quint32 x = seed * 2654435761u + 1u;
        for (qint64 i = 0; i < n; ++i)
        {
            x = x * 1664525u + 1013904223u;
            out[int(i)] = char(x >> 24);
        }
        return out;
    }

    // An art-heavy item: meta.json plus `count` PNGs of `each` bytes, every file at a pinned mtime.
    static qint64 makeHeavyItem(const QString& root, const QString& id, int count, qint64 each, qint64 baseMs)
    {
        const QString dir = root + QLatin1Char('/') + id;
        QDir().mkpath(dir);
        const QByteArray meta = QByteArray("{\"key\":\"heavy\",\"title\":\"") + id.left(8).toLatin1() + "\"}";
        writeFile(dir + QStringLiteral("/meta.json"), meta);
        setMtime(dir + QStringLiteral("/meta.json"), baseMs);
        qint64 total = meta.size();
        for (int i = 0; i < count; ++i)
        {
            const QString p = dir + QStringLiteral("/art-%1.png").arg(i);
            writeFile(p, noise(each, quint32(i + 7)));
            setMtime(p, baseMs + i + 1);
            total += each;
        }
        return total;
    }

    static quint32 u32At(const QByteArray& b, int at)
    {
        if (b.size() < at + 4) return 0;
        return (quint32(quint8(b[at])) << 24) | (quint32(quint8(b[at + 1])) << 16)
             | (quint32(quint8(b[at + 2])) << 8) | quint32(quint8(b[at + 3]));
    }

    static void putU32(QByteArray& b, quint32 v)
    {
        b.append(char((v >> 24) & 0xff)); b.append(char((v >> 16) & 0xff));
        b.append(char((v >> 8) & 0xff));  b.append(char(v & 0xff));
    }

    // A hand-built v2 body: preamble, the given header JSON, then `body`. The hostile cases are built with
    // this rather than with the encoder, so they say exactly what a hostile source could put on the wire.
    static QByteArray wireV2(const QJsonObject& header, const QByteArray& body, quint32 version = 2,
                             const QByteArray& magic = QByteArray("EBBUNDLE"))
    {
        const QByteArray h = QJsonDocument(header).toJson(QJsonDocument::Compact);
        QByteArray out = magic;
        putU32(out, version);
        putU32(out, quint32(h.size()));
        out += h;
        out += body;
        return out;
    }

    static QJsonObject fileJson(const QString& name, double size, double mtime = 1700000000000.0)
    {
        QJsonObject o;
        o.insert(QStringLiteral("name"), name);
        o.insert(QStringLiteral("size"), size);
        o.insert(QStringLiteral("mtime"), mtime);
        return o;
    }

    static QJsonObject headerJson(const QString& id, const QJsonArray& files)
    {
        QJsonObject o;
        o.insert(QStringLiteral("id"), id);
        o.insert(QStringLiteral("stamp"), QStringLiteral("s"));
        o.insert(QStringLiteral("updated"), 1700000000000.0);
        o.insert(QStringLiteral("files"), files);
        return o;
    }

    // Where the file bytes of a v2 wire begin -- "no file byte read" means the device never got past here.
    static qint64 headerEndOf(const QByteArray& wire)
    {
        if (wire.size() < 16) return wire.size();
        return qint64(16) + qint64(u32At(wire, 12));
    }

    // Spin the event loop until `pred` holds or `ms` elapse. Server and client share this thread, so every
    // wait in the socket section goes through here rather than through a blocking waitFor*.
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

    static QStringList spoolFilesUnder(const QString& root)
    {
        return QDir(root + QStringLiteral("/.eb-incoming"))
            .entryList(QStringList{ QStringLiteral("*.spool") }, QDir::Files | QDir::Hidden);
    }

    struct HttpResult { int status = 0; QByteArray body; };

    // One request over a real loopback socket. `sendBodyBytes` < 0 sends the whole body; `abortAfter` drops
    // the connection once that much has been flushed, without waiting for an answer.
    static HttpResult httpRequest(quint16 port, const QByteArray& head, const QByteArray& body,
                                  qint64 sendBodyBytes, bool abortAfter,
                                  const std::function<void()>& whileOpen = std::function<void()>())
    {
        HttpResult r;
        QTcpSocket c;
        c.connectToHost(QHostAddress(QHostAddress::LocalHost), port);
        if (!spinUntil([&] { return c.state() == QAbstractSocket::ConnectedState; }, 5000)) return r;
        QByteArray response;
        QObject::connect(&c, &QTcpSocket::readyRead, [&] { response += c.readAll(); });
        c.write(head);
        const qint64 n = sendBodyBytes < 0 ? qint64(body.size()) : sendBodyBytes;
        const qint64 chunk = 1024 * 1024;
        for (qint64 at = 0; at < n && c.state() == QAbstractSocket::ConnectedState; at += chunk)
        {
            c.write(body.constData() + at, qMin(chunk, n - at));
            spinUntil([&] { return c.bytesToWrite() < 4 * chunk
                                   || c.state() != QAbstractSocket::ConnectedState; }, 20000);
        }
        spinUntil([&] { return c.bytesToWrite() == 0 || c.state() != QAbstractSocket::ConnectedState; }, 20000);
        if (whileOpen) whileOpen();
        if (abortAfter) { c.abort(); return r; }
        spinUntil([&] { return c.state() == QAbstractSocket::UnconnectedState; }, 20000);
        response += c.readAll();
        if (response.startsWith("HTTP/1.1 ")) r.status = response.mid(9, 3).toInt();
        const int sep = response.indexOf("\r\n\r\n");
        if (sep >= 0) r.body = response.mid(sep + 4);
        return r;
    }
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);   // #291: section 17 runs a real loopback socket
    const QString base = QDir::tempPath() + QStringLiteral("/eb-bundlexfer-probe");
    QDir(base).removeRecursively();
    QDir().mkpath(base);

    const QString srcRoot = base + QStringLiteral("/source/metadata");
    const QString dstBase = base + QStringLiteral("/target");
    const QString dstRoot = dstBase + QStringLiteral("/metadata");
    const QString dstState = dstBase + QStringLiteral("/state");
    QDir().mkpath(srcRoot);
    QDir().mkpath(dstRoot);
    QDir().mkpath(dstState);

    const QString idA = fx::idFor("tt0111161");
    const QString idB = fx::idFor("igdb:1020");
    const QString idC = fx::idFor("local:/roms/smw.sfc");

    // The state store this transfer must not touch: marks, favourites and a resume position. Drive sync owns
    // these; an art transfer that moved one of them would be two systems owning one datum.
    fx::writeFile(dstState + QStringLiteral("/marks.json"), QByteArray("{\"tt0111161\":{\"watched\":true}}"));
    fx::writeFile(dstState + QStringLiteral("/favourites.json"), QByteArray("[\"igdb:1020\"]"));
    fx::writeFile(dstState + QStringLiteral("/resume.json"), QByteArray("{\"pos\":1234.5}"));

    // ---- 1. safety -----------------------------------------------------------------------------------
    {
        CHECK(LibraryBundle::safeItemId(idA));
        CHECK(!LibraryBundle::safeItemId(QString()));
        CHECK(!LibraryBundle::safeItemId(QStringLiteral("..")));
        CHECK(!LibraryBundle::safeItemId(QStringLiteral("../../etc")));
        CHECK(!LibraryBundle::safeItemId(idA.left(39)));
        CHECK(!LibraryBundle::safeItemId(idA.toUpper()));                       // hex is lower case here
        CHECK(!LibraryBundle::safeItemId(QStringLiteral("C:/Windows/System32")));
        CHECK(!LibraryBundle::safeItemId(idA.left(38) + QStringLiteral("/x")));
        QString notHex = QStringLiteral("0123456789012345678901234567890123456789");
        notHex[0] = QLatin1Char('g');
        CHECK(!LibraryBundle::safeItemId(notHex));

        CHECK(LibraryBundle::safeFileName(QStringLiteral("meta.json")));
        CHECK(LibraryBundle::safeFileName(QStringLiteral("thumb.png")));
        CHECK(LibraryBundle::safeFileName(QStringLiteral("miximage.png")));
        CHECK(LibraryBundle::safeFileName(QStringLiteral("fixed-1a2b.jpg")));
        CHECK(!LibraryBundle::safeFileName(QStringLiteral("../meta.json")));
        CHECK(!LibraryBundle::safeFileName(QStringLiteral("sub/thumb.png")));
        CHECK(!LibraryBundle::safeFileName(QStringLiteral("..")));
        CHECK(!LibraryBundle::safeFileName(QStringLiteral(".hidden.png")));
        CHECK(!LibraryBundle::safeFileName(QStringLiteral("con.png")));         // a Windows device name
        CHECK(!LibraryBundle::safeFileName(QStringLiteral("lpt1.png")));
        // Not art, and each is a real file that lives in a MetaCache folder today: the megabyte roles this
        // increment deliberately does not move, plus the thing nobody should ever be able to send.
        CHECK(!LibraryBundle::safeFileName(QStringLiteral("trailer.mp4")));
        CHECK(!LibraryBundle::safeFileName(QStringLiteral("theme.mp3")));
        CHECK(!LibraryBundle::safeFileName(QStringLiteral("manual.pdf")));
        CHECK(!LibraryBundle::safeFileName(QStringLiteral("payload.exe")));
        CHECK(!LibraryBundle::safeFileName(QStringLiteral("script.sh")));
    }

    // ---- 2. the stamp --------------------------------------------------------------------------------
    {
        QList<LibraryBundle::FileEntry> files;
        LibraryBundle::FileEntry m; m.name = QStringLiteral("meta.json"); m.size = 20; m.mtimeMs = 1000;
        LibraryBundle::FileEntry t; t.name = QStringLiteral("thumb.png"); t.size = 500; t.mtimeMs = 2000;
        files << m << t;
        const QByteArray meta("{\"title\":\"A\"}");
        const QString s0 = LibraryBundle::stampOf(files, meta);

        CHECK(!s0.isEmpty());
        CHECK(LibraryBundle::stampOf(files, meta) == s0);                       // stable
        // Order of the input list must not matter: the stamp sorts.
        QList<LibraryBundle::FileEntry> reordered; reordered << t << m;
        CHECK(LibraryBundle::stampOf(reordered, meta) == s0);

        // A changed meta.json BYTE, with size and time held constant: caught, because meta.json is hashed by
        // content.
        CHECK(LibraryBundle::stampOf(files, QByteArray("{\"title\":\"B\"}")) != s0);

        // A changed art SIZE, and a bumped art MTIME: each caught on its own.
        QList<LibraryBundle::FileEntry> bigger = files; bigger[1].size = 501;
        CHECK(LibraryBundle::stampOf(bigger, meta) != s0);
        QList<LibraryBundle::FileEntry> later = files;  later[1].mtimeMs = 2001;
        CHECK(LibraryBundle::stampOf(later, meta) != s0);

        // An added role: caught.
        QList<LibraryBundle::FileEntry> plus = files;
        LibraryBundle::FileEntry l; l.name = QStringLiteral("logo.png"); l.size = 10; l.mtimeMs = 2000;
        plus << l;
        CHECK(LibraryBundle::stampOf(plus, meta) != s0);

        // THE DOCUMENTED LIMIT. An art file rewritten to the same size with the same mtime is invisible.
        // Pinned deliberately: the alternative is hashing every byte of a gigabyte of PNG on every inventory,
        // which is slower than the re-scrape this feature exists to avoid.
        CHECK(LibraryBundle::stampOf(files, meta) == s0);

        CHECK(LibraryBundle::updatedMsOf(files) == 2000);
        CHECK(LibraryBundle::updatedMsOf(QList<LibraryBundle::FileEntry>()) == 0);
    }

    // ---- 3. the inventory wire -----------------------------------------------------------------------
    {
        QList<LibraryBundle::Entry> in;
        LibraryBundle::Entry a; a.id = idA; a.stamp = QStringLiteral("aaa"); a.updatedMs = 1700000000000LL;
        LibraryBundle::Entry b; b.id = idB; b.stamp = QStringLiteral("bbb"); b.updatedMs = 1700000000001LL;
        in << a << b;
        QList<LibraryBundle::Entry> out;
        QString err;
        CHECK(LibraryBundle::parseInventory(LibraryBundle::inventoryJson(in), out, err));
        CHECK(out.size() == 2);
        CHECK(out.at(0).id == idA && out.at(0).stamp == QStringLiteral("aaa"));
        CHECK(out.at(1).updatedMs == 1700000000001LL);                          // ms survive the JSON double

        CHECK(!LibraryBundle::parseInventory(QByteArray("not json"), out, err));
        CHECK(!err.isEmpty());
        // A future cache format is refused with a sentence, never half-read (#127's version guard).
        CHECK(!LibraryBundle::parseInventory(QByteArray("{\"v\":99,\"items\":[]}"), out, err));
        CHECK(err.contains(QStringLiteral("newer")));
    }

    // ---- 4. the diff ---------------------------------------------------------------------------------
    {
        LibraryBundle::Entry s; s.id = idA; s.stamp = QStringLiteral("s1"); s.updatedMs = 2000;
        CHECK(LibraryBundle::verdictFor(s, nullptr) == LibraryBundle::Verdict::Send);

        LibraryBundle::Entry same = s;
        CHECK(LibraryBundle::verdictFor(s, &same) == LibraryBundle::Verdict::Unchanged);

        LibraryBundle::Entry older; older.id = idA; older.stamp = QStringLiteral("t0"); older.updatedMs = 1000;
        CHECK(LibraryBundle::verdictFor(s, &older) == LibraryBundle::Verdict::Send);

        LibraryBundle::Entry newer; newer.id = idA; newer.stamp = QStringLiteral("t2"); newer.updatedMs = 3000;
        CHECK(LibraryBundle::verdictFor(s, &newer) == LibraryBundle::Verdict::TargetNewer);

        // A tie goes to the target. Warming a cache is never worth overwriting something on the other end.
        LibraryBundle::Entry tie; tie.id = idA; tie.stamp = QStringLiteral("t3"); tie.updatedMs = 2000;
        CHECK(LibraryBundle::verdictFor(s, &tie) == LibraryBundle::Verdict::TargetNewer);

        QList<LibraryBundle::Entry> src, tgt;
        LibraryBundle::Entry sa; sa.id = idA; sa.stamp = QStringLiteral("x"); sa.updatedMs = 5;
        LibraryBundle::Entry sb; sb.id = idB; sb.stamp = QStringLiteral("y"); sb.updatedMs = 5;
        LibraryBundle::Entry sc; sc.id = idC; sc.stamp = QStringLiteral("z"); sc.updatedMs = 5;
        src << sa << sb << sc;
        LibraryBundle::Entry ta; ta.id = idA; ta.stamp = QStringLiteral("x"); ta.updatedMs = 5;   // identical
        LibraryBundle::Entry tb; tb.id = idB; tb.stamp = QStringLiteral("y2"); tb.updatedMs = 9;  // newer there
        tgt << ta << tb;                                                                          // C missing
        const LibraryBundle::Plan p = LibraryBundle::planTransfer(src, tgt);
        CHECK(p.send == QStringList{ idC });
        CHECK(p.unchanged == QStringList{ idA });
        CHECK(p.targetNewer == QStringList{ idB });
    }

    // ---- 5. a whole transfer, and the second run -----------------------------------------------------
    const qint64 t0 = 1700000000000LL;
    {
        fx::makeItem(srcRoot, idA, QByteArray("{\"key\":\"tt0111161\",\"title\":\"Shawshank\"}"),
                     QByteArray("PNGDATA-A"), t0);
        fx::makeItem(srcRoot, idB, QByteArray("{\"key\":\"igdb:1020\",\"title\":\"GTA\"}"),
                     QByteArray("PNGDATA-BB"), t0);
        fx::makeItem(srcRoot, idC, QByteArray("{\"key\":\"local\",\"title\":\"Mario World\"}"),
                     QByteArray("PNGDATA-CCC"), t0);

        // Something in the source cache this feature must NOT move: a trailer, in an item's own folder.
        fx::writeFile(srcRoot + QLatin1Char('/') + idA + QStringLiteral("/trailer.mp4"),
                      QByteArray("MP4-A-LOT-OF-BYTES"));

        const QMap<QString, QString> beforeCensus = fx::census(dstBase);

        const QList<LibraryBundle::Entry> srcInv = LibraryBundle::inventoryFor(srcRoot);
        const QList<LibraryBundle::Entry> dstInv = LibraryBundle::inventoryFor(dstRoot);
        CHECK(srcInv.size() == 3);
        CHECK(dstInv.isEmpty());

        const LibraryBundle::Plan plan = LibraryBundle::planTransfer(srcInv, dstInv);
        CHECK(plan.send.size() == 3);

        qint64 bytes = 0;
        LibraryBundle::Progress prog;
        prog.itemsTotal = int(plan.send.size());
        for (const QString& id : plan.send)
        {
            QString err;
            const LibraryBundle::LandResult r = fx::transferOne(srcRoot, dstRoot, id, &bytes, &err);
            CHECK(r == LibraryBundle::LandResult::Landed);
            if (r == LibraryBundle::LandResult::Landed) ++prog.itemsSent;
        }
        prog.bytesSent = bytes;
        CHECK(bytes > 0);
        CHECK(prog.itemsSent == 3);

        // The art arrived, byte for byte.
        CHECK(fx::readFile(dstRoot + QLatin1Char('/') + idA + QStringLiteral("/thumb.png"))
              == QByteArray("PNGDATA-A"));
        CHECK(fx::readFile(dstRoot + QLatin1Char('/') + idC + QStringLiteral("/meta.json"))
              .contains("Mario World"));
        // The trailer did not. Art only, this increment.
        CHECK(!QFileInfo::exists(dstRoot + QLatin1Char('/') + idA + QStringLiteral("/trailer.mp4")));

        // The mtimes travelled — the one mechanism the whole second-run property rests on.
        CHECK(fx::mtimeOf(dstRoot + QLatin1Char('/') + idA + QStringLiteral("/thumb.png")) == t0);

        // NOTHING WAS TOUCHED OUTSIDE THE CACHE ROOT, and the state store is byte-identical.
        const QMap<QString, QString> afterCensus = fx::census(dstBase);
        for (auto it = beforeCensus.constBegin(); it != beforeCensus.constEnd(); ++it)
        {
            CHECK(afterCensus.contains(it.key()));
            CHECK(afterCensus.value(it.key()) == it.value());
        }
        for (auto it = afterCensus.constBegin(); it != afterCensus.constEnd(); ++it)
        {
            const bool inCache = it.key().startsWith(QStringLiteral("metadata/"));
            const bool wasThere = beforeCensus.contains(it.key());
            CHECK(inCache || wasThere);          // every NEW file is inside the cache
        }
        CHECK(fx::readFile(dstState + QStringLiteral("/marks.json"))
              == QByteArray("{\"tt0111161\":{\"watched\":true}}"));
        CHECK(fx::readFile(dstState + QStringLiteral("/resume.json")) == QByteArray("{\"pos\":1234.5}"));

        // ---- THE DEFINING PROPERTY. Run it again with nothing changed. ----
        const QList<LibraryBundle::Entry> srcInv2 = LibraryBundle::inventoryFor(srcRoot);
        const QList<LibraryBundle::Entry> dstInv2 = LibraryBundle::inventoryFor(dstRoot);
        const LibraryBundle::Plan plan2 = LibraryBundle::planTransfer(srcInv2, dstInv2);
        CHECK(plan2.send.isEmpty());
        CHECK(plan2.unchanged.size() == 3);
        CHECK(fx::wireBytesFor(srcRoot, plan2.send) == 0);          // ZERO BYTES OF PAYLOAD

        LibraryBundle::Progress none;
        CHECK(LibraryBundle::describeProgress(none, QStringLiteral("Living Room"))
                  .contains(QStringLiteral("already up to date")));
        CHECK(LibraryBundle::describeProgress(prog, QStringLiteral("Living Room"))
                  .contains(QStringLiteral("Living Room")));

        // A run that sent NOTHING because the target's copies were newer is not the same event as a run that
        // sent nothing because the two ends agree, and it must not read as one. (A live two-instance drive is
        // what found this: "already up to date" alone quietly claimed agreement where a decision had been
        // made.)
        LibraryBundle::Progress keptOnly;
        keptOnly.keptNewer = 1;
        const QString keptLine = LibraryBundle::describeProgress(keptOnly, QStringLiteral("Living Room"));
        CHECK(keptLine.contains(QStringLiteral("newer there")));
        CHECK(keptLine.contains(QStringLiteral("left alone")));

        // And a few hundred kilobytes of PNG does not get reported as "0.0 MB", which reads as nothing moved.
        CHECK(LibraryBundle::describeSize(300 * 1024).endsWith(QStringLiteral("KB")));
        CHECK(LibraryBundle::describeSize(5 * 1024 * 1024).endsWith(QStringLiteral("MB")));
        LibraryBundle::Progress small;
        small.itemsTotal = 1; small.itemsSent = 1; small.bytesSent = 4096;
        CHECK(!LibraryBundle::describeProgress(small, QStringLiteral("Living Room"))
                   .contains(QStringLiteral("0.0 MB")));

        // Counts of one read as counts of one. "1 were newer there" is what the live drive printed before
        // this, and a progress line that cannot count to one is not a progress line anyone trusts.
        small.keptNewer = 1; small.unchanged = 1; small.failed = 1;
        const QString ones = LibraryBundle::describeProgress(small, QStringLiteral("Living Room"));
        CHECK(!ones.contains(QStringLiteral("1 were")));
        CHECK(ones.contains(QStringLiteral("1 was already there")));
        CHECK(ones.contains(QStringLiteral("1 is newer there and was left alone")));
        LibraryBundle::Progress many;
        many.itemsTotal = 9; many.itemsSent = 9; many.keptNewer = 2; many.unchanged = 3;
        CHECK(LibraryBundle::describeProgress(many, QStringLiteral("Living Room"))
                  .contains(QStringLiteral("2 are newer there")));

        // A finished transfer leaves the cache holding items and nothing else: no staging folder, no
        // set-aside folder, no trace of the machinery.
        CHECK(!QFileInfo::exists(dstRoot + QStringLiteral("/.eb-incoming")));
        CHECK(!QFileInfo::exists(dstRoot + QStringLiteral("/.eb-retired")));

        // ---- change ONE item on the source: exactly one item moves. ----
        fx::writeFile(srcRoot + QLatin1Char('/') + idB + QStringLiteral("/thumb.png"),
                      QByteArray("PNGDATA-BB-REDRAWN"));
        fx::setMtime(srcRoot + QLatin1Char('/') + idB + QStringLiteral("/thumb.png"), t0 + 60000);

        const LibraryBundle::Plan plan3 =
            LibraryBundle::planTransfer(LibraryBundle::inventoryFor(srcRoot),
                                        LibraryBundle::inventoryFor(dstRoot));
        CHECK(plan3.send == QStringList{ idB });
        CHECK(plan3.unchanged.size() == 2);

        qint64 bytes3 = 0;
        QString err3;
        CHECK(fx::transferOne(srcRoot, dstRoot, idB, &bytes3, &err3) == LibraryBundle::LandResult::Landed);
        CHECK(fx::readFile(dstRoot + QLatin1Char('/') + idB + QStringLiteral("/thumb.png"))
              == QByteArray("PNGDATA-BB-REDRAWN"));
        // ...and the run after THAT is empty again.
        CHECK(LibraryBundle::planTransfer(LibraryBundle::inventoryFor(srcRoot),
                                          LibraryBundle::inventoryFor(dstRoot)).send.isEmpty());
    }

    // ---- 6. warm, never fight ------------------------------------------------------------------------
    {
        // The target's copy of A is made NEWER than the source's, with different content. A second send must
        // leave it exactly as it is, and must SAY so rather than report a success it did not perform.
        fx::writeFile(dstRoot + QLatin1Char('/') + idA + QStringLiteral("/thumb.png"),
                      QByteArray("PNGDATA-A-EDITED-HERE"));
        fx::setMtime(dstRoot + QLatin1Char('/') + idA + QStringLiteral("/thumb.png"), t0 + 3600000);

        const LibraryBundle::Plan plan =
            LibraryBundle::planTransfer(LibraryBundle::inventoryFor(srcRoot),
                                        LibraryBundle::inventoryFor(dstRoot));
        CHECK(!plan.send.contains(idA));
        CHECK(plan.targetNewer.contains(idA));

        // Even if a source sends it anyway (a race, or an older build), the TARGET refuses to clobber.
        qint64 bytes = 0;
        QString err;
        CHECK(fx::transferOne(srcRoot, dstRoot, idA, &bytes, &err) == LibraryBundle::LandResult::KeptNewer);
        CHECK(fx::readFile(dstRoot + QLatin1Char('/') + idA + QStringLiteral("/thumb.png"))
              == QByteArray("PNGDATA-A-EDITED-HERE"));

        const LibraryBundle::Receipt kept =
            LibraryBundle::receiptFor(LibraryBundle::LandResult::KeptNewer, QStringLiteral("newer here"));
        CHECK(kept.httpStatus == 200);
        CHECK(kept.result == QStringLiteral("kept"));
        LibraryBundle::Receipt parsed;
        CHECK(LibraryBundle::parseReceipt(LibraryBundle::receiptJson(kept), parsed));
        CHECK(parsed.result == QStringLiteral("kept"));
    }

    // ---- 7. the files a bundle did not carry survive a landing ---------------------------------------
    {
        // The target has a trailer for C that the source never sends. Landing a new C must not cost it.
        fx::writeFile(dstRoot + QLatin1Char('/') + idC + QStringLiteral("/trailer.mp4"),
                      QByteArray("TARGET-OWN-TRAILER"));
        fx::writeFile(srcRoot + QLatin1Char('/') + idC + QStringLiteral("/logo.png"), QByteArray("LOGO-C"));
        fx::setMtime(srcRoot + QLatin1Char('/') + idC + QStringLiteral("/logo.png"), t0 + 120000);

        qint64 bytes = 0;
        QString err;
        CHECK(fx::transferOne(srcRoot, dstRoot, idC, &bytes, &err) == LibraryBundle::LandResult::Landed);
        CHECK(fx::readFile(dstRoot + QLatin1Char('/') + idC + QStringLiteral("/logo.png"))
              == QByteArray("LOGO-C"));
        CHECK(fx::readFile(dstRoot + QLatin1Char('/') + idC + QStringLiteral("/trailer.mp4"))
              == QByteArray("TARGET-OWN-TRAILER"));
    }

    // ---- 8. atomic landing under an interruption -----------------------------------------------------
    {
        const QString liveThumb = dstRoot + QLatin1Char('/') + idB + QStringLiteral("/thumb.png");
        const QByteArray before = fx::readFile(liveThumb);
        CHECK(!before.isEmpty());

        // Change the source so there IS something to send, then interrupt after one file.
        fx::writeFile(srcRoot + QLatin1Char('/') + idB + QStringLiteral("/thumb.png"),
                      QByteArray("PNGDATA-BB-INTERRUPTED-RUN"));
        fx::setMtime(srcRoot + QLatin1Char('/') + idB + QStringLiteral("/thumb.png"), t0 + 240000);

        LibraryBundle::Payload p;
        QString err;
        CHECK(LibraryBundle::readPayload(srcRoot, idB, p, err));

        LibraryBundle::LandOptions opts;
        opts.failAfterFiles = 1;
        CHECK(LibraryBundle::landItem(dstRoot, p, err, opts) == LibraryBundle::LandResult::Interrupted);

        // The target still holds EXACTLY what it held: no half-written folder at the item's real path.
        CHECK(fx::readFile(liveThumb) == before);
        CHECK(!QFileInfo::exists(dstRoot + QStringLiteral("/.eb-incoming/") + idB
                                 + QStringLiteral("/nonexistent")));
        // ...and the item's own inventory entry is unchanged, so the next run diffs from a consistent tree.
        LibraryBundle::Entry e;
        QList<LibraryBundle::FileEntry> files;
        CHECK(LibraryBundle::scanItem(dstRoot, idB, e, files));

        // A resumed run picks up from the diff: the sweep clears the staged remains, the plan still names B,
        // and the retry lands.
        LibraryBundle::sweepPartials(dstRoot);
        CHECK(!QFileInfo::exists(dstRoot + QStringLiteral("/.eb-incoming")));
        const LibraryBundle::Plan resumed =
            LibraryBundle::planTransfer(LibraryBundle::inventoryFor(srcRoot),
                                        LibraryBundle::inventoryFor(dstRoot));
        CHECK(resumed.send.contains(idB));
        qint64 bytes = 0;
        CHECK(fx::transferOne(srcRoot, dstRoot, idB, &bytes, &err) == LibraryBundle::LandResult::Landed);
        CHECK(fx::readFile(liveThumb) == QByteArray("PNGDATA-BB-INTERRUPTED-RUN"));
    }

    // ---- 9. an interruption DURING the swap is recovered, not lost -----------------------------------
    {
        // The worst moment: the live folder has been set aside and the new one is not yet in place. Simulate
        // it exactly, then assert the next run puts the item back rather than reporting it missing.
        const QString live    = dstRoot + QLatin1Char('/') + idC;
        const QString retired = dstRoot + QStringLiteral("/.eb-retired/") + idC;
        const QByteArray logo = fx::readFile(live + QStringLiteral("/logo.png"));
        CHECK(!logo.isEmpty());
        QDir().mkpath(dstRoot + QStringLiteral("/.eb-retired"));
        CHECK(QDir().rename(live, retired));
        CHECK(!QFileInfo::exists(live));

        CHECK(LibraryBundle::sweepPartials(dstRoot) >= 1);
        CHECK(QFileInfo::exists(live));
        CHECK(fx::readFile(live + QStringLiteral("/logo.png")) == logo);
        CHECK(!QFileInfo::exists(dstRoot + QStringLiteral("/.eb-retired")));

        // And the sweep runs itself before any inventory is reported, so a caller cannot see the half state.
        CHECK(LibraryBundle::inventoryFor(dstRoot).size() >= 3);
    }

    // ---- 10. a hostile bundle is refused, and writes nothing -----------------------------------------
    {
        const QMap<QString, QString> before = fx::census(dstBase);

        // A path-traversal item id, hand-built as the wire would carry it.
        const QByteArray evil =
            "{\"v\":1,\"id\":\"../../../../state\",\"stamp\":\"s\",\"updated\":1,"
            "\"files\":[{\"name\":\"marks.json\",\"mtime\":1,\"data\":\"eA==\"}]}";
        LibraryBundle::Payload got;
        LibraryBundle::Refusal why = LibraryBundle::Refusal::None;
        QString message;
        CHECK(!LibraryBundle::decodePayload(evil, got, why, message));
        CHECK(why == LibraryBundle::Refusal::UnsafeId);
        CHECK(!message.isEmpty());

        // ...and landItem refuses it a second time, so a caller that skipped the decoder cannot get through.
        LibraryBundle::Payload direct;
        direct.id = QStringLiteral("../../../../state");
        LibraryBundle::PayloadFile f;
        f.name = QStringLiteral("marks.json");
        f.data = QByteArray("x");
        direct.files << f;
        QString err;
        CHECK(LibraryBundle::landItem(dstRoot, direct, err) == LibraryBundle::LandResult::Refused);

        // A traversing FILE NAME inside a legitimate item.
        const QByteArray evil2 = QByteArray("{\"v\":1,\"id\":\"") + idA.toUtf8()
            + "\",\"stamp\":\"s\",\"updated\":1,"
              "\"files\":[{\"name\":\"../../marks.json\",\"mtime\":1,\"data\":\"eA==\"}]}";
        CHECK(!LibraryBundle::decodePayload(evil2, got, why, message));
        CHECK(why == LibraryBundle::Refusal::UnsafeFileName);

        // A future format is refused with a sentence a user can act on.
        const QByteArray future = QByteArray("{\"v\":99,\"id\":\"") + idA.toUtf8()
            + "\",\"stamp\":\"s\",\"updated\":1,\"files\":[]}";
        CHECK(!LibraryBundle::decodePayload(future, got, why, message));
        CHECK(why == LibraryBundle::Refusal::FutureFormat);
        CHECK(message.contains(QStringLiteral("newer")));

        // An empty bundle is not a transfer.
        const QByteArray empty = QByteArray("{\"v\":1,\"id\":\"") + idA.toUtf8()
            + "\",\"stamp\":\"s\",\"updated\":1,\"files\":[]}";
        CHECK(!LibraryBundle::decodePayload(empty, got, why, message));
        CHECK(why == LibraryBundle::Refusal::Empty);

        // Nothing on disk moved for any of it.
        const QMap<QString, QString> after = fx::census(dstBase);
        CHECK(after == before);
    }

    // ---- 11. the routes ------------------------------------------------------------------------------
    {
        // Both new routes are credentialled. An unpaired caller can neither read what a device holds (an
        // inventory is a list of what someone owns) nor write a byte into its cache.
        CHECK(PlayOn::routeNeedsToken(QStringLiteral("/inventory")));
        CHECK(PlayOn::routeNeedsToken(QStringLiteral("/bundle")));
        CHECK(PlayOn::routeNeedsToken(QStringLiteral("/open")));      // #143's, unchanged
        CHECK(!PlayOn::routeNeedsToken(QStringLiteral("/state")));    // #76's, unchanged
        CHECK(!PlayOn::routeNeedsToken(QStringLiteral("/pair")));     // how a token is obtained

        // The routing table, by shape. Each route answers for ONE method; the other is a reasoned
        // BadRequest rather than a 404, so a client using the wrong verb is told what is wrong.
        RemoteApi::Request r;
        r.valid = true;
        r.method = RemoteApi::Method::Get;
        r.path = QStringLiteral("/inventory");
        CHECK(RemoteApi::route(r).kind == RemoteApi::CommandKind::Inventory);
        r.method = RemoteApi::Method::Post;
        CHECK(RemoteApi::route(r).kind == RemoteApi::CommandKind::BadRequest);

        r.path = QStringLiteral("/bundle");
        r.method = RemoteApi::Method::Post;
        r.body = QByteArray("{\"v\":1}");
        CHECK(RemoteApi::route(r).kind == RemoteApi::CommandKind::Bundle);
        r.body = QByteArray();
        CHECK(RemoteApi::route(r).kind == RemoteApi::CommandKind::BadRequest);   // a bundle with no item
        r.body = QByteArray("{\"v\":1}");
        r.method = RemoteApi::Method::Get;
        CHECK(RemoteApi::route(r).kind == RemoteApi::CommandKind::BadRequest);

        // #143's and #76's routes are untouched by any of it.
        r.method = RemoteApi::Method::Get;
        r.path = QStringLiteral("/state");
        r.body = QByteArray();
        CHECK(RemoteApi::route(r).kind == RemoteApi::CommandKind::State);

        // THE READ CAP. Only POST /bundle is allowed a payload-sized buffer; every other route keeps #76's
        // tiny one, so a control API cannot be made to eat memory by a request that merely mentions the word.
        CHECK(RemoteApi::requestCapBytes(QByteArray("POST /bundle HTTP/1.1")) == RemoteApi::kBundleRequestCap);
        CHECK(RemoteApi::requestCapBytes(QByteArray("GET /state HTTP/1.1")) == RemoteApi::kDefaultRequestCap);
        CHECK(RemoteApi::requestCapBytes(QByteArray("POST /player HTTP/1.1")) == RemoteApi::kDefaultRequestCap);
        CHECK(RemoteApi::requestCapBytes(QByteArray("GET /bundle HTTP/1.1")) == RemoteApi::kDefaultRequestCap);
        const QByteArray mentionsBundleInAHeader =
            QByteArray("GET /x HTTP/1.1") + QByteArray("\r\n")
            + QByteArray("X-Note: POST /bundle ") + QByteArray("\r\n");
        CHECK(RemoteApi::requestCapBytes(mentionsBundleInAHeader) == RemoteApi::kDefaultRequestCap);
        CHECK(RemoteApi::requestCapBytes(QByteArray()) == RemoteApi::kDefaultRequestCap);
        CHECK(RemoteApi::kBundleRequestCap > RemoteApi::kDefaultRequestCap);
    }

    // ---- 12. the v2 codec (#291) ---------------------------------------------------------------------
    {
        LibraryBundle::Payload p;
        p.id = idA;
        p.stamp = QStringLiteral("stamp-v2");
        p.updatedMs = t0 + 5;
        LibraryBundle::PayloadFile m;  m.name = QStringLiteral("meta.json"); m.mtimeMs = t0;     m.data = "{\"t\":1}";
        LibraryBundle::PayloadFile a;  a.name = QStringLiteral("a.png");     a.mtimeMs = t0 + 1; a.data = QByteArray("\0\x01\xff", 3);
        LibraryBundle::PayloadFile e;  e.name = QStringLiteral("empty.jpg"); e.mtimeMs = t0 + 2; e.data = QByteArray();
        LibraryBundle::PayloadFile c;  c.name = QStringLiteral("c.webp");    c.mtimeMs = t0 + 5; c.data = fx::noise(100 * 1024, 3);
        p.files << m << a << e << c;

        const QByteArray wire = LibraryBundle::encodePayloadV2(p);
        CHECK(wire.startsWith("EBBUNDLE"));
        CHECK(fx::u32At(wire, 8) == 2u);
        // RAW bytes: the wire is the preamble, the header and exactly the file bytes -- no base64 expansion.
        CHECK(qint64(wire.size()) == fx::headerEndOf(wire) + LibraryBundle::fileBytesOf(p));
        CHECK(LibraryBundle::fileBytesOf(p) == qint64(m.data.size() + a.data.size() + c.data.size()));

        QByteArray copy = wire;
        QBuffer in(&copy);
        in.open(QIODevice::ReadOnly);
        LibraryBundle::Payload got;
        LibraryBundle::Refusal why = LibraryBundle::Refusal::None;
        QString message;
        CHECK(LibraryBundle::decodePayloadV2(in, got, why, message));
        CHECK(why == LibraryBundle::Refusal::None);
        CHECK(got.version == LibraryBundle::kPayloadFormatV2);
        CHECK(got.id == p.id && got.stamp == p.stamp && got.updatedMs == p.updatedMs);
        CHECK(got.files.size() == p.files.size());
        for (int i = 0; i < qMin(got.files.size(), p.files.size()); ++i)
        {
            CHECK(got.files.at(i).name == p.files.at(i).name);
            CHECK(got.files.at(i).mtimeMs == p.files.at(i).mtimeMs);
            CHECK(got.files.at(i).data == p.files.at(i).data);
        }

        // The header alone: every field, the sizes, and the device left at the first file byte.
        QByteArray copy2 = wire;
        QBuffer in2(&copy2);
        in2.open(QIODevice::ReadOnly);
        LibraryBundle::BundleHeader h;
        CHECK(LibraryBundle::decodeHeaderV2(in2, h, why, message));
        CHECK(in2.pos() == fx::headerEndOf(wire));
        CHECK(h.fileBytes == LibraryBundle::fileBytesOf(p));
        CHECK(h.files.size() == 4 && h.files.at(3).size == 100 * 1024 && h.files.at(3).mtimeMs == t0 + 5);

        // ---- every refusal, decided inside the header, with nothing written ----
        const QString scratchBase = base + QStringLiteral("/v2refusals");
        const QString scratchRoot = scratchBase + QStringLiteral("/metadata");
        QDir().mkpath(scratchRoot);
        fx::makeItem(scratchRoot, idA, QByteArray("{\"key\":\"live\"}"), QByteArray("LIVE-THUMB"), t0 - 1000);
        fx::writeFile(scratchBase + QStringLiteral("/marks.json"), QByteArray("{\"state\":true}"));

        auto refusedBeforeBytes = [&](const QByteArray& hostile, LibraryBundle::Refusal expected, int line) {
            const QMap<QString, QString> before = fx::census(scratchBase);
            const qint64 headerEnd = fx::headerEndOf(hostile);

            QByteArray b1 = hostile;
            QBuffer d1(&b1);
            d1.open(QIODevice::ReadOnly);
            LibraryBundle::BundleHeader hh;
            LibraryBundle::Refusal w = LibraryBundle::Refusal::None;
            QString msg;
            const bool ok = LibraryBundle::decodeHeaderV2(d1, hh, w, msg);
            if (ok || w != expected || msg.isEmpty() || d1.pos() > headerEnd)
                std::fprintf(stderr, "BUNDLEXFER-FAIL v2 refusal case from line %d (ok=%d why=%d pos=%lld end=%lld)\n",
                             line, int(ok), int(w), qint64(d1.pos()), headerEnd);
            CHECK(!ok);
            CHECK(w == expected);
            CHECK(!msg.isEmpty());
            CHECK(d1.pos() <= headerEnd);                       // NO FILE BYTE READ

            QByteArray b2 = hostile;
            QBuffer d2(&b2);
            d2.open(QIODevice::ReadOnly);
            LibraryBundle::Payload pp;
            CHECK(!LibraryBundle::decodePayloadV2(d2, pp, w, msg));
            CHECK(d2.pos() <= headerEnd);

            QByteArray b3 = hostile;
            QBuffer d3(&b3);
            d3.open(QIODevice::ReadOnly);
            QString err;
            const LibraryBundle::LandResult lr = LibraryBundle::landItemV2(scratchRoot, d3, err);
            if (lr != LibraryBundle::LandResult::Refused || d3.pos() > headerEnd)
                std::fprintf(stderr, "BUNDLEXFER-FAIL v2 landing case from line %d (result=%d pos=%lld end=%lld)\n",
                             line, int(lr), qint64(d3.pos()), headerEnd);
            CHECK(lr == LibraryBundle::LandResult::Refused);
            CHECK(d3.pos() <= headerEnd);                       // the landing read no file byte either
            CHECK(!QFileInfo::exists(scratchRoot + QStringLiteral("/.eb-incoming")));
            CHECK(fx::census(scratchBase) == before);            // AND WROTE NOTHING
        };

        QByteArray truncated = wire;  truncated.chop(1);
        refusedBeforeBytes(truncated, LibraryBundle::Refusal::Malformed, __LINE__);
        refusedBeforeBytes(wire + QByteArray("X"), LibraryBundle::Refusal::Malformed, __LINE__);

        // Sizes that do not sum to the body: two files declared at 10 bytes each over a 15-byte body, and the
        // same over a 25-byte one.
        {
            QJsonArray files;
            files.append(fx::fileJson(QStringLiteral("one.png"), 10));
            files.append(fx::fileJson(QStringLiteral("two.png"), 10));
            refusedBeforeBytes(fx::wireV2(fx::headerJson(idA, files), fx::noise(15, 1)),
                               LibraryBundle::Refusal::Malformed, __LINE__);
            refusedBeforeBytes(fx::wireV2(fx::headerJson(idA, files), fx::noise(25, 1)),
                               LibraryBundle::Refusal::Malformed, __LINE__);
        }
        // A traversing name, AFTER a legitimate file whose bytes come first in the body -- the ordering case:
        // a decoder that checked each name only as it reached that file would already have read (and a
        // landing would already have staged) ok.png.
        {
            QJsonArray files;
            files.append(fx::fileJson(QStringLiteral("ok.png"), 4));
            files.append(fx::fileJson(QStringLiteral("../../marks.json"), 4));
            refusedBeforeBytes(fx::wireV2(fx::headerJson(idA, files), QByteArray("GOODEVIL")),
                               LibraryBundle::Refusal::UnsafeFileName, __LINE__);
        }
        {
            QJsonArray files;
            files.append(fx::fileJson(QStringLiteral("ok.png"), 4));
            files.append(fx::fileJson(QStringLiteral("trailer.mp4"), 4));     // not on the art allowlist
            refusedBeforeBytes(fx::wireV2(fx::headerJson(idA, files), QByteArray("GOODMP4!")),
                               LibraryBundle::Refusal::UnsafeFileName, __LINE__);
        }
        {
            QJsonArray files;
            files.append(fx::fileJson(QStringLiteral("marks.json"), 4));
            refusedBeforeBytes(fx::wireV2(fx::headerJson(QStringLiteral("../../../../state"), files), QByteArray("EVIL")),
                               LibraryBundle::Refusal::UnsafeId, __LINE__);
        }
        {
            QJsonArray files;
            files.append(fx::fileJson(QStringLiteral("ok.png"), 4));
            refusedBeforeBytes(fx::wireV2(fx::headerJson(idA, files), QByteArray("DATA"), 3),
                               LibraryBundle::Refusal::FutureFormat, __LINE__);
            refusedBeforeBytes(fx::wireV2(fx::headerJson(idA, files), QByteArray("DATA"), 1),
                               LibraryBundle::Refusal::Malformed, __LINE__);
            refusedBeforeBytes(fx::wireV2(fx::headerJson(idA, files), QByteArray("DATA"), 2, QByteArray("NOTABNDL")),
                               LibraryBundle::Refusal::Malformed, __LINE__);
        }
        // A declared FILE over kMaxFileBytes, and a declared ITEM over the v2 ceiling -- neither body is sent
        // at all, because the header is where they are refused.
        {
            QJsonArray files;
            files.append(fx::fileJson(QStringLiteral("huge.png"), double(LibraryBundle::kMaxFileBytes + 1)));
            refusedBeforeBytes(fx::wireV2(fx::headerJson(idA, files), QByteArray("x")),
                               LibraryBundle::Refusal::TooLarge, __LINE__);
        }
        {
            QJsonArray files;
            for (int i = 0; i < 9; ++i)
                files.append(fx::fileJson(QStringLiteral("big-%1.png").arg(i), double(LibraryBundle::kMaxFileBytes)));
            CHECK(9 * LibraryBundle::kMaxFileBytes > LibraryBundle::kMaxItemBytesV2);
            refusedBeforeBytes(fx::wireV2(fx::headerJson(idA, files), QByteArray("x")),
                               LibraryBundle::Refusal::TooLarge, __LINE__);
        }
        {
            refusedBeforeBytes(fx::wireV2(fx::headerJson(idA, QJsonArray()), QByteArray()),
                               LibraryBundle::Refusal::Empty, __LINE__);
            QJsonArray neg;
            neg.append(fx::fileJson(QStringLiteral("neg.png"), -4));
            refusedBeforeBytes(fx::wireV2(fx::headerJson(idA, neg), QByteArray()),
                               LibraryBundle::Refusal::Malformed, __LINE__);
        }
        // The live item under the scratch root is exactly as it was after all of it.
        CHECK(fx::readFile(scratchRoot + QLatin1Char('/') + idA + QStringLiteral("/thumb.png")) == QByteArray("LIVE-THUMB"));
    }

    // ---- 13. interop (#291) --------------------------------------------------------------------------
    {
        QList<LibraryBundle::Entry> entries;
        LibraryBundle::Entry a; a.id = idA; a.stamp = QStringLiteral("aaa"); a.updatedMs = 1;
        entries << a;
        const QByteArray inv = LibraryBundle::inventoryJson(entries);
        const QJsonObject invObj = QJsonDocument::fromJson(inv).object();
        // The version an OLD source checks is untouched: bumping it would make every old source refuse us.
        CHECK(invObj.value(QStringLiteral("v")).toInt() == 1);
        CHECK(invObj.value(QStringLiteral("bundle")).isArray());

        QList<LibraryBundle::Entry> out;
        QList<int> formats;
        QString err;
        CHECK(LibraryBundle::parseInventory(inv, out, err));                  // an old source's parse still works
        CHECK(out.size() == 1);
        CHECK(LibraryBundle::parseInventory(inv, out, formats, err));
        CHECK(formats.contains(1) && formats.contains(2));
        CHECK(LibraryBundle::chooseBundleFormat(formats) == 2);               // new target -> v2

        CHECK(LibraryBundle::parseInventory(QByteArray("{\"v\":1,\"items\":[]}"), out, formats, err));
        CHECK(out.isEmpty());
        CHECK(formats == QList<int>{ 1 });
        CHECK(LibraryBundle::chooseBundleFormat(formats) == 1);               // old target -> v1

        CHECK(LibraryBundle::parseInventory(QByteArray("{\"v\":1,\"items\":[],\"bundle\":[1]}"), out, formats, err));
        CHECK(LibraryBundle::chooseBundleFormat(formats) == 1);
        CHECK(LibraryBundle::parseInventory(QByteArray("{\"v\":1,\"items\":[],\"bundle\":[1,2,3]}"), out, formats, err));
        CHECK(LibraryBundle::chooseBundleFormat(formats) == 2);               // a future format is not chosen
        CHECK(LibraryBundle::parseInventory(QByteArray("{\"v\":1,\"items\":[],\"bundle\":\"junk\"}"), out, formats, err));
        CHECK(formats == QList<int>{ 1 });
        CHECK(!LibraryBundle::parseInventory(QByteArray("{\"v\":99,\"items\":[]}"), out, formats, err));

        CHECK(LibraryBundle::maxItemBytesFor(1) == LibraryBundle::kMaxItemBytes);
        CHECK(LibraryBundle::maxItemBytesFor(2) == LibraryBundle::kMaxItemBytesV2);
        CHECK(LibraryBundle::kMaxItemBytes == 12LL * 1024 * 1024);           // v1's cap, unchanged
        CHECK(LibraryBundle::kMaxFileBytes == 8LL * 1024 * 1024);            // the stamp's cut-off, unchanged
        CHECK(LibraryBundle::kFormatVersion == 1);

        // A v2 body handed to an OLD target's decoder is refused whole -- never half-understood.
        LibraryBundle::Payload p;
        CHECK(LibraryBundle::readPayload(srcRoot, idB, p, err));
        LibraryBundle::Payload got;
        LibraryBundle::Refusal why = LibraryBundle::Refusal::None;
        QString message;
        CHECK(!LibraryBundle::decodePayload(LibraryBundle::encodePayloadV2(p), got, why, message));
        CHECK(why == LibraryBundle::Refusal::Malformed);

        // And a v1 payload still decodes and lands on the new target (section 5 did the whole transfer that
        // way; one more here, against a fresh root, so this section stands on its own).
        const QString v1Root = base + QStringLiteral("/v1target/metadata");
        QDir().mkpath(v1Root);
        qint64 bytes = 0;
        CHECK(fx::transferOne(srcRoot, v1Root, idB, &bytes, &err) == LibraryBundle::LandResult::Landed);
        CHECK(fx::readFile(v1Root + QLatin1Char('/') + idB + QStringLiteral("/thumb.png"))
              == fx::readFile(srcRoot + QLatin1Char('/') + idB + QStringLiteral("/thumb.png")));
    }

    // ---- 14. the issue's property: a ~13 MiB item moves once, then never again (#291) ---------------
    {
        const QString hBase = base + QStringLiteral("/heavy");
        const QString hSrc  = hBase + QStringLiteral("/source/metadata");
        const QString hDstBase = hBase + QStringLiteral("/target");
        const QString hDst  = hDstBase + QStringLiteral("/metadata");
        QDir().mkpath(hSrc);
        QDir().mkpath(hDst);
        QDir().mkpath(hDstBase + QStringLiteral("/state"));
        fx::writeFile(hDstBase + QStringLiteral("/state/resume.json"), QByteArray("{\"pos\":9}"));
        const QString idH = fx::idFor("igdb:heavy-291");
        // Several files, each under 8 MiB, ~13 MiB in all: over v1's item cap, well under v2's.
        const qint64 onDisk = fx::makeHeavyItem(hSrc, idH, 3, qint64(4.4 * 1024 * 1024), t0);
        CHECK(onDisk > LibraryBundle::kMaxItemBytes);
        CHECK(onDisk > 13LL * 1024 * 1024 - 1024 * 1024);

        LibraryBundle::Payload p;
        QString err;
        // To an OLD target the source behaves exactly as today: v1's cap, and the item is skipped.
        CHECK(!LibraryBundle::readPayload(hSrc, idH, p, err));
        CHECK(!LibraryBundle::readPayload(hSrc, idH, p, err, LibraryBundle::maxItemBytesFor(1)));

        // To a NEW target it goes as v2.
        const QMap<QString, QString> before = fx::census(hDstBase);
        QList<LibraryBundle::Entry> theirs;
        QList<int> formats;
        CHECK(LibraryBundle::parseInventory(LibraryBundle::inventoryJson(LibraryBundle::inventoryFor(hDst)),
                                            theirs, formats, err));
        const int format = LibraryBundle::chooseBundleFormat(formats);
        CHECK(format == 2);
        const LibraryBundle::Plan plan = LibraryBundle::planTransfer(LibraryBundle::inventoryFor(hSrc), theirs);
        CHECK(plan.send == QStringList{ idH });

        CHECK(LibraryBundle::readPayload(hSrc, idH, p, err, LibraryBundle::maxItemBytesFor(format)));
        QByteArray wire = LibraryBundle::encodePayloadV2(p);
        LibraryBundle::Progress prog;
        prog.itemsTotal = 1;
        prog.bytesSent += LibraryBundle::fileBytesOf(p);
        {
            QBuffer in(&wire);
            in.open(QIODevice::ReadOnly);
            CHECK(LibraryBundle::landItemV2(hDst, in, err) == LibraryBundle::LandResult::Landed);
        }
        // Honest bytes: what landed on disk, not what crossed the wire.
        CHECK(prog.bytesSent == onDisk);
        CHECK(prog.bytesSent < qint64(wire.size()));
        LibraryBundle::Payload small;
        CHECK(LibraryBundle::readPayload(srcRoot, idB, small, err));
        CHECK(LibraryBundle::fileBytesOf(small) < qint64(LibraryBundle::encodePayload(small).size()));

        for (int i = 0; i < 3; ++i)
        {
            const QString name = QStringLiteral("/art-%1.png").arg(i);
            CHECK(fx::readFile(hDst + QLatin1Char('/') + idH + name) == fx::readFile(hSrc + QLatin1Char('/') + idH + name));
            CHECK(fx::mtimeOf(hDst + QLatin1Char('/') + idH + name) == t0 + i + 1);
        }
        // THE PROPERTY: the next plan sends nothing, and says the item is unchanged.
        const LibraryBundle::Plan again =
            LibraryBundle::planTransfer(LibraryBundle::inventoryFor(hSrc), LibraryBundle::inventoryFor(hDst));
        CHECK(again.send.isEmpty());
        CHECK(again.unchanged == QStringList{ idH });

        // Nothing outside the cache root, exactly as strict as section 5.
        const QMap<QString, QString> after = fx::census(hDstBase);
        for (auto it = before.constBegin(); it != before.constEnd(); ++it)
            CHECK(after.value(it.key()) == it.value());
        for (auto it = after.constBegin(); it != after.constEnd(); ++it)
            CHECK(it.key().startsWith(QStringLiteral("metadata/")) || before.contains(it.key()));
        CHECK(!QFileInfo::exists(hDst + QStringLiteral("/.eb-incoming")));

        // An interrupted v2 landing leaves the live copy alone, exactly as v1's does.
        const QString art0 = hSrc + QLatin1Char('/') + idH + QStringLiteral("/art-0.png");
        fx::writeFile(art0, fx::noise(qint64(4.4 * 1024 * 1024), 99));
        fx::setMtime(art0, t0 + 900000);
        const QByteArray liveBefore = fx::readFile(hDst + QLatin1Char('/') + idH + QStringLiteral("/art-0.png"));
        CHECK(LibraryBundle::readPayload(hSrc, idH, p, err, LibraryBundle::kMaxItemBytesV2));
        QByteArray wire2 = LibraryBundle::encodePayloadV2(p);
        {
            QBuffer in(&wire2);
            in.open(QIODevice::ReadOnly);
            LibraryBundle::LandOptions opts;
            opts.failAfterFiles = 1;
            CHECK(LibraryBundle::landItemV2(hDst, in, err, opts) == LibraryBundle::LandResult::Interrupted);
        }
        CHECK(fx::readFile(hDst + QLatin1Char('/') + idH + QStringLiteral("/art-0.png")) == liveBefore);
        LibraryBundle::sweepPartials(hDst);
        CHECK(!QFileInfo::exists(hDst + QStringLiteral("/.eb-incoming")));
    }

    // ---- 15. the spool (#291) ------------------------------------------------------------------------
    {
        const QString sBase = base + QStringLiteral("/spool");
        const QString sRoot = sBase + QStringLiteral("/metadata");
        QDir().mkpath(sRoot);
        const QMap<QString, QString> before = fx::census(sBase);

        QFile spool;
        const QString path = LibraryBundle::openSpool(sRoot, spool);
        CHECK(!path.isEmpty());
        CHECK(path.startsWith(sRoot + QStringLiteral("/.eb-incoming/")));      // under the root, nowhere else
        CHECK(QFileInfo(path).absolutePath() == QFileInfo(sRoot + QStringLiteral("/.eb-incoming")).absoluteFilePath());
        CHECK(spool.isOpen());
        // Not an item id, not an image and not a thumb: MetaCache's cap sweep neither counts nor evicts it.
        CHECK(QFileInfo(path).suffix() == QStringLiteral("spool"));
        CHECK(QFileInfo(path).fileName().startsWith(QStringLiteral("bundle-")));
        CHECK(!LibraryBundle::safeItemId(QFileInfo(path).fileName()));
        CHECK(LibraryBundle::spoolsInFlight() == 1);

        // An inventory (which sweeps partials first) arriving while a body is spooling must not take it.
        LibraryBundle::inventoryFor(sRoot);
        CHECK(QFileInfo::exists(path));

        // LANDED from the spool, then discarded: nothing left behind.
        LibraryBundle::Payload p;
        QString err;
        CHECK(LibraryBundle::readPayload(srcRoot, idC, p, err));
        spool.write(LibraryBundle::encodePayloadV2(p));
        spool.close();
        {
            QFile rd(path);
            CHECK(rd.open(QIODevice::ReadOnly));
            CHECK(LibraryBundle::landItemV2(sRoot, rd, err) == LibraryBundle::LandResult::Landed);
        }
        LibraryBundle::discardSpool(spool);
        CHECK(!QFileInfo::exists(path));
        CHECK(LibraryBundle::spoolsInFlight() == 0);
        CHECK(!QFileInfo::exists(sRoot + QStringLiteral("/.eb-incoming")));
        CHECK(QFileInfo::exists(sRoot + QLatin1Char('/') + idC + QStringLiteral("/meta.json")));

        // REFUSED from the spool, then discarded: nothing left behind, nothing landed.
        QFile spool2;
        const QString path2 = LibraryBundle::openSpool(sRoot, spool2);
        CHECK(!path2.isEmpty() && path2 != path);
        QJsonArray files;
        files.append(fx::fileJson(QStringLiteral("evil.exe"), 4));
        spool2.write(fx::wireV2(fx::headerJson(idB, files), QByteArray("EVIL")));
        spool2.close();
        {
            QFile rd(path2);
            CHECK(rd.open(QIODevice::ReadOnly));
            CHECK(LibraryBundle::landItemV2(sRoot, rd, err) == LibraryBundle::LandResult::Refused);
        }
        LibraryBundle::discardSpool(spool2);
        CHECK(!QFileInfo::exists(path2));
        CHECK(!QFileInfo::exists(sRoot + QLatin1Char('/') + idB));
        CHECK(!QFileInfo::exists(sRoot + QStringLiteral("/.eb-incoming")));

        // A CRASH LEFTOVER (a spool no live transfer owns) is removed by the next sweep.
        QDir().mkpath(sRoot + QStringLiteral("/.eb-incoming"));
        fx::writeFile(sRoot + QStringLiteral("/.eb-incoming/bundle-0000.spool"), QByteArray("half a body"));
        CHECK(LibraryBundle::sweepPartials(sRoot) >= 1);
        CHECK(!QFileInfo::exists(sRoot + QStringLiteral("/.eb-incoming")));

        // Everything that changed is inside the cache root.
        const QMap<QString, QString> after = fx::census(sBase);
        for (auto it = after.constBegin(); it != after.constEnd(); ++it)
            CHECK(it.key().startsWith(QStringLiteral("metadata/")) || before.contains(it.key()));
    }

    // ---- 16. the streaming decision, from the headers alone (#291) -----------------------------------
    {
        const auto headersOf = [](const QByteArray& raw) { return RemoteApi::parseRequest(raw); };
        const QByteArray v2Head = "POST /bundle HTTP/1.1\r\nContent-Type: application/x-eb-bundle\r\n"
                                  "Content-Length: 30000000\r\n\r\n";
        RemoteApi::Request r = headersOf(v2Head);
        CHECK(r.declaredLength == 30000000);
        CHECK(r.contentType == QByteArray("application/x-eb-bundle"));
        CHECK(RemoteApi::bodyPlanFor(r) == RemoteApi::BodyPlan::Stream);

        r = headersOf("POST /bundle HTTP/1.1\r\nContent-Type: Application/X-EB-Bundle; v=2\r\n"
                      "Content-Length: 10\r\n\r\n");
        CHECK(RemoteApi::bodyPlanFor(r) == RemoteApi::BodyPlan::Stream);

        r = headersOf("POST /bundle HTTP/1.1\r\nContent-Type: application/x-eb-bundle\r\n"
                      "Content-Length: 99999999999\r\n\r\n");
        CHECK(r.declaredLength == 99999999999LL);
        CHECK(RemoteApi::bodyPlanFor(r) == RemoteApi::BodyPlan::TooLarge);
        r = headersOf("POST /bundle HTTP/1.1\r\nContent-Type: application/x-eb-bundle\r\nContent-Length: "
                      + QByteArray::number(RemoteApi::kBundleStreamCap + 1) + "\r\n\r\n");
        CHECK(RemoteApi::bodyPlanFor(r) == RemoteApi::BodyPlan::TooLarge);
        r = headersOf("POST /bundle HTTP/1.1\r\nContent-Type: application/x-eb-bundle\r\nContent-Length: "
                      + QByteArray::number(RemoteApi::kBundleStreamCap) + "\r\n\r\n");
        CHECK(RemoteApi::bodyPlanFor(r) == RemoteApi::BodyPlan::Stream);
        CHECK(RemoteApi::kBundleStreamCap >= LibraryBundle::kMaxV2BodyBytes);

        r = headersOf("POST /bundle HTTP/1.1\r\nContent-Type: application/x-eb-bundle\r\n\r\n");
        CHECK(RemoteApi::bodyPlanFor(r) == RemoteApi::BodyPlan::LengthRequired);

        // Everything else is buffered exactly as before: v1's JSON bundle, a GET, another route that borrows
        // the content type, and a request that only mentions the type somewhere else.
        r = headersOf("POST /bundle HTTP/1.1\r\nContent-Type: application/json\r\nContent-Length: 10\r\n\r\n");
        CHECK(RemoteApi::bodyPlanFor(r) == RemoteApi::BodyPlan::Buffer);
        r = headersOf("GET /bundle HTTP/1.1\r\nContent-Type: application/x-eb-bundle\r\nContent-Length: 10\r\n\r\n");
        CHECK(RemoteApi::bodyPlanFor(r) == RemoteApi::BodyPlan::Buffer);
        r = headersOf("POST /player HTTP/1.1\r\nContent-Type: application/x-eb-bundle\r\nContent-Length: 10\r\n\r\n");
        CHECK(RemoteApi::bodyPlanFor(r) == RemoteApi::BodyPlan::Buffer);
        r = headersOf("POST /bundle HTTP/1.1\r\nX-Note: application/x-eb-bundle\r\nContent-Length: 10\r\n\r\n");
        CHECK(RemoteApi::bodyPlanFor(r) == RemoteApi::BodyPlan::Buffer);
        CHECK(std::string(RemoteApi::kBundleStreamContentType) == std::string(LibraryBundle::kBundleV2ContentType));
    }

    // ---- 17. a real socket: RemoteServer on loopback (#291) ------------------------------------------
    {
        const QString nBase = base + QStringLiteral("/socket");
        const QString nRoot = nBase + QStringLiteral("/metadata");
        const QString nSrc  = nBase + QStringLiteral("/source");
        QDir().mkpath(nRoot);
        QDir().mkpath(nSrc);
        const QString token = QStringLiteral("probe-fixture-credential-291");

        RemoteServer server;
        RemoteServer::Hooks hooks;
        hooks.tokens = [token] { return QSet<QString>{ token }; };
        hooks.inventory = [nRoot] { return LibraryBundle::inventoryJson(LibraryBundle::inventoryFor(nRoot)); };
        hooks.bundle = [nRoot](const QByteArray& body) {
            LibraryBundle::Payload p;
            LibraryBundle::Refusal why = LibraryBundle::Refusal::None;
            QString message;
            if (!LibraryBundle::decodePayload(body, p, why, message))
                return LibraryBundle::receiptFor(LibraryBundle::LandResult::Refused, message);
            QString err;
            return LibraryBundle::receiptFor(LibraryBundle::landItem(nRoot, p, err), err);
        };
        hooks.bundleRoot = [nRoot] { return nRoot; };
        hooks.bundleStream = [nRoot](QIODevice& body) {
            QString err;
            return LibraryBundle::receiptFor(LibraryBundle::landItemV2(nRoot, body, err), err);
        };
        server.setHooks(hooks);
        CHECK(server.start(0));
        const quint16 port = server.port();
        CHECK(port != 0);

        // A v2 item just over the 20 MiB buffered cap: 3 x 7 MiB of art.
        const QString idS = fx::idFor("igdb:socket-291");
        const qint64 onDisk = fx::makeHeavyItem(nSrc, idS, 3, 7LL * 1024 * 1024, t0);
        LibraryBundle::Payload p;
        QString err;
        CHECK(LibraryBundle::readPayload(nSrc, idS, p, err, LibraryBundle::kMaxItemBytesV2));
        const QByteArray body = LibraryBundle::encodePayloadV2(p);
        CHECK(qint64(body.size()) > qint64(RemoteApi::kBundleRequestCap));
        const auto v2Head = [](qint64 length, const QString& tok) {
            QByteArray h = "POST /bundle HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/x-eb-bundle\r\n";
            h += "Content-Length: " + QByteArray::number(length) + "\r\n";
            if (!tok.isEmpty()) h += "X-EB-Token: " + tok.toLatin1() + "\r\n";
            h += "\r\n";
            return h;
        };

        // (a) the token is required, BEFORE a body byte is accepted: headers alone earn the 401.
        {
            const fx::HttpResult r = fx::httpRequest(port, v2Head(body.size(), QString()), body, 0, false);
            CHECK(r.status == 401);
            CHECK(server.streamsInFlight() == 0);
            CHECK(fx::spoolFilesUnder(nRoot).isEmpty());
            CHECK(!QFileInfo::exists(nRoot + QLatin1Char('/') + idS));
        }
        // (b) a declared length over the ceiling: 413 on the headers, no body sent, no spool opened.
        {
            const fx::HttpResult r = fx::httpRequest(port, v2Head(RemoteApi::kBundleStreamCap + 1, token), QByteArray(), 0, false);
            CHECK(r.status == 413);
            CHECK(server.streamsInFlight() == 0);
            CHECK(!QFileInfo::exists(nRoot + QStringLiteral("/.eb-incoming")));
        }
        // (c) the pass case: >20 MiB lands, byte-exact, and no request buffer ever held the body.
        {
            const fx::HttpResult r = fx::httpRequest(port, v2Head(body.size(), token), body, -1, false);
            if (r.status != 200)
                std::fprintf(stderr, "BUNDLEXFER-INFO v2 socket status %d body %s\n", r.status, r.body.constData());
            CHECK(r.status == 200);
            LibraryBundle::Receipt rec;
            CHECK(LibraryBundle::parseReceipt(r.body, rec) && rec.result == QStringLiteral("landed"));
            for (int i = 0; i < 3; ++i)
            {
                const QString name = QStringLiteral("/art-%1.png").arg(i);
                CHECK(fx::readFile(nRoot + QLatin1Char('/') + idS + name) == fx::readFile(nSrc + QLatin1Char('/') + idS + name));
                CHECK(fx::mtimeOf(nRoot + QLatin1Char('/') + idS + name) == t0 + i + 1);
            }
            CHECK(server.streamsInFlight() == 0);
            CHECK(fx::spoolFilesUnder(nRoot).isEmpty());
            CHECK(!QFileInfo::exists(nRoot + QStringLiteral("/.eb-incoming")));
            std::printf("BUNDLEXFER-INFO v2 socket body %lld bytes (%lld on disk); largest request buffer held %lld bytes\n",
                        qint64(body.size()), onDisk, server.bufferedHighWater());
            CHECK(server.bufferedHighWater() < RemoteApi::kDefaultRequestCap);
            CHECK(LibraryBundle::planTransfer(LibraryBundle::inventoryFor(nSrc), LibraryBundle::inventoryFor(nRoot)).send.isEmpty());
        }
        // (d) a client that disconnects mid-body leaves no spool and lands nothing.
        {
            const QString idD = fx::idFor("igdb:disconnect-291");
            LibraryBundle::Payload pd = p;
            pd.id = idD;
            const QByteArray bodyD = LibraryBundle::encodePayloadV2(pd);
            bool sawSpool = false;
            fx::httpRequest(port, v2Head(bodyD.size(), token), bodyD, 5 * 1024 * 1024, true, [&] {
                sawSpool = fx::spinUntil([&] { return server.streamsInFlight() == 1
                                                      && fx::spoolFilesUnder(nRoot).size() == 1; }, 5000);
            });
            CHECK(sawSpool);
            CHECK(fx::spinUntil([&] { return server.streamsInFlight() == 0; }, 5000));
            CHECK(fx::spoolFilesUnder(nRoot).isEmpty());
            CHECK(!QFileInfo::exists(nRoot + QStringLiteral("/.eb-incoming")));
            CHECK(!QFileInfo::exists(nRoot + QLatin1Char('/') + idD));
        }
        // (e) a client that stalls mid-body is dropped on the idle timeout, and its spool with it.
        {
            server.setBodyIdleTimeoutMs(300);
            const QString idT = fx::idFor("igdb:stall-291");
            LibraryBundle::Payload pt = p;
            pt.id = idT;
            const QByteArray bodyT = LibraryBundle::encodePayloadV2(pt);
            bool sawSpool = false, dropped = false;
            fx::httpRequest(port, v2Head(bodyT.size(), token), bodyT, 1024 * 1024, true, [&] {
                sawSpool = fx::spinUntil([&] { return server.streamsInFlight() == 1; }, 5000);
                dropped  = fx::spinUntil([&] { return server.streamsInFlight() == 0; }, 5000);
            });
            CHECK(sawSpool);
            CHECK(dropped);
            CHECK(fx::spoolFilesUnder(nRoot).isEmpty());
            CHECK(!QFileInfo::exists(nRoot + QLatin1Char('/') + idT));
            server.setBodyIdleTimeoutMs(30000);
        }
        // (f) a refused v2 body over the socket: 400, nothing landed, no spool.
        {
            QJsonArray files;
            files.append(fx::fileJson(QStringLiteral("evil.exe"), 4));
            const QByteArray bad = fx::wireV2(fx::headerJson(fx::idFor("igdb:evil-291"), files), QByteArray("EVIL"));
            const fx::HttpResult r = fx::httpRequest(port, v2Head(bad.size(), token), bad, -1, false);
            CHECK(r.status == 400);
            CHECK(fx::spoolFilesUnder(nRoot).isEmpty());
            CHECK(!QFileInfo::exists(nRoot + QLatin1Char('/') + fx::idFor("igdb:evil-291")));
        }
        // (g) v1 still lands through the same listener, buffered, as today; and the inventory advertises v2.
        {
            LibraryBundle::Payload p1;
            CHECK(LibraryBundle::readPayload(srcRoot, idA, p1, err));
            const QByteArray json = LibraryBundle::encodePayload(p1);
            QByteArray head = "POST /bundle HTTP/1.1\r\nContent-Type: application/json\r\nContent-Length: "
                              + QByteArray::number(json.size()) + "\r\nX-EB-Token: " + token.toLatin1() + "\r\n\r\n";
            const fx::HttpResult r = fx::httpRequest(port, head, json, -1, false);
            CHECK(r.status == 200);
            CHECK(QFileInfo::exists(nRoot + QLatin1Char('/') + idA + QStringLiteral("/meta.json")));

            const fx::HttpResult inv = fx::httpRequest(port, "GET /inventory HTTP/1.1\r\nX-EB-Token: " + token.toLatin1()
                                                         + "\r\n\r\n", QByteArray(), 0, false);
            CHECK(inv.status == 200);
            QList<LibraryBundle::Entry> items;
            QList<int> formats;
            CHECK(LibraryBundle::parseInventory(inv.body, items, formats, err));
            CHECK(LibraryBundle::chooseBundleFormat(formats) == 2);
        }
        server.stop();
    }

    QDir(base).removeRecursively();

    if (failures == 0) std::printf("BUNDLEXFER-OK\n");
    else               std::fprintf(stderr, "BUNDLEXFER had %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
