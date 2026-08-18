// Headless test for ArchiveRom::extractToTemp. Covers two shipped fixes:
//   (A) the large-zip OOM fix (streamed selection + extract, below), and
//   (B) MAGIC-BYTE routing: the ROM cache names files by the content server's choice (".zip") regardless
//       of the real container, so a 7-Zip archive can arrive named ".zip" and the extension router sent it
//       to miniz, which rejected it as "not a valid zip archive". extractToTemp now sniffs the header and
//       routes by content. Cases 4-6 pin BOTH cross directions (7z-named-.zip, zip-named-.7z) plus a normal
//       .7z, using a REAL 7-Zip archive (kSevenZipBytes) written under the wrong name. Mutation target: the
//       sniff/routing branch — force always-Zip and case 4 fails (miniz cannot read a 7z).
//   (C) ZIP-SLIP guard (case 10): extractAll now unpacks user/content-server disc archives, so a crafted
//       member name ("../escape.txt", an absolute or drive/UNC path) must not write outside destDir. The
//       shared ArchiveSafePath::join guard rejects it; drop the guard and the parent-dir escape file lands.
//
// (original OOM-fix note) The bug: extractToTemp
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
#include "../src/core/ArchiveSafePath.h"

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

// ---- a REAL 7-Zip archive, authored out-of-band by the 7-Zip tool (magic 37 7A BC AF 27 1C) --------
// It holds one stored member "inner.rvz" whose bytes are the literal below. These are the actual bytes
// of a .7z file — the probe writes them under a ".zip" NAME to prove routing follows content, not the
// extension (the confirmed bug: a 7z-content ROM cached as ".zip"). The expected inner content is an
// independent oracle (the string that was fed to the 7-Zip tool), never read back out of the function.
static const unsigned char kSevenZipBytes[] = {
    0x37,0x7a,0xbc,0xaf,0x27,0x1c,0x00,0x04,0x05,0x48,0xe9,0xd2,0x28,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x5a,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xd4,0x7f,0xa5,0xec,
    0x01,0x00,0x23,0x53,0x45,0x56,0x45,0x4e,0x5a,0x49,0x50,0x2d,0x49,0x4e,0x4e,0x45,
    0x52,0x2d,0x50,0x41,0x59,0x4c,0x4f,0x41,0x44,0x2d,0xde,0xad,0xbe,0xef,0x2d,0x67,
    0x61,0x6d,0x65,0x63,0x75,0x62,0x65,0x00,0x01,0x04,0x06,0x00,0x01,0x09,0x28,0x00,
    0x07,0x0b,0x01,0x00,0x01,0x21,0x21,0x01,0x00,0x0c,0x24,0x00,0x08,0x0a,0x01,0xe7,
    0xe7,0x40,0x48,0x00,0x00,0x05,0x01,0x19,0x0c,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x11,0x15,0x00,0x69,0x00,0x6e,0x00,0x6e,0x00,0x65,0x00,
    0x72,0x00,0x2e,0x00,0x72,0x00,0x76,0x00,0x7a,0x00,0x00,0x00,0x14,0x0a,0x01,0x00,
    0x5e,0x57,0x7d,0xab,0x50,0x2b,0xdd,0x01,0x15,0x06,0x01,0x00,0x20,0x00,0x00,0x00,
    0x00,0x00
};
// The exact bytes fed to the 7-Zip tool as inner.rvz — the oracle for the extracted content.
static const QByteArray kSevenZipInner =
    QByteArray("SEVENZIP-INNER-PAYLOAD-\xDE\xAD\xBE\xEF-gamecube", 36);

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
        const QStringList gcExts = { QStringLiteral(".rvz"), QStringLiteral(".iso"), QStringLiteral(".gcm"),
                                     QStringLiteral(".gcz"), QStringLiteral(".ciso"), QStringLiteral(".wia"),
                                     QStringLiteral(".wbfs"), QStringLiteral(".wad") };
        const QString got = ArchiveRom::extractToTemp(QString::fromLocal8Bit(realPath), gcExts, &err);
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

    // ---- 4. ROUTE BY CONTENT: a real 7-Zip archive NAMED ".zip" must still extract (the confirmed bug).
    // Extension routing sent this down the miniz zip branch, which rejected it as "not a valid zip
    // archive". The magic-byte sniff must send it to the SevenZip decoder instead. Mutation-kill: force
    // the classifier to always-Zip and this case fails (miniz cannot read a 7z), so the assertion fires.
    const QString sevenAsZip = base + QStringLiteral("/actually7z.zip");
    { QFile f(sevenAsZip);
      if (f.open(QIODevice::WriteOnly))
          f.write(reinterpret_cast<const char*>(kSevenZipBytes), qint64(sizeof(kSevenZipBytes))); }
    const QString sevenPicked = ArchiveRom::extractToTemp(sevenAsZip, { QStringLiteral(".rvz") }, &err);
    CHECK(!sevenPicked.isEmpty(),
          "7z-content named .zip routes to SevenZip and extracts (the reported bug)");
    CHECK(sevenPicked.endsWith(QStringLiteral("inner.rvz")),
          "extracted the 7z's inner member (name preserved) despite the .zip extension");
    CHECK(readFile(sevenPicked) == kSevenZipInner,
          "7z-named-.zip inner content is byte-identical to the oracle (routed by content, not name)");

    // ---- 5. the mirror case: a real ZIP NAMED ".7z" must route to the miniz zip path, not the 7z SDK.
    // Same fix, opposite direction — proves the sniff drives routing both ways.
    Entry zi; zi.nameUtf8 = QByteArray("payload.gcm");
              zi.data     = QByteArray("ZIP-CONTENT-UNDER-A-7Z-NAME", 27);
    const QByteArray zipUnder7z = buildStoreZip({ zi });
    const QString zipAs7z = base + QStringLiteral("/actuallyzip.7z");
    { QFile f(zipAs7z); if (f.open(QIODevice::WriteOnly)) f.write(zipUnder7z); }
    const QString zipPicked = ArchiveRom::extractToTemp(zipAs7z, { QStringLiteral(".gcm") }, &err);
    CHECK(!zipPicked.isEmpty(),
          "zip-content named .7z routes to the miniz zip path and extracts");
    CHECK(readFile(zipPicked) == zi.data,
          "zip-named-.7z inner content is byte-identical to the oracle (routed by content, not name)");

    // ---- 6. a NORMAL .7z (name matches content) still works — no regression on the 7z branch --------
    const QString sevenNormal = base + QStringLiteral("/normal.7z");
    { QFile f(sevenNormal);
      if (f.open(QIODevice::WriteOnly))
          f.write(reinterpret_cast<const char*>(kSevenZipBytes), qint64(sizeof(kSevenZipBytes))); }
    const QString sevenNormalPicked = ArchiveRom::extractToTemp(sevenNormal, { QStringLiteral(".rvz") }, &err);
    CHECK(readFile(sevenNormalPicked) == kSevenZipInner,
          "a normal .7z (name == content) still extracts correctly");

    // ---- 7. MULTI-FILE DISC: a .cue + its .bin in one archive must BOTH be extracted, and the sheet is
    // handed back. Pre-fix, extractToTemp pulled a single member, so the emulator opened the .cue and then
    // couldn't find its .bin ("no such file or directory"). A sheet ext in wantedExts now forces a whole-archive
    // extraction. Mutation-kill: revert to single-member extraction and the sibling .bin is absent, failing below.
    Entry cue; cue.nameUtf8 = QByteArray("Game (USA).cue");
               cue.data     = QByteArray("FILE \"Game (USA).bin\" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n");
    Entry bin; bin.nameUtf8 = QByteArray("Game (USA).bin");
               bin.data     = QByteArray(400, '\xCC');           // the data track — larger than the sheet
    const QByteArray discZipBytes = buildStoreZip({ cue, bin });
    const QString discZip = base + QStringLiteral("/disc.zip");
    { QFile f(discZip); if (f.open(QIODevice::WriteOnly)) f.write(discZipBytes); }
    const QString discPicked = ArchiveRom::extractToTemp(
        discZip, { QStringLiteral(".cue"), QStringLiteral(".bin"), QStringLiteral(".chd") }, &err);
    CHECK(discPicked.endsWith(QStringLiteral("Game (USA).cue")),
          "disc archive returns the .cue sheet, not the larger .bin");
    const QString sibBin = QFileInfo(discPicked).absolutePath() + QStringLiteral("/Game (USA).bin");
    CHECK(QFileInfo::exists(sibBin),
          "the .cue's sibling .bin was ALSO extracted (the reported no-such-file bug)");
    CHECK(readFile(sibBin) == bin.data,
          "extracted sibling .bin content is byte-identical to the fixture");

    // ---- 8. DISC via a 7z-content archive NAMED .zip: the disc branch must ALSO route by content, not name.
    // wantedExts carries a sheet ext (.cue) so the whole-archive path runs; its extractAll must sniff the 7z
    // magic (miniz would reject it). Mutation-kill: route extractAll by extension and this returns empty.
    const QString disc7zAsZip = base + QStringLiteral("/disc_actually7z.zip");
    { QFile f(disc7zAsZip);
      if (f.open(QIODevice::WriteOnly))
          f.write(reinterpret_cast<const char*>(kSevenZipBytes), qint64(sizeof(kSevenZipBytes))); }
    const QString disc7zPicked = ArchiveRom::extractToTemp(
        disc7zAsZip, { QStringLiteral(".cue"), QStringLiteral(".rvz") }, &err);
    CHECK(!disc7zPicked.isEmpty() && readFile(disc7zPicked) == kSevenZipInner,
          "disc branch routes a 7z-content archive named .zip by content (extractAll sniffs), not by name");

    // ---- 9. SHEET-LESS disc archive: a bare .iso whose ext is NOT in the system's list must still launch via
    // the largest-non-junk fallback (pre-fix behavior). wantedExts has a sheet ext (so the disc branch runs)
    // plus .chd, but NOT .iso. Mutation-kill: drop the largest-any fallback and this errors "no disc image".
    Entry iso;  iso.nameUtf8  = QByteArray("Bare Disc (USA).iso");
                iso.data      = QByteArray(500, '\xEE');
    Entry note; note.nameUtf8 = QByteArray("readme.txt");
                note.data     = QByteArray("junk, must never win\n");
    const QByteArray sheetlessBytes = buildStoreZip({ iso, note });
    const QString sheetlessZip = base + QStringLiteral("/sheetless.zip");
    { QFile f(sheetlessZip); if (f.open(QIODevice::WriteOnly)) f.write(sheetlessBytes); }
    const QString isoPicked = ArchiveRom::extractToTemp(
        sheetlessZip, { QStringLiteral(".cue"), QStringLiteral(".chd") }, &err);
    CHECK(isoPicked.endsWith(QStringLiteral("Bare Disc (USA).iso")),
          "sheet-less disc archive falls back to the largest non-junk member (bare .iso), not an error");

    // ---- 10. ZIP-SLIP: a member named "../escape.txt" must NOT be written outside destDir. extractAll now
    // routes user/content-server disc archives (the multi-file disc-image fix), so a crafted member name can
    // no longer be trusted. The guard (ArchiveSafePath::join) rejects the archive; the parent-dir escape file
    // must be absent afterwards. Mutation-kill: drop the guard and miniz writes destDir/../escape.txt, so the
    // "not written outside" assertion below fires. The escape payload is authored here — an independent oracle.
    Entry evil; evil.nameUtf8 = QByteArray("../escape.txt");
                evil.data     = QByteArray("ZIP-SLIP-SHOULD-NEVER-LAND-HERE\n");
    const QByteArray slipZip = buildStoreZip({ evil });
    const QString slipPath = base + QStringLiteral("/slip.zip");
    { QFile f(slipPath); if (f.open(QIODevice::WriteOnly)) f.write(slipZip); }
    const QString slipDest = base + QStringLiteral("/slip-dest"); // "../escape.txt" from here == base/escape.txt
    QDir().mkpath(slipDest);
    const QString escapeTarget = base + QStringLiteral("/escape.txt"); // the parent-dir path the member aims at
    QFile::remove(escapeTarget); // ensure a clean slate (isolation gives a fresh temp, but be explicit)
    QString slipErr;
    const bool slipOk = ArchiveRom::extractAll(slipPath, slipDest, &slipErr);
    CHECK(!slipOk, "extractAll rejects an archive with a path-traversal member (returns false)");
    CHECK(!QFileInfo::exists(escapeTarget),
          "zip-slip member '../escape.txt' was NOT written outside destDir (the parent-dir file is absent)");
    // Direct coverage of the shared guard's branches — the same join() both extractors use. Each rejection
    // is a distinct escape shape; the last is a legitimate nested path that MUST resolve inside destDir.
    CHECK(ArchiveSafePath::join(slipDest, QStringLiteral("../escape.txt")).isEmpty(),
          "join rejects a parent-traversal member");
    CHECK(ArchiveSafePath::join(slipDest, QStringLiteral("a/b/../../../escape.txt")).isEmpty(),
          "join rejects deep traversal that climbs past destDir");
    CHECK(ArchiveSafePath::join(slipDest, QStringLiteral("..\\escape.txt")).isEmpty(),
          "join rejects a backslash-separator traversal (Windows-made archive)");
    CHECK(ArchiveSafePath::join(slipDest, QStringLiteral("/etc/passwd")).isEmpty(),
          "join rejects a POSIX-absolute member");
    CHECK(ArchiveSafePath::join(slipDest, QStringLiteral("C:\\Windows\\system32\\evil.dll")).isEmpty(),
          "join rejects a drive-letter absolute member");
    const QString safeJoined = ArchiveSafePath::join(slipDest, QStringLiteral("sub/dir/rom.bin"));
    CHECK(!safeJoined.isEmpty() && safeJoined.endsWith(QStringLiteral("slip-dest/sub/dir/rom.bin")),
          "join accepts a legitimate nested member and keeps it under destDir");

    // ---- cleanup: remove the fixture and ALL extraction dirs we created ------------------------------
    QDir(outDirFor(disc7zAsZip)).removeRecursively();
    QDir(outDirFor(sheetlessZip)).removeRecursively();
    QDir(outDirFor(discZip)).removeRecursively();
    QDir(outDirFor(zipPath)).removeRecursively();
    QDir(outDirFor(sevenAsZip)).removeRecursively();
    QDir(outDirFor(zipAs7z)).removeRecursively();
    QDir(outDirFor(sevenNormal)).removeRecursively();
    QDir(base).removeRecursively();

    if (fails == 0) std::printf("ARCHIVEROM-OK\n");
    else            std::printf("ARCHIVEROM had %d failure(s)\n", fails);
    return fails == 0 ? 0 : 1;
}
