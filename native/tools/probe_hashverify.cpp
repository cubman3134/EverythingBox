// Headless check of ROM dump verification against Logiqx DAT files (src/core/HashVerify, issue #97). The
// feature's correctness core is that it hashes the ROM *payload*, not the raw file — so the load-bearing case
// here is the iNES one: a headered .nes whose PAYLOAD hash is in the DAT (Verified) while the WHOLE-file hash
// is something else entirely. If the 16-byte-header skip regressed, a good dump would read as Bad, and this
// probe would catch it.
//
// EVERY expected CRC/MD5/SHA1 in the fixtures below was computed with an INDEPENDENT oracle — Python's
// zlib.crc32 / hashlib.md5 / hashlib.sha1 — and hard-coded here, NOT by running HashVerify's own hasher. A
// fixture derived from the code under test would be a fixed point that passes no matter what the hasher does
// (CONTRIBUTING.md). So a bug in HashVerify::hashBytes cannot forge a matching digest and hide behind it: the
// oracle and the code would disagree and the CHECK would fire.
//
//   Oracle values (payload P = bytes 0x00..0x1F; Q = P with the last byte -> 0xFF; R = eight 0xAA):
//     P   crc=91267e8a md5=b4ffcb23737cec315a4a4d1aa2a620ce sha1=ae5bd8efea5322c4d9986d06680a781392f9a642
//     Q   crc=312c9cf2 md5=f9c4044ff1491479b30787cde7eb1db4 sha1=9ca36248b123382f98a0d89218bcf8cd7050659b
//     R   crc=abb622f0 md5=cffca1eda37be3e57211c384059eb3c1 sha1=ab86b17c3d37e5fa094723648664d1fe2242ff4d
//     whole 48-byte headered file (16B iNES header + P): sha1=3781e2a6dbbf89f7049f7280b113a29d29c4b2d9
//
// Covered: DAT parse + indexing; the iNES payload skip (payload hash matches while whole-file hash does not);
// Verified / Bad / Unknown classification (incl. the Bad-vs-Unknown boundary — name-in-DAT is what makes a
// hash miss "Bad"); the CHD v5-header SHA1 read (no decompression); hashRomFile over real files on disk; and
// the path+mtime stamp cache (a changed file invalidates its stamp). Prints HASHVERIFY-OK / -FAIL.
//
// Isolation: AppPaths::dataDir() is this process's scratch dir (EB_ISOLATED_DATA_DIR) — every file we write
// lives under it and nothing touches a real ROM, DAT or ini.
#include "HashVerify.h"
#include "AppPaths.h"

#include <QCoreApplication>
#include <QByteArray>
#include <QDir>
#include <QFile>
#include <cstdio>

using HashVerify::Status;

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "HASHVERIFY-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

static QByteArray seq(int lo, int hi) { QByteArray b; for (int i = lo; i < hi; ++i) b.append(char(i)); return b; }

static bool writeFile(const QString& path, const QByteArray& data)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    const bool ok = f.write(data) == data.size();
    f.close();
    return ok;
}

// The hand-built fixture DAT. "Test Game (USA)" carries P's oracle hashes; "Disc Game (USA)" carries a CHD
// SHA1 we control. No hash here was produced by HashVerify — they are the Python oracle's output.
static const char* kDatXml =
    "<?xml version=\"1.0\"?>\n"
    "<datafile>\n"
    "  <header><name>Fixture</name></header>\n"
    "  <game name=\"Test Game (USA)\">\n"
    "    <rom name=\"Test Game (USA).nes\" size=\"32\" crc=\"91267e8a\""
    " md5=\"b4ffcb23737cec315a4a4d1aa2a620ce\" sha1=\"ae5bd8efea5322c4d9986d06680a781392f9a642\"/>\n"
    "  </game>\n"
    "  <game name=\"Disc Game (USA)\">\n"
    "    <rom name=\"Disc Game (USA).chd\" sha1=\"1122334455667788990011223344556677889900\"/>\n"
    "  </game>\n"
    "</datafile>\n";

