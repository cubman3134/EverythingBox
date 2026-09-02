// probe_ebookformats — the three BOOK formats issue #144 added to the reader, plus the one list that routes
// them: FictionBook 2 (src/ebook/Fb2Meta + Fb2Book), the Kindle container including KF8/AZW3 and its DRM
// refusal (src/ebook/MobiHeader + MobiBook), plain text and Markdown (src/ebook/TextBook + MarkdownHtml),
// and src/ebook/EbookFormats.
//
// THE FIXTURES SHARE NOTHING WITH THE PARSERS. The Palm/MOBI containers come from tools/BookFixtures.h,
// which lays them out a byte at a time from the format's own published offsets — the record list at 78, the
// PalmDOC header's encryption field at record-0 offset 12, the MOBI header's file version at 36, the full
// name at 84/88, the EXTH block after the header — and includes nothing from src/. The FB2 documents are
// hand-written XML, the Markdown is hand-written Markdown, and every expected value below is a literal
// written in this file. Nothing here is a parser's own output fed back to it.
//
// WHAT IT PINS:
//
//   1. FB2's <description>: title, the split author name joined, the <sequence> series with a DECIMAL index,
//      language, the year (from <publish-info>, and from <document-info> only when the book states no other),
//      and the cover BINARY the <coverpage> declares — with the XLink '#' stripped exactly once. Both wire
//      forms: the plain .fb2 and the zipped .fb2.zip, which must parse IDENTICALLY.
//   2. FB2's bodies onto the reader's chapter model: one chapter per top-level <section>, its <title> as the
//      TOC entry, the prose mapped to HTML, the embedded image staged as a real file and referenced by name,
//      and <body name="notes"> NOT read as a chapter.
//   3. THE MOBI FULL-NAME OFFSET, which was being read SIXTEEN BYTES LATE (at the Output Language field) and
//      returned six bytes of binary as every MOBI's title. The fixture states its title at the offset the
//      format documents; a reader that looks anywhere else gets something else.
//   4. AZW3/KF8, in BOTH shapes: a standalone file whose record 0 states file version 8, and a COMBINED file
//      whose MOBI6 record 0 points at a second header through EXTH 121. In the combined case the text that
//      comes back must be the KF8 half's, which is asserted by it being DIFFERENT from the MOBI6 half's.
//   5. DRM IS REFUSED BY NAME. A container whose PalmDOC header declares encryption yields DrmProtected, a
//      message that says "DRM-protected", NO text, and no cover — and the reader's own open() fails with
//      that same sentence rather than rendering a page of decompressed ciphertext.
//   6. THE PALMDOC DECOMPRESSOR's back-reference, on a stream this file encodes by hand.
//   7. THE ENCODING LADDER for plain text: a BOM wins and is consumed, clean UTF-8 is taken as UTF-8, and
//      bytes that are NOT valid UTF-8 fall through to an 8-bit codec rather than to replacement characters.
//   8. THE MARKDOWN PASS: every construct in scope, the escaping that happens BEFORE any of them, code spans
//      keeping their asterisks, and the chapter split — including a '#' inside a fenced block that must not
//      become a chapter.
//   9. THE ROUTING LIST: what opens in the book reader, and the three things deliberately left out of it.
//
// Prints EBOOKFMT-OK on success; any failure prints EBOOKFMT-FAIL <cond> (line) and exits non-zero.
#include "BookFixtures.h"   // the Palm/MOBI byte placer, shared with probe_cbr/probe_books
#include "EbookFormats.h"
#include "Fb2Book.h"
#include "Fb2Meta.h"
#include "MarkdownHtml.h"
#include "MobiBook.h"
#include "MobiHeader.h"
#include "TextBook.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QVector>
#include <cstdio>
#include <cstring>

#include "miniz.h"

static int g_fails = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "EBOOKFMT-FAIL %s (line %d)\n", #cond, __LINE__); ++g_fails; } \
} while (0)

// ---------------------------------------------------------------------------------------------------------
// Fixture plumbing
// ---------------------------------------------------------------------------------------------------------
static bool writeFile(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.write(bytes);
    return true;
}

static bool writeZip(const QString& path, const QVector<QPair<QString, QByteArray>>& members)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile::remove(path);
    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    if (!mz_zip_writer_init_file(&zip, path.toUtf8().constData(), 0)) return false;
    bool ok = true;
    for (const QPair<QString, QByteArray>& m : members)
        if (!mz_zip_writer_add_mem(&zip, m.first.toUtf8().constData(), m.second.constData(),
                                   size_t(m.second.size()), MZ_BEST_SPEED))
            ok = false;
    ok = mz_zip_writer_finalize_archive(&zip) && ok;
    mz_zip_writer_end(&zip);
    return ok;
}

static QString readAll(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QString();
    return QString::fromUtf8(f.readAll());
}

