// probe_cbr — the .cbr (RAR) comic reader (issue #144): src/comic/RarComic, over the vendored unarr 1.1.1
// RAR decoder, plus the natural page order it shares with every other comic container.
//
// THE FIXTURES ARE HAND-LAID BYTES, INDEPENDENT OF THE CODE UNDER TEST. tools/BookFixtures.h writes RAR 4
// archives one field at a time — the 7-byte signature, a 13-byte main block, a 32+n-byte file block per
// entry, the header CRC16 (the low half of a CRC-32 over the block from its type byte on) and the entry's
// own data CRC-32 — from a CRC table of its own, and includes nothing from src/. Every expected value below
// is a literal written in this file. A fixture produced by the reader would prove only that the reader
// agrees with itself.
//
// WHAT IT PINS:
//
//   1. THE SIGNATURE SORT, which is the whole reason this reader can say anything useful about a file it
//      cannot open. RAR4 and RAR5 differ in ONE byte (the eighth), unarr reads only the first, and the
//      difference is the difference between "damaged" and "a format this build does not implement yet".
//   2. LISTING WITHOUT DECOMPRESSING. imageNames() walks RAR's block-header chain and inflates nothing,
//      which is what lets a .cbr into the #134 library scan when a .cb7 and a .cbt are still kept out. The
//      assertion is BEHAVIOURAL and not a comment: an archive whose headers are intact but whose DATA has
//      been corrupted still LISTS all three pages and EXTRACTS none of them. A listing that decompressed
//      would fail the first half; an extractor that ignored CRCs would fail the second.
//   3. THE PAGE SET: image members only. ComicInfo.xml is not a page, a directory entry is not a page, and
//      the __MACOSX resource-fork shadow every Mac-built archive carries is not a page — that last one
//      sorts FIRST, so without the rule the cover of half the comics in the world is an AppleDouble stub.
//   4. THE PAGE ORDER, and that the COVER follows it rather than the archive's. The fixture stores its
//      pages in the order page10, page2, page1 on purpose: a cover taken from "the first entry" would come
//      back as page 10, and the shelf would show a picture the reader never opens on (#205 / #134).
//   5. EXACT BYTES back out of a real extraction, per page.
//   6. EVERY FAILURE MODE HAS A SENTENCE. A RAR5 archive, a truncated one, one holding no images and a path
//      that is not there each yield their own Status and a non-empty, DISTINCT message.
//
// NOT PINNED HERE, and said plainly rather than implied: RAR's COMPRESSED methods (LZSS / PPMd). Producing a
// compressed RAR needs a RAR compressor, which this repo does not have and will not vendor; the fixtures are
// method 0x30 (store), which is what a comic packer uses for already-compressed JPEGs anyway. The
// decompressors are unarr's own, covered by unarr's upstream test corpus, and reached here through exactly
// the same ar_entry_uncompress call the stored path uses.
//
// Prints CBR-OK on success; any failure prints CBR-FAIL <cond> (line) and exits non-zero.
#include "BookFixtures.h"   // the RAR 4 byte placer, shared with probe_ebookformats/probe_books
#include "RarComic.h"
#include "ComicPageOrder.h"

#include <QByteArray>
#include <QCollator>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QVector>
#include <algorithm>
#include <cstdio>

static int g_fails = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "CBR-FAIL %s (line %d)\n", #cond, __LINE__); ++g_fails; } \
} while (0)

using BookFixtures::buildRar4;
using FixtureEntry = BookFixtures::RarEntry;

static bool writeFile(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.write(bytes);
    return true;
}

// Distinguishable page bodies. Not real JPEGs: RarComic never decodes an image, it hands the encoded bytes
// on, so what matters is that the bytes that went in are the bytes that come out.
static QByteArray pageBody(char tag, int len) { return QByteArray(len, tag); }

