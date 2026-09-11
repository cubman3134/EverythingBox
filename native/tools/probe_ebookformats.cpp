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
//  10. SINGLE-FILE HTML (issue #259, src/ebook/HtmlText + TextBook): each branch of the chapter rule, the title
//      and author fallbacks, the declared charset beating the ladder and a BOM beating the declaration, and THE
//      PRIVACY LINE one rule per case - remote, protocol-relative, UNC, absolute and ".."-escaping images
//      dropped, a sibling image kept, scripts / frames / objects / stylesheets / event handlers stripped - with
//      a sweep over every staged document for any location-bearing attribute that names a remote place.
//
// Prints EBOOKFMT-OK on success; any failure prints EBOOKFMT-FAIL <cond> (line) and exits non-zero.
#include "BookFixtures.h"   // the Palm/MOBI byte placer, shared with probe_cbr/probe_books
#include "EbookFormats.h"
#include "Fb2Book.h"
#include "Fb2Meta.h"
#include "HtmlText.h"
#include "MarkdownHtml.h"
#include "MobiBook.h"
#include "MobiHeader.h"
#include "TextBook.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QUrl>
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

        // Issue #259: a single-file HTML document, in either spelling and either case...
        for (const char* yes : { "/x/a.html", "/x/a.htm", "/X/A.HTML", "/x/a.HTM" })
            CHECK(EbookFormats::opensInBookReader(QString::fromLatin1(yes)));
        // ...and NOT the things whose names merely end in "html". An .xhtml is an EPUB's insides, an .mht is a
        // MIME archive, and a .html.zip is a zip (the bare-zip refusal above, unchanged).
        for (const char* no : { "/x/a.xhtml", "/x/a.shtml", "/x/a.html.zip", "/x/a.mht", "/x/a.mhtml" })
            CHECK(!EbookFormats::opensInBookReader(QString::fromLatin1(no)));
    }

    // =====================================================================================================
    // §10 Single-file HTML (issue #259)
    // =====================================================================================================
    // Every document here is hand-written in this file and every expectation is a literal. The cases go
    // through TextBook::open() and read back the chapter files it STAGED, because those files are exactly what
    // the reader renders: a check against them is a check against what the page would show and fetch.
    {
        const QString hdir = base + QStringLiteral("/htmlbook");
        // The files a relative image may and may not reach. All of them EXIST, so every refusal below is the
        // rule refusing - never just a missing file.
        const QByteArray png = QByteArray::fromHex("89504e470d0a1a0a0000000d49484452");
        CHECK(writeFile(hdir + QStringLiteral("/pic.png"), png));
        CHECK(writeFile(hdir + QStringLiteral("/images/inner.png"), png));
        CHECK(writeFile(base + QStringLiteral("/secret.png"), png));   // one level ABOVE the book's folder
        const QString hdirCanon = QFileInfo(hdir).canonicalFilePath();

        // Every staged chapter, joined: what the reader will render for this document.
        QString err;
        auto stage = [&](TextBook& b, const QString& name, const QByteArray& bytes) -> QString {
            const QString p = hdir + QLatin1Char('/') + name;
            if (!writeFile(p, bytes) || !b.open(p, &err)) return QString();
            QString all;
            for (const QString& f : b.chapterFiles()) all += readAll(f);
            return all;
        };
        // Every src= the staged HTML carries. Read the way the reader reads it: an attribute, quoted.
        auto srcs = [](const QString& html) {
            QStringList out;
            static const QRegularExpression re(QStringLiteral("src=\"([^\"]*)\""));
            auto it = re.globalMatch(html);
            while (it.hasNext()) out << it.next().captured(1);
            return out;
        };
        // An image source is LOCAL AND INSIDE the book's folder: a file URL whose file exists and sits under it.
        auto insideBookDir = [&](const QString& src) {
            const QUrl u(src);
            if (!u.isLocalFile()) return false;
            const QString canon = QFileInfo(u.toLocalFile()).canonicalFilePath();
            return !canon.isEmpty() && canon.startsWith(hdirCanon + QLatin1Char('/'));
        };
        // THE PRIVACY SWEEP, applied to every document below: no attribute that could name a location carries
        // a remote or protocol-relative one. Written against the raw staged bytes, not against a parse of them.
        static const QRegularExpression reRemote(QStringLiteral(
            "(?i)(src|href|srcset|background|poster|data|action|cite)\\s*=\\s*[\"']?\\s*(https?:|ftp:|//|\\\\\\\\)"));
        QStringList all;   // every staged document, for the sweep at the end

        // ---- (a) several top-level headings: one chapter each, and the <head>'s title and author ----------
        {
            TextBook b;
            const QString h = stage(b, QStringLiteral("three parts.html"),
                "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>The Glass &amp; Stone</title>"
                "<meta name=\"author\" content=\"Ada Byron\"></head><body>"
                "<h1>Part One</h1><p>First <b>bold</b> words.</p>"
                "<h1>Part Two</h1><p>Second.</p>"
                "<h1>Part Three</h1><p>Third.</p></body></html>");
            all << h;
            CHECK(b.chapterFiles().size() == 3);
            CHECK(b.toc().size() == 3);
            if (b.toc().size() == 3)
            {
                CHECK(b.toc().at(0).title == QStringLiteral("Part One"));
                CHECK(b.toc().at(1).title == QStringLiteral("Part Two"));
                CHECK(b.toc().at(2).title == QStringLiteral("Part Three"));
                CHECK(b.chapterIndexForHref(b.toc().at(2).href) == 2);
            }
            // <title> beats the first heading; its entity is decoded for the shelf, not shown as "&amp;".
            CHECK(b.title() == QStringLiteral("The Glass & Stone"));
            CHECK(b.author() == QStringLiteral("Ada Byron"));
            CHECK(h.contains(QStringLiteral("<b>bold</b>")));   // inline formatting survives
            if (b.chapterFiles().size() == 3)
            {
                const QString two = readAll(b.chapterFiles().at(1));
                CHECK(two.contains(QStringLiteral("Second.")));
                CHECK(!two.contains(QStringLiteral("First")));
                CHECK(two.contains(QStringLiteral("<h1>Part Two</h1>")));
                // The <title> is metadata, never a line of body text on page one.
                CHECK(!readAll(b.chapterFiles().at(0)).contains(QStringLiteral("Glass")));
            }
        }

        // ---- (b) ONE top-level heading is the document's title: split at the next level down -------------
        {
            TextBook b;
            const QString h = stage(b, QStringLiteral("titled.html"),
                "<meta name=\"generator\" content=\"Some Word Processor\">"
                "<h1>The Whole Book</h1><p>A preface.</p>"
                "<h2>One</h2><p>alpha</p><h2>Two</h2><p>beta</p>");
            all << h;
            // The front matter (the title heading and the preface) is a chapter of its own, named by that
            // heading, and then one chapter per <h2>.
            CHECK(b.chapterFiles().size() == 3);
            CHECK(b.toc().size() == 3);
            if (b.toc().size() == 3)
            {
                CHECK(b.toc().at(0).title == QStringLiteral("The Whole Book"));
                CHECK(b.toc().at(1).title == QStringLiteral("One"));
                CHECK(b.toc().at(2).title == QStringLiteral("Two"));
            }
            if (b.chapterFiles().size() == 3)
            {
                CHECK(readAll(b.chapterFiles().at(0)).contains(QStringLiteral("A preface.")));
                CHECK(readAll(b.chapterFiles().at(1)).contains(QStringLiteral("alpha")));
                CHECK(!readAll(b.chapterFiles().at(1)).contains(QStringLiteral("beta")));
            }
            CHECK(b.title() == QStringLiteral("The Whole Book"));   // no <title>: the first heading
            CHECK(b.author().isEmpty());                              // a generator is not an author
        }

        // ---- (c) no headings at all: one chapter, no contents panel, the filename as the title -----------
        {
            TextBook b;
            const QString h = stage(b, QStringLiteral("loose leaf.htm"),
                "<html><head><title>   </title><meta name=\"Author\" content=\"  Mary Shelley \"></head>"
                "<body><p>Just prose.</p><p>More prose.</p></body></html>");
            all << h;
            CHECK(b.chapterFiles().size() == 1);
            CHECK(b.toc().isEmpty());
            CHECK(b.title() == QStringLiteral("loose leaf"));        // blank <title>, no heading: the filename
            CHECK(b.author() == QStringLiteral("Mary Shelley"));     // the name attribute is case-insensitive
            CHECK(h.contains(QStringLiteral("<p>Just prose.</p>")));
            CHECK(!h.contains(QStringLiteral("&lt;p&gt;")));         // markup is markup, not escaped text
        }

        // ---- (d) the shallowest level PRESENT is the top level, whatever its number -----------------------
        {
            TextBook b;
            all << stage(b, QStringLiteral("h2 book.html"),
                "<h2>Alpha</h2><p>x</p><h3>sub</h3><p>y</p><h2>Beta</h2><p>z</p>");
            CHECK(b.toc().size() == 2);
            if (b.toc().size() == 2)
            {
                CHECK(b.toc().at(0).title == QStringLiteral("Alpha"));
                CHECK(b.toc().at(1).title == QStringLiteral("Beta"));
            }
            CHECK(b.title() == QStringLiteral("Alpha"));
        }

        // ---- (e) "the next level down" is the next level that is THERE: one <h1>, then only <h3>s ---------
        {
            TextBook b;
            all << stage(b, QStringLiteral("gap.html"),
                "<h1>Solo</h1><h3>i</h3><p>one</p><h3>ii</h3><p>two</p>");
            CHECK(b.toc().size() == 3);
            if (b.toc().size() == 3)
            {
                CHECK(b.toc().at(0).title == QStringLiteral("Solo"));
                CHECK(b.toc().at(1).title == QStringLiteral("i"));
                CHECK(b.toc().at(2).title == QStringLiteral("ii"));
            }
        }

        // ---- (f) a single heading and nothing below it: one chapter, and <title> still wins ---------------
        {
            TextBook b;
            all << stage(b, QStringLiteral("one heading.html"),
                "<title>Named</title><h1>Only Heading</h1><p>body</p>");
            CHECK(b.chapterFiles().size() == 1);
            CHECK(b.toc().isEmpty());
            CHECK(b.title() == QStringLiteral("Named"));
        }

        // ---- (g) a split inside a wrapper keeps every chapter balanced -----------------------------------
        {
            TextBook b;
            all << stage(b, QStringLiteral("wrapped.html"),
                "<body><div id=\"book\"><h1>A</h1><p>1</p><h1>B</h1><p>2</p></div></body>");
            CHECK(b.chapterFiles().size() == 2);
            for (const QString& f : b.chapterFiles())
            {
                const QString c = readAll(f);
                CHECK(c.count(QStringLiteral("<div")) >= 1);
                CHECK(c.count(QStringLiteral("<div")) == c.count(QStringLiteral("</div>")));
            }
        }

        // ---- (h) a declared charset BEATS the ladder ------------------------------------------------------
        // The body bytes are valid UTF-8, so the ladder alone would read "café". The document says Latin-1,
        // and a document that says what it is gets believed: C3 A9 is "Ã©" in it.
        {
            TextBook b;
            const QString h = stage(b, QStringLiteral("declared.html"),
                "<meta charset=\"iso-8859-1\"><p>caf\xC3\xA9</p>");
            CHECK(h.contains(QString::fromUtf8("caf\xC3\x83\xC2\xA9")));   // "cafÃ©"
            CHECK(!h.contains(QString::fromUtf8("caf\xC3\xA9")));
        }
        // ...in the http-equiv spelling as well.
        {
            TextBook b;
            const QString h = stage(b, QStringLiteral("http-equiv.html"),
                "<meta http-equiv=\"Content-Type\" content=\"text/html; charset=windows-1252\">"
                "<p>caf\xC3\xA9</p>");
            CHECK(h.contains(QString::fromUtf8("caf\xC3\x83\xC2\xA9")));
        }
        // "iso-8859-1" MEANS windows-1252 on the web (the Encoding Standard maps the label), so its curly
        // quotes are curly quotes and not two invisible C1 controls. The same on every platform.
        {
            TextBook b;
            const QString h = stage(b, QStringLiteral("quotes.html"),
                "<meta charset=\"ISO-8859-1\"><p>\x93quoted\x94</p>");
            CHECK(h.contains(QString::fromUtf8("\xE2\x80\x9Cquoted\xE2\x80\x9D")));
        }
        // A BOM beats the declaration: the bytes' own mark outranks a claim written inside them.
        {
            TextBook b;
            const QString h = stage(b, QStringLiteral("bom.html"),
                "\xEF\xBB\xBF<meta charset=\"iso-8859-1\"><p>caf\xC3\xA9</p>");
            CHECK(h.contains(QString::fromUtf8("caf\xC3\xA9")));
            CHECK(!h.contains(QString::fromUtf8("caf\xC3\x83\xC2\xA9")));
        }
        // A declaration inside a comment is not a declaration, and a label nobody can decode is not one either:
        // both fall through to the ladder, which reads valid UTF-8 as UTF-8.
        {
            TextBook b;
            CHECK(stage(b, QStringLiteral("commented.html"),
                        "<!-- <meta charset=\"iso-8859-1\"> --><p>caf\xC3\xA9</p>")
                      .contains(QString::fromUtf8("caf\xC3\xA9")));
            TextBook c;
            CHECK(stage(c, QStringLiteral("unknown label.html"),
                        "<meta charset=\"x-no-such-codec\"><p>caf\xC3\xA9</p>")
                      .contains(QString::fromUtf8("caf\xC3\xA9")));
        }

        // ---- (p) THE PRIVACY LINE, one case per rule ------------------------------------------------------
        // (p1) A remote image is dropped - an <img> that loads from a server is a tracking pixel.
        {
            TextBook b;
            const QString h = stage(b, QStringLiteral("p1.html"),
                "<p>a</p><img src=\"https://tracker.example/pixel.gif\"><IMG SRC='http://tracker.example/p2.gif'>");
            all << h;
            CHECK(!h.contains(QStringLiteral("tracker.example")));
            CHECK(!h.contains(QStringLiteral("<img"), Qt::CaseInsensitive));
            CHECK(h.contains(QStringLiteral("<p>a</p>")));
        }
        // (p2) ...and so is a protocol-relative one, which is the same fetch with the scheme left off.
        {
            TextBook b;
            const QString h = stage(b, QStringLiteral("p2.html"), "<img src=\"//tracker.example/proto.gif\"><p>b</p>");
            all << h;
            CHECK(!h.contains(QStringLiteral("tracker.example")));
            CHECK(!h.contains(QStringLiteral("<img")));
        }
        // (p3) A UNC path is a network fetch too (SMB, and on Windows it hands over the user's credentials).
        {
            TextBook b;
            const QString h = stage(b, QStringLiteral("p3.html"),
                "<img src=\"\\\\tracker-host\\share\\x.png\"><img src=\"file://tracker-host/share/y.png\"><p>c</p>");
            all << h;
            CHECK(!h.contains(QStringLiteral("tracker-host")));
            CHECK(!h.contains(QStringLiteral("<img")));
        }
        // (p4) An absolute path, a drive letter or a file: URL is outside the book whatever it names.
        {
            TextBook b;
            const QString h = stage(b, QStringLiteral("p4.html"),
                "<img src=\"/etc/passwd.png\"><img src=\"C:/Windows/x.png\"><img src=\"c:\\x.png\">"
                "<img src=\"file:///C:/x.png\"><p>d</p>");
            all << h;
            CHECK(!h.contains(QStringLiteral("<img")));
        }
        // (p5) A relative path that climbs OUT of the book's folder is refused - the file exists, it is refused.
        {
            TextBook b;
            const QString h = stage(b, QStringLiteral("p5.html"),
                "<img src=\"../secret.png\"><img src=\"images/../../secret.png\"><img src=\"..%2Fsecret.png\"><p>e</p>");
            all << h;
            CHECK(!h.contains(QStringLiteral("secret")));
            CHECK(!h.contains(QStringLiteral("<img")));
        }
        // (p6) A sibling image, and one in a subfolder, are KEPT - resolved against the book's own folder, not
        // against wherever the reader staged the chapter.
        {
            TextBook b;
            const QString h = stage(b, QStringLiteral("p6.html"),
                "<p>f</p><img src=\"pic.png\" alt=\"a sibling\"><img src=\"images/inner.png\">"
                "<img src=\"not-there.png\">");
            all << h;
            const QStringList s = srcs(h);
            CHECK(s.size() == 2);                  // the two that exist; a dangling reference is dropped
            for (const QString& v : s) CHECK(insideBookDir(v));
            if (s.size() == 2)
            {
                CHECK(QFileInfo(QUrl(s.at(0)).toLocalFile()).fileName() == QStringLiteral("pic.png"));
                CHECK(QFileInfo(QUrl(s.at(1)).toLocalFile()).fileName() == QStringLiteral("inner.png"));
            }
            CHECK(h.contains(QStringLiteral("alt=\"a sibling\"")));
        }
        // (p7) A script is stripped WITH its body - including a '<' and a fake heading inside it, neither of
        // which may become markup or a chapter.
        {
            TextBook b;
            const QString h = stage(b, QStringLiteral("p7.html"),
                "<p>before</p><script>if (a<b) trackMe('<h1>fake</h1>');</script>"
                "<SCRIPT type=\"text/javascript\">trackMe()</SCRIPT><noscript><p>after</p></noscript>");
            all << h;
            CHECK(!h.contains(QStringLiteral("trackMe")));
            CHECK(!h.contains(QStringLiteral("<script"), Qt::CaseInsensitive));
            CHECK(!h.contains(QStringLiteral("fake")));
            CHECK(b.chapterFiles().size() == 1);
            CHECK(h.contains(QStringLiteral("before")));
            CHECK(h.contains(QStringLiteral("after")));
        }
        // (p8) Event-handler attributes are stripped, in any case, from elements that are kept.
        {
            TextBook b;
            const QString h = stage(b, QStringLiteral("p8.html"),
                "<body ONLOAD=\"trackMe()\"><p onclick=\"trackMe()\">x</p>"
                "<img src=\"pic.png\" onerror=\"trackMe()\"><a href=\"#n\" OnMouseOver=\"trackMe()\">y</a></body>");
            all << h;
            CHECK(!h.contains(QStringLiteral("trackme"), Qt::CaseInsensitive));
            for (const char* on : { "onload", "onclick", "onerror", "onmouseover" })
                CHECK(!h.contains(QString::fromLatin1(on), Qt::CaseInsensitive));
            CHECK(srcs(h).size() == 1);           // the image stays; only its handler went
            CHECK(h.contains(QStringLiteral(">x</p>")));
        }
        // (p9) Frames, objects and embeds are stripped, and so is what they would have loaded.
        {
            TextBook b;
            const QString h = stage(b, QStringLiteral("p9.html"),
                "<iframe src=\"https://frame.example/\">inner</iframe>"
                "<object data=\"https://obj.example/x.swf\"><param name=\"movie\" value=\"https://obj.example/x.swf\">"
                "<embed src=\"https://embed.example/y.swf\"></object>"
                "<embed src=\"https://embed.example/z.swf\"><frameset><frame src=\"https://frame.example/f\"></frameset>"
                "<p>kept</p>");
            all << h;
            for (const char* bad : { "frame.example", "obj.example", "embed.example", "<iframe", "<object",
                                     "<embed", "<param", "<frame" })
                CHECK(!h.contains(QString::fromLatin1(bad), Qt::CaseInsensitive));
            CHECK(h.contains(QStringLiteral("<p>kept</p>")));
        }
        // (p10) Stylesheets go, linked or inline or in an attribute: CSS is a second way to name a URL.
        {
            TextBook b;
            const QString h = stage(b, QStringLiteral("p10.html"),
                "<link rel=\"stylesheet\" href=\"https://cdn.example/s.css\">"
                "<style>@import url(https://cdn.example/i.css); p{background:url(https://tracker.example/bg.png)}</style>"
                "<p style=\"background-image:url(https://tracker.example/i.png)\">styled</p>"
                "<table background=\"https://tracker.example/t.gif\"><tr><td background=\"https://tracker.example/c.gif\">cell</td></tr></table>");
            all << h;
            for (const char* bad : { "cdn.example", "tracker.example", "<style", "<link", "style=" })
                CHECK(!h.contains(QString::fromLatin1(bad), Qt::CaseInsensitive));
            CHECK(h.contains(QStringLiteral("styled")));
            CHECK(h.contains(QStringLiteral("cell")));
        }
        // (p11) Media, responsive sources, SVG and a <base> or refresh that would redirect everything else.
        {
            TextBook b;
            const QString h = stage(b, QStringLiteral("p11.html"),
                "<base href=\"https://evil.example/\"><meta http-equiv=\"refresh\" content=\"0;url=https://evil.example/\">"
                "<video src=\"https://media.example/v.mp4\" poster=\"https://media.example/p.jpg\">"
                "<source src=\"https://media.example/v.webm\">Your reader cannot play video.</video>"
                "<audio src=\"https://media.example/a.mp3\"></audio>"
                "<picture><source srcset=\"https://tracker.example/a.webp\">"
                "<img src=\"pic.png\" srcset=\"https://tracker.example/b.png 2x\"></picture>"
                "<svg><image href=\"https://tracker.example/s.png\"/></svg><p>g</p>");
            all << h;
            for (const char* bad : { "evil.example", "media.example", "tracker.example", "srcset", "<video",
                                     "<audio", "<source", "<svg", "cannot play" })
                CHECK(!h.contains(QString::fromLatin1(bad), Qt::CaseInsensitive));
            const QStringList s = srcs(h);
            CHECK(s.size() == 1);                  // the <picture>'s own <img>, from the book's folder
            for (const QString& v : s) CHECK(insideBookDir(v));   // <base> did not re-root it
        }
        // (p12) A link keeps its text but only an in-document href: a javascript: or web href is removed.
        {
            TextBook b;
            const QString h = stage(b, QStringLiteral("p12.html"),
                "<p><a href=\"javascript:trackMe()\">js</a> <a href=\"https://site.example/\">web</a> "
                "<a href=\"#note1\">note</a></p>");
            all << h;
            CHECK(!h.contains(QStringLiteral("javascript"), Qt::CaseInsensitive));
            CHECK(!h.contains(QStringLiteral("site.example")));
            CHECK(h.contains(QStringLiteral("href=\"#note1\"")));
            CHECK(h.contains(QStringLiteral(">js<")));
            CHECK(h.contains(QStringLiteral(">web<")));
        }

        // (p13) An image carried INSIDE the file is not a fetch: it is staged beside the chapters under a name
        // this code chose, and loads from there. A data: URI that is not an image is refused like any scheme.
        {
            TextBook b;
            const QString h = stage(b, QStringLiteral("p13.html"),
                QByteArray("<p>h</p><img src=\"data:image/png;base64,") + png.toBase64()
                    + QByteArray("\"><img src=\"data:text/html;base64,PHNjcmlwdD4=\">"));
            all << h;
            const QStringList s = srcs(h);
            CHECK(s.size() == 1);
            if (s.size() == 1 && !b.chapterFiles().isEmpty())
            {
                CHECK(!s.first().contains(QLatin1Char(':')));
                CHECK(!s.first().contains(QLatin1Char('/')));
                QFile f(QFileInfo(b.chapterFiles().first()).absolutePath() + QLatin1Char('/') + s.first());
                CHECK(f.open(QIODevice::ReadOnly));
                CHECK(f.readAll() == png);
            }
        }

        // THE SWEEP: across every document above, no location-bearing attribute names a remote place.
        for (const QString& h : all)
            CHECK(!reRemote.match(h).hasMatch());
        CHECK(all.size() >= 18);

        // ---- (u) the pieces underneath, directly -----------------------------------------------------------
        {
            using Enc = TextBook::Encoding;
            Enc used = Enc::Latin1;
            // The declared rung answers and SAYS so; a BOM answers over it; no label is the plain ladder.
            CHECK(TextBook::decode(QByteArray("caf\xC3\xA9"), QByteArray("iso-8859-1"), &used)
                  == QString::fromUtf8("caf\xC3\x83\xC2\xA9"));
            CHECK(used == Enc::Declared);
            CHECK(TextBook::decode(QByteArray("\xEF\xBB\xBF" "caf\xC3\xA9"), QByteArray("iso-8859-1"), &used)
                  == QString::fromUtf8("caf\xC3\xA9"));
            CHECK(used == Enc::Utf8Bom);
            CHECK(TextBook::decode(QByteArray("caf\xC3\xA9"), QByteArray(), &used) == QString::fromUtf8("caf\xC3\xA9"));
            CHECK(used == Enc::Utf8);
            // A declared UTF-8 that is NOT UTF-8 falls through to the ladder instead of into U+FFFD.
            CHECK(!TextBook::decode(QByteArray("caf\xE9 cr\xE8me"), QByteArray("utf-8"), &used).contains(QChar(0xFFFD)));
            CHECK(used != Enc::Declared);
            // ...and so does one whose last character is cut short (a stateful decoder would hold it back and
            // report no error at all).
            TextBook::decode(QByteArray("caf\xC3"), QByteArray("utf-8"), &used);
            CHECK(used != Enc::Declared);
            // A UTF-16 label found by an ASCII scan contradicts itself and is read as UTF-8.
            CHECK(TextBook::decode(QByteArray("caf\xC3\xA9"), QByteArray("UTF-16LE"), &used)
                  == QString::fromUtf8("caf\xC3\xA9"));
            CHECK(used == Enc::Declared);
            CHECK(std::strcmp(TextBook::encodingName(Enc::Declared), TextBook::encodingName(Enc::Utf8)) != 0);

            // open() reports the rung that answered for the file it read.
            TextBook declared, bom, plain;
            CHECK(declared.open(hdir + QStringLiteral("/declared.html"), &err));
            CHECK(declared.encoding() == Enc::Declared);
            CHECK(bom.open(hdir + QStringLiteral("/bom.html"), &err));
            CHECK(bom.encoding() == Enc::Utf8Bom);
            CHECK(plain.open(hdir + QStringLiteral("/commented.html"), &err));
            CHECK(plain.encoding() == Enc::Utf8);

            // Where a declaration is found, and where it is not.
            CHECK(HtmlText::declaredCharset(QByteArray("<html><head><meta charset='Shift_JIS'>")) == "shift_jis");
            CHECK(HtmlText::declaredCharset(QByteArray(
                      "<meta http-equiv=\"Content-Type\" content=\"text/html; charset=UTF-8\">")) == "utf-8");
            CHECK(HtmlText::declaredCharset(QByteArray("<!-- <meta charset=\"koi8-r\"> --><p>x</p>")).isEmpty());
            CHECK(HtmlText::declaredCharset(QByteArray("<metadata charset=\"koi8-r\">")).isEmpty());
            CHECK(HtmlText::declaredCharset(QByteArray(5000, ' ') + QByteArray("<meta charset=\"koi8-r\">")).isEmpty());

            // The Latin-1 entity names are a table indexed by its own order: its first and last entries pin it.
            CHECK(HtmlText::decodeEntities(QStringLiteral("&nbsp;")) == QString(QChar(0xA0)));
            CHECK(HtmlText::decodeEntities(QStringLiteral("caf&eacute; &yuml; &#8212; &#x2019; &amp;amp; &bogus;"))
                  == QString::fromUtf8("caf\xC3\xA9 \xC3\xBF \xE2\x80\x94 \xE2\x80\x99 &amp; &bogus;"));

            // The split level, as a number, for each branch of the rule.
            CHECK(HtmlText::parse(QStringLiteral("<h1>A</h1><h1>B</h1>"), QString()).splitLevel == 1);
            CHECK(HtmlText::parse(QStringLiteral("<h1>T</h1><h2>A</h2><h2>B</h2>"), QString()).splitLevel == 2);
            CHECK(HtmlText::parse(QStringLiteral("<h1>T</h1><h3>A</h3><h3>B</h3>"), QString()).splitLevel == 3);
            CHECK(HtmlText::parse(QStringLiteral("<h1>T</h1><p>x</p>"), QString()).splitLevel == 0);
            CHECK(HtmlText::parse(QStringLiteral("<p>x</p>"), QString()).splitLevel == 0);
            CHECK(HtmlText::parse(QString(), QString()).chapters.size() == 1);   // never none
            // No folder, no images: a metadata pass resolves nothing, not even a sibling that exists.
            const HtmlText::Document noDir =
                HtmlText::parse(QStringLiteral("<p>x</p><img src=\"pic.png\">"), QString());
            CHECK(!noDir.chapters.first().html.contains(QStringLiteral("<img")));
            CHECK(noDir.images.isEmpty());
        }
    }

    QDir(base).removeRecursively();

    if (g_fails == 0) std::printf("EBOOKFMT-OK\n");
    else              std::printf("EBOOKFMT had %d failure(s)\n", g_fails);
    return g_fails == 0 ? 0 : 1;
}
