// Headless check of romhack installation (src/core/RomhackInstall + RomPatch::writePatched) — the seam that
// turns a downloaded patch into a playable game in the ROMs folder.
//
// The promise this feature makes is that your original ROM is never at risk and a refused patch never becomes
// a file that looks playable, so that is what this pins: the base ROM is hashed before and after every case,
// a refusal writes nothing at all, and a re-install is idempotent rather than littering the library.
//
// Fixtures are hand-built byte arrays constructed from the IPS/BPS specs, with every expected output computed
// by hand — never by running the code under test, which would make the assertion a fixed point that passes
// whatever the applier does (CONTRIBUTING.md). The BPS CRC32 footers were computed with an independent oracle
// (Python zlib.crc32) and hardcoded, so a bug in RomPatch's own CRC32 cannot forge a match and hide behind it.
//
//   * sanitize: path separators and Windows-reserved characters become spaces; a name that reduces to
//     nothing is refused rather than producing "Game ().sfc".
//   * destinationFor: "<base> (<hack>).<base ext>" in the target folder, keeping the ROM's own extension.
//   * destinationForRom: a ONE-file release keeps the clean "<hack>.<ext>"; a release shipping SEVERAL names
//     the file after the revision that was picked, so two revisions cannot land on one path and hand back
//     whichever was installed first.
//   * install: writes the patched bytes, returns the path, leaves the base ROM byte-for-byte unchanged.
//   * idempotent: installing twice yields the same path, the same bytes, and no second file in the folder.
//   * a BPS built for a DIFFERENT ROM is refused on its embedded source checksum, and NOTHING is written.
//   * a buffer that is not a patch at all is refused, and nothing is written.
//   * a hack title that sanitises into the base name is refused rather than overwriting the original.
//
// Prints ROMHACK-OK on success; any failure prints ROMHACK-FAIL <cond> (line) and exits non-zero.
//
// Isolation: AppPaths::dataDir() is this process's own scratch dir (issue #42); every fixture is written under
// it and the tree is removed at exit. Nothing is written beside the exe.
#include "RomhackInstall.h"
#include "RomhackClient.h"
#include "RomPatch.h"
#include "AppPaths.h"
#include "BoundedFetch.h"    // the size decision the last section pins, against a loopback server
#include "RomhackTarget.h"   // the pure decision the last section pins
#include "RemoteLeafResolve.h" // the remote-leaf fallback the last section pins
#include "SystemCatalog.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <cstdio>
#include <functional>
#include <memory>