int main()
{
    const QString base = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                             .filePath(QStringLiteral("eb-probe-cbr"));
    QDir(base).removeRecursively();
    QDir().mkpath(base);

    const QByteArray p1 = pageBody('1', 61);
    const QByteArray p2 = pageBody('2', 62);
    const QByteArray p10 = pageBody('A', 63);

    // THE PAGES ARE STORED OUT OF ORDER ON PURPOSE (10, 2, 1), so every order assertion below is about the
    // collation and not about the archive.
    const QVector<FixtureEntry> comic = {
        { QStringLiteral("__MACOSX/._page1.jpg"), pageBody('X', 82), false },  // AppleDouble shadow
        { QStringLiteral("pages"), QByteArray(), true },                       // a directory entry
        { QStringLiteral("pages/page10.jpg"), p10, false },
        { QStringLiteral("ComicInfo.xml"), QByteArray("<ComicInfo/>"), false },// metadata, not a page
        { QStringLiteral("pages/page2.png"), p2, false },
        { QStringLiteral("pages/page1.jpg"), p1, false },
    };

    const QString cbr = base + QStringLiteral("/Saga 001.cbr");
    CHECK(writeFile(cbr, buildRar4(comic)));

    // ---- 1. The signature sort -------------------------------------------------------------------------
    {
        CHECK(RarComic::signatureOf(cbr) == RarComic::Status::Ok);

        // RAR5: SEVEN identical bytes and an eighth that differs. This is the case unarr reads as "not a
        // RAR I know" and the user has to be told about by name.
        const QString rar5 = base + QStringLiteral("/modern.cbr");
        CHECK(writeFile(rar5, QByteArray("Rar!\x1A\x07\x01\x00", 8) + QByteArray(64, '\x00')));
        CHECK(RarComic::signatureOf(rar5) == RarComic::Status::Rar5);

        // A zip named .cbr, and a file that is not there at all.
        const QString notRar = base + QStringLiteral("/actually.cbr");
        CHECK(writeFile(notRar, QByteArray("PK\x03\x04", 4) + QByteArray(64, '\x00')));
        CHECK(RarComic::signatureOf(notRar) == RarComic::Status::NotRar);
        CHECK(RarComic::signatureOf(base + QStringLiteral("/missing.cbr")) == RarComic::Status::NotRar);

        // Seven bytes and no more: a RAR4 signature with nothing after it is still a RAR4 signature.
        const QString bare = base + QStringLiteral("/bare.cbr");
        CHECK(writeFile(bare, QByteArray("Rar!\x1A\x07\x00", 7)));
        CHECK(RarComic::signatureOf(bare) == RarComic::Status::Ok);

        // ...and each failure has its own SENTENCE. Ok says nothing; the rest say different things.
        CHECK(RarComic::message(RarComic::Status::Ok).isEmpty());
        CHECK(RarComic::message(RarComic::Status::Rar5).contains(QStringLiteral("RAR5")));
        CHECK(!RarComic::message(RarComic::Status::NotRar).isEmpty());
        CHECK(!RarComic::message(RarComic::Status::Unreadable).isEmpty());
        CHECK(!RarComic::message(RarComic::Status::NoPages).isEmpty());
        CHECK(RarComic::message(RarComic::Status::Rar5) != RarComic::message(RarComic::Status::Unreadable));
        CHECK(RarComic::message(RarComic::Status::NotRar) != RarComic::message(RarComic::Status::NoPages));

        // A RAR5 comic is REFUSED, not attempted: no pages, and the status the message is built from.
        RarComic::Status st = RarComic::Status::Ok;
        CHECK(RarComic::imagePages(rar5, &st).isEmpty());
        CHECK(st == RarComic::Status::Rar5);
        CHECK(RarComic::imageNames(rar5, &st).isEmpty());
        CHECK(st == RarComic::Status::Rar5);
        CHECK(RarComic::coverBytes(rar5, &st).isEmpty());
        CHECK(st == RarComic::Status::Rar5);
    }

    // ---- 2/3. The page set, listed from the headers ----------------------------------------------------
    {
        RarComic::Status st = RarComic::Status::NotRar;
        const QStringList names = RarComic::imageNames(cbr, &st);
        CHECK(st == RarComic::Status::Ok);
        // THREE, in the archive's own order — the directory, the ComicInfo.xml and the __MACOSX shadow are
        // all gone, and nothing has been sorted yet.
        CHECK(names.size() == 3);
        if (names.size() == 3)
        {
            CHECK(names.at(0) == QStringLiteral("pages/page10.jpg"));
            CHECK(names.at(1) == QStringLiteral("pages/page2.png"));
            CHECK(names.at(2) == QStringLiteral("pages/page1.jpg"));
        }
    }

    // ---- 5. Extraction: exact bytes, still in the archive's order ---------------------------------------
    {
        RarComic::Status st = RarComic::Status::NotRar;
        const QVector<QPair<QString, QByteArray>> pages = RarComic::imagePages(cbr, &st);
        CHECK(st == RarComic::Status::Ok);
        CHECK(pages.size() == 3);
        if (pages.size() == 3)
        {
            CHECK(pages.at(0).first == QStringLiteral("pages/page10.jpg"));
            CHECK(pages.at(0).second == p10);
            CHECK(pages.at(1).second == p2);
            CHECK(pages.at(2).second == p1);
            // Lengths differ by page, so a reader that returned the wrong member's bytes could not pass the
            // equality above by accident.
            CHECK(pages.at(0).second.size() == 63);
            CHECK(pages.at(2).second.size() == 61);
        }
    }

    // ---- 4. The COVER is page one in the READER's order, not the archive's ------------------------------
    {
        RarComic::Status st = RarComic::Status::NotRar;
        const QByteArray cover = RarComic::coverBytes(cbr, &st);
        CHECK(st == RarComic::Status::Ok);
        // page1.jpg is the LAST entry in the archive. A cover taken from "the first entry" would be page10.
        CHECK(cover == p1);
        CHECK(cover != p10);

        // ...and the collation the cover leans on is the shared one: 1, 2, 10 — never lexical 1, 10, 2.
        const QCollator coll = ComicPages::collator();
        QStringList order = { QStringLiteral("pages/page10.jpg"), QStringLiteral("pages/page2.png"),
                              QStringLiteral("pages/page1.jpg") };
        std::sort(order.begin(), order.end(),
                  [&coll](const QString& a, const QString& b) { return ComicPages::lessThan(coll, a, b); });
        CHECK(order.at(0) == QStringLiteral("pages/page1.jpg"));
        CHECK(order.at(1) == QStringLiteral("pages/page2.png"));
        CHECK(order.at(2) == QStringLiteral("pages/page10.jpg"));
    }

    // ---- 2 (the behavioural half). A LISTING DOES NOT DECOMPRESS ---------------------------------------
    // The same archive with every entry's stored checksum deliberately wrong. The block headers still chain,
    // so the header walk still finds all three pages; every extraction now fails its CRC-32, so nothing comes
    // back out. A listing that inflated would fail the first assertion; an extractor that skipped the
    // checksum would fail the second.
    {
        QVector<FixtureEntry> damagedSet = comic;
        for (FixtureEntry& e : damagedSet) e.badCrc = true;
        const QString bad = base + QStringLiteral("/scrambled.cbr");
        CHECK(writeFile(bad, buildRar4(damagedSet)));

        RarComic::Status st = RarComic::Status::NotRar;
        CHECK(RarComic::imageNames(bad, &st).size() == 3);
        CHECK(st == RarComic::Status::Ok);

        st = RarComic::Status::Ok;
        CHECK(RarComic::imagePages(bad, &st).isEmpty());
        CHECK(st == RarComic::Status::NoPages);
    }

    // ---- 6. The other ways it can come to nothing ------------------------------------------------------
    {
        // A readable RAR holding no page images at all.
        const QString textOnly = base + QStringLiteral("/notes.cbr");
        CHECK(writeFile(textOnly, buildRar4({ { QStringLiteral("readme.txt"), QByteArray("hello"), false } })));
        RarComic::Status st = RarComic::Status::Ok;
        CHECK(RarComic::imageNames(textOnly, &st).isEmpty());
        CHECK(st == RarComic::Status::NoPages);
        st = RarComic::Status::Ok;
        CHECK(RarComic::imagePages(textOnly, &st).isEmpty());
        CHECK(st == RarComic::Status::NoPages);

        // TRUNCATED: a real RAR4 signature and half a block after it. Not "no pages" — unreadable.
        const QByteArray whole = buildRar4(comic);
        const QString cut = base + QStringLiteral("/truncated.cbr");
        CHECK(writeFile(cut, whole.left(9)));
        st = RarComic::Status::Ok;
        CHECK(RarComic::imagePages(cut, &st).isEmpty());
        CHECK(st == RarComic::Status::Unreadable);
        CHECK(!RarComic::message(st).isEmpty());

        // Cut mid-way through the pages: whatever parsed is kept, and it is fewer than all of them.
        const QString half = base + QStringLiteral("/half.cbr");
        CHECK(writeFile(half, whole.left(whole.size() / 2)));
        st = RarComic::Status::NotRar;
        const QVector<QPair<QString, QByteArray>> some = RarComic::imagePages(half, &st);
        CHECK(some.size() < 3);

        // A path that is not there is NotRar, and never a crash.
        st = RarComic::Status::Ok;
        CHECK(RarComic::imagePages(base + QStringLiteral("/nope.cbr"), &st).isEmpty());
        CHECK(st == RarComic::Status::NotRar);
        CHECK(RarComic::coverBytes(base + QStringLiteral("/nope.cbr")).isEmpty());
        CHECK(RarComic::imageNames(base + QStringLiteral("/nope.cbr")).isEmpty());

        // Every entry point tolerates a null status pointer.
        CHECK(RarComic::imageNames(cbr).size() == 3);
        CHECK(RarComic::imagePages(cbr).size() == 3);
        CHECK(RarComic::coverBytes(cbr) == p1);
    }

    QDir(base).removeRecursively();

    if (g_fails == 0) std::printf("CBR-OK\n");
    else              std::printf("CBR had %d failure(s)\n", g_fails);
    return g_fails == 0 ? 0 : 1;
}
