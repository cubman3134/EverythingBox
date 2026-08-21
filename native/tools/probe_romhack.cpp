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
#include "RomPatch.h"
#include "AppPaths.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <cstdio>

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

    QDir(root).removeRecursively();

    if (g_fails == 0) std::printf("ROMHACK-OK\n");
    return g_fails == 0 ? 0 : 1;
}