using BookFixtures::be16At;
using BookFixtures::buildPalmDb;
using BookFixtures::palmDocLiterals;
using HeaderSpec = BookFixtures::MobiSpec;
static QByteArray buildHeaderRecord(const HeaderSpec& s) { return BookFixtures::buildMobiHeaderRecord(s); }

int main()
{
    const QString base = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                             .filePath(QStringLiteral("eb-probe-ebookfmt"));
    QDir(base).removeRecursively();
    QDir().mkpath(base);

    // =====================================================================================================
    // §1 FB2 metadata
    // =====================================================================================================
    // A 1x1 GIF, so the cover binary that comes back is real bytes with a recognisable head.
    const QByteArray gif = QByteArray::fromBase64("R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7");
    const QString gifB64 = QString::fromLatin1(gif.toBase64());

    const QByteArray fb2Xml =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<FictionBook xmlns=\"http://www.gribuser.ru/xml/fictionbook/2.0\""
        " xmlns:l=\"http://www.w3.org/1999/xlink\">\n"
        "  <description>\n"
        "    <title-info>\n"
        "      <genre>sf</genre>\n"
        "      <author><first-name>Ivan</first-name><middle-name>Petrovich</middle-name>"
        "<last-name>Sidorov</last-name></author>\n"
        "      <book-title>The Glass Bead Game</book-title>\n"
        "      <lang>ru</lang>\n"
        "      <sequence name=\"Bead Cycle\" number=\"2.5\"/>\n"
        "      <coverpage><image l:href=\"#cover.gif\"/></coverpage>\n"
        "    </title-info>\n"
        "    <document-info><date value=\"2011-04-02\">2 April 2011</date></document-info>\n"
        "    <publish-info><year>1943</year></publish-info>\n"
        "  </description>\n"
        "  <body>\n"
        "    <title><p>The Glass Bead Game</p></title>\n"
        "    <section><title><p>One</p></title>"
        "<p>The <emphasis>first</emphasis> chapter.</p>"
        "<p><image l:href=\"#plate.gif\"/></p></section>\n"
        "    <section><title><p>Two</p></title><p>The second chapter.</p>"
        "<section><title><p>Two point one</p></title><p>Nested.</p></section></section>\n"
        "    <section><p>An untitled third.</p></section>\n"
        "  </body>\n"
        "  <body name=\"notes\">\n"
        "    <section id=\"n1\"><p>A footnote, which is not a chapter.</p></section>\n"
        "  </body>\n"
        "  <binary id=\"cover.gif\" content-type=\"image/gif\">" + gifB64.toUtf8() + "</binary>\n"
        "  <binary id=\"plate.gif\" content-type=\"image/gif\">" + gifB64.toUtf8() + "</binary>\n"
        "</FictionBook>\n";

    const QString fb2Path = base + QStringLiteral("/glass.fb2");
    CHECK(writeFile(fb2Path, fb2Xml));

    {
        // The path gate, including the two zipped wire forms and the two things it must NOT claim.
        CHECK(Fb2Meta::isFb2Path(QStringLiteral("/x/a.fb2")));
        CHECK(Fb2Meta::isFb2Path(QStringLiteral("/x/A.FB2")));
        CHECK(Fb2Meta::isFb2Path(QStringLiteral("/x/a.fb2.zip")));
        CHECK(Fb2Meta::isFb2Path(QStringLiteral("/x/a.fbz")));
        CHECK(!Fb2Meta::isFb2Path(QStringLiteral("/x/a.zip")));
        CHECK(!Fb2Meta::isFb2Path(QStringLiteral("/x/a.epub")));

        Fb2Meta::Metadata m;
        CHECK(Fb2Meta::readXml(fb2Xml, &m));
        CHECK(m.title == QStringLiteral("The Glass Bead Game"));
        CHECK(m.author == QStringLiteral("Ivan Petrovich Sidorov"));
        CHECK(m.series == QStringLiteral("Bead Cycle"));
        CHECK(m.seriesIndex == 2.5);          // a DECIMAL: 2.5 truncated to 2 collides with book two
        CHECK(m.language == QStringLiteral("ru"));
        CHECK(m.year == 1943);                // the PUBLICATION year, not the 2011 transcription date
        CHECK(m.coverId == QStringLiteral("cover.gif"));   // the XLink '#' stripped, exactly once

        // The cover binary, decoded from its base64, byte for byte.
        CHECK(Fb2Meta::binary(fb2Xml, QStringLiteral("cover.gif")) == gif);
        CHECK(Fb2Meta::binary(fb2Xml, QStringLiteral("nothing.gif")).isEmpty());

        // readFile() adds the chapter count, and the notes body is not one of them.
        Fb2Meta::Metadata mf;
        QByteArray cover;
        CHECK(Fb2Meta::readFile(fb2Path, &mf, &cover));
        CHECK(mf.sectionCount == 3);          // three top-level sections; the nested one is not a fourth
        CHECK(cover == gif);
        CHECK(mf.title == m.title && mf.author == m.author);

        // A FictionBook that declares NOTHING parses to an empty Metadata and still succeeds: "the file said
        // nothing" is a fact about the file, not a failure to read it.
        Fb2Meta::Metadata empty;
        CHECK(Fb2Meta::readXml("<FictionBook><description/><body><p>x</p></body></FictionBook>", &empty));
        CHECK(empty.title.isEmpty() && empty.author.isEmpty() && empty.series.isEmpty());
        // ...and a document that is not FictionBook at all is refused.
        Fb2Meta::Metadata no;
        CHECK(!Fb2Meta::readXml("<html><body>hello</body></html>", &no));
        CHECK(!Fb2Meta::readXml(QByteArray(), &no));

        // A year stated ONLY as a document date is taken, and only then.
        Fb2Meta::Metadata dated;
        CHECK(Fb2Meta::readXml("<FictionBook><description><document-info>"
                               "<date value=\"1998-07-01\"/></document-info></description></FictionBook>",
                               &dated));
        CHECK(dated.year == 1998);
    }

    // §1b The zipped wire form parses IDENTICALLY -------------------------------------------------------
    {
        const QString zipped = base + QStringLiteral("/glass.fb2.zip");
        CHECK(writeZip(zipped, { { QStringLiteral("glass.fb2"), fb2Xml } }));
        Fb2Meta::Metadata a, b;
        QByteArray coverA, coverB;
        CHECK(Fb2Meta::readFile(fb2Path, &a, &coverA));
        CHECK(Fb2Meta::readFile(zipped, &b, &coverB));
        CHECK(a.title == b.title);
        CHECK(a.author == b.author);
        CHECK(a.series == b.series && a.seriesIndex == b.seriesIndex);
        CHECK(a.year == b.year && a.sectionCount == b.sectionCount);
        CHECK(coverA == coverB && !coverB.isEmpty());
        // A zip holding no .fb2 member yields nothing rather than a guess at its largest member.
        const QString wrong = base + QStringLiteral("/other.fb2.zip");
        CHECK(writeZip(wrong, { { QStringLiteral("readme.txt"), QByteArray("hi") } }));
        CHECK(Fb2Meta::documentBytes(wrong).isEmpty());
    }

    // =====================================================================================================
    // §2 FB2 onto the reader's chapter model
    // =====================================================================================================
    {
        Fb2Book book;
        QString err;
        CHECK(book.open(fb2Path, &err));
        CHECK(err.isEmpty());
        CHECK(book.isOpen());
        CHECK(book.title() == QStringLiteral("The Glass Bead Game"));
        CHECK(book.author() == QStringLiteral("Ivan Petrovich Sidorov"));
        CHECK(book.sourcePath() == fb2Path);
        // THREE chapters: the three top-level sections. The <body name="notes"> is apparatus, not a fourth,
        // and the body's own <title> was absorbed into chapter one rather than becoming a chapter of its own.
        CHECK(book.chapterFiles().size() == 3);
        CHECK(book.toc().size() == 3);
        if (book.toc().size() == 3)
        {
            CHECK(book.toc().at(0).title == QStringLiteral("One"));
            CHECK(book.toc().at(1).title == QStringLiteral("Two"));
            // A section with no <title> still gets a row, or every entry after it points at the wrong chapter.
            CHECK(!book.toc().at(2).title.isEmpty());
            CHECK(book.chapterIndexForHref(book.toc().at(1).href) == 1);
            CHECK(book.chapterIndexForHref(QStringLiteral("nope.html")) == -1);
        }
        if (book.chapterFiles().size() == 3)
        {
            const QString one = readAll(book.chapterFiles().at(0));
            CHECK(one.contains(QStringLiteral("The Glass Bead Game")));   // the absorbed body title
            CHECK(one.contains(QStringLiteral("<h2>One</h2>")));
            CHECK(one.contains(QStringLiteral("The <i>first</i> chapter.")));
            // The embedded image is a REAL FILE beside the chapter, referenced by the name it was staged as.
            CHECK(one.contains(QStringLiteral("<img src=\"img")));
            const QString dir = QFileInfo(book.chapterFiles().at(0)).absolutePath();
            CHECK(QFileInfo::exists(dir + QStringLiteral("/img1.gif")));
            CHECK(QFileInfo::exists(dir + QStringLiteral("/img2.gif")));

            const QString two = readAll(book.chapterFiles().at(1));
            CHECK(two.contains(QStringLiteral("The second chapter.")));
            CHECK(two.contains(QStringLiteral("<h3>Two point one</h3>")));  // nested section: a heading, not a chapter
            CHECK(two.contains(QStringLiteral("Nested.")));

            const QString three = readAll(book.chapterFiles().at(2));
            CHECK(three.contains(QStringLiteral("An untitled third.")));
            // The footnote body is nowhere in the book.
            for (const QString& f : book.chapterFiles())
                CHECK(!readAll(f).contains(QStringLiteral("which is not a chapter")));
        }

        // Something that is not a FictionBook fails with a sentence, not a blank reader.
        const QString notFb2 = base + QStringLiteral("/plain.fb2");
        CHECK(writeFile(notFb2, "<html><body>no</body></html>"));
        Fb2Book bad;
        QString badErr;
        CHECK(!bad.open(notFb2, &badErr));
        CHECK(!badErr.isEmpty());
        CHECK(!bad.isOpen());
    }

    // =====================================================================================================
    // §3/§4/§6 The Kindle container: MOBI6, standalone AZW3, combined MOBI6+KF8
    // =====================================================================================================
    const QByteArray mobiText = "<html><head><guide/></head><body><p>The old markup.</p>"
                                "<img recindex=\"00001\"/></body></html>";
    const QByteArray kf8Text  = "<html><head><link href=\"kindle:flow:0001?mime=text/css\"/></head>"
                                "<body aid=\"0\"><p>The KF8 markup.</p></body></html>";

    {
        CHECK(MobiHeader::isMobiContainer(QByteArray(60, '\0') + QByteArray("BOOKMOBI")));
        CHECK(MobiHeader::isMobiContainer(QByteArray(60, '\0') + QByteArray("TEXtREAd")));
        CHECK(!MobiHeader::isMobiContainer(QByteArray("PK\x03\x04", 4)));
        CHECK(!MobiHeader::isMobiContainer(QByteArray()));

        // ---- A plain MOBI6, uncompressed --------------------------------------------------------------
        HeaderSpec m6;
        m6.fullName = QStringLiteral("A Fine Old Book");
        m6.author   = QStringLiteral("Mary Shelley");
        m6.textRecords = 1;
        const QString mobiPath = base + QStringLiteral("/old.mobi");
        CHECK(writeFile(mobiPath, buildPalmDb({ buildHeaderRecord(m6), mobiText })));

        QFile f(mobiPath);
        CHECK(f.open(QIODevice::ReadOnly));
        const QByteArray mobiBytes = f.readAll();
        f.close();

        MobiHeader::Info info;
        CHECK(MobiHeader::read(mobiBytes, &info) == MobiHeader::Result::Ok);
        // §3: THE TITLE, from record-0 offsets 84/88. Read sixteen bytes late this is the Output Language
        // field, and the "title" is a few bytes of the record's own binary.
        CHECK(info.title == QStringLiteral("A Fine Old Book"));
        CHECK(info.author == QStringLiteral("Mary Shelley"));
        CHECK(info.kf8 == false);
        CHECK(info.bootRecord == 0);
        CHECK(info.fileVersion == 6);
        CHECK(info.hasCover == false);
        CHECK(MobiHeader::coverBytes(mobiBytes).isEmpty());

        QByteArray text;
        CHECK(MobiHeader::readText(mobiBytes, &info, &text) == MobiHeader::Result::Ok);
        CHECK(text == mobiText);

        // ---- §6 A PalmDoc-compressed record, with a real back-reference --------------------------------
        // "abc" then "copy 6 bytes from 3 back" == "abcabcabc". The stream is encoded here by hand.
        QByteArray lz = palmDocLiterals("abc");
        lz.append(char(0x80));                       // 0x801B: distance 3, length (3 & 7) + 3 == 6
        lz.append(char(0x1B));
        HeaderSpec packed;
        packed.compression = 2;
        packed.fullName = QStringLiteral("Packed");
        const QString packedPath = base + QStringLiteral("/packed.mobi");
        CHECK(writeFile(packedPath, buildPalmDb({ buildHeaderRecord(packed), lz })));
        QFile pf(packedPath);
        CHECK(pf.open(QIODevice::ReadOnly));
        const QByteArray packedBytes = pf.readAll();
        pf.close();
        MobiHeader::Info pinfo;
        QByteArray ptext;
        CHECK(MobiHeader::readText(packedBytes, &pinfo, &ptext) == MobiHeader::Result::Ok);
        CHECK(ptext == QByteArray("abcabcabc"));
        CHECK(pinfo.title == QStringLiteral("Packed"));

        // ---- §4a A STANDALONE AZW3: record 0 is the KF8 header ----------------------------------------
        HeaderSpec kf8;
        kf8.fileVersion = 8;
        kf8.fullName = QStringLiteral("A Modern Book");
        kf8.author   = QStringLiteral("Ada Lovelace");
        const QString azw3Path = base + QStringLiteral("/modern.azw3");
        CHECK(writeFile(azw3Path, buildPalmDb({ buildHeaderRecord(kf8), kf8Text })));
        QFile af(azw3Path);
        CHECK(af.open(QIODevice::ReadOnly));
        const QByteArray azwBytes = af.readAll();
        af.close();

        MobiHeader::Info ainfo;
        QByteArray atext;
        CHECK(MobiHeader::readText(azwBytes, &ainfo, &atext) == MobiHeader::Result::Ok);
        CHECK(ainfo.kf8 == true);
        CHECK(ainfo.fileVersion == 8);
        CHECK(ainfo.bootRecord == 0);
        CHECK(ainfo.title == QStringLiteral("A Modern Book"));
        CHECK(atext == kf8Text);

        // ---- §4b A COMBINED file: MOBI6 record 0, EXTH 121 -> the KF8 header ---------------------------
        // Records: [0] MOBI6 header, [1] MOBI6 text, [2] KF8 header, [3] KF8 text.
        HeaderSpec base6;
        base6.fullName = QStringLiteral("Both Halves");
        base6.author   = QStringLiteral("Anon");
        base6.kf8Boundary = 2;
        HeaderSpec half8;
        half8.fileVersion = 8;
        half8.fullName = QStringLiteral("Both Halves");
        const QString bothPath = base + QStringLiteral("/both.mobi");
        CHECK(writeFile(bothPath, buildPalmDb({ buildHeaderRecord(base6), mobiText,
                                                buildHeaderRecord(half8), kf8Text })));
        QFile bf(bothPath);
        CHECK(bf.open(QIODevice::ReadOnly));
        const QByteArray bothBytes = bf.readAll();
        bf.close();

        MobiHeader::Info binfo;
        QByteArray btext;
        CHECK(MobiHeader::readText(bothBytes, &binfo, &btext) == MobiHeader::Result::Ok);
        CHECK(binfo.kf8 == true);
        CHECK(binfo.bootRecord == 2);          // the KF8 header, found through EXTH 121
        // THE TEXT IS THE KF8 HALF'S. Asserted by DIFFERENCE as well as by equality, so a reader that
        // ignored the boundary and read records 1..N could not pass.
        CHECK(btext == kf8Text);
        CHECK(btext != mobiText);
        CHECK(binfo.author == QStringLiteral("Anon"));   // stated only by the MOBI6 half, still reported

        // ---- §3b The EXTH cover, through the shared image records --------------------------------------
        HeaderSpec art;
        art.fullName = QStringLiteral("Illustrated");
        art.textRecords = 1;
        art.firstImage = 2;      // images start at record 2
        art.coverOffset = 0;     // ...and the cover is the first of them
        const QString artPath = base + QStringLiteral("/art.mobi");
        CHECK(writeFile(artPath, buildPalmDb({ buildHeaderRecord(art), mobiText, gif })));
        QFile gf(artPath);
        CHECK(gf.open(QIODevice::ReadOnly));
        const QByteArray artBytes = gf.readAll();
        gf.close();
        MobiHeader::Info ginfo;
        CHECK(MobiHeader::read(artBytes, &ginfo) == MobiHeader::Result::Ok);
        CHECK(ginfo.hasCover == true);
        CHECK(MobiHeader::coverBytes(artBytes) == gif);

        // A cover record that does NOT start like an image is one of the format's index records, not a
        // picture: the reader hands back nothing rather than a blob the shelf will fail to decode.
        const QString fakeArtPath = base + QStringLiteral("/fakeart.mobi");
        CHECK(writeFile(fakeArtPath, buildPalmDb({ buildHeaderRecord(art), mobiText,
                                                   QByteArray("INDX not an image at all") })));
        QFile ff(fakeArtPath);
        CHECK(ff.open(QIODevice::ReadOnly));
        CHECK(MobiHeader::coverBytes(ff.readAll()).isEmpty());
        ff.close();

        // ---- Malformed containers -----------------------------------------------------------------------
        MobiHeader::Info junk;
        CHECK(MobiHeader::read(QByteArray("not a book at all"), &junk) == MobiHeader::Result::NotMobi);
        QByteArray oneRecord = buildPalmDb({ buildHeaderRecord(m6) });   // a record list of ONE
        CHECK(MobiHeader::read(oneRecord, &junk) == MobiHeader::Result::Corrupt);
        QByteArray liar = mobiBytes;
        be16At(liar, 76, 30000);                                        // a record count the file cannot hold
        CHECK(MobiHeader::read(liar, &junk) == MobiHeader::Result::Corrupt);
    }

    // =====================================================================================================
    // §5 DRM is refused BY NAME
    // =====================================================================================================
    {
        HeaderSpec drm;
        drm.encryption = 2;                       // Amazon/Mobipocket DRM
        drm.fullName = QStringLiteral("A Bought Book");
        drm.author = QStringLiteral("Somebody");
        drm.firstImage = 2;
        drm.coverOffset = 0;
        const QString drmPath = base + QStringLiteral("/bought.azw3");
        CHECK(writeFile(drmPath, buildPalmDb({ buildHeaderRecord(drm), mobiText, gif })));
        QFile f(drmPath);
        CHECK(f.open(QIODevice::ReadOnly));
        const QByteArray drmBytes = f.readAll();
        f.close();

        MobiHeader::Info info;
        CHECK(MobiHeader::read(drmBytes, &info) == MobiHeader::Result::DrmProtected);
        QByteArray text;
        CHECK(MobiHeader::readText(drmBytes, &info, &text) == MobiHeader::Result::DrmProtected);
        CHECK(text.isEmpty());                    // NOT ONE BYTE decoded
        CHECK(MobiHeader::coverBytes(drmBytes).isEmpty());   // not even the cover

        const QString msg = MobiHeader::message(MobiHeader::Result::DrmProtected);
        CHECK(msg.contains(QStringLiteral("DRM-protected")));
        CHECK(msg != MobiHeader::message(MobiHeader::Result::Corrupt));
        CHECK(msg != MobiHeader::message(MobiHeader::Result::NotMobi));
        CHECK(MobiHeader::message(MobiHeader::Result::Ok).isEmpty());

        // ...and that is the sentence the READER shows: the open fails, and it fails saying this.
        MobiBook book;
        QString err;
        CHECK(!book.open(drmPath, &err));
        CHECK(err == msg);
        CHECK(!book.isOpen());
        CHECK(book.chapterFiles().isEmpty());
    }

    // §4c The reader renders an AZW3's text through the same path a MOBI's goes through -----------------
    {
        HeaderSpec kf8;
        kf8.fileVersion = 8;
        kf8.fullName = QStringLiteral("A Modern Book");
        kf8.author = QStringLiteral("Ada Lovelace");
        const QString azw3Path = base + QStringLiteral("/render.azw3");
        CHECK(writeFile(azw3Path, buildPalmDb({ buildHeaderRecord(kf8), kf8Text })));

        MobiBook book;
        QString err;
        CHECK(book.open(azw3Path, &err));
        CHECK(book.title() == QStringLiteral("A Modern Book"));
        CHECK(book.author() == QStringLiteral("Ada Lovelace"));
        CHECK(book.chapterFiles().size() == 1);
        if (book.chapterFiles().size() == 1)
        {
            const QString html = readAll(book.chapterFiles().first());
            CHECK(html.contains(QStringLiteral("The KF8 markup.")));
            // KF8's kindle:flow: stylesheet link is stripped - nothing outside a Kindle can resolve it, and
            // QTextBrowser would try to fetch every one.
            CHECK(!html.contains(QStringLiteral("kindle:flow")));
            CHECK(!html.contains(QStringLiteral("<link")));
            CHECK(html.startsWith(QStringLiteral("<!DOCTYPE html>")));
        }
    }

    // =====================================================================================================
    // §7 The plain-text encoding ladder
    // =====================================================================================================
    {
        using Enc = TextBook::Encoding;
        Enc used = Enc::Latin1;

        // 1. A BOM wins, and is CONSUMED — never left as a zero-width space at the top of page one.
        CHECK(TextBook::decode(QByteArray("\xEF\xBB\xBF", 3) + QByteArray("caf\xC3\xA9"), &used)
              == QString::fromUtf8("caf\xC3\xA9"));
        CHECK(used == Enc::Utf8Bom);
        CHECK(TextBook::decode(QByteArray("\xFF\xFE", 2) + QByteArray("h\0i\0", 4), &used)
              == QStringLiteral("hi"));
        CHECK(used == Enc::Utf16LeBom);
        CHECK(TextBook::decode(QByteArray("\xFE\xFF", 2) + QByteArray("\0h\0i", 4), &used)
              == QStringLiteral("hi"));
        CHECK(used == Enc::Utf16BeBom);

        // 2. Clean UTF-8 with no BOM is taken as UTF-8, on the ERROR FLAG and not on a look at the output.
        CHECK(TextBook::decode(QByteArray("caf\xC3\xA9 \xE2\x80\x94 done"), &used)
              == QString::fromUtf8("caf\xC3\xA9 \xE2\x80\x94 done"));
        CHECK(used == Enc::Utf8);

        // 3. A lone 0xE9 is not valid UTF-8. It falls through to an 8-bit codec, and the accented letter
        //    SURVIVES — the failure this ladder exists to prevent is a page of replacement characters.
        const QString eight = TextBook::decode(QByteArray("caf\xE9 cr\xE8me"), &used);
        CHECK(used == Enc::System || used == Enc::Latin1);
        CHECK(eight.contains(QChar(0x00E9)));      // é
        CHECK(!eight.contains(QChar(0xFFFD)));     // and NOT the replacement character
        CHECK(std::strlen(TextBook::encodingName(used)) > 0);
        CHECK(std::strcmp(TextBook::encodingName(Enc::Utf8), TextBook::encodingName(Enc::Latin1)) != 0);

        // Empty input is empty text, not a crash.
        CHECK(TextBook::decode(QByteArray(), &used).isEmpty());
        CHECK(TextBook::decode(QByteArray("hi")) == QStringLiteral("hi"));   // the null-pointer overload
    }

    // §7b Plain text -> paragraphs -----------------------------------------------------------------------
    {
        const QString html = TextBook::plainTextToHtml(
            QStringLiteral("One line\nwrapped onto two.\n\nA second paragraph.\n\n\nA third <b>one</b>.\n"));
        // Hard-wrapped prose is ONE paragraph, joined with a space — not forty <p>s.
        CHECK(html.contains(QStringLiteral("<p>One line wrapped onto two.</p>")));
        CHECK(html.contains(QStringLiteral("<p>A second paragraph.</p>")));
        // HTML metacharacters are escaped: a .txt is a file off somebody's disk, and <b> in one is text.
        CHECK(html.contains(QStringLiteral("&lt;b&gt;one&lt;/b&gt;")));
        CHECK(!html.contains(QStringLiteral("<b>")));
        CHECK(html.count(QStringLiteral("<p>")) == 3);   // three paragraphs, not four (the run of blanks)
        CHECK(!TextBook::plainTextToHtml(QString()).isEmpty());   // an empty file is an empty page, not a crash
    }

    // =====================================================================================================
    // §8 Markdown
    // =====================================================================================================
    {
        // Inline: every construct in scope, plus the escaping that runs before any of them.
        CHECK(MarkdownHtml::inlineToHtml(QStringLiteral("a **bold** word"))
              == QStringLiteral("a <b>bold</b> word"));
        CHECK(MarkdownHtml::inlineToHtml(QStringLiteral("a __bold__ word"))
              == QStringLiteral("a <b>bold</b> word"));
        CHECK(MarkdownHtml::inlineToHtml(QStringLiteral("an *italic* word"))
              == QStringLiteral("an <i>italic</i> word"));
        CHECK(MarkdownHtml::inlineToHtml(QStringLiteral("an _italic_ word"))
              == QStringLiteral("an <i>italic</i> word"));
        // Strong is matched BEFORE emphasis, or "**x**" reads as an empty italic wrapping "*x*".
        CHECK(!MarkdownHtml::inlineToHtml(QStringLiteral("**x**")).contains(QStringLiteral("<i>")));
        // A code span keeps its asterisks: code comes out before the emphasis rules and goes back after.
        CHECK(MarkdownHtml::inlineToHtml(QStringLiteral("run `a * b * c` now"))
              == QStringLiteral("run <code>a * b * c</code> now"));
        CHECK(MarkdownHtml::inlineToHtml(QStringLiteral("[the docs](guide.html) here"))
              == QStringLiteral("<a href=\"guide.html\">the docs</a> here"));
        // An image is a link with a bang, so it is matched FIRST — by a RELATIVE path, as the brief says.
        CHECK(MarkdownHtml::inlineToHtml(QStringLiteral("![a plate](art/plate.png)"))
              == QStringLiteral("<img src=\"art/plate.png\" alt=\"a plate\">"));
        // ESCAPED, ALWAYS: a .md is a file off somebody's disk.
        CHECK(MarkdownHtml::inlineToHtml(QStringLiteral("<script>alert(1)</script>"))
              == QStringLiteral("&lt;script&gt;alert(1)&lt;/script&gt;"));
        CHECK(MarkdownHtml::inlineToHtml(QStringLiteral("a & b")) == QStringLiteral("a &amp; b"));

        const QString md =
            QStringLiteral("Front matter before any heading.\n"
                           "\n"
                           "# Chapter One\n"
                           "\n"
                           "Some **prose** here.\n"
                           "\n"
                           "## A subheading\n"
                           "\n"
                           "- first\n"
                           "- second\n"
                           "\n"
                           "1. one\n"
                           "2. two\n"
                           "\n"
                           "> quoted line\n"
                           "\n"
                           "---\n"
                           "\n"
                           "```\n"
                           "# not a chapter\n"
                           "code *stays* literal <b>\n"
                           "```\n"
                           "\n"
                           "# Chapter Two\n"
                           "\n"
                           "The end.\n");

        const QVector<MarkdownHtml::Section> secs = MarkdownHtml::render(md);
        // THREE sections: the front matter, then one per top-level heading. The '#' inside the fenced block
        // is a comment in a code sample and must not split the book.
        CHECK(secs.size() == 3);
        if (secs.size() == 3)
        {
            CHECK(secs.at(0).title.isEmpty());
            CHECK(secs.at(0).html.contains(QStringLiteral("Front matter")));
            CHECK(secs.at(1).title == QStringLiteral("Chapter One"));
            CHECK(secs.at(2).title == QStringLiteral("Chapter Two"));

            const QString one = secs.at(1).html;
            CHECK(one.contains(QStringLiteral("<h1>Chapter One</h1>")));
            CHECK(one.contains(QStringLiteral("<p>Some <b>prose</b> here.</p>")));
            CHECK(one.contains(QStringLiteral("<h2>A subheading</h2>")));
            CHECK(one.contains(QStringLiteral("<ul><li>first</li><li>second</li></ul>")));
            CHECK(one.contains(QStringLiteral("<ol><li>one</li><li>two</li></ol>")));
            CHECK(one.contains(QStringLiteral("<blockquote><p>quoted line</p></blockquote>")));
            CHECK(one.contains(QStringLiteral("<hr>")));
            CHECK(one.contains(QStringLiteral("<pre><code>")));
            CHECK(one.contains(QStringLiteral("# not a chapter")));           // kept, verbatim, as code
            CHECK(one.contains(QStringLiteral("code *stays* literal &lt;b&gt;")));  // no emphasis, escaped
            CHECK(secs.at(2).html.contains(QStringLiteral("The end.")));
        }

        // The cheap listing the library scan uses agrees with the render, heading for heading.
        const QStringList heads = MarkdownHtml::topLevelHeadings(md);
        CHECK(heads == QStringList({ QStringLiteral("Chapter One"), QStringLiteral("Chapter Two") }));
        // A document with NO top-level heading is exactly one section and names none.
        const QVector<MarkdownHtml::Section> flat =
            MarkdownHtml::render(QStringLiteral("Just prose.\n\nAnd more of it.\n"));
        CHECK(flat.size() == 1);
        CHECK(flat.first().title.isEmpty());
        CHECK(MarkdownHtml::topLevelHeadings(QStringLiteral("## only a subheading\n")).isEmpty());
        CHECK(MarkdownHtml::render(QString()).isEmpty());
    }

    // §8b TextBook over real files -----------------------------------------------------------------------
    {
        CHECK(TextBook::isPlainTextPath(QStringLiteral("/x/a.TXT")));
        CHECK(TextBook::isPlainTextPath(QStringLiteral("/x/a.text")));
        CHECK(!TextBook::isPlainTextPath(QStringLiteral("/x/a.md")));
        CHECK(TextBook::isMarkdownPath(QStringLiteral("/x/a.md")));
        CHECK(TextBook::isMarkdownPath(QStringLiteral("/x/a.markdown")));
        CHECK(TextBook::isTextBookPath(QStringLiteral("/x/a.txt")));
        CHECK(!TextBook::isTextBookPath(QStringLiteral("/x/a.epub")));

        const QString mdPath = base + QStringLiteral("/manuscript.md");
        CHECK(writeFile(mdPath, "# The First Part\n\nOne.\n\n# The Second Part\n\nTwo.\n"));
        TextBook md;
        QString err;
        CHECK(md.open(mdPath, &err));
        CHECK(md.chapterFiles().size() == 2);
        CHECK(md.toc().size() == 2);
        if (md.toc().size() == 2)
        {
            CHECK(md.toc().at(0).title == QStringLiteral("The First Part"));
            CHECK(md.toc().at(1).title == QStringLiteral("The Second Part"));
            CHECK(md.chapterIndexForHref(md.toc().at(1).href) == 1);
        }
        // The first top-level heading is the nearest thing Markdown has to a stated title.
        CHECK(md.title() == QStringLiteral("The First Part"));
        CHECK(md.author().isEmpty());
        if (!md.chapterFiles().isEmpty())
            CHECK(readAll(md.chapterFiles().first()).contains(QStringLiteral("<p>One.</p>")));

        // A .txt is ALWAYS one chapter: nothing in plain text distinguishes a chapter heading from a line of
        // dialogue in capitals, and a contents panel of one row saying "Chapter 1" is furniture.
        const QString txtPath = base + QStringLiteral("/notes on the war.txt");
        CHECK(writeFile(txtPath, "Chapter One\n\nIt began badly.\n\nAnd continued.\n"));
        TextBook txt;
        CHECK(txt.open(txtPath, &err));
        CHECK(txt.chapterFiles().size() == 1);
        CHECK(txt.toc().isEmpty());
        CHECK(txt.title() == QStringLiteral("notes on the war"));   // its filename, as an untagged EPUB gets
        CHECK(readAll(txt.chapterFiles().first()).contains(QStringLiteral("<p>It began badly.</p>")));

        TextBook missing;
        CHECK(!missing.open(base + QStringLiteral("/nothing.txt"), &err));
        CHECK(!err.isEmpty());
    }

    // =====================================================================================================
    // §9 The routing list
    // =====================================================================================================
    {
        for (const char* yes : { "/x/a.epub", "/x/a.fb2", "/x/a.fb2.zip", "/x/a.fbz", "/x/a.azw3",
                                 "/x/a.azw", "/x/a.mobi", "/x/a.txt", "/x/a.md", "/x/a.markdown" })
            CHECK(EbookFormats::opensInBookReader(QString::fromLatin1(yes)));
        CHECK(EbookFormats::opensInBookReader(QStringLiteral("/X/A.EPUB")));   // case-insensitive
        // The three deliberate exclusions: the PDF two-step, the comic reader's own list, and a bare zip.
        for (const char* no : { "/x/a.pdf", "/x/a.cbz", "/x/a.cbr", "/x/a.cb7", "/x/a.cbt", "/x/a.zip",
                                "/x/a.mp3", "/x/a.mkv", "" })
            CHECK(!EbookFormats::opensInBookReader(QString::fromLatin1(no)));
    }

    QDir(base).removeRecursively();

    if (g_fails == 0) std::printf("EBOOKFMT-OK\n");
    else              std::printf("EBOOKFMT had %d failure(s)\n", g_fails);
    return g_fails == 0 ? 0 : 1;
}
