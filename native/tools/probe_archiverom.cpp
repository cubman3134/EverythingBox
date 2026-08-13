// Headless test for ArchiveRom::extractToTemp's .zip path (the large-zip OOM fix). The bug: extractToTemp
// used to buffer the WHOLE archive into memory (QFile::readAll + mz_zip_reader_init_mem) and extract the
// chosen entry ENTIRELY into the heap (mz_zip_reader_extract_to_heap) before writing it — ~2.2GB of
// allocations for an 829MB TorrentZip of a 1.4GB GameCube dump, which failed and was mis-reported as
// "not a valid zip archive". The fix mirrors extractAll: mz_zip_reader_init_file + extract_to_file, both
// streamed from/to disk. This probe pins the SELECTION and the STREAMED-EXTRACT correctness that must
// survive that rewrite.
//
// The fixture is a STORE-method .zip HAND-BUILT here (miniz's writer APIs are compiled out, so we emit the
// ZIP local-headers + central-directory + EOCD ourselves). Because this probe authors every byte, the
// expected content is an independent oracle — never read back out of the function under test. Entries:
//   * readme.txt   — a junk sidecar (kJunkExts) that must NOT win when no ext filter is given.
//   * bigdata.bin  — a non-junk file LARGER than the ROM, so the size rule (empty-exts) picks it, and the
//                    ext rule ({".rvz"}) must beat it despite being smaller.
//   * Pokémon Snap.rvz — the target ROM, with a NON-ASCII (UTF-8) name so the unicode extract-to-file path
//                    (miniz MZ_FOPEN = _wfopen_s on Windows) is exercised end to end.
//
// Set EB_ZIPROM_REAL=<path-to-zip> to instead run extractToTemp({".rvz"}) on a real archive and print the
// result — used for the local large-file confirmation, not committed to any gate.
//
// Prints ARCHIVEROM-OK on success; ARCHIVEROM-FAIL (nonzero exit) on any miss.
#include <QCoreApplication>
#include <QByteArray>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <cstdio>

#include "../src/core/ArchiveRom.h"

extern "C" {
#include "miniz.h" // mz_crc32 for the ZIP CRC fields (reader-side util; writer APIs are compiled out)
}

static int fails = 0;
#define CHECK(cond, name) do { if (cond) std::printf("PASS %s\n", name); \
    else { std::printf("FAIL %s\n", name); ++fails; } } while (0)

// ---- little-endian ZIP field emitters -----------------------------------------------------------------
static void put16(QByteArray& b, quint16 v) { b.append(char(v & 0xFF)); b.append(char((v >> 8) & 0xFF)); }
static void put32(QByteArray& b, quint32 v)
{
    b.append(char(v & 0xFF)); b.append(char((v >> 8) & 0xFF));
    b.append(char((v >> 16) & 0xFF)); b.append(char((v >> 24) & 0xFF));
}

struct Entry { QByteArray nameUtf8; QByteArray data; };

// Build a valid STORE-method (uncompressed) .zip from the given entries. Flag bit 11 (0x0800) marks the
// filename as UTF-8 so a non-ASCII name round-trips through miniz's QString::fromUtf8 read.
static QByteArray buildStoreZip(const QList<Entry>& entries)
{
    QByteArray out;
    QList<quint32> offsets;
    QList<quint32> crcs;
    for (const Entry& e : entries)
    {
        offsets.append(quint32(out.size()));
        const quint32 crc = quint32(mz_crc32(MZ_CRC32_INIT,
            reinterpret_cast<const unsigned char*>(e.data.constData()), size_t(e.data.size())));
        crcs.append(crc);
        put32(out, 0x04034b50);              // local file header signature
        put16(out, 20);                      // version needed
        put16(out, 0x0800);                  // flags: UTF-8 filename
        put16(out, 0);                       // method: store
        put16(out, 0); put16(out, 0);        // mod time / date
        put32(out, crc);
        put32(out, quint32(e.data.size()));  // compressed size (== uncompressed for store)
        put32(out, quint32(e.data.size()));  // uncompressed size
        put16(out, quint16(e.nameUtf8.size()));
        put16(out, 0);                       // extra length
        out.append(e.nameUtf8);
        out.append(e.data);
    }
    const quint32 cdStart = quint32(out.size());
    for (int i = 0; i < entries.size(); ++i)
    {
        const Entry& e = entries[i];
        put32(out, 0x02014b50);              // central directory header signature
        put16(out, 20);                      // version made by
        put16(out, 20);                      // version needed
        put16(out, 0x0800);                  // flags: UTF-8 filename
        put16(out, 0);                       // method: store
        put16(out, 0); put16(out, 0);        // mod time / date
        put32(out, crcs[i]);
        put32(out, quint32(e.data.size()));
        put32(out, quint32(e.data.size()));
        put16(out, quint16(e.nameUtf8.size()));
        put16(out, 0);                       // extra length
        put16(out, 0);                       // comment length
        put16(out, 0);                       // disk number start
        put16(out, 0);                       // internal attributes
        put32(out, 0);                       // external attributes
        put32(out, offsets[i]);              // local header offset
        out.append(e.nameUtf8);
    }
    const quint32 cdSize = quint32(out.size()) - cdStart;
    put32(out, 0x06054b50);                  // end of central directory
    put16(out, 0); put16(out, 0);            // disk numbers
    put16(out, quint16(entries.size()));     // entries on this disk
    put16(out, quint16(entries.size()));     // total entries
    put32(out, cdSize);
    put32(out, cdStart);
    put16(out, 0);                           // comment length
    return out;
}

