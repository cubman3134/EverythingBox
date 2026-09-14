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
// Issue #292 adds gamelist sidecars -- their own payload kind, landing beside ROMs rather than in the cache --
// and pins them in 18-23 against two ROM trees (the cache-payload assertions above are unchanged):
//
//  18. NAMES. A system or ROM name that is not one safe segment (separators, "..", a drive, a UNC share, a
//      device name, a leading dot) is refused before a file byte is read and before anything is written; so is
//      a video, a non-image extension, a duplicate role, a length that does not add up, and the other kind.
//  19. LANDING RULES. The target lands only in a system folder it already has, for a ROM file it already has;
//      a game its gamelist lists (by GamelistStore's own rule) stays byte-identical; nothing is written outside
//      <root>/<that system>/; GamelistStore reads the landed art back; and the second plan is empty.
//  20. XML. Fields are escaped text -- "</game><game>" in a name injects nothing -- and the existing file's
//      bytes are kept, with one block inserted before </gameList>; no list is created from a broken one.
//  21. IMAGES. Named by the target, never overwriting an existing file (that image is dropped from the entry).
//  22. ATOMIC. An interrupted rewrite leaves the old gamelist.xml and removes the images it wrote.
//  23. INTEROP. Advertised in a field an old parser ignores; a gamelist body is refused readably by a target
//      that does not take the kind; over a real socket, token first, the entry lands and the art path still works.
//
// Prints BUNDLEXFER-OK on success; any failure prints BUNDLEXFER-FAIL <cond> (line) and exits non-zero.
#include "GamelistStore.h"
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
#include <QXmlStreamReader>

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

    // ---- #292 fixtures ----

    // Everything under `dir` -- files AND directories, hidden ones included -- as relative path -> sha256 (a
    // directory maps to "<dir>"). A sidecar census has to see a folder that was created and a temp file that
    // was left behind, which the art census above was never asked to.
    static QMap<QString, QString> censusAll(const QString& dir)
    {
        QMap<QString, QString> out;
        QDir root(dir);
        QStringList stack;
        stack << dir;
        while (!stack.isEmpty())
        {
            const QString cur = stack.takeLast();
            const QFileInfoList entries = QDir(cur).entryInfoList(
                QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDir::Name);
            for (const QFileInfo& fi : entries)
            {
                const QString rel = root.relativeFilePath(fi.absoluteFilePath());
                if (fi.isDir())
                {
                    out.insert(rel + QLatin1Char('/'), QStringLiteral("<dir>"));
                    stack << fi.absoluteFilePath();
                    continue;
                }
                out.insert(rel, QString::fromLatin1(
                    QCryptographicHash::hash(readFile(fi.absoluteFilePath()), QCryptographicHash::Sha256).toHex()));
            }
        }
        return out;
    }

    static void writeTree(const QString& path, const QByteArray& data)
    {
        QDir().mkpath(QFileInfo(path).absolutePath());
        writeFile(path, data);
    }

    static QJsonObject sideFile(const QString& role, const QString& ext, double size)
    {
        QJsonObject o;
        o.insert(QStringLiteral("role"), role);
        o.insert(QStringLiteral("ext"), ext);
        o.insert(QStringLiteral("size"), size);
        return o;
    }

    static QJsonObject sideHeader(const QString& system, const QString& rom, const QJsonArray& files,
                                  const QJsonObject& game = QJsonObject())
    {
        QJsonObject o;
        o.insert(QStringLiteral("kind"), QStringLiteral("gamelist"));
        o.insert(QStringLiteral("system"), system);
        o.insert(QStringLiteral("rom"), rom);
        QJsonObject g = game;
        if (g.isEmpty()) g.insert(QStringLiteral("name"), QStringLiteral("A Name"));
        o.insert(QStringLiteral("game"), g);
        o.insert(QStringLiteral("files"), files);
        return o;
    }

    // A hand-made payload for one game with the given images, so a landing case does not depend on a source tree.
    static LibraryBundle::SidecarPayload sidePayload(const QString& system, const QString& rom, const QString& name,
                                                     const QList<LibraryBundle::SidecarImage>& images)
    {
        LibraryBundle::SidecarPayload p;
        p.system = system;
        p.rom = rom;
        p.fields.name = name;
        p.images = images;
        return p;
    }

    static LibraryBundle::SidecarImage sideImage(const QString& role, const QString& ext, const QByteArray& data)
    {
        LibraryBundle::SidecarImage i;
        i.role = role;
        i.ext = ext;
        i.data = data;
        return i;
    }

    // Land a wire through the target function exactly as the spool is read: a random-access device.
    static LibraryBundle::LandResult landSide(const QString& romsRoot, const QByteArray& wire, QString* error,
                                              qint64* posAfter, int failAfterFiles)
    {
        QByteArray copy = wire;
        QBuffer in(&copy);
        in.open(QIODevice::ReadOnly);
        QString err;
        LibraryBundle::LandOptions o;
        o.failAfterFiles = failAfterFiles;
        const LibraryBundle::LandResult r = LibraryBundle::landSidecarV2(romsRoot, in, err, o);
        if (error) *error = err;
        if (posAfter) *posAfter = in.pos();
        return r;
    }

    static int countGameElements(const QByteArray& xml, bool* wellFormed)
    {
        QXmlStreamReader r(xml);
        int n = 0;
        while (!r.atEnd())
        {
            r.readNext();
            if (r.isStartElement() && r.name() == QLatin1String("game")) ++n;
        }
        if (wellFormed) *wellFormed = !r.hasError();
        return n;
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

    // ================================ #292: gamelist sidecars (18-23) ================================
    //
    // Two ROM trees stand in for two devices. The SOURCE's snes/gamelist.xml lists A, B and C with images (and a
    // video for A, and a fanart path that climbs out of the system folder). The TARGET has ROMs A and B, and its
    // own gamelist -- hand-kept, with a comment, attributes and bytes after </gameList> -- already lists B.
    // A sibling folder beside the target's ROM root holds state that nothing may touch.
    const QString gBase    = base + QStringLiteral("/gamelists");
    const QString sRoms    = gBase + QStringLiteral("/source/roms");
    const QString tBase    = gBase + QStringLiteral("/target");
    const QString tRoms    = tBase + QStringLiteral("/roms");
    const QByteArray artA  = QByteArray("\x89PNG\r\n\x1a\n", 8) + fx::noise(3000, 11);
    const QByteArray artAm = QByteArray("\x89PNG\r\n\x1a\n", 8) + fx::noise(1700, 12);
    const QByteArray artB  = QByteArray("\x89PNG\r\n\x1a\n", 8) + fx::noise(900, 13);
    const QByteArray artC  = QByteArray("\x89PNG\r\n\x1a\n", 8) + fx::noise(800, 14);
    {
        fx::writeTree(sRoms + QStringLiteral("/snes/A.sfc"), QByteArray("ROM-A"));
        fx::writeTree(sRoms + QStringLiteral("/snes/B.sfc"), QByteArray("ROM-B"));
        fx::writeTree(sRoms + QStringLiteral("/snes/C.sfc"), QByteArray("ROM-C"));
        fx::writeTree(sRoms + QStringLiteral("/snes/images/A-thumb.png"), artA);
        fx::writeTree(sRoms + QStringLiteral("/snes/images/A-marquee.png"), artAm);
        fx::writeTree(sRoms + QStringLiteral("/snes/images/B-thumb.png"), artB);
        fx::writeTree(sRoms + QStringLiteral("/snes/images/C-thumb.png"), artC);
        fx::writeTree(sRoms + QStringLiteral("/snes/videos/A-video.mp4"), QByteArray("MP4-NEVER-TRAVELS"));
        fx::writeTree(sRoms + QStringLiteral("/outside-secret.png"), QByteArray("\x89PNG-SECRET", 11));
        fx::writeTree(sRoms + QStringLiteral("/snes/gamelist.xml"), QByteArray(
            "<?xml version=\"1.0\"?>\n<gameList>\n"
            "\t<game>\n\t\t<path>./A.sfc</path>\n\t\t<name>Alpha Quest</name>\n"
            "\t\t<desc>The first game &amp; the best.</desc>\n\t\t<releasedate>19930101T000000</releasedate>\n"
            "\t\t<developer>Dev A</developer>\n\t\t<publisher>Pub A</publisher>\n\t\t<genre>RPG</genre>\n"
            "\t\t<players>1</players>\n\t\t<rating>0.8</rating>\n"
            "\t\t<thumbnail>./images/A-thumb.png</thumbnail>\n\t\t<marquee>./images/A-marquee.png</marquee>\n"
            "\t\t<fanart>../outside-secret.png</fanart>\n\t\t<video>./videos/A-video.mp4</video>\n\t</game>\n"
            "\t<game><path>./B.sfc</path><name>Bravo (Source)</name><thumbnail>./images/B-thumb.png</thumbnail></game>\n"
            "\t<game><path>./C.sfc</path><name>Charlie</name><thumbnail>./images/C-thumb.png</thumbnail></game>\n"
            "\t<game><path>./sub/D.sfc</path><name>Delta In A Subfolder</name></game>\n"
            "</gameList>\n"));
        // Gamelists that are NOT directly in a system folder under the root: never sent.
        fx::writeTree(sRoms + QStringLiteral("/gamelist.xml"),
                      QByteArray("<gameList><game><path>./E.sfc</path><name>Echo</name></game></gameList>"));
        fx::writeTree(sRoms + QStringLiteral("/snes/extra/gamelist.xml"),
                      QByteArray("<gameList><game><path>./F.sfc</path><name>Foxtrot</name></game></gameList>"));

        fx::writeTree(tRoms + QStringLiteral("/snes/A.sfc"), QByteArray("ROM-A"));
        fx::writeTree(tRoms + QStringLiteral("/snes/B.sfc"), QByteArray("ROM-B"));
        fx::writeTree(tRoms + QStringLiteral("/snes/images/B-thumb.png"), QByteArray("TARGET-B-ART"));
        fx::writeTree(tRoms + QStringLiteral("/snes/gamelist.xml"), QByteArray(
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n<!-- kept by hand: do not reformat -->\r\n<gameList>\r\n"
            "  <game id=\"7\" source=\"ScreenScraper\">\r\n    <path>./B.sfc</path>\r\n"
            "    <name>Bravo (Target's own)</name>\r\n    <thumbnail>./images/B-thumb.png</thumbnail>\r\n"
            "  </game>\r\n</gameList>\r\n<!-- trailing -->\r\n"));
        fx::writeTree(tRoms + QStringLiteral("/megadrive/Sonic.md"), QByteArray("ROM-SONIC"));
        fx::writeTree(tBase + QStringLiteral("/state/marks.json"), QByteArray("{\"watched\":true}"));
    }

    // ---- 18. sidecar names: one safe segment, or refused before anything is written (#292) ----------
    {
        const auto ok = [](const char* s) { return LibraryBundle::safePathSegment(QString::fromUtf8(s)); };
        CHECK(ok("snes"));
        CHECK(ok("megadrive"));
        CHECK(ok("Super Mario World (USA).sfc"));
        CHECK(ok("Legend of Zelda, The - A Link to the Past (USA) [!].sfc"));
        const QString accented = QString::fromUtf8("Pok\xc3\xa9mon - Edici\xc3\xb3n Roja.gb");
        CHECK(LibraryBundle::safePathSegment(accented));
        CHECK(ok("Tom & Jerry's Game.nes"));
        CHECK(!ok(""));
        CHECK(!ok("."));
        CHECK(!ok(".."));
        CHECK(!ok("../snes"));
        CHECK(!ok("snes/../.."));
        CHECK(!ok("a/b"));
        CHECK(!ok("a\\b"));
        CHECK(!ok("C:"));
        CHECK(!ok("C:snes"));
        CHECK(!ok("C:\\Windows"));
        CHECK(!ok("\\\\host\\share"));
        CHECK(!ok("//host/share"));
        CHECK(!ok("/etc"));
        CHECK(!ok("con"));
        CHECK(!ok("CON"));
        CHECK(!ok("nul.sfc"));
        CHECK(!ok("lpt1.sfc"));
        CHECK(!ok("COM9.zip"));
        CHECK(!ok(".hidden"));
        CHECK(!ok(".A.sfc"));
        CHECK(!ok("trailing."));
        CHECK(!ok("trailing "));
        CHECK(!ok("sn*es"));
        CHECK(!ok("what?.sfc"));
        CHECK(!ok("pipe|name"));
        const QString control = QStringLiteral("ctl") + QChar(0x01) + QStringLiteral("name");
        CHECK(!LibraryBundle::safePathSegment(control));
        CHECK(!LibraryBundle::safePathSegment(QString(300, QLatin1Char('a'))));

        // The TARGET names every image, GamelistWriter's way; a role that is not an image role has no name.
        CHECK(LibraryBundle::sidecarImageName(QStringLiteral("A (USA).sfc"), QStringLiteral("thumbnail"), QStringLiteral("png"))
              == QStringLiteral("A (USA)-thumb.png"));
        CHECK(LibraryBundle::sidecarImageName(QStringLiteral("A.sfc"), QStringLiteral("image"), QStringLiteral("jpg"))
              == QStringLiteral("A-image.jpg"));
        CHECK(LibraryBundle::sidecarImageName(QStringLiteral("A.sfc"), QStringLiteral("marquee"), QStringLiteral("png"))
              == QStringLiteral("A-marquee.png"));
        CHECK(LibraryBundle::sidecarImageName(QStringLiteral("A.sfc"), QStringLiteral("fanart"), QStringLiteral("webp"))
              == QStringLiteral("A-fanart.webp"));
        CHECK(LibraryBundle::sidecarImageName(QStringLiteral("A.sfc"), QStringLiteral("video"), QStringLiteral("mp4")).isEmpty());
        CHECK(LibraryBundle::sidecarImageName(QStringLiteral("A.sfc"), QStringLiteral("thumbnail"), QStringLiteral("exe")).isEmpty());
        CHECK(LibraryBundle::sidecarImageRoles()
              == (QStringList{ QStringLiteral("thumbnail"), QStringLiteral("image"), QStringLiteral("marquee"), QStringLiteral("fanart") }));
        CHECK(LibraryBundle::sidecarImageExtension(QStringLiteral("png")));
        CHECK(LibraryBundle::sidecarImageExtension(QStringLiteral("jpeg")));
        CHECK(!LibraryBundle::sidecarImageExtension(QStringLiteral("svg")));    // an image that can carry script
        CHECK(!LibraryBundle::sidecarImageExtension(QStringLiteral("mp4")));
        CHECK(!LibraryBundle::sidecarImageExtension(QStringLiteral("../png")));

        // Every hostile header, refused by the decoder AND by the landing, with no file byte read and nothing
        // written anywhere under the target (its ROM tree and the state beside it).
        auto sideRefused = [&](const QByteArray& hostile, LibraryBundle::Refusal expected, int line) {
            const QMap<QString, QString> before = fx::censusAll(tBase);
            const qint64 headerEnd = fx::headerEndOf(hostile);
            QByteArray b1 = hostile;
            QBuffer d1(&b1);
            d1.open(QIODevice::ReadOnly);
            LibraryBundle::SidecarHeader hh;
            LibraryBundle::Refusal w = LibraryBundle::Refusal::None;
            QString msg;
            const bool decoded = LibraryBundle::decodeSidecarHeaderV2(d1, hh, w, msg);
            qint64 pos = 0;
            QString err;
            const LibraryBundle::LandResult lr = fx::landSide(tRoms, hostile, &err, &pos, -1);
            const bool untouched = fx::censusAll(tBase) == before;
            if (decoded || w != expected || msg.isEmpty() || d1.pos() > headerEnd
                || lr != LibraryBundle::LandResult::Refused || pos > headerEnd || !untouched)
                std::fprintf(stderr, "BUNDLEXFER-FAIL sidecar refusal case from line %d (decoded=%d why=%d pos=%lld "
                                     "land=%d landpos=%lld end=%lld untouched=%d)\n",
                             line, int(decoded), int(w), qint64(d1.pos()), int(lr), pos, headerEnd, int(untouched));
            CHECK(!decoded);
            CHECK(w == expected);
            CHECK(!msg.isEmpty());
            CHECK(d1.pos() <= headerEnd);
            CHECK(lr == LibraryBundle::LandResult::Refused);
            CHECK(pos <= headerEnd);
            CHECK(untouched);
        };
        const QByteArray png4("\x89PNG", 4);
        QJsonArray oneThumb;
        oneThumb.append(fx::sideFile(QStringLiteral("thumbnail"), QStringLiteral("png"), 4));

        for (const char* sys : { "..", "../snes", "snes/../..", "C:", "C:\\Windows", "\\\\host\\share", "//host/share",
                                 "/etc", "con", "CON", "nul.x", ".hidden", "", "snes ", "sn*es" })
            sideRefused(fx::wireV2(fx::sideHeader(QString::fromUtf8(sys), QStringLiteral("A.sfc"), oneThumb), png4),
                        LibraryBundle::Refusal::UnsafeId, __LINE__);
        for (const char* rom : { "../A.sfc", "sub/A.sfc", "..\\A.sfc", "C:A.sfc", "lpt1.sfc", ".A.sfc", "gamelist.xml",
                                 "A.sfc.", "..", "" })
            sideRefused(fx::wireV2(fx::sideHeader(QStringLiteral("snes"), QString::fromUtf8(rom), oneThumb), png4),
                        LibraryBundle::Refusal::UnsafeId, __LINE__);
        {
            QJsonObject noSystem = fx::sideHeader(QStringLiteral("snes"), QStringLiteral("A.sfc"), oneThumb);
            noSystem.remove(QStringLiteral("system"));
            sideRefused(fx::wireV2(noSystem, png4), LibraryBundle::Refusal::UnsafeId, __LINE__);
        }
        // A VIDEO -- after a legitimate image, so a decoder that judged files as it reached them would already
        // have read the image.
        {
            QJsonArray files;
            files.append(fx::sideFile(QStringLiteral("thumbnail"), QStringLiteral("png"), 4));
            files.append(fx::sideFile(QStringLiteral("video"), QStringLiteral("mp4"), 4));
            sideRefused(fx::wireV2(fx::sideHeader(QStringLiteral("snes"), QStringLiteral("A.sfc"), files), QByteArray("\x89PNGMP4!", 8)),
                        LibraryBundle::Refusal::UnsafeFileName, __LINE__);
        }
        for (const char* ext : { "exe", "svg", "mp4", "../png", "PNG/..", "" })
        {
            QJsonArray files;
            files.append(fx::sideFile(QStringLiteral("thumbnail"), QString::fromUtf8(ext), 4));
            sideRefused(fx::wireV2(fx::sideHeader(QStringLiteral("snes"), QStringLiteral("A.sfc"), files), png4),
                        LibraryBundle::Refusal::UnsafeFileName, __LINE__);
        }
        {
            QJsonArray files;
            files.append(fx::sideFile(QStringLiteral("manual"), QStringLiteral("png"), 4));
            sideRefused(fx::wireV2(fx::sideHeader(QStringLiteral("snes"), QStringLiteral("A.sfc"), files), png4),
                        LibraryBundle::Refusal::UnsafeFileName, __LINE__);
        }
        {
            QJsonArray dup;
            dup.append(fx::sideFile(QStringLiteral("thumbnail"), QStringLiteral("png"), 4));
            dup.append(fx::sideFile(QStringLiteral("thumbnail"), QStringLiteral("jpg"), 4));
            sideRefused(fx::wireV2(fx::sideHeader(QStringLiteral("snes"), QStringLiteral("A.sfc"), dup), QByteArray("12345678")),
                        LibraryBundle::Refusal::Malformed, __LINE__);
            QJsonArray huge;
            huge.append(fx::sideFile(QStringLiteral("thumbnail"), QStringLiteral("png"), double(LibraryBundle::kMaxFileBytes + 1)));
            sideRefused(fx::wireV2(fx::sideHeader(QStringLiteral("snes"), QStringLiteral("A.sfc"), huge), png4),
                        LibraryBundle::Refusal::TooLarge, __LINE__);
            const QByteArray good = fx::wireV2(fx::sideHeader(QStringLiteral("snes"), QStringLiteral("A.sfc"), oneThumb), png4);
            QByteArray shortBody = good;
            shortBody.chop(1);
            sideRefused(shortBody, LibraryBundle::Refusal::Malformed, __LINE__);
            sideRefused(good + QByteArray("X"), LibraryBundle::Refusal::Malformed, __LINE__);
            sideRefused(fx::wireV2(fx::sideHeader(QStringLiteral("snes"), QStringLiteral("A.sfc"), oneThumb), png4, 3),
                        LibraryBundle::Refusal::FutureFormat, __LINE__);
            QJsonObject numericName;
            numericName.insert(QStringLiteral("name"), 5);
            sideRefused(fx::wireV2(fx::sideHeader(QStringLiteral("snes"), QStringLiteral("A.sfc"), oneThumb, numericName), png4),
                        LibraryBundle::Refusal::Malformed, __LINE__);
        }
        // An ART body handed to the sidecar decoder, and an unknown kind: refused as a kind, with a sentence.
        {
            QJsonArray files;
            files.append(fx::fileJson(QStringLiteral("thumb.png"), 4));
            sideRefused(fx::wireV2(fx::headerJson(idA, files), png4), LibraryBundle::Refusal::UnsupportedKind, __LINE__);
            QJsonObject other = fx::sideHeader(QStringLiteral("snes"), QStringLiteral("A.sfc"), oneThumb);
            other.insert(QStringLiteral("kind"), QStringLiteral("romfile"));
            sideRefused(fx::wireV2(other, png4), LibraryBundle::Refusal::UnsupportedKind, __LINE__);
        }
    }

    // ---- 19. landing rules: the target decides, and a listed game is never touched (#292) -----------
    {
        // THE SOURCE: exactly the three games in snes/gamelist.xml whose <path> is one segment. Not the
        // root-level gamelist, not the one in snes/extra/, not the subfolder entry.
        const QList<LibraryBundle::SidecarGame> games = LibraryBundle::sidecarGamesFor(sRoms);
        QStringList keys;
        for (const LibraryBundle::SidecarGame& g : games) keys << g.system + QLatin1Char('|') + g.rom;
        CHECK(keys == (QStringList{ QStringLiteral("snes|A.sfc"), QStringLiteral("snes|B.sfc"), QStringLiteral("snes|C.sfc") }));

        LibraryBundle::SidecarPayload payA;
        QString err;
        const LibraryBundle::SidecarGame* gameA = games.isEmpty() ? nullptr : &games.first();
        CHECK(gameA && LibraryBundle::readSidecarPayload(sRoms, *gameA, payA, err));
        CHECK(payA.fields.name == QStringLiteral("Alpha Quest"));
        CHECK(payA.fields.desc == QStringLiteral("The first game & the best."));
        CHECK(payA.fields.developer == QStringLiteral("Dev A") && payA.fields.rating == QStringLiteral("0.8"));
        QStringList rolesA;
        for (const LibraryBundle::SidecarImage& i : payA.images) rolesA << i.role;
        // thumbnail and marquee. NOT the fanart whose path climbs out of the system folder, and never the video.
        CHECK(rolesA == (QStringList{ QStringLiteral("thumbnail"), QStringLiteral("marquee") }));
        CHECK(payA.images.size() == 2 && payA.images.at(0).data == artA && payA.images.at(1).data == artAm);
        CHECK(LibraryBundle::fileBytesOf(payA) == qint64(artA.size() + artAm.size()));
        const QByteArray wireA = LibraryBundle::encodeSidecarV2(payA);
        CHECK(!wireA.contains("MP4-NEVER-TRAVELS"));
        CHECK(!wireA.contains("SECRET"));
        CHECK(!wireA.contains("./images/"));                    // no path from the source rides the wire

        // THE TARGET's lists: the ROMs present, and which of them its gamelist lists.
        const QList<LibraryBundle::SidecarSystem> inv = LibraryBundle::sidecarInventoryFor(tRoms);
        const QByteArray invJson = LibraryBundle::sidecarInventoryJson(inv);
        QList<LibraryBundle::SidecarSystem> invBack;
        CHECK(LibraryBundle::parseSidecarInventory(invJson, invBack, err));
        CHECK(invBack.size() == 2);
        for (const LibraryBundle::SidecarSystem& s : invBack)
        {
            if (s.name == QStringLiteral("snes"))
            {
                CHECK(s.roms == (QStringList{ QStringLiteral("A.sfc"), QStringLiteral("B.sfc") }));
                CHECK(s.listed == QStringList{ QStringLiteral("B.sfc") });
            }
            else
            {
                CHECK(s.name == QStringLiteral("megadrive"));
                CHECK(s.roms == QStringList{ QStringLiteral("Sonic.md") } && s.listed.isEmpty());
            }
        }

        // THE PLAN: A only; B is already listed; C is not applicable.
        const LibraryBundle::SidecarPlan plan = LibraryBundle::planSidecars(games, invBack);
        CHECK(plan.send.size() == 1 && !plan.send.isEmpty() && plan.send.first().rom == QStringLiteral("A.sfc"));
        CHECK(plan.alreadyListed == 1);
        CHECK(plan.notApplicable == 1);

        // LAND A.
        const QString listPath = tRoms + QStringLiteral("/snes/gamelist.xml");
        const QByteArray oldList = fx::readFile(listPath);
        const QMap<QString, QString> before = fx::censusAll(tBase);
        CHECK(fx::landSide(tRoms, wireA, &err, nullptr, -1) == LibraryBundle::LandResult::Landed);
        CHECK(fx::readFile(tRoms + QStringLiteral("/snes/images/A-thumb.png")) == artA);
        CHECK(fx::readFile(tRoms + QStringLiteral("/snes/images/A-marquee.png")) == artAm);
        CHECK(!QFileInfo::exists(tRoms + QStringLiteral("/snes/videos")));
        CHECK(fx::readFile(tRoms + QStringLiteral("/snes/images/B-thumb.png")) == QByteArray("TARGET-B-ART"));

        // The existing file's bytes are preserved; exactly one <game> is inserted before </gameList>.
        const QByteArray newList = fx::readFile(listPath);
        const int close = oldList.lastIndexOf("</gameList>");
        CHECK(close > 0);
        CHECK(newList.size() > oldList.size());
        CHECK(newList.left(close) == oldList.left(close));
        CHECK(newList.right(oldList.size() - close) == oldList.mid(close));
        const QByteArray inserted = newList.mid(close, newList.size() - oldList.size());
        CHECK(inserted.count("<game>") == 1 && inserted.count("</game>") == 1);
        CHECK(inserted.contains("<path>./A.sfc</path>"));
        CHECK(inserted.contains("<name>Alpha Quest</name>"));
        CHECK(inserted.contains("<desc>The first game &amp; the best.</desc>"));
        CHECK(inserted.contains("<thumbnail>./images/A-thumb.png</thumbnail>"));
        CHECK(inserted.contains("<marquee>./images/A-marquee.png</marquee>"));
        CHECK(!inserted.contains("fanart") && !inserted.contains("video"));
        bool wellFormed = false;
        CHECK(fx::countGameElements(newList, &wellFormed) == 2 && wellFormed);

        // ...and GamelistStore -- the reader the UI uses -- now finds A's art on the target, and B unchanged.
        GamelistStore::clearCache();
        const MediaDetail dA = GamelistStore::lookup(tRoms + QStringLiteral("/snes/A.sfc"));
        CHECK(dA.valid && dA.title == QStringLiteral("Alpha Quest"));
        CHECK(dA.art.image(QStringLiteral("box")) == QDir::cleanPath(tRoms + QStringLiteral("/snes/images/A-thumb.png")));
        CHECK(dA.art.image(QStringLiteral("logo")) == QDir::cleanPath(tRoms + QStringLiteral("/snes/images/A-marquee.png")));
        CHECK(GamelistStore::lookup(tRoms + QStringLiteral("/snes/B.sfc")).title == QStringLiteral("Bravo (Target's own)"));

        // THE GUARD: every new or changed entry is under <root>/<the existing system>/; nothing was removed;
        // megadrive, the state folder and everything else are exactly as they were.
        const QMap<QString, QString> after = fx::censusAll(tBase);
        for (auto it = before.constBegin(); it != before.constEnd(); ++it) CHECK(after.contains(it.key()));
        for (auto it = after.constBegin(); it != after.constEnd(); ++it)
        {
            const bool changed = !before.contains(it.key()) || before.value(it.key()) != it.value();
            if (changed && !it.key().startsWith(QStringLiteral("roms/snes/")))
                std::fprintf(stderr, "BUNDLEXFER-FAIL sidecar wrote outside its system: %s\n", qPrintable(it.key()));
            CHECK(!changed || it.key().startsWith(QStringLiteral("roms/snes/")));
        }

        // B: already listed -> "current", and the gamelist stays BYTE-IDENTICAL.
        LibraryBundle::SidecarPayload payB;
        CHECK(games.size() == 3 && LibraryBundle::readSidecarPayload(sRoms, games.at(1), payB, err));
        const QMap<QString, QString> beforeB = fx::censusAll(tBase);
        CHECK(fx::landSide(tRoms, LibraryBundle::encodeSidecarV2(payB), &err, nullptr, -1)
              == LibraryBundle::LandResult::AlreadyCurrent);
        CHECK(fx::readFile(listPath) == newList);
        CHECK(fx::censusAll(tBase) == beforeB);

        // C: no ROM there -> not applicable, no write. A system folder the target lacks -> not applicable, and
        // the folder is NOT created. A "ROM" that is a directory is not a ROM.
        LibraryBundle::SidecarPayload payC;
        CHECK(games.size() == 3 && LibraryBundle::readSidecarPayload(sRoms, games.at(2), payC, err));
        const QMap<QString, QString> beforeC = fx::censusAll(tBase);
        CHECK(fx::landSide(tRoms, LibraryBundle::encodeSidecarV2(payC), &err, nullptr, -1)
              == LibraryBundle::LandResult::NotApplicable);
        CHECK(!err.isEmpty());
        LibraryBundle::SidecarPayload n64 = payA;
        n64.system = QStringLiteral("n64");
        CHECK(fx::landSide(tRoms, LibraryBundle::encodeSidecarV2(n64), &err, nullptr, -1)
              == LibraryBundle::LandResult::NotApplicable);
        CHECK(!QFileInfo::exists(tRoms + QStringLiteral("/n64")));
        LibraryBundle::SidecarPayload missing = payA;
        missing.system = QStringLiteral("megadrive");
        missing.rom = QStringLiteral("Missing.md");
        CHECK(fx::landSide(tRoms, LibraryBundle::encodeSidecarV2(missing), &err, nullptr, -1)
              == LibraryBundle::LandResult::NotApplicable);
        CHECK(fx::censusAll(tBase) == beforeC);
        QDir().mkpath(tRoms + QStringLiteral("/megadrive/Folder.md"));
        const QMap<QString, QString> beforeDir = fx::censusAll(tBase);
        LibraryBundle::SidecarPayload dirRom = payA;
        dirRom.system = QStringLiteral("megadrive");
        dirRom.rom = QStringLiteral("Folder.md");
        CHECK(fx::landSide(tRoms, LibraryBundle::encodeSidecarV2(dirRom), &err, nullptr, -1)
              == LibraryBundle::LandResult::NotApplicable);
        CHECK(fx::censusAll(tBase) == beforeDir);
        CHECK(!QFileInfo::exists(tRoms + QStringLiteral("/megadrive/gamelist.xml")));
        QDir(tRoms + QStringLiteral("/megadrive/Folder.md")).removeRecursively();

        // THE SECOND RUN moves nothing.
        QList<LibraryBundle::SidecarSystem> inv2;
        CHECK(LibraryBundle::parseSidecarInventory(
            LibraryBundle::sidecarInventoryJson(LibraryBundle::sidecarInventoryFor(tRoms)), inv2, err));
        const LibraryBundle::SidecarPlan plan2 = LibraryBundle::planSidecars(LibraryBundle::sidecarGamesFor(sRoms), inv2);
        CHECK(plan2.send.isEmpty());
        CHECK(plan2.alreadyListed == 2);
        CHECK(plan2.notApplicable == 1);

        // The sentences for both runs: games added counted on their own, including "0 added, N already listed".
        LibraryBundle::Progress run1;
        run1.gamelists = true; run1.gamesAdded = 1; run1.gamesListed = 1; run1.gamesNotApplicable = 1;
        run1.bytesSent = LibraryBundle::fileBytesOf(payA);
        const QString line1 = LibraryBundle::describeProgress(run1, QStringLiteral("Den"));
        CHECK(line1.contains(QStringLiteral("1 game added")));
        CHECK(line1.contains(QStringLiteral("1 already listed")));
        CHECK(!line1.contains(QStringLiteral("could not")));
        CHECK(!line1.contains(QStringLiteral("nothing to send")));
        LibraryBundle::Progress run2;
        run2.gamelists = true; run2.gamesListed = 2; run2.gamesNotApplicable = 1;
        const QString line2 = LibraryBundle::describeProgress(run2, QStringLiteral("Den"));
        CHECK(line2.contains(QStringLiteral("already up to date")));
        CHECK(line2.contains(QStringLiteral("0 games added, 2 already listed")));
        LibraryBundle::Progress both;
        both.itemsTotal = 5; both.itemsSent = 5; both.bytesSent = 4096;
        both.gamelists = true; both.gamesAdded = 3; both.gamesFailed = 1;
        const QString line3 = LibraryBundle::describeProgress(both, QStringLiteral("Den"));
        CHECK(line3.contains(QStringLiteral("Sent 5 of 5 items")));
        CHECK(line3.contains(QStringLiteral("3 games added")));
        CHECK(line3.contains(QStringLiteral("1 could not be added")));
        LibraryBundle::Progress noSide;
        CHECK(!LibraryBundle::describeProgress(noSide, QStringLiteral("Den")).contains(QStringLiteral("game")));

        // GamelistStore's OWN matching rule decides "listed", on both ends: a GoodNES name against a No-Intro
        // entry (clean title), a different extension (base name), and a game that is genuinely not there.
        fx::writeTree(tRoms + QStringLiteral("/nes/Super Mario Bros 3 (U) [!].nes"), QByteArray("ROM"));
        fx::writeTree(tRoms + QStringLiteral("/nes/Metroid.nes"), QByteArray("ROM"));
        fx::writeTree(tRoms + QStringLiteral("/nes/Zelda.nes"), QByteArray("ROM"));
        fx::writeTree(tRoms + QStringLiteral("/nes/gamelist.xml"), QByteArray(
            "<gameList><game><path>./Super Mario Bros. 3 (USA) (Rev 1).nes</path><name>Super Mario Bros. 3</name></game>"
            "<game><path>./metroid.zip</path><name>Metroid</name></game></gameList>"));
        GamelistStore::clearCache();
        for (const LibraryBundle::SidecarSystem& s : LibraryBundle::sidecarInventoryFor(tRoms))
        {
            if (s.name != QStringLiteral("nes")) continue;
            CHECK(s.roms.size() == 3);
            for (const QString& rom : s.roms)
            {
                const bool store = GamelistStore::has(tRoms + QStringLiteral("/nes/") + rom);
                if (store != s.listed.contains(rom))
                    std::fprintf(stderr, "BUNDLEXFER-FAIL listed disagrees with GamelistStore for %s\n", qPrintable(rom));
                CHECK(store == s.listed.contains(rom));
            }
            CHECK(s.listed.size() == 2 && !s.listed.contains(QStringLiteral("Zelda.nes")));
        }
        const QByteArray nesBefore = fx::readFile(tRoms + QStringLiteral("/nes/gamelist.xml"));
        LibraryBundle::SidecarPayload smb3 = fx::sidePayload(QStringLiteral("nes"), QStringLiteral("Super Mario Bros 3 (U) [!].nes"),
                                                             QStringLiteral("SMB3 from elsewhere"), {});
        CHECK(fx::landSide(tRoms, LibraryBundle::encodeSidecarV2(smb3), &err, nullptr, -1)
              == LibraryBundle::LandResult::AlreadyCurrent);
        CHECK(fx::readFile(tRoms + QStringLiteral("/nes/gamelist.xml")) == nesBefore);
    }

    // ---- 20. XML: structured fields, escaped; the existing file preserved (#292) --------------------
    {
        CHECK(LibraryBundle::gamelistXmlText(QStringLiteral("a & b < c > d")) == QStringLiteral("a &amp; b &lt; c &gt; d"));
        const QString withControl = QStringLiteral("x") + QChar(0x01) + QStringLiteral("y") + QChar(0xFFFF) + QStringLiteral("z");
        CHECK(LibraryBundle::gamelistXmlText(withControl) == QStringLiteral("xyz"));

        fx::writeTree(tRoms + QStringLiteral("/gba/Evil.gba"), QByteArray("ROM"));
        fx::writeTree(tRoms + QStringLiteral("/gba/Other.gba"), QByteArray("ROM"));
        const QByteArray gbaOld("<?xml version=\"1.0\"?>\n<gameList>\n\t<game>\n\t\t<path>./Other.gba</path>\n"
                                "\t\t<name>Other</name>\n\t</game>\n</gameList>\n");
        fx::writeTree(tRoms + QStringLiteral("/gba/gamelist.xml"), gbaOld);

        const QString hostileName = QStringLiteral("Evil</name></game><game><path>./Other.gba</path><name>pwn</name>");
        const QString hostileDesc = QStringLiteral("<![CDATA[ & ]]> <!-- --> ") + QChar(0x01) + QStringLiteral("end");
        LibraryBundle::SidecarPayload evil = fx::sidePayload(QStringLiteral("gba"), QStringLiteral("Evil.gba"), hostileName, {});
        evil.fields.desc = hostileDesc;
        evil.fields.developer = QStringLiteral("\"quoted\" 'apos'");
        QString err;
        CHECK(fx::landSide(tRoms, LibraryBundle::encodeSidecarV2(evil), &err, nullptr, -1) == LibraryBundle::LandResult::Landed);
        const QByteArray gbaNew = fx::readFile(tRoms + QStringLiteral("/gba/gamelist.xml"));
        bool wellFormed = false;
        CHECK(fx::countGameElements(gbaNew, &wellFormed) == 2);      // not 3: no element was injected
        CHECK(wellFormed);
        const int gbaClose = gbaOld.lastIndexOf("</gameList>");
        CHECK(gbaNew.left(gbaClose) == gbaOld.left(gbaClose));
        CHECK(gbaNew.endsWith(gbaOld.mid(gbaClose)));
        const QList<LibraryBundle::GamelistGame> parsed = LibraryBundle::parseGamelist(gbaNew);
        CHECK(parsed.size() == 2);
        if (parsed.size() == 2)
        {
            CHECK(parsed.at(0).path == QStringLiteral("./Other.gba") && parsed.at(0).fields.name == QStringLiteral("Other"));
            CHECK(parsed.at(1).path == QStringLiteral("./Evil.gba"));
            CHECK(parsed.at(1).fields.name == hostileName);        // the text survived as text
            CHECK(parsed.at(1).fields.desc == QStringLiteral("<![CDATA[ & ]]> <!-- --> end"));
        }
        GamelistStore::clearCache();
        CHECK(GamelistStore::lookup(tRoms + QStringLiteral("/gba/Other.gba")).title == QStringLiteral("Other"));

        // No gamelist yet: a new, well-formed file holding the one entry.
        LibraryBundle::SidecarPayload sonic = fx::sidePayload(QStringLiteral("megadrive"), QStringLiteral("Sonic.md"),
                                                              QStringLiteral("Sonic"), {});
        CHECK(fx::landSide(tRoms, LibraryBundle::encodeSidecarV2(sonic), &err, nullptr, -1) == LibraryBundle::LandResult::Landed);
        const QByteArray mdList = fx::readFile(tRoms + QStringLiteral("/megadrive/gamelist.xml"));
        CHECK(mdList.startsWith("<?xml"));
        CHECK(fx::countGameElements(mdList, &wellFormed) == 1 && wellFormed);

        // A non-empty gamelist with no </gameList> is not rewritten: the landing fails, the file is untouched
        // and no image it carried is left behind.
        fx::writeTree(tRoms + QStringLiteral("/psx/Game.bin"), QByteArray("ROM"));
        const QByteArray broken("<gameList><game><path>./Other.bin</path>");
        fx::writeTree(tRoms + QStringLiteral("/psx/gamelist.xml"), broken);
        const QMap<QString, QString> psxBefore = fx::censusAll(tRoms + QStringLiteral("/psx"));
        LibraryBundle::SidecarPayload psx = fx::sidePayload(QStringLiteral("psx"), QStringLiteral("Game.bin"), QStringLiteral("Game"),
            { fx::sideImage(QStringLiteral("thumbnail"), QStringLiteral("png"), QByteArray("PSXART")) });
        CHECK(fx::landSide(tRoms, LibraryBundle::encodeSidecarV2(psx), &err, nullptr, -1) == LibraryBundle::LandResult::WriteFailed);
        CHECK(fx::censusAll(tRoms + QStringLiteral("/psx")) == psxBefore);

        // The pure insert on its own terms.
        QByteArray out;
        CHECK(LibraryBundle::insertGamelistEntry(QByteArray("<gameList>\n</gameList>tail"), QByteArray("X\n"), out));
        CHECK(out == QByteArray("<gameList>\nX\n</gameList>tail"));
        CHECK(!LibraryBundle::insertGamelistEntry(broken, QByteArray("X"), out));
        CHECK(LibraryBundle::insertGamelistEntry(QByteArray(), QByteArray("\t<game/>\n"), out) && out.contains("<gameList>"));
    }

    // ---- 21. images: target-named, never overwriting, never video (#292) ----------------------------
    {
        fx::writeTree(tRoms + QStringLiteral("/gbc/Kept.gbc"), QByteArray("ROM"));
        fx::writeTree(tRoms + QStringLiteral("/gbc/images/Kept-thumb.png"), QByteArray("USER-ART"));
        const QByteArray marquee = QByteArray("\x89PNG", 4) + fx::noise(500, 21);
        const QByteArray thumb   = QByteArray("\x89PNG", 4) + fx::noise(600, 22);
        LibraryBundle::SidecarPayload kept = fx::sidePayload(QStringLiteral("gbc"), QStringLiteral("Kept.gbc"), QStringLiteral("Kept"),
            { fx::sideImage(QStringLiteral("thumbnail"), QStringLiteral("png"), thumb),
              fx::sideImage(QStringLiteral("marquee"), QStringLiteral("png"), marquee) });
        QString err;
        CHECK(fx::landSide(tRoms, LibraryBundle::encodeSidecarV2(kept), &err, nullptr, -1) == LibraryBundle::LandResult::Landed);
        CHECK(fx::readFile(tRoms + QStringLiteral("/gbc/images/Kept-thumb.png")) == QByteArray("USER-ART"));
        CHECK(fx::readFile(tRoms + QStringLiteral("/gbc/images/Kept-marquee.png")) == marquee);
        const QByteArray gbcList = fx::readFile(tRoms + QStringLiteral("/gbc/gamelist.xml"));
        CHECK(!gbcList.contains("<thumbnail>"));                   // the dropped image is not in the entry
        CHECK(gbcList.contains("<marquee>./images/Kept-marquee.png</marquee>"));

        // A source whose gamelist names a video (and only a video) sends no file at all.
        LibraryBundle::SidecarGame onlyVideo;
        onlyVideo.system = QStringLiteral("snes");
        onlyVideo.rom = QStringLiteral("A.sfc");
        onlyVideo.fields.name = QStringLiteral("A");
        onlyVideo.images << qMakePair(QStringLiteral("video"), QStringLiteral("./videos/A-video.mp4"));
        LibraryBundle::SidecarPayload pv;
        CHECK(LibraryBundle::readSidecarPayload(sRoms, onlyVideo, pv, err));
        CHECK(pv.images.isEmpty());
    }

    // ---- 22. an interrupted gamelist rewrite leaves the old file (#292) -----------------------------
    {
        fx::writeTree(tRoms + QStringLiteral("/pce/Turbo.pce"), QByteArray("ROM"));
        const QByteArray pceOld("<?xml version=\"1.0\"?>\n<gameList>\n\t<game>\n\t\t<path>./Other.pce</path>\n\t</game>\n</gameList>\n");
        fx::writeTree(tRoms + QStringLiteral("/pce/gamelist.xml"), pceOld);
        LibraryBundle::SidecarPayload turbo = fx::sidePayload(QStringLiteral("pce"), QStringLiteral("Turbo.pce"), QStringLiteral("Turbo"),
            { fx::sideImage(QStringLiteral("thumbnail"), QStringLiteral("png"), QByteArray("\x89PNG-T", 6)),
              fx::sideImage(QStringLiteral("fanart"), QStringLiteral("jpg"), QByteArray("JPEG-F")) });
        const QByteArray wire = LibraryBundle::encodeSidecarV2(turbo);
        const QMap<QString, QString> before = fx::censusAll(tRoms + QStringLiteral("/pce"));
        QString err;
        for (int failAfter : { 2, 1, 0 })        // 2 = at the gamelist rewrite itself; 1 and 0 = among the images
        {
            const LibraryBundle::LandResult r = fx::landSide(tRoms, wire, &err, nullptr, failAfter);
            if (r != LibraryBundle::LandResult::Interrupted)
                std::fprintf(stderr, "BUNDLEXFER-FAIL interrupt at %d gave %d\n", failAfter, int(r));
            CHECK(r == LibraryBundle::LandResult::Interrupted);
            CHECK(fx::readFile(tRoms + QStringLiteral("/pce/gamelist.xml")) == pceOld);
            CHECK(fx::censusAll(tRoms + QStringLiteral("/pce")) == before);   // no image, no temp file, no folder
        }
        CHECK(fx::landSide(tRoms, wire, &err, nullptr, -1) == LibraryBundle::LandResult::Landed);
        CHECK(fx::readFile(tRoms + QStringLiteral("/pce/images/Turbo-thumb.png")) == QByteArray("\x89PNG-T", 6));
        CHECK(fx::readFile(tRoms + QStringLiteral("/pce/gamelist.xml")).contains("<fanart>./images/Turbo-fanart.jpg</fanart>"));
    }

    // ---- 23. interop: advertised, kind-marked, refused readably where not taken (#292) -------------
    {
        QList<LibraryBundle::Entry> entries;
        LibraryBundle::Entry a; a.id = idA; a.stamp = QStringLiteral("aaa"); a.updatedMs = 1;
        entries << a;
        const QByteArray oldInv = LibraryBundle::inventoryJson(entries);
        CHECK(!LibraryBundle::advertisesSidecars(oldInv));
        CHECK(!LibraryBundle::advertisesSidecars(QByteArray("{\"v\":1,\"items\":[]}")));
        CHECK(!LibraryBundle::advertisesSidecars(QByteArray("{\"v\":1,\"items\":[],\"sidecars\":\"junk\"}")));
        const QByteArray newInv = LibraryBundle::inventoryJson(entries, true);
        CHECK(LibraryBundle::advertisesSidecars(newInv));
        const QJsonObject newObj = QJsonDocument::fromJson(newInv).object();
        CHECK(newObj.value(QStringLiteral("v")).toInt() == 1);          // an old source still reads it
        QList<LibraryBundle::Entry> out;
        QList<int> formats;
        QString err;
        CHECK(LibraryBundle::parseInventory(newInv, out, formats, err) && out.size() == 1);
        CHECK(LibraryBundle::chooseBundleFormat(formats) == 2);           // "bundle" means what it meant
        CHECK(LibraryBundle::parseInventory(newInv, out, err));

        // The kind marker, read from the header alone, with the device put back.
        LibraryBundle::SidecarPayload sp = fx::sidePayload(QStringLiteral("snes"), QStringLiteral("A.sfc"), QStringLiteral("A"),
            { fx::sideImage(QStringLiteral("thumbnail"), QStringLiteral("png"), QByteArray("\x89PNG", 4)) });
        const QByteArray sideWire = LibraryBundle::encodeSidecarV2(sp);
        LibraryBundle::Payload artP;
        CHECK(LibraryBundle::readPayload(srcRoot, idB, artP, err));
        const QByteArray artWire = LibraryBundle::encodePayloadV2(artP);
        auto kindOf = [](const QByteArray& w, qint64* pos) {
            QByteArray c = w;
            QBuffer b(&c);
            b.open(QIODevice::ReadOnly);
            const LibraryBundle::BodyKind k = LibraryBundle::bodyKindV2(b);
            if (pos) *pos = b.pos();
            return k;
        };
        qint64 kpos = -1;
        CHECK(kindOf(sideWire, &kpos) == LibraryBundle::BodyKind::Gamelist && kpos == 0);
        CHECK(kindOf(artWire, &kpos) == LibraryBundle::BodyKind::Art && kpos == 0);
        CHECK(kindOf(QByteArray("not a bundle at all"), &kpos) == LibraryBundle::BodyKind::Unknown && kpos == 0);

        // A gamelist entry handed to the ART decoder -- a target that does not take the kind -- is refused as a
        // kind, readably, before a file byte, and nothing lands in the cache.
        {
            const QString cacheBase = base + QStringLiteral("/sidecar-to-cache");
            const QString cacheRoot = cacheBase + QStringLiteral("/metadata");
            QDir().mkpath(cacheRoot);
            const QMap<QString, QString> before = fx::censusAll(cacheBase);
            QByteArray c1 = sideWire;
            QBuffer d1(&c1);
            d1.open(QIODevice::ReadOnly);
            LibraryBundle::BundleHeader h;
            LibraryBundle::Refusal why = LibraryBundle::Refusal::None;
            QString msg;
            CHECK(!LibraryBundle::decodeHeaderV2(d1, h, why, msg));
            CHECK(why == LibraryBundle::Refusal::UnsupportedKind);
            CHECK(msg.contains(QStringLiteral("gamelist")));
            CHECK(d1.pos() <= fx::headerEndOf(sideWire));
            QByteArray c2 = sideWire;
            QBuffer d2(&c2);
            d2.open(QIODevice::ReadOnly);
            CHECK(LibraryBundle::landItemV2(cacheRoot, d2, err) == LibraryBundle::LandResult::Refused);
            CHECK(fx::censusAll(cacheBase) == before);
        }

        // The routes.
        {
            const RemoteApi::Request get = RemoteApi::parseRequest("GET /gamelists HTTP/1.1\r\n\r\n");
            CHECK(RemoteApi::route(get).kind == RemoteApi::CommandKind::Gamelists);
            const RemoteApi::Request post = RemoteApi::parseRequest("POST /gamelists HTTP/1.1\r\nContent-Length: 2\r\n\r\n{}");
            CHECK(RemoteApi::route(post).kind == RemoteApi::CommandKind::BadRequest);
            CHECK(PlayOn::routeNeedsToken(QStringLiteral("/gamelists")));
            CHECK(RemoteApi::requestCapBytes("GET /gamelists HTTP/1.1\r\n") == RemoteApi::kDefaultRequestCap);
        }

        // Over a real socket.
        const QString sBase = base + QStringLiteral("/sidecar-socket");
        const QString sCache = sBase + QStringLiteral("/metadata");
        const QString sRomsT = sBase + QStringLiteral("/roms");
        QDir().mkpath(sCache);
        fx::writeTree(sRomsT + QStringLiteral("/snes/A.sfc"), QByteArray("ROM-A"));
        const QString token = QStringLiteral("probe-fixture-credential-292");
        const auto head = [&token](const QByteArray& method, const QByteArray& path, qint64 length, bool withToken) {
            QByteArray h = method + " " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\n";
            if (length >= 0) h += "Content-Type: application/x-eb-bundle\r\nContent-Length: " + QByteArray::number(length) + "\r\n";
            if (withToken) h += "X-EB-Token: " + token.toLatin1() + "\r\n";
            return h + "\r\n";
        };

        RemoteServer::Hooks hooks;
        hooks.tokens = [token] { return QSet<QString>{ token }; };
        hooks.inventory = [sCache] { return LibraryBundle::inventoryJson(LibraryBundle::inventoryFor(sCache)); };
        hooks.bundleRoot = [sCache] { return sCache; };
        hooks.bundleStream = [sCache](QIODevice& body) {
            QString e;
            return LibraryBundle::receiptFor(LibraryBundle::landItemV2(sCache, body, e), e);
        };

        // (a) A target WITHOUT the sidecar hooks: does not advertise, refuses the kind with a sentence, 503 on the list.
        {
            RemoteServer old;
            old.setHooks(hooks);
            CHECK(old.start(0));
            const QMap<QString, QString> before = fx::censusAll(sBase);
            const fx::HttpResult r = fx::httpRequest(old.port(), head("POST", "/bundle", sideWire.size(), true), sideWire, -1, false);
            CHECK(r.status == 400);
            LibraryBundle::Receipt rec;
            CHECK(LibraryBundle::parseReceipt(r.body, rec) && rec.result == QStringLiteral("refused"));
            CHECK(rec.reason.contains(QStringLiteral("gamelist")));
            CHECK(fx::censusAll(sBase) == before);
            const fx::HttpResult inv = fx::httpRequest(old.port(), head("GET", "/inventory", -1, true), QByteArray(), 0, false);
            CHECK(inv.status == 200 && !LibraryBundle::advertisesSidecars(inv.body));
            CHECK(fx::httpRequest(old.port(), head("GET", "/gamelists", -1, true), QByteArray(), 0, false).status == 503);
            CHECK(fx::httpRequest(old.port(), head("GET", "/gamelists", -1, false), QByteArray(), 0, false).status == 401);
            old.stop();
        }
        // (b) A target WITH them: advertises, answers the lists behind the token, lands the entry beside the ROM.
        {
            RemoteServer::Hooks full = hooks;
            full.inventory = [sCache] { return LibraryBundle::inventoryJson(LibraryBundle::inventoryFor(sCache), true); };
            full.gamelists = [sRomsT] { return LibraryBundle::sidecarInventoryJson(LibraryBundle::sidecarInventoryFor(sRomsT)); };
            full.sidecarStream = [sRomsT](QIODevice& body) {
                QString e;
                return LibraryBundle::receiptFor(LibraryBundle::landSidecarV2(sRomsT, body, e), e);
            };
            RemoteServer srv;
            srv.setHooks(full);
            CHECK(srv.start(0));
            const fx::HttpResult inv = fx::httpRequest(srv.port(), head("GET", "/inventory", -1, true), QByteArray(), 0, false);
            CHECK(inv.status == 200 && LibraryBundle::advertisesSidecars(inv.body));
            CHECK(fx::httpRequest(srv.port(), head("GET", "/gamelists", -1, false), QByteArray(), 0, false).status == 401);
            const fx::HttpResult lists = fx::httpRequest(srv.port(), head("GET", "/gamelists", -1, true), QByteArray(), 0, false);
            CHECK(lists.status == 200);
            QList<LibraryBundle::SidecarSystem> systems;
            CHECK(LibraryBundle::parseSidecarInventory(lists.body, systems, err));
            CHECK(systems.size() == 1 && systems.first().roms == QStringList{ QStringLiteral("A.sfc") });
            // Without the token: 401 before a spool exists.
            CHECK(fx::httpRequest(srv.port(), head("POST", "/bundle", sideWire.size(), false), sideWire, 0, false).status == 401);
            const fx::HttpResult r = fx::httpRequest(srv.port(), head("POST", "/bundle", sideWire.size(), true), sideWire, -1, false);
            CHECK(r.status == 200);
            LibraryBundle::Receipt rec;
            CHECK(LibraryBundle::parseReceipt(r.body, rec) && rec.result == QStringLiteral("landed"));
            CHECK(fx::readFile(sRomsT + QStringLiteral("/snes/images/A-thumb.png")) == QByteArray("\x89PNG", 4));
            CHECK(fx::readFile(sRomsT + QStringLiteral("/snes/gamelist.xml")).contains("<path>./A.sfc</path>"));
            CHECK(fx::spoolFilesUnder(sCache).isEmpty());
            CHECK(!QFileInfo::exists(sCache + QStringLiteral("/.eb-incoming")));
            // An ART body still lands in the cache through the same listener.
            const fx::HttpResult art = fx::httpRequest(srv.port(), head("POST", "/bundle", artWire.size(), true), artWire, -1, false);
            CHECK(art.status == 200);
            CHECK(QFileInfo::exists(sCache + QLatin1Char('/') + idB + QStringLiteral("/meta.json")));
            // And the second plan is empty.
            const fx::HttpResult lists2 = fx::httpRequest(srv.port(), head("GET", "/gamelists", -1, true), QByteArray(), 0, false);
            QList<LibraryBundle::SidecarGame> srcGames;
            LibraryBundle::SidecarGame g;
            g.system = QStringLiteral("snes"); g.rom = QStringLiteral("A.sfc"); g.fields.name = QStringLiteral("A");
            srcGames << g;
            CHECK(LibraryBundle::parseSidecarInventory(lists2.body, systems, err));
            CHECK(LibraryBundle::planSidecars(srcGames, systems).send.isEmpty());
            srv.stop();
        }

        // Size: a library with thousands of ROMs, which is why the lists do not ride /inventory.
        QList<LibraryBundle::SidecarSystem> big;
        for (int s = 0; s < 10; ++s)
        {
            LibraryBundle::SidecarSystem sys;
            sys.name = QStringLiteral("system%1").arg(s);
            for (int i = 0; i < 500; ++i)
            {
                sys.roms << QStringLiteral("Some Fairly Typical Game Title %1 (USA) (Rev 1).zip").arg(i);
                if (i % 2 == 0) sys.listed << sys.roms.last();
            }
            big << sys;
        }
        std::printf("BUNDLEXFER-INFO /gamelists for 5000 ROMs (half listed) is %lld bytes\n",
                    qint64(LibraryBundle::sidecarInventoryJson(big).size()));
    }

    QDir(base).removeRecursively();

    if (failures == 0) std::printf("BUNDLEXFER-OK\n");
    else               std::fprintf(stderr, "BUNDLEXFER had %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