static int g_fails = 0;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) { std::printf("ROMHACK-FAIL %s (%d)\n", #cond, __LINE__); ++g_fails; } \
    } while (0)

static QByteArray readAll(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QByteArray();
    return f.readAll();
}

static QByteArray sha1(const QByteArray& b)
{
    return QCryptographicHash::hash(b, QCryptographicHash::Sha1);
}

static bool writeFile(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    return f.write(bytes) == bytes.size();
}

// A 32-bit little-endian CRC32 footer as BPS stores it.
static void appendLe32(QByteArray& b, quint32 v)
{
    b.append(char(v & 0xFF));
    b.append(char((v >> 8) & 0xFF));
    b.append(char((v >> 16) & 0xFF));
    b.append(char((v >> 24) & 0xFF));
}

// BPS variable-length number, as the spec encodes it.
static void appendVlq(QByteArray& b, quint64 v)
{
    while (true)
    {
        const quint8 x = quint8(v & 0x7F);
        v >>= 7;
        if (v == 0) { b.append(char(x | 0x80)); break; }
        b.append(char(x));
        --v;
    }
}

// ---- the loopback half of the BoundedFetch section --------------------------------------------
// A minimal HTTP responder, one answer per connection. Binds an EPHEMERAL port — listen(…, 0) — so there is
// no fixed port to be unlucky with on a busy CI box.
//
// `pieces` is what makes the ceiling testable at all: it writes the body in several parts with the event loop
// turning in between, so the reply raises more than one readyRead. A single-shot body would let a fetch that
// only ever checks its size ONCE still look correct, which is precisely the mistake being guarded against.
struct Loopback
{
    QTcpServer srv;
    std::function<QList<QByteArray>(const QByteArray& path)> pieces;
    // A path whose socket is written to and then simply LEFT OPEN. Without it there is no way to test a
    // deadline at all: a server that closes when it runs out of pieces produces a prompt
    // RemoteHostClosedError, which is a different failure reaching the same verdict by a route that proves
    // nothing about the timeout.
    QByteArray stallPath;

    static void writePieces(QTcpSocket* c, std::shared_ptr<QList<QByteArray>> parts, int i, bool keepOpen)
    {
        if (!c || c->state() != QAbstractSocket::ConnectedState) return;
        if (i >= parts->size())
        {
            c->flush();
            if (!keepOpen) c->disconnectFromHost();
            return;
        }
        c->write(parts->at(i));
        c->flush();
        QTimer::singleShot(10, c, [c, parts, i, keepOpen] { writePieces(c, parts, i + 1, keepOpen); });
    }

    bool start()
    {
        if (!srv.listen(QHostAddress::LocalHost, 0)) return false;
        QObject::connect(&srv, &QTcpServer::newConnection, &srv, [this] {
            QTcpSocket* c = srv.nextPendingConnection();
            if (!c) return;
            auto buf = std::make_shared<QByteArray>();
            auto answered = std::make_shared<bool>(false);
            QObject::connect(c, &QTcpSocket::readyRead, c, [this, c, buf, answered] {
                buf->append(c->readAll());
                const int end = buf->indexOf("\r\n\r\n");
                if (end < 0 || *answered) return;
                *answered = true;
                const QByteArray path = buf->left(end).split('\n').value(0).trimmed().split(' ').value(1);
                writePieces(c, std::make_shared<QList<QByteArray>>(pieces(path)), 0,
                            !stallPath.isEmpty() && path == stallPath);
            });
            QObject::connect(c, &QTcpSocket::disconnected, c, &QObject::deleteLater);
        });
        return true;
    }

    QString url(const QString& path) const
    { return QStringLiteral("http://127.0.0.1:%1%2").arg(srv.serverPort()).arg(path); }
};

// One head, with or without a Content-Length. Split from the body so a response can DECLARE a length it never
// delivers. "No Content-Length" is spelled as CHUNKED rather than as a body ended by the connection closing:
// chunked is what a real server without a length actually sends, and it ends the body definitively, where a
// close-delimited body leaves "the transfer finished" and "the peer went away" as the same event.
static QByteArray head200(qint64 declaredLength)
{
    QByteArray h = "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n";
    if (declaredLength >= 0) h += "Content-Length: " + QByteArray::number(declaredLength) + "\r\n";
    else                     h += "Transfer-Encoding: chunked\r\n";
    h += "\r\n";
    return h;
}
static QByteArray chunked(const QByteArray& data)
{ return QByteArray::number(data.size(), 16) + "\r\n" + data + "\r\n"; }
static QByteArray chunkedEnd() { return QByteArray("0\r\n\r\n"); }

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("EverythingBoxProbe"));
    QCoreApplication::setApplicationName(QStringLiteral("probe_romhack"));

    const QString root = AppPaths::dataDir() + QStringLiteral("/romhack-probe");
    QDir(root).removeRecursively();
    QDir().mkpath(root);

    // ---- sanitizeHackTitle -------------------------------------------------------------------------------
    CHECK(RomhackInstall::sanitizeHackTitle(QStringLiteral("Flames of Eternity"))
          == QStringLiteral("Flames of Eternity"));
    // Reserved characters become spaces and collapse; the name stays readable rather than being glued shut.
    CHECK(RomhackInstall::sanitizeHackTitle(QStringLiteral("Zelda: Parallel/Worlds"))
          == QStringLiteral("Zelda Parallel Worlds"));
    // A traversal attempt cannot survive into a path segment.
    CHECK(!RomhackInstall::sanitizeHackTitle(QStringLiteral("../../evil")).contains(QLatin1Char('/')));
    CHECK(!RomhackInstall::sanitizeHackTitle(QStringLiteral("..\\..\\evil")).contains(QLatin1Char('\\')));
    // Nothing usable left => empty, which install() refuses rather than writing "Game ().sfc".
    CHECK(RomhackInstall::sanitizeHackTitle(QStringLiteral("///")).isEmpty());
    CHECK(RomhackInstall::sanitizeHackTitle(QStringLiteral("   ")).isEmpty());
    // A trailing dot is legal to construct and impossible to open on Windows.
    CHECK(!RomhackInstall::sanitizeHackTitle(QStringLiteral("Hack v1.")).endsWith(QLatin1Char('.')));

    // ---- destinationFor ----------------------------------------------------------------------------------
    {
        const QString base = root + QStringLiteral("/roms/snes/Chrono Trigger.sfc");
        const QString dest = RomhackInstall::destinationFor(base, QStringLiteral("Flames of Eternity"),
                                                            root + QStringLiteral("/roms/snes"));
        CHECK(QFileInfo(dest).fileName() == QStringLiteral("Chrono Trigger (Flames of Eternity).sfc"));
        // The base ROM's own extension is kept: patching does not change the container, and the emulator
        // resolves the system from it downstream.
        CHECK(QFileInfo(dest).suffix() == QStringLiteral("sfc"));
        CHECK(RomhackInstall::destinationFor(base, QStringLiteral("///"),
                                             root + QStringLiteral("/roms/snes")).isEmpty());
    }

    // ---- a real IPS install ------------------------------------------------------------------------------
    // Source: 8 bytes 00..07. Patch: one record writing 0xAA,0xBB at offset 2. Expected output computed by
    // hand from the IPS spec: 00 01 AA BB 04 05 06 07.
    const QByteArray source = QByteArray::fromHex("0001020304050607");
    QByteArray ips;
    ips.append("PATCH");
    ips.append(char(0x00)); ips.append(char(0x00)); ips.append(char(0x02));   // offset 2 (3 bytes, BE)
    ips.append(char(0x00)); ips.append(char(0x02));                           // length 2 (2 bytes, BE)
    ips.append(char(0xAA)); ips.append(char(0xBB));                           // the data
    ips.append("EOF");
    const QByteArray expected = QByteArray::fromHex("0001AABB04050607");

    const QString romsDir = root + QStringLiteral("/roms/snes");
    const QString baseRom = romsDir + QStringLiteral("/Chrono Trigger.sfc");
    CHECK(writeFile(baseRom, source));
    const QByteArray baseBefore = sha1(readAll(baseRom));

    QString err;
    const QString installed = RomhackInstall::install(baseRom, ips, QStringLiteral("Flames of Eternity"),
                                                      romsDir, &err);
    CHECK(!installed.isEmpty());
    CHECK(err.isEmpty());
    CHECK(QFileInfo::exists(installed));
    CHECK(readAll(installed) == expected);
    CHECK(QFileInfo(installed).fileName() == QStringLiteral("Chrono Trigger (Flames of Eternity).sfc"));
    // The promise: the original is untouched.
    CHECK(sha1(readAll(baseRom)) == baseBefore);

    // ---- idempotent --------------------------------------------------------------------------------------
    {
        const int before = QDir(romsDir).entryList(QDir::Files).size();
        QString err2;
        const QString again = RomhackInstall::install(baseRom, ips, QStringLiteral("Flames of Eternity"),
                                                      romsDir, &err2);
        CHECK(again == installed);
        CHECK(err2.isEmpty());
        CHECK(readAll(again) == expected);
        // No "Chrono Trigger (Flames of Eternity) (1).sfc" and no leftover .part.
        CHECK(QDir(romsDir).entryList(QDir::Files).size() == before);
    }

    // ---- a BPS built for a DIFFERENT ROM is refused, and nothing is written -------------------------------
    {
        // Header targets a source whose CRC32 is 0xDEADBEEF — not our 8-byte source — so the applier must
        // refuse on the embedded checksum rather than produce a corrupt game.
        QByteArray bps;
        bps.append("BPS1");
        appendVlq(bps, 8);          // source size
        appendVlq(bps, 8);          // target size
        appendVlq(bps, 0);          // metadata size
        appendVlq(bps, (8 << 2) | 0); // one SourceRead action covering the whole file
        appendLe32(bps, 0xDEADBEEFu); // source CRC32: deliberately not ours
        appendLe32(bps, 0x88AA4B2Du); // target CRC32 (independent oracle; unreachable — source check fails first)
        appendLe32(bps, 0x00000000u); // patch CRC32 placeholder

        const int before = QDir(romsDir).entryList(QDir::Files).size();
        QString berr;
        const QString bad = RomhackInstall::install(baseRom, bps, QStringLiteral("Wrong Dump Hack"),
                                                    romsDir, &berr);
        CHECK(bad.isEmpty());
        CHECK(!berr.isEmpty());                                   // refusals are never silent
        CHECK(QDir(romsDir).entryList(QDir::Files).size() == before); // nothing written, not even a .part
        CHECK(!QFileInfo::exists(romsDir + QStringLiteral("/Chrono Trigger (Wrong Dump Hack).sfc")));
        CHECK(sha1(readAll(baseRom)) == baseBefore);
    }

    // ---- a buffer that is not a patch at all is refused ---------------------------------------------------
    {
        const int before = QDir(romsDir).entryList(QDir::Files).size();
        QString nerr;
        const QString bad = RomhackInstall::install(baseRom, QByteArray("this is just a readme"),
                                                    QStringLiteral("Not A Patch"), romsDir, &nerr);
        CHECK(bad.isEmpty());
        CHECK(!nerr.isEmpty());
        CHECK(QDir(romsDir).entryList(QDir::Files).size() == before);
    }

    // ---- a hack title that collapses onto the base name is refused ----------------------------------------
    {
        QString oerr;
        // destinationFor always parenthesises, so the only way to collide is to aim at the base file itself.
        const QString collide = RomhackInstall::install(baseRom, ips, QStringLiteral("Flames of Eternity"),
                                                        romsDir, &oerr);
        CHECK(collide != baseRom);       // never the original, whatever the title
        CHECK(sha1(readAll(baseRom)) == baseBefore);
    }

    // ---- an empty patch and a missing base ROM are errors, not crashes ------------------------------------
    {
        QString e1, e2;
        CHECK(RomhackInstall::install(baseRom, QByteArray(), QStringLiteral("X"), romsDir, &e1).isEmpty());
        CHECK(!e1.isEmpty());
        CHECK(RomhackInstall::install(romsDir + QStringLiteral("/nope.sfc"), ips,
                                      QStringLiteral("X"), romsDir, &e2).isEmpty());
        CHECK(!e2.isEmpty());
    }

    // ---- the archived case: patch the EXTRACTED ROM, name the install after the library entry ------------
    // The archive is unpacked by the caller (ArchiveRom), so what reaches install() is a temp file whose name
    // is whatever was inside the .7z. Without the override the game would install under that inner name; with
    // it, the name comes from the library entry and only the EXTENSION comes from the extracted ROM — which
    // is the whole point, because the archive's own ".7z" is what would otherwise be kept.
    {
        const QString tempExtracted = root + QStringLiteral("/tmp/smb3 (U) [!].nes");
        CHECK(writeFile(tempExtracted, source));
        const QByteArray extractedBefore = sha1(readAll(tempExtracted));

        QString aerr;
        const QString out = RomhackInstall::install(tempExtracted, ips, QStringLiteral("Flames of Eternity"),
                                                   romsDir, &aerr,
                                                   QStringLiteral("Super Mario Bros. 3"));
        CHECK(!out.isEmpty());
        CHECK(aerr.isEmpty());
        // Named for the library entry, extension from the EXTRACTED rom — never ".7z", never the inner name.
        CHECK(QFileInfo(out).fileName() == QStringLiteral("Super Mario Bros. 3 (Flames of Eternity).nes"));
        CHECK(QFileInfo(out).absolutePath() == QFileInfo(romsDir).absoluteFilePath());
        CHECK(readAll(out) == expected);
        // The extracted source is left alone too — patching reads it, never rewrites it.
        CHECK(sha1(readAll(tempExtracted)) == extractedBefore);

        // An override that sanitises to nothing is refused rather than producing " (Hack).nes".
        QString berr;
        CHECK(RomhackInstall::install(tempExtracted, ips, QStringLiteral("Hack"), romsDir, &berr,
                                      QStringLiteral("///")).isEmpty());
        CHECK(!berr.isEmpty());
    }

    // ---- RomhackClient: parsing what the server says, and building the URLs to ask it ---------------------
    {
        const QByteArray listJson = R"JSON([
          {"id":"rhdn:translations:7272","source":"ROMhacking.net","title":"Last Battle",
           "releasedBy":"Ernani S. Costa","version":"1.2","category":"Translation","language":"EN",
           "genre":"Action","date":"30 Apr 2024"},
          {"id":"","title":"No id, unfetchable"},
          {"id":"rhdn:hacks:99","title":"Sparse"}
        ])JSON";
        const QVector<RomhackEntry> rows = RomhackClient::parseList(listJson);
        // The id-less row is dropped: a row we could never fetch is not worth offering.
        CHECK(rows.size() == 2);
        CHECK(rows[0].id == QStringLiteral("rhdn:translations:7272"));
        CHECK(rows[0].title == QStringLiteral("Last Battle"));
        CHECK(rows[0].menuLabel().contains(QStringLiteral("Last Battle")));
        CHECK(rows[0].menuLabel().contains(QStringLiteral("ROMhacking.net")));
        // A sparse row reads as a plain title, not as a row of empty separators.
        CHECK(!rows[1].menuLabel().contains(QStringLiteral("·")));

        // Anything that is not a JSON array means "no hacks", never a crash: an error page, a challenge
        // body, a truncated response.
        CHECK(RomhackClient::parseList(QByteArray("<html>Just a moment...</html>")).isEmpty());
        CHECK(RomhackClient::parseList(QByteArray("{}")).isEmpty());
        CHECK(RomhackClient::parseList(QByteArray()).isEmpty());

        // A fetch carries a URL per patch, not the file. A patch may be a 14-byte IPS or a gigabyte-scale
        // pre-applied disc image, and only one of those fits inside a JSON response: embedding the large one
        // put a measured 5,286 MB into the server for a single fetch, and offered no Range, so no resume.
        // A delimiter of its own on the raw string, because the targetNote below carries a ')'.
        const QByteArray fetchJson = QByteArray(R"JSON({"id":"rhdn:hacks:1","version":"1.0",
          "targetNote":"BIN Format (GEN)","patches":[
            {"name":"hack.ips","patchFormat":"ips","url":"romhack-file/L3RtcC9oYWNrLmlwcw"}]})JSON");
        const RomhackFetch f = RomhackClient::parseFetch(fetchJson);
        CHECK(f.valid);
        CHECK(f.patches.size() == 1);
        CHECK(f.patches[0].name == QStringLiteral("hack.ips"));
        CHECK(f.patches[0].format == QStringLiteral("ips"));
        CHECK(f.patches[0].url == QStringLiteral("romhack-file/L3RtcC9oYWNrLmlwcw"));
        CHECK(f.targetNote == QStringLiteral("BIN Format (GEN)"));

        // A url we will not follow is dropped at PARSE time, so it never becomes a menu row that can only
        // fail after a person has read it and chosen it.
        CHECK(!RomhackClient::parseFetch(QByteArray(
            R"JSON({"id":"x","patches":[{"name":"a","patchFormat":"ips","url":"https://evil.example/x"}]})JSON")).valid);
        CHECK(!RomhackClient::parseFetch(QByteArray(
            R"JSON({"id":"x","patches":[{"name":"a","patchFormat":"ips","url":"/etc/passwd"}]})JSON")).valid);
        CHECK(!RomhackClient::parseFetch(QByteArray(
            R"JSON({"id":"x","patches":[{"name":"a","patchFormat":"ips","url":""}]})JSON")).valid);
        // …and a bad row beside a good one drops only itself, rather than failing the whole release.
        {
            const RomhackFetch mixed = RomhackClient::parseFetch(QByteArray(
                R"JSON({"id":"x","patches":[
                  {"name":"bad","patchFormat":"ips","url":"https://evil.example/x"},
                  {"name":"good","patchFormat":"ips","url":"romhack-file/AAA"}]})JSON"));
            CHECK(mixed.valid);
            CHECK(mixed.patches.size() == 1);
            CHECK(mixed.patches[0].name == QStringLiteral("good"));
        }

        // No usable patch => not a valid fetch. There is nothing to install, and saying so here saves every
        // caller from checking the list separately.
        CHECK(!RomhackClient::parseFetch(QByteArray(R"JSON({"id":"x","patches":[]})JSON")).valid);
        CHECK(!RomhackClient::parseFetch(QByteArray("nonsense")).valid);

        // A patch is carried by url and ONLY by url. A response that still embeds base64 gets no special
        // handling: the url decides, and bytes riding along are neither read nor accepted as a substitute.
        {
            const RomhackFetch legacy = RomhackClient::parseFetch(QByteArray(
                R"JSON({"id":"x","patches":[{"name":"a","patchFormat":"ips","bytes":"UEFUQ0hFT0Y="}]})JSON"));
            CHECK(!legacy.valid);
        }

        // ---- a hack published as a FINISHED ROM ----------------------------------------------------------
        // No base ROM, no patch: the bytes ARE the game, and they arrive through the ordinary download queue.
        // So what is left to decide here is the NAME that download lands under — which is exactly where a
        // release shipping more than one revision can go silently wrong.
        {
            const QString romDir = root + QStringLiteral("/finished");
            QDir().mkpath(romDir);

            // Named for ITSELF. A finished ROM's own name already says which game and which hack, so pairing
            // it with the base game would read "Arkanoid (Arkanoid (J) [T-Port])".
            const QString dest = RomhackInstall::destinationForRom(
                QStringLiteral("Arkanoid (J) [T-Port]"), QStringLiteral("nes"), romDir);
            CHECK(QFileInfo(dest).fileName() == QStringLiteral("Arkanoid (J) [T-Port].nes"));
            // A leading dot on the extension must not double it.
            CHECK(RomhackInstall::destinationForRom(QStringLiteral("X"), QStringLiteral(".nes"), romDir)
                  == RomhackInstall::destinationForRom(QStringLiteral("X"), QStringLiteral("nes"), romDir));

            // A release shipping MORE THAN ONE finished ROM. The UI asks which, showing the files' own
            // names, and passes the chosen one down here — because the hack TITLE is the same for every
            // revision, and without the variant both land on ONE path: the second install finds a file
            // already there, adopts it, and announces the revision the user did not pick. So they must part.
            const QString usa = RomhackInstall::destinationForRom(
                QStringLiteral("Hack v1.2"), QStringLiteral("nes"), romDir,
                QStringLiteral("Hack v1.2 (USA)"));
            const QString eur = RomhackInstall::destinationForRom(
                QStringLiteral("Hack v1.2"), QStringLiteral("nes"), romDir,
                QStringLiteral("Hack v1.2 (Europe)"));
            CHECK(!usa.isEmpty() && !eur.isEmpty());
            CHECK(usa != eur);
            // The variant already carries the title, so it stands alone rather than doubling it.
            CHECK(QFileInfo(usa).fileName() == QStringLiteral("Hack v1.2 (USA).nes"));
            CHECK(QFileInfo(eur).fileName() == QStringLiteral("Hack v1.2 (Europe).nes"));

            // A bare revision marker qualifies the title instead of replacing it — a file called "usa.nes"
            // sitting in the ROMs folder names no game at all.
            CHECK(QFileInfo(RomhackInstall::destinationForRom(
                      QStringLiteral("Hack v1.2"), QStringLiteral("nes"), romDir, QStringLiteral("usa")))
                      .fileName() == QStringLiteral("Hack v1.2 (usa).nes"));
            CHECK(RomhackInstall::destinationForRom(QStringLiteral("Hack v1.2"), QStringLiteral("nes"),
                                                    romDir, QStringLiteral("usa"))
                  != RomhackInstall::destinationForRom(QStringLiteral("Hack v1.2"), QStringLiteral("nes"),
                                                       romDir, QStringLiteral("eur")));

            // Still idempotent WITH a variant: the same pick recomputes the same path, which is the property
            // the adopt-what-is-already-there short-circuit at the call site rests on.
            CHECK(RomhackInstall::destinationForRom(QStringLiteral("Hack v1.2"), QStringLiteral("nes"),
                                                    romDir, QStringLiteral("Hack v1.2 (USA)")) == usa);

            // A ONE-file release passes no variant and keeps the clean name. The shape above must not creep
            // into the single-patch case, which is the one people actually re-run.
            CHECK(QFileInfo(RomhackInstall::destinationForRom(
                      QStringLiteral("Hack v1.2"), QStringLiteral("nes"), romDir)).fileName()
                  == QStringLiteral("Hack v1.2.nes"));

            // A variant that sanitises away to nothing is REFUSED, not quietly dropped back to the colliding
            // name — the caller passed one precisely because the title alone will not do.
            CHECK(RomhackInstall::destinationForRom(QStringLiteral("Hack v1.2"), QStringLiteral("nes"),
                                                    romDir, QStringLiteral("///")).isEmpty());
            // …and a title that sanitises away is still refused whatever the variant says.
            CHECK(RomhackInstall::destinationForRom(QStringLiteral("///"), QStringLiteral("nes"), romDir,
                                                    QStringLiteral("USA")).isEmpty());
        }

        // ---- the dump a patch says it targets ------------------------------------------------------------
        // The point of the field: IPS applies cleanly to ANY bytes, so a stated target is the only thing that
        // can tell a right ROM from a wrong one before the damage is done.
        {
            const QByteArray withTarget =
                "{\"id\":\"x\",\"patches\":[{\"name\":\"p.ips\",\"patchFormat\":\"ips\","
                "\"url\":\"romhack-file/AAA\"}],"
                "\"target\":{\"fileName\":\"Some Game (Japan).sfc\",\"crc32\":\"C1BC267D\","
                "\"sha1\":\"E937B54FFF99838E2E853697E4F559359AA91FD6\",\"region\":\"Japan\"}}";
            const RomhackFetch f = RomhackClient::parseFetch(withTarget);
            CHECK(f.valid);
            CHECK(f.target.fileName == QStringLiteral("Some Game (Japan).sfc"));
            CHECK(f.target.region == QStringLiteral("Japan"));
            // Lowercased on the way in, so no comparison downstream can turn on the source's casing.
            CHECK(f.target.crc32 == QStringLiteral("c1bc267d"));
            CHECK(f.target.sha1 == QStringLiteral("e937b54fff99838e2e853697e4f559359aa91fd6"));
            CHECK(f.target.checkable());
            CHECK(!f.target.isEmpty());

            // A source that states nothing must not look like one that stated something: the applier REFUSES
            // on a mismatch, so a target invented here would refuse the right ROM.
            const QByteArray noTarget =
                "{\"id\":\"x\",\"patches\":[{\"name\":\"p.ips\",\"patchFormat\":\"ips\","
                "\"url\":\"romhack-file/AAA\"}]}";
            const RomhackFetch n = RomhackClient::parseFetch(noTarget);
            CHECK(n.valid);
            CHECK(n.target.isEmpty());
            CHECK(!n.target.checkable());

            // A region alone names a release for a PERSON but cannot be compared to a file.
            const QByteArray regionOnly =
                "{\"id\":\"x\",\"patches\":[{\"name\":\"p.ips\",\"patchFormat\":\"ips\","
                "\"url\":\"romhack-file/AAA\"}],\"target\":{\"region\":\"J\"}}";
            const RomhackFetch r = RomhackClient::parseFetch(regionOnly);
            CHECK(!r.target.isEmpty());
            CHECK(!r.target.checkable());
        }

        // The checksum the verification compares with is the SAME one the patch formats are verified with —
        // two implementations would be two chances to disagree about one file. ("123456789" is the standard
        // CRC-32 check vector.)
        CHECK(RomPatch::crc32(QByteArray("123456789", 9)) == 0xCBF43926u);

        // ---- checking a ROM against a stated target ------------------------------------------------------
        {
            const QString dir = root + QStringLiteral("/verify");
            QDir().mkpath(dir);
            const QString rom = dir + QStringLiteral("/game.nes");
            CHECK(writeFile(rom, QByteArray("123456789", 9)));
            const QString crc = QStringLiteral("cbf43926");
            const QString sha = QString::fromLatin1(
                QCryptographicHash::hash(QByteArray("123456789", 9), QCryptographicHash::Sha1).toHex());

            CHECK(RomhackInstall::romMatches(rom, crc, QString()));
            CHECK(RomhackInstall::romMatches(rom, QString(), sha));
            CHECK(RomhackInstall::romMatches(rom, crc.toUpper(), QString()));   // never turns on case
            // SHA-1 wins when both are stated: a source publishing both is not offering a choice.
            CHECK(RomhackInstall::romMatches(rom, QStringLiteral("deadbeef"), sha));
            CHECK(!RomhackInstall::romMatches(rom, crc, QString(40, QLatin1Char('a'))));

            // The refusals, each of which must be a refusal and not an accident-shaped pass.
            CHECK(!RomhackInstall::romMatches(rom, QStringLiteral("deadbeef"), QString()));
            CHECK(!RomhackInstall::romMatches(rom, QString(), QString()));      // nothing stated to match
            CHECK(!RomhackInstall::romMatches(dir + QStringLiteral("/nope.nes"), crc, QString()));

            // A checksum with a leading zero is still eight wide; formatted shorter it could never match.
            const QString zeroLead = dir + QStringLiteral("/z.nes");
            for (int i = 0; i < 4096; ++i)
            {
                QByteArray probe(i + 1, char(i & 0x7f));
                const QString hex = QStringLiteral("%1").arg(RomPatch::crc32(probe), 8, 16, QLatin1Char('0'));
                if (!hex.startsWith(QLatin1Char('0'))) continue;
                CHECK(writeFile(zeroLead, probe));
                CHECK(RomhackInstall::romMatches(zeroLead, hex, QString()));
                break;
            }
        }

        // ---- the URLs ------------------------------------------------------------------------------------
        CHECK(RomhackClient::listUrl(QStringLiteral("https://h/tok/"), QStringLiteral("snes"),
                                     QStringLiteral("Chrono Trigger"))
              == QStringLiteral("https://h/tok/romhacks/snes?title=Chrono%20Trigger"));
        // THE injection guard: an id shaped like a URL becomes ONE percent-encoded segment on our own
        // server, never a request to somewhere else.
        const QString hostile = RomhackClient::fetchUrl(QStringLiteral("https://h/tok"),
                                                        QStringLiteral("https://evil.example/x"));
        CHECK(hostile.startsWith(QStringLiteral("https://h/tok/romhack/")));
        CHECK(!hostile.contains(QStringLiteral("evil.example/x")));   // the slashes are encoded away
        CHECK(RomhackClient::fetchUrl(QStringLiteral("https://h/tok"), QStringLiteral("rhdn:hacks:1"))
              == QStringLiteral("https://h/tok/romhack/rhdn%3Ahacks%3A1"));

        // ---- the patch FILE url: relative to the server we already asked, or not followed at all --------
        CHECK(RomhackClient::isSafeRelativeFileUrl(QStringLiteral("romhack-file/L3RtcA")));
        // ".." is a SEGMENT rule, not a substring one: a token that merely carries dots climbs nowhere.
        CHECK(RomhackClient::isSafeRelativeFileUrl(QStringLiteral("romhack-file/a..b")));
        CHECK(!RomhackClient::isSafeRelativeFileUrl(QStringLiteral("https://evil.example/x")));
        CHECK(!RomhackClient::isSafeRelativeFileUrl(QStringLiteral("//evil.example/x")));   // protocol-relative
        CHECK(!RomhackClient::isSafeRelativeFileUrl(QStringLiteral("/romhack-file/x")));    // rooted on the host
        CHECK(!RomhackClient::isSafeRelativeFileUrl(QStringLiteral("file:///C:/Windows/win.ini")));
        CHECK(!RomhackClient::isSafeRelativeFileUrl(QStringLiteral("C:/Windows/win.ini")));  // "C:" IS a scheme
        CHECK(!RomhackClient::isSafeRelativeFileUrl(QStringLiteral("..\\..\\secrets")));
        CHECK(!RomhackClient::isSafeRelativeFileUrl(QStringLiteral("a/../../b")));
        CHECK(!RomhackClient::isSafeRelativeFileUrl(QStringLiteral("..")));
        CHECK(!RomhackClient::isSafeRelativeFileUrl(QString()));
        CHECK(!RomhackClient::isSafeRelativeFileUrl(QStringLiteral("   ")));
        // The ".." rule reads the raw string, so an encoded climb is only refused by refusing '%' itself.
        CHECK(!RomhackClient::isSafeRelativeFileUrl(QStringLiteral("a/%2e%2e/b")));
        CHECK(!RomhackClient::isSafeRelativeFileUrl(QStringLiteral("a/%2E%2E/b")));
        CHECK(!RomhackClient::isSafeRelativeFileUrl(QStringLiteral("a/..%2f..%2fb")));
        // A fragment never leaves the client and a query is not the path, so either one names one file and
        // fetches another.
        CHECK(!RomhackClient::isSafeRelativeFileUrl(QStringLiteral("romhack-file/AAA#x")));
        CHECK(!RomhackClient::isSafeRelativeFileUrl(QStringLiteral("romhack-file/AAA?x=1")));
        // A ':' AFTER the first slash is an ordinary path character, not a scheme. Refusing it would refuse
        // legitimate references, so the rule is RFC 3986's and not "contains a colon".
        CHECK(RomhackClient::isSafeRelativeFileUrl(QStringLiteral("romhack-file/a:b")));

        CHECK(RomhackClient::fileUrl(QStringLiteral("https://h/tok/"), QStringLiteral("romhack-file/AAA"))
              == QStringLiteral("https://h/tok/romhack-file/AAA"));
        CHECK(RomhackClient::fileUrl(QStringLiteral("https://h/tok"), QStringLiteral("romhack-file/AAA"))
              == QStringLiteral("https://h/tok/romhack-file/AAA"));
        // THE injection guard, in the one shape a url can arrive that an id cannot.
        CHECK(RomhackClient::fileUrl(QStringLiteral("https://h/tok"),
                                     QStringLiteral("https://evil.example/x")).isEmpty());
        CHECK(RomhackClient::fileUrl(QString(), QStringLiteral("romhack-file/AAA")).isEmpty());
        // Padding around an otherwise good reference is the server's whitespace, not part of the path.
        CHECK(RomhackClient::fileUrl(QStringLiteral("https://h/tok"), QStringLiteral(" romhack-file/AAA "))
              == QStringLiteral("https://h/tok/romhack-file/AAA"));
    }

    // ---- which console the BASE-ROM crawl carries (browse/RomhackTarget.h) --------------------------------
    // The live defect this pins: Romhacks was offered on a game reached from Recents, the patch was fetched
    // fine, and then the base-ROM download failed with "Nothing here could be downloaded." because the crawl
    // went looking with no console. The verb is offered on three signals (the item's system hint, the ROM's
    // folder/extension, the console page); the crawl used to read only the third, by walking the browse stack
    // for a "platform" level. Off a console page there is no such level, and the search that finds a base ROM
    // answers NOTHING to a bare title — it asks for a console to be named as well.
    {
        // (a) Drilled in from a console page: the page is the console, exactly as before.
        const QVector<browse::CrawlLevel> drilled = {
            { QStringLiteral("Games"),                QStringLiteral("directory") },
            { QStringLiteral("SNES / Super Famicom"), QStringLiteral("platform") },
            { QStringLiteral("A-M"),                  QStringLiteral("directory") },
        };
        const browse::CrawlParent onPage = browse::romhackCrawlParent(drilled, QStringLiteral("snes"));
        CHECK(onPage.title == QStringLiteral("SNES / Super Famicom"));
        CHECK(onPage.type == QStringLiteral("platform"));
        // The PAGE stays authoritative over the system id — it is the more specific answer, and a system
        // resolved some other way must not quietly redirect a download started from a console page.
        const browse::CrawlParent stillPage = browse::romhackCrawlParent(drilled, QStringLiteral("nes"));
        CHECK(stillPage.title == QStringLiteral("SNES / Super Famicom"));
        CHECK(stillPage.type == QStringLiteral("platform"));

        // (b) THE DEFECT: no platform level anywhere on the stack (Recents, a search result, a favourites
        // row), where the item's own hint is the only thing that knows the console. The crawl must still
        // carry one, and it must be the console the verb was offered on.
        const QVector<browse::CrawlLevel> noPlatform = {
            { QStringLiteral("Home"),            QStringLiteral("directory") },
            { QStringLiteral("Recently played"), QStringLiteral("directory") },
        };
        const browse::CrawlParent recents = browse::romhackCrawlParent(noPlatform, QStringLiteral("snes"));
        CHECK(recents.type == QStringLiteral("platform"));          // else the crawl reads no console at all
        CHECK(recents.title != QStringLiteral("Recently played"));  // a shelf name is not a console
        CHECK(SystemCatalog::forConsoleName(recents.title) != nullptr);
        CHECK(SystemCatalog::forConsoleName(recents.title)
              && SystemCatalog::forConsoleName(recents.title)->id == QStringLiteral("snes"));

        // (c) No system to derive one from: nothing is INVENTED. A wrong console would send the search to the
        // wrong platform, which is worse than sending it without one.
        const browse::CrawlParent noSystem = browse::romhackCrawlParent(noPlatform, QString());
        CHECK(noSystem.title == QStringLiteral("Recently played"));
        CHECK(noSystem.type == QStringLiteral("directory"));
        // ... and an id no catalog knows is the same "no console", never a name.
        CHECK(browse::romhackCrawlParent(noPlatform, QStringLiteral("nosuchsystem")).type
              == QStringLiteral("directory"));

        // (d) An empty stack is not a crash, and still answers with the console when it can.
        const QVector<browse::CrawlLevel> empty;
        CHECK(browse::romhackCrawlParent(empty, QString()).title.isEmpty());
        const browse::CrawlParent bare = browse::romhackCrawlParent(empty, QStringLiteral("gba"));
        CHECK(bare.type == QStringLiteral("platform"));
        CHECK(SystemCatalog::forConsoleName(bare.title)
              && SystemCatalog::forConsoleName(bare.title)->id == QStringLiteral("gba"));

        // (e) The name a system is searched BY: what people call the console, never the shelf label's list of
        // alternate spellings and never the emulator that runs it. (The full round trip over every built-in
        // system is pinned in probe_syscatalog, beside the catalog itself.)
        CHECK(SystemCatalog::consoleNameFor(QStringLiteral("snes")) == QStringLiteral("SNES"));
        CHECK(SystemCatalog::consoleNameFor(QStringLiteral("nes")) == QStringLiteral("NES"));
        CHECK(SystemCatalog::consoleNameFor(QStringLiteral("gc")) == QStringLiteral("GameCube"));
        CHECK(SystemCatalog::consoleNameFor(QStringLiteral("psp")) == QStringLiteral("PlayStation Portable"));
        CHECK(SystemCatalog::consoleNameFor(QStringLiteral("gba")) == QStringLiteral("Game Boy Advance"));
        CHECK(SystemCatalog::consoleNameFor(QStringLiteral("nosuchsystem")).isEmpty());
        CHECK(SystemCatalog::consoleNameFor(QString()).isEmpty());
    }

    // ---- a remote source's game leaf that resolves to nothing (browse/RemoteLeafResolve.h) ----------------
    // The live defect this pins, measured against a running server: /stream/game/<a real ROM id>.json answers
    // with one stream, but /stream/game/igdb:1022.json and /stream/game/tgdb:1022.json answer with ZERO — and a
    // game leaf browsed from a console page or a metadata catalog carries exactly such a metadata id. The
    // remote path emitted nothing and moved on, so pressing Romhacks on A Link to the Past ended in "Nothing
    // here could be downloaded." The search that finds it does exist: the same catalog answers
    // search=Zelda A Link to the Past SNES with the real ROM, whose id then resolves to one stream. It was
    // simply unreachable from this transport, because only the LOCAL bridge ever searched by title+console.
    //
    // Not romhack-specific: this is the ordinary Download verb on any game leaf from a remote addon.
    {
        // A recording harness for the crawl's three sinks. `finishes` is the one that matters most — dlNext()
        // must run exactly once per leaf on every path, or the queue advances past an unresolved node (twice)
        // or hangs on a toast that never clears (never).
        struct Trace
        {
            int  emits = 0, searches = 0, finishes = 0;
            QString emittedUrl, emittedMime;
            browse::RemoteLeafPlan issued;
        };
        Trace t;
        browse::RemoteLeafSinks sinks;
        sinks.emitFound = [&t](const QString& u, const QString& m) { ++t.emits; t.emittedUrl = u; t.emittedMime = m; };
        sinks.search    = [&t](const browse::RemoteLeafPlan& p) { ++t.searches; t.issued = p; };
        sinks.finish    = [&t] { ++t.finishes; };
        auto reset = [&t] { t = Trace(); };

        // (a) The id resolved. The fast path is UNTOUCHED: the file is queued, the crawl moves on, and no
        // search is issued — an id that works must not start paying for a second round trip.
        reset();
        browse::remoteLeafResolved(QStringLiteral("http://host/rom.sfc"), QStringLiteral("application/zip"),
                                   QStringLiteral("game"), QStringLiteral("The Legend of Zelda: A Link to the Past"),
                                   QStringLiteral("SNES"), QStringLiteral("platform"), sinks);
        CHECK(t.searches == 0);
        CHECK(t.emits == 1);
        CHECK(t.emittedUrl == QStringLiteral("http://host/rom.sfc"));
        CHECK(t.emittedMime == QStringLiteral("application/zip"));
        CHECK(t.finishes == 1);

        // (b) THE DEFECT: the id resolved to nothing and the leaf is a game. A title+console search is issued,
        // carrying BOTH words — the console is what makes the search answerable at all — and the result is
        // emitted. The judging title is the game's own, without the console, or every candidate looks wrong.
        reset();
        browse::remoteLeafResolved(QString(), QString(),
                                   QStringLiteral("game"), QStringLiteral("The Legend of Zelda: A Link to the Past"),
                                   QStringLiteral("SNES"), QStringLiteral("platform"), sinks);
        CHECK(t.searches == 1);
        CHECK(t.emits == 0);        // nothing may be queued before the search answers
        CHECK(t.finishes == 0);     // ... and the crawl may not move on either: stage 2 owns the finish
        CHECK(t.issued.search);
        CHECK(t.issued.consoleKnown);
        CHECK(t.issued.query.contains(QStringLiteral("A Link to the Past")));
        CHECK(t.issued.query.contains(QStringLiteral("SNES")));                 // the word without which it finds nothing
        CHECK(t.issued.query == QStringLiteral("The Legend of Zelda: A Link to the Past SNES"));
        CHECK(t.issued.wantTitle == QStringLiteral("The Legend of Zelda: A Link to the Past"));
        CHECK(!t.issued.wantTitle.contains(QStringLiteral("SNES")));            // judged by the title alone
        CHECK(t.issued.catalogType == QStringLiteral("game"));
        // The search found it: queued, and the crawl moves on exactly once.
        browse::remoteLeafSearchDone(QStringLiteral("http://host/zelda.sfc"), QStringLiteral("application/zip"), sinks);
        CHECK(t.emits == 1);
        CHECK(t.emittedUrl == QStringLiteral("http://host/zelda.sfc"));
        CHECK(t.finishes == 1);
        CHECK(t.searches == 1);     // one search, not a retry loop

        // (c) Both fail. Nothing is queued, and the crawl still ends cleanly on EXACTLY ONE dlNext() — the
        // toast has to resolve to "Nothing here could be downloaded." rather than spinning forever.
        reset();
        browse::remoteLeafResolved(QString(), QString(),
                                   QStringLiteral("game"), QStringLiteral("No Such Game"),
                                   QStringLiteral("SNES"), QStringLiteral("platform"), sinks);
        CHECK(t.searches == 1);
        CHECK(t.finishes == 0);
        browse::remoteLeafSearchDone(QString(), QString(), sinks);   // the search came up empty too
        CHECK(t.emits == 0);
        CHECK(t.finishes == 1);

        // (d) A non-game leaf whose resolve came back empty: unchanged. Only a game is found by title+console,
        // and a movie/episode has its own bridge (its /meta, for the IMDB id) that this must not divert.
        for (const QString& type : { QStringLiteral("movie"), QStringLiteral("episode"),
                                     QStringLiteral("book"), QStringLiteral("audiobook"),
                                     QStringLiteral("comic_issue"), QStringLiteral("track") })
        {
            reset();
            browse::remoteLeafResolved(QString(), QString(), type, QStringLiteral("Some Title"),
                                       QStringLiteral("SNES"), QStringLiteral("platform"), sinks);
            CHECK(t.searches == 0);
            CHECK(t.emits == 0);
            CHECK(t.finishes == 1);   // straight on, exactly as before
        }
        // ... and a non-game that DID resolve is still queued and still searched for nothing.
        reset();
        browse::remoteLeafResolved(QStringLiteral("http://host/x.cbz"), QStringLiteral("application/zip"),
                                   QStringLiteral("book"), QStringLiteral("Some Book"),
                                   QString(), QString(), sinks);
        CHECK(t.searches == 0);
        CHECK(t.emits == 1);
        CHECK(t.finishes == 1);

        // (e) No console. The search is still attempted with the title alone — it is the only chance left —
        // but it is not pretended to be equivalent: consoleKnown says so, and at least one live source answers
        // a console-less game search with zero results and a hint asking for one.
        reset();
        browse::remoteLeafResolved(QString(), QString(),
                                   QStringLiteral("game"), QStringLiteral("Chrono Trigger"),
                                   QStringLiteral("Recently played"), QStringLiteral("directory"), sinks);
        CHECK(t.searches == 1);
        CHECK(t.issued.query == QStringLiteral("Chrono Trigger"));   // no shelf name glued on
        CHECK(!t.issued.consoleKnown);
        CHECK(t.issued.wantTitle == QStringLiteral("Chrono Trigger"));
        browse::remoteLeafSearchDone(QString(), QString(), sinks);
        CHECK(t.finishes == 1);

        // (f) A titleless leaf is not searched for at all: a bare console name matches whatever ranks first,
        // and the title gate cannot refuse it because there is no title to judge by.
        reset();
        browse::remoteLeafResolved(QString(), QString(), QStringLiteral("game"), QStringLiteral("   "),
                                   QStringLiteral("SNES"), QStringLiteral("platform"), sinks);
        CHECK(t.searches == 0);
        CHECK(t.emits == 0);
        CHECK(t.finishes == 1);

        // (g) The plan on its own, since the guard in it is the thing that keeps working ids on the fast path.
        CHECK(!browse::remoteLeafFallbackPlan(QStringLiteral("http://host/a"), QStringLiteral("game"),
                                              QStringLiteral("A"), QStringLiteral("SNES"),
                                              QStringLiteral("platform")).search);
        CHECK(browse::remoteLeafFallbackPlan(QString(), QStringLiteral("game"),
                                             QStringLiteral("A"), QStringLiteral("SNES"),
                                             QStringLiteral("platform")).search);
    }


    // ---------------------------------- 8. BoundedFetch: the response decides how it should be fetched
    // A romhack patch is a few kilobytes or it is disc-scale, and nothing but the response can say which.
    // What is pinned here is the DECISION and its cost: a refusal that arrives after the whole body has been
    // read is not a refusal, it is the bug with a different return value — so every over-ceiling case
    // asserts the BYTE COUNT and not merely the verdict.
    {
        const qint64 kCeiling = 4096;          // small, so a fixture stays a fixture; the app's is 16 MiB

        Loopback lb;
        const QByteArray small(1000, 'a');
        const QByteArray big(60000, 'b');
        lb.stallPath = "/stall";
        lb.pieces = [&](const QByteArray& path) -> QList<QByteArray> {
            if (path == "/small") return { head200(small.size()), small };
            // Sliced, and that is not decoration. A 60 KB body written in ONE piece arrives in ONE readyRead,
            // so `read` would be 60000 however early the decision was made and the "it stopped" assertion
            // below could not fail. Six-kilobyte slices with the loop turning between them are what make the
            // difference between deciding at the head and deciding at the end observable at all.
            if (path == "/big")
            {
                QList<QByteArray> parts{ head200(big.size()) };
                for (int i = 0; i < 10; ++i) parts << big.mid(i * 6000, 6000);
                return parts;
            }
            if (path == "/small-undeclared") return { head200(-1), chunked(small), chunkedEnd() };
            if (path == "/big-undeclared")
            {
                QList<QByteArray> parts{ head200(-1) };
                for (int i = 0; i < 10; ++i) parts << chunked(QByteArray(6000, 'c'));
                parts << chunkedEnd();
                return parts;
            }
            if (path == "/missing") return { QByteArray("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n") };
            if (path == "/stall")   return { head200(1000000) };   // head only, and the socket stays open
            return { QByteArray("HTTP/1.1 500 Server Error\r\nContent-Length: 0\r\n\r\n") };
        };
        CHECK(lb.start());

        // (a) Declared, under the ceiling: an ordinary fetch, and the body is byte-exact.
        {
            const BoundedFetch::Result r = BoundedFetch::get(lb.url(QStringLiteral("/small")), 10000, kCeiling);
            CHECK(r.verdict == BoundedFetch::Result::Ok);
            CHECK(r.body == small);
            CHECK(r.declared == small.size());
            CHECK(r.status == 200);
        }

        // (b) Declared, over the ceiling: refused, and refused AT THE HEAD. The byte count is the assertion —
        // a verdict-only check passes just as happily on an implementation that read all 60 000 bytes first.
        {
            const BoundedFetch::Result r = BoundedFetch::get(lb.url(QStringLiteral("/big")), 10000, kCeiling);
            CHECK(r.verdict == BoundedFetch::Result::TooBig);
            CHECK(r.declared == big.size());          // the fact the caller can put in a sentence
            // It STOPPED; it did not merely disapprove afterwards. One 6 KB slice is what a decision made at
            // the head costs; the bound allows a couple to coalesce and still fails an implementation that
            // read the body out before looking at its length.
            CHECK(r.read < 20000);
            CHECK(r.body.isEmpty());                  // and it hands back nothing it refused
        }

        // (c) No Content-Length at all, under the ceiling: still an ordinary fetch. `declared` stays -1,
        // which is the difference between "the server said" and "we found out".
        {
            const BoundedFetch::Result r =
                BoundedFetch::get(lb.url(QStringLiteral("/small-undeclared")), 10000, kCeiling);
            CHECK(r.verdict == BoundedFetch::Result::Ok);
            CHECK(r.body == small);
            CHECK(r.declared == -1);
        }

        // (d) No Content-Length, over the ceiling: the running count catches it mid-stream. This is the case
        // a head-only implementation gets wrong — it has nothing to read, so it reads everything.
        {
            const BoundedFetch::Result r =
                BoundedFetch::get(lb.url(QStringLiteral("/big-undeclared")), 10000, kCeiling);
            CHECK(r.verdict == BoundedFetch::Result::TooBig);
            CHECK(r.declared == -1);
            CHECK(r.read > kCeiling);                 // it had to cross the line to know
            CHECK(r.read < 20000);                    // …and stopped there rather than finishing
            CHECK(r.body.isEmpty());
        }

        // (e) A refusal is a failure, and it says which one. `status` is what lets the caller tell "the server
        // answered and the file is gone" from "the server never answered" — two different things to do next.
        {
            const BoundedFetch::Result r = BoundedFetch::get(lb.url(QStringLiteral("/missing")), 10000, kCeiling);
            CHECK(r.verdict == BoundedFetch::Result::Failed);
            CHECK(r.status == 404);
            CHECK(r.body.isEmpty());
        }

        // (f) A head that arrives and a body that never does: the deadline ends it, and it ends NEAR the
        // deadline rather than hanging. The elapsed check is the half that matters — a call that returns the
        // right verdict after blocking forever has not passed.
        {
            QElapsedTimer t; t.start();
            const BoundedFetch::Result r = BoundedFetch::get(lb.url(QStringLiteral("/stall")), 1200, kCeiling);
            CHECK(r.verdict == BoundedFetch::Result::Failed);
            CHECK(t.elapsed() >= 1000);
            CHECK(t.elapsed() < 8000);
            CHECK(r.body.isEmpty());
            // The property the CALLER's two messages hang off: a head that said 200 and then stalled must not
            // arrive looking like a refusal, or a timeout would be reported as "the file is gone from the
            // server" and send someone to re-open a chooser that was never the problem.
            CHECK(r.status < 400);
        }

        // (g) An unreachable host is a failure with no status at all — nothing answered, so there is nothing
        // to report about what it said. The control for (e): without this, `status == 0` and `status == 404`
        // could both be produced by a stub that never sets it.
        {
            const BoundedFetch::Result r =
                BoundedFetch::get(QStringLiteral("http://127.0.0.1:1/nothing"), 3000, kCeiling);
            CHECK(r.verdict == BoundedFetch::Result::Failed);
            CHECK(r.status == 0);
        }
    }

    QDir(root).removeRecursively();

    if (g_fails == 0) std::printf("ROMHACK-OK\n");
    return g_fails == 0 ? 0 : 1;
}