// P's oracle digests, as literals (NOT from HashVerify).
static const QString P_CRC  = QStringLiteral("91267e8a");
static const QString P_MD5  = QStringLiteral("b4ffcb23737cec315a4a4d1aa2a620ce");
static const QString P_SHA1 = QStringLiteral("ae5bd8efea5322c4d9986d06680a781392f9a642");
static const QString WHOLE_SHA1 = QStringLiteral("3781e2a6dbbf89f7049f7280b113a29d29c4b2d9");
static const QString CHD_SHA1   = QStringLiteral("1122334455667788990011223344556677889900");

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    const QByteArray P = seq(0, 32);
    QByteArray Q = seq(0, 31); Q.append(char(0xFF));
    const QByteArray R = QByteArray(8, char(0xAA));
    const QByteArray iHeader = QByteArray("NES\x1A", 4) + QByteArray("\x02\x01", 2) + QByteArray(10, '\0'); // 16B
    const QByteArray headered = iHeader + P;
    CHECK(iHeader.size() == 16);

    const HashVerify::DatDb db = HashVerify::parseDat(QByteArray(kDatXml));

    // ---- 1. DAT parse + indexing --------------------------------------------------------------------------
    CHECK(db.entries.size() == 2);
    CHECK(db.byCrc.contains(P_CRC));
    CHECK(db.byMd5.contains(P_MD5));
    CHECK(db.bySha1.contains(P_SHA1));
    CHECK(db.bySha1.contains(CHD_SHA1));
    CHECK(db.hasName(HashVerify::normalizeName(QStringLiteral("Test Game (USA)"))));
    CHECK(db.hasName(HashVerify::normalizeName(QStringLiteral("Test Game (USA).nes")))); // ext-stripped name eq
    CHECK(!db.hasName(HashVerify::normalizeName(QStringLiteral("Totally Unlisted Game"))));

    // ---- 2. The iNES payload skip: payload hash == oracle P; whole-file hash is DIFFERENT ------------------
    {
        const HashVerify::Hashes payH = HashVerify::hashPayload(headered, QStringLiteral("nes"));
        CHECK(payH.crc  == P_CRC);   // matches the oracle of the PAYLOAD, i.e. the header was skipped
        CHECK(payH.md5  == P_MD5);
        CHECK(payH.sha1 == P_SHA1);
        // And hashing the WHOLE file (no skip) gives the other oracle value — so the skip is load-bearing, not
        // cosmetic: without it, classification below would miss the DAT entry.
        const HashVerify::Hashes wholeH = HashVerify::hashBytes(headered);
        CHECK(wholeH.sha1 == WHOLE_SHA1);
        CHECK(wholeH.sha1 != P_SHA1);
        // A non-iNES buffer is hashed whole (no accidental 16-byte lop-off).
        CHECK(HashVerify::hashPayload(P, QStringLiteral("nes")).sha1 == P_SHA1);
    }

    // ---- 3. Classification: Verified / Bad / Unknown ------------------------------------------------------
    {
        const HashVerify::Hashes payH = HashVerify::hashPayload(headered, QStringLiteral("nes"));
        // Verified: the payload hash is in the DAT.
        CHECK(HashVerify::classify(payH, db, QStringLiteral("Test Game (USA)")) == Status::Verified);
        // Bad: a DAT covers a game of THIS name, but the bytes (Q) don't match any hash.
        CHECK(HashVerify::classify(HashVerify::hashBytes(Q), db, QStringLiteral("Test Game (USA)")) == Status::Bad);
        // Unknown vs Bad boundary: same wrong bytes, but a name NO DAT covers -> Unknown, not Bad. (This is
        // what proves the Bad verdict is driven by the name index, not merely by "hash didn't match".)
        CHECK(HashVerify::classify(HashVerify::hashBytes(Q), db, QStringLiteral("Zzz Unlisted")) == Status::Unknown);
        // Unknown: unlisted bytes AND unlisted name.
        CHECK(HashVerify::classify(HashVerify::hashBytes(R), db, QStringLiteral("Zzz Unlisted")) == Status::Unknown);
    }

    // ---- 4. CHD v5 header SHA1 (no decompression) ---------------------------------------------------------
    {
        QByteArray chd(124, '\0');
        chd.replace(0, 8, "MComprHD");
        chd[8] = 0; chd[9] = 0; chd[10] = 0; chd[11] = 124;   // header length (BE)
        chd[12] = 0; chd[13] = 0; chd[14] = 0; chd[15] = 5;   // version (BE) = 5
        chd.replace(84, 20, QByteArray::fromHex(CHD_SHA1.toLatin1())); // the field a MAME CHD DAT lists
        CHECK(HashVerify::chdSha1FromHeader(chd) == CHD_SHA1);
        // A CHD stamped Verified against its DAT entry.
        HashVerify::Hashes ch; ch.sha1 = CHD_SHA1;
        CHECK(HashVerify::classify(ch, db, QStringLiteral("Disc Game (USA)")) == Status::Verified);
        // Not a CHD / wrong version / truncated -> empty (never a bogus SHA1).
        CHECK(HashVerify::chdSha1FromHeader(QByteArray("NOTACHD!") + QByteArray(120, '\0')).isEmpty());
        QByteArray v4 = chd; v4[15] = 4;
        CHECK(HashVerify::chdSha1FromHeader(v4).isEmpty());
        CHECK(HashVerify::chdSha1FromHeader(chd.left(80)).isEmpty());
    }

    // ---- 5. hashRomFile over real files on disk (the iNES skip + the CHD header, via a path) ---------------
    const QString root = AppPaths::dataDir() + QStringLiteral("/hvfix");
    QDir().mkpath(root);
    {
        const QString nesPath = root + QStringLiteral("/Test Game (USA).nes");
        CHECK(writeFile(nesPath, headered));
        QString err;
        const HashVerify::Hashes fh = HashVerify::hashRomFile(nesPath, QStringLiteral("nes"), &err);
        CHECK(err.isEmpty());
        CHECK(fh.sha1 == P_SHA1);   // the file's PAYLOAD, header skipped

        QByteArray chd(124, '\0');
        chd.replace(0, 8, "MComprHD"); chd[11] = 124; chd[15] = 5;
        chd.replace(84, 20, QByteArray::fromHex(CHD_SHA1.toLatin1()));
        const QString chdPath = root + QStringLiteral("/Disc Game (USA).chd");
        CHECK(writeFile(chdPath, chd));
        const HashVerify::Hashes chdH = HashVerify::hashRomFile(chdPath, QStringLiteral("chd"), nullptr);
        CHECK(chdH.sha1 == CHD_SHA1);
    }

    // ---- 6. The per-ROM stamp cache: verify, read back, and invalidate on change --------------------------
    {
        const QString nesPath = root + QStringLiteral("/Cached Game.nes");
        // Name the file to match the DAT entry so a payload match is expected; write P WITH an iNES header.
        // (completeBaseName "Cached Game" is not in the DAT, but the payload hash IS, so it's Verified anyway.)
        CHECK(writeFile(nesPath, headered));
        const HashVerify::Stamp s1 = HashVerify::verifyAndCache(nesPath, QStringLiteral("nes"), db);
        CHECK(s1.valid);
        CHECK(s1.status == Status::Verified);
        CHECK(s1.sha1 == P_SHA1);
        CHECK(s1.datGame == QStringLiteral("Test Game (USA)"));   // canonical name recorded (identity use)

        // A cheap read returns the same stamp without re-hashing.
        const HashVerify::Stamp s2 = HashVerify::cachedStamp(nesPath);
        CHECK(s2.valid);
        CHECK(s2.status == Status::Verified);

        // Change the file's bytes: the path+mtime+size gate must drop the stamp (a swapped dump is re-verified,
        // never served the old verdict). Bump the size so the gate fires even at 1-second mtime resolution.
        CHECK(writeFile(nesPath, headered + QByteArray("EXTRA")));
        const HashVerify::Stamp s3 = HashVerify::cachedStamp(nesPath);
        CHECK(!s3.valid);

        // A Bad file lands a Bad stamp (name matches the DAT, bytes don't).
        const QString badPath = root + QStringLiteral("/Test Game (USA).bin");
        CHECK(writeFile(badPath, Q));
        const HashVerify::Stamp sb = HashVerify::verifyAndCache(badPath, QStringLiteral("bin"), db);
        CHECK(sb.valid);
        CHECK(sb.status == Status::Bad);
    }

    QDir(root).removeRecursively();
    HashVerify::clearCache();

    if (failures == 0) std::printf("HASHVERIFY-OK\n");
    else std::fprintf(stderr, "HASHVERIFY: %d check(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}