static QByteArray readFile(const QString& p)
{
    QFile f(p);
    if (!f.open(QIODevice::ReadOnly)) return QByteArray();
    return f.readAll();
}

// Mirror ArchiveRom's private outDirFor() so we can clean up precisely.
static QString outDirFor(const QString& archivePath)
{
    const QByteArray h = QCryptographicHash::hash(archivePath.toUtf8(), QCryptographicHash::Sha1).toHex().left(16);
    return QDir::tempPath() + QStringLiteral("/everythingbox-roms/") + QString::fromLatin1(h);
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ---- optional real-file confirmation mode (not part of the gate) ---------------------------------
    const QByteArray realPath = qgetenv("EB_ZIPROM_REAL");
    if (!realPath.isEmpty())
    {
        QString err;
        const QString got = ArchiveRom::extractToTemp(QString::fromLocal8Bit(realPath),
                                                      { QStringLiteral(".rvz") }, &err);
        if (got.isEmpty())
            std::printf("REAL-FAIL: %s\n", err.toUtf8().constData());
        else
            std::printf("REAL-OK: extracted -> %s (%lld bytes)\n",
                        got.toUtf8().constData(), qint64(QFileInfo(got).size()));
        return got.isEmpty() ? 1 : 0;
    }

    // ---- author the fixture (independent oracle: every byte chosen here) -----------------------------
    Entry readme;  readme.nameUtf8  = QByteArray("readme.txt");
                   readme.data      = QByteArray("this is a junk readme, must never be picked\n");
    Entry big;     big.nameUtf8     = QByteArray("bigdata.bin");
                   big.data         = QByteArray(300, '\xBB');           // largest non-junk file
    Entry rom;     rom.nameUtf8     = QByteArray("Pok\xC3\xA9mon Snap.rvz"); // UTF-8 'é', target ext
                   rom.data         = QByteArray("RVZ\x01""GAMECUBE-ROM-PAYLOAD-\xDE\xAD\xBE\xEF", 26);
    const QByteArray zipBytes = buildStoreZip({ readme, big, rom });

    QString base = QDir::tempPath() + QStringLiteral("/eb-ziprom-fixture");
    QDir().mkpath(base);
    const QString zipPath = base + QStringLiteral("/sample.zip");
    { QFile f(zipPath); if (f.open(QIODevice::WriteOnly)) f.write(zipBytes); }

    // Sanity: our hand-built zip must be readable as an archive at all (guards a fixture-shape bug).
    QString err;
    // ---- 1. ext filter selects the ROM even though it is SMALLER than bigdata.bin --------------------
    const QString picked = ArchiveRom::extractToTemp(zipPath, { QStringLiteral(".rvz") }, &err);
    CHECK(!picked.isEmpty(), "extractToTemp({.rvz}) returns a path");
    CHECK(picked.endsWith(QStringLiteral("Pok\xC3\xA9mon Snap.rvz")),
          "picked the .rvz target (ext beats larger bigdata.bin), unicode name preserved");
    CHECK(readFile(picked) == rom.data,
          "extracted .rvz content is byte-identical to the fixture (correct index, streamed write)");

    // ---- 2. no ext filter: largest NON-JUNK file wins; the junk readme is skipped --------------------
    const QString largest = ArchiveRom::extractToTemp(zipPath, {}, &err);
    CHECK(largest.endsWith(QStringLiteral("bigdata.bin")),
          "empty-exts picks the largest non-junk file (bigdata.bin), not the junk readme");
    CHECK(readFile(largest) == big.data,
          "extracted bigdata.bin content matches the fixture");

    // ---- 3. cache reuse: a second open returns the SAME file WITHOUT re-extracting -------------------
    // Overwrite the already-extracted ROM with a same-size sentinel. If the cache early-return fires, the
    // next open returns that path untouched (content == sentinel). If it re-extracts, content reverts to
    // the fixture bytes — which is exactly what a broken cache-reuse mutation does, so this kills it.
    QByteArray sentinel = rom.data; sentinel.fill('\x5A'); // same length, different bytes
    { QFile f(picked); if (f.open(QIODevice::WriteOnly)) f.write(sentinel); }
    const QString again = ArchiveRom::extractToTemp(zipPath, { QStringLiteral(".rvz") }, &err);
    CHECK(again == picked, "cache reuse returns the same path on re-open");
    CHECK(readFile(again) == sentinel,
          "cache reuse did NOT re-extract (sentinel content survived)");

    // ---- cleanup: remove the fixture and BOTH extraction dirs we created -----------------------------
    QDir(outDirFor(zipPath)).removeRecursively();
    QDir(base).removeRecursively();

    if (fails == 0) std::printf("ARCHIVEROM-OK\n");
    else            std::printf("ARCHIVEROM had %d failure(s)\n", fails);
    return fails == 0 ? 0 : 1;
}
