// Headless check of the local READING LIBRARY (issue #134, increment 1): the container reader
// (src/ebook/EpubMeta + src/core/BookMeta), the comic filename rule (src/comic/ComicName), the scan and the
// index (src/core/BookLibrary), and the browse builders over it (src/browse/BookCatalogs).
//
// THE FIXTURES ARE REAL FILES. Every .epub and .cbz below is an actual zip written with miniz and read back
// through the same code the app runs; every .pdf is an actual PDF loaded by PDFium. Nothing here fabricates
// a FileEntry by hand, for the reason probe_musiclibrary builds real tagged files: a hand-built index proves
// only that the builders agree with a hand-built index, and every bug this feature can have lives in the
// step between a file on disk and that index.
//
// What it pins, in the order #134 cares about:
//   1. THE OPF, as a pure parse over bytes: title, author, Calibre series + decimal index, EPUB 3
//      belongs-to-collection, the precedence between the two, a stated `set` refused, and the two declared
//      ways of naming a cover. Also that a package saying NOTHING parses to an empty Metadata rather than
//      to a failure.
//   2. A REAL .epub, end to end: metadata off the archive with no unpacking, and the declared cover member
//      coming back as decodable bytes.
//   3. THE COMIC FILENAME RULE, which is the risky half of this feature. Every shape in the brief, and —
//      the point of the whole design — the CONSERVATISM: a bare trailing number is only an issue number
//      when a sibling in the same folder agrees, a four-digit year is never one, and a name that says
//      nothing keeps all of itself.
//   4. THE GROUPING, MUTATION-HOSTILE IN BOTH DIRECTIONS. §3g asserts that a two-file run DOES group (so a
//      mutant that raises the corroboration threshold dies) and §3h that a lone numbered title does NOT (so
//      a mutant that lowers it dies). Both are behavioural: neither reads the constant.
//   5. A .pdf with document info and one WITHOUT: the first shows what the file says, the second falls back
//      to its filename and still appears.
//   6. AN UNTAGGED BOOK STILL APPEARS. Under its own file name, in the unknown-author bucket, which sorts
//      LAST and is never hidden.
//   7. THE SCAN'S EXTENSION GATE: a .txt, a .mobi, a .cb7 and a folder of loose .jpg files in the root are
//      not scanned at all, and no audio extension is ever claimed.
//   8. ORDERING through NaturalOrder, on both rules: a series by its index, and two untagged files by their
//      natural TITLE - "part 2" before "part 10", the case a plain QCollator gets silently wrong under the
//      C locale (#205), which is the trap that was FOUND in this app's comic page order.
//   9. THE INCREMENTAL rescan does not re-read an unchanged file, and the persisted index round-trips.
//  10. THE BROWSE builders render what the index holds: the Series door only when the dimension exists, a
//      stale key yielding an empty titled catalog rather than a crash, and a book row carrying the file url
//      plus the local-leaf mime so both surfaces open it in its reader.
//  11. THE OTHER LIBRARIES ARE UNTOUCHED — three separate claims, because "untouched" is the requirement
//      most easily asserted vacuously: three different roots, three different index files, and a scan that
//      cannot claim an audio file whatever root it is pointed at. (The strongest version of this claim is
//      structural and lives in CMakeLists: this probe links no TagLib and no MusicLibrary.)
//  12. Nothing configured / a missing root are dormant and instant, not errors.
//
// Prints BOOKS-OK on success; any failure prints BOOKS-FAIL <cond> (line) and exits non-zero.
//
// Isolation: AppPaths::dataDir() is this process's own scratch dir (issue #42), so the whole fixture library
// is written under it and goes away at exit. Nothing is written beside the exe.
#include "BookLibrary.h"
#include "BookMeta.h"
#include "BookCatalogs.h"
#include "ComicName.h"
#include "EpubMeta.h"
#include "LeafRoute.h"   // kLocalBookMime — header only; the routing itself is probe_leafroute's job
#include "AppPaths.h"
#include "Settings.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QString>
#include <QVector>
#include <cstdio>
#include <cstring>

#include "miniz.h"

static int g_fails = 0;

#define CHECK(cond)                                                                     \
    do {                                                                                \
        if (!(cond)) { std::printf("BOOKS-FAIL %s (%d)\n", #cond, __LINE__); ++g_fails; } \
    } while (0)

using BookLibrary::Book;
using BookLibrary::Index;
using BookLibrary::Kind;
using BookLibrary::ScanStats;

// ---------------------------------------------------------------------------------------------------------
// Fixture builders — real archives, real PDFs.
// ---------------------------------------------------------------------------------------------------------

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

// A real, decodable PNG of a flat colour — small enough to be free, real enough that QImage::loadFromData
// on the far side is a genuine decode rather than a no-op.
static QByteArray pngBytes(const QColor& c, int w = 40, int h = 60)
{
    QImage img(w, h, QImage::Format_RGB32);
    img.fill(c);
    QByteArray out;
    QBuffer buf(&out);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    return out;
}

// One .epub: mimetype, container, an OPF the caller supplies, one chapter and (optionally) a cover image at
// "OEBPS/cover.png".
static bool writeEpub(const QString& path, const QByteArray& opf, bool withCover)
{
    QVector<QPair<QString, QByteArray>> m;
    m.append({ QStringLiteral("mimetype"), QByteArrayLiteral("application/epub+zip") });
    m.append({ QStringLiteral("META-INF/container.xml"),
               QByteArrayLiteral("<?xml version=\"1.0\"?>\n"
                                 "<container version=\"1.0\" "
                                 "xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">"
                                 "<rootfiles><rootfile full-path=\"OEBPS/content.opf\" "
                                 "media-type=\"application/oebps-package+xml\"/></rootfiles></container>") });
    m.append({ QStringLiteral("OEBPS/content.opf"), opf });
    m.append({ QStringLiteral("OEBPS/ch1.xhtml"),
               QByteArrayLiteral("<html><body><p>Chapter one.</p></body></html>") });
    if (withCover) m.append({ QStringLiteral("OEBPS/cover.png"), pngBytes(QColor(200, 40, 40)) });
    return writeZip(path, m);
}

// The OPF of a book with everything: title, author, language, date, a Calibre series and a declared cover.
static QByteArray fullOpf(const char* title, const char* author, const char* series, const char* index)
{
    return QByteArrayLiteral("<?xml version=\"1.0\"?>\n<package xmlns=\"http://www.idpf.org/2007/opf\" "
                             "xmlns:opf=\"http://www.idpf.org/2007/opf\" "
                             "version=\"2.0\"><metadata "
                             "xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
                             "<dc:title>") + title + QByteArrayLiteral("</dc:title>"
                             "<dc:creator opf:role=\"aut\">") + author + QByteArrayLiteral("</dc:creator>"
                             "<dc:language>en</dc:language><dc:date>2011-03-04</dc:date>"
                             "<meta name=\"calibre:series\" content=\"") + series + QByteArrayLiteral("\"/>"
                             "<meta name=\"calibre:series_index\" content=\"") + index
         + QByteArrayLiteral("\"/><meta name=\"cover\" content=\"cover-img\"/></metadata>"
                             "<manifest><item id=\"cover-img\" href=\"cover.png\" media-type=\"image/png\"/>"
                             "<item id=\"c1\" href=\"ch1.xhtml\" media-type=\"application/xhtml+xml\"/>"
                             "</manifest><spine><itemref idref=\"c1\"/></spine></package>");
}

// A package that says nothing at all: the case a scanner must still put on the shelf.
static QByteArray bareOpf()
{
    return QByteArrayLiteral("<?xml version=\"1.0\"?>\n<package xmlns=\"http://www.idpf.org/2007/opf\" "
                             "version=\"2.0\"><metadata/>"
                             "<manifest><item id=\"c1\" href=\"ch1.xhtml\" "
                             "media-type=\"application/xhtml+xml\"/></manifest>"
                             "<spine><itemref idref=\"c1\"/></spine></package>");
}

// One .cbz of `pages` flat-colour PNGs, named page1..pageN so the natural order is exercised rather than
// merely satisfied by a zero-padded one.
static bool writeCbz(const QString& path, int pages)
{
    QVector<QPair<QString, QByteArray>> m;
    for (int i = 1; i <= pages; ++i)
        m.append({ QStringLiteral("page%1.png").arg(i), pngBytes(QColor(20 * i % 255, 90, 160)) });
    return writeZip(path, m);
}

// ---- A minimal PDF, written by hand so the information dictionary is under this probe's control ---------
// QPdfWriter can produce a PDF but has no way to set /Author, and the whole point of §5 is what happens
// when a document dictionary DOES and does NOT carry one. This is the smallest complete PDF: catalog, page
// tree, one page, one content stream, one font and (optionally) an Info dict, with a real cross-reference
// table — PDFium will not open one with wrong offsets, so the offsets are recorded as the objects are
// emitted rather than guessed.
static QByteArray pdfString(const QString& s)
{
    QByteArray b = s.toLatin1();
    b.replace('\\', "\\\\").replace('(', "\\(").replace(')', "\\)");
    return b;
}

static bool writeMinimalPdf(const QString& path, const QString& title, const QString& author)
{
    const bool withInfo = !title.isEmpty() || !author.isEmpty();
    QVector<QByteArray> objs;
    objs.append(QByteArrayLiteral("<< /Type /Catalog /Pages 2 0 R >>"));
    objs.append(QByteArrayLiteral("<< /Type /Pages /Kids [3 0 R] /Count 1 >>"));
    objs.append(QByteArrayLiteral("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
                                  "/Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >> >>"));
    const QByteArray stream = QByteArrayLiteral("BT /F1 24 Tf 72 700 Td (EverythingBox) Tj ET");
    objs.append(QByteArrayLiteral("<< /Length ") + QByteArray::number(stream.size())
                + QByteArrayLiteral(" >>\nstream\n") + stream + QByteArrayLiteral("\nendstream"));
    objs.append(QByteArrayLiteral("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"));
    if (withInfo)
    {
        QByteArray info = QByteArrayLiteral("<<");
        if (!title.isEmpty())  info += QByteArrayLiteral(" /Title (") + pdfString(title) + ')';
        if (!author.isEmpty()) info += QByteArrayLiteral(" /Author (") + pdfString(author) + ')';
        info += QByteArrayLiteral(" >>");
        objs.append(info);
    }

    QByteArray out = QByteArrayLiteral("%PDF-1.4\n");
    QVector<int> offsets;
    for (int i = 0; i < objs.size(); ++i)
    {
        offsets.append(out.size());
        out += QByteArray::number(i + 1) + QByteArrayLiteral(" 0 obj\n") + objs.at(i)
             + QByteArrayLiteral("\nendobj\n");
    }
    const int xrefAt = out.size();
    out += QByteArrayLiteral("xref\n0 ") + QByteArray::number(objs.size() + 1) + '\n';
    out += QByteArrayLiteral("0000000000 65535 f \n");
    for (int off : offsets)
    {
        QByteArray n = QByteArray::number(off);
        while (n.size() < 10) n.prepend('0');
        out += n + QByteArrayLiteral(" 00000 n \n");
    }
    out += QByteArrayLiteral("trailer\n<< /Size ") + QByteArray::number(objs.size() + 1)
         + QByteArrayLiteral(" /Root 1 0 R");
    if (withInfo) out += QByteArrayLiteral(" /Info ") + QByteArray::number(objs.size()) + QByteArrayLiteral(" 0 R");
    out += QByteArrayLiteral(" >>\nstartxref\n") + QByteArray::number(xrefAt) + QByteArrayLiteral("\n%%EOF\n");

    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    return f.write(out) == out.size();
}

// ---------------------------------------------------------------------------------------------------------

static const Book* findBook(const Index& idx, const QString& titleWanted)
{
    for (const BookLibrary::Author& a : idx.authors)
        for (const Book& b : a.books)
            if (b.title == titleWanted) return &b;
    return nullptr;
}

int main(int argc, char** argv)
{
    // A PLATFORM OF OUR OWN, BEFORE QGuiApplication EXISTS. QtPdf's renderer needs a QGuiApplication, and a
    // QGuiApplication with no platform aborts (SIGABRT, rc 134) the moment it cannot open a display. The
    // suite's runner loop launches every probe bare, with no -platform argument and no guarantee about the
    // environment: a developer who exports QT_QPA_PLATFORM=offscreen sees this pass and a CI runner with a
    // DISPLAY-less container sees it die before main's first assertion, which is the difference between a
    // green local run and a red one for reasons that have nothing to do with the code under test. It cost
    // this increment one red CI run to learn. Set only when unset, so `-platform` and a deliberate override
    // still win (the probe_shaderassets rule).
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    const QString base = AppPaths::dataDir() + QStringLiteral("/probe_books");
    QDir(base).removeRecursively();
    QDir().mkpath(base);

    // ---- §1 The OPF, as a pure parse ------------------------------------------------------------------
    {
        using EpubMeta::Metadata;
        const Metadata m = EpubMeta::parseOpfMetadata(fullOpf("Foundation", "Isaac Asimov",
                                                              "Foundation", "2.5"));
        CHECK(m.title == QStringLiteral("Foundation"));
        CHECK(m.author == QStringLiteral("Isaac Asimov"));
        CHECK(m.series == QStringLiteral("Foundation"));
        CHECK(m.seriesIndex == 2.5);          // DECIMAL: truncating collides a novella with book 2
        CHECK(m.language == QStringLiteral("en"));
        CHECK(m.year == 2011);
        CHECK(m.spineCount == 1);
        CHECK(m.coverHref == QStringLiteral("cover.png"));   // named through <meta name="cover">
        CHECK(!m.isEmpty());

        // A package that says nothing is a SUCCESS with an empty Metadata, not a failure. The difference
        // matters: a failure would drop the book, and #134 requires it on the shelf under its filename.
        const Metadata bare = EpubMeta::parseOpfMetadata(bareOpf());
        CHECK(bare.isEmpty());
        CHECK(bare.title.isEmpty() && bare.author.isEmpty() && bare.series.isEmpty());
        CHECK(bare.seriesIndex == 0.0);
        CHECK(bare.coverHref.isEmpty());   // NEVER guessed from a member that happens to be called cover.*

        // EPUB 3's own spelling, with the position on a refines meta.
        const QByteArray e3 = QByteArrayLiteral(
            "<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\"><metadata "
            "xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:title>Ancillary Sword</dc:title>"
            "<dc:creator>Ann Leckie</dc:creator>"
            "<meta property=\"belongs-to-collection\" id=\"c1\">Imperial Radch</meta>"
            "<meta refines=\"#c1\" property=\"collection-type\">series</meta>"
            "<meta refines=\"#c1\" property=\"group-position\">2</meta></metadata>"
            "<manifest><item id=\"cv\" href=\"img/c.jpg\" properties=\"svg cover-image\" "
            "media-type=\"image/jpeg\"/></manifest><spine><itemref idref=\"x\"/>"
            "<itemref idref=\"y\"/></spine></package>");
        const Metadata m3 = EpubMeta::parseOpfMetadata(e3);
        CHECK(m3.series == QStringLiteral("Imperial Radch"));
        CHECK(m3.seriesIndex == 2.0);
        CHECK(m3.spineCount == 2);
        CHECK(m3.coverHref == QStringLiteral("img/c.jpg"));   // the EPUB 3 manifest PROPERTY

        // A stated `set` is a boxed bundle, not the axis a shelf orders by: skipped rather than filed as a
        // series the owner never named.
        QByteArray asSet = e3;
        asSet.replace("property=\"collection-type\">series<", "property=\"collection-type\">set<");
        CHECK(EpubMeta::parseOpfMetadata(asSet).series.isEmpty());

        // Calibre wins where a file carries both: a Calibre-managed file is one whose owner curates that
        // field.
        QByteArray both = e3;
        both.replace("<dc:creator>Ann Leckie</dc:creator>",
                     "<dc:creator>Ann Leckie</dc:creator>"
                     "<meta name=\"calibre:series\" content=\"Radch\"/>"
                     "<meta name=\"calibre:series_index\" content=\"3\"/>");
        const Metadata mb = EpubMeta::parseOpfMetadata(both);
        CHECK(mb.series == QStringLiteral("Radch"));
        CHECK(mb.seriesIndex == 3.0);

        // AN UNDECLARED PREFIX DOES NOT STOP THE READ. `opf:role` with no xmlns:opf on the package is a real
        // thing real exporters emit; with XML namespace processing left on, QXmlStreamReader treats it as
        // FATAL and everything after it is silently missing — the title survives (it is read first) and the
        // author, series and cover do not. This fixture is that file, and it is the case that turned up
        // first when this probe was written. See EpubMeta.cpp.
        QByteArray undeclared = fullOpf("Dune", "Frank Herbert", "Dune", "1");
        undeclared.replace(" xmlns:opf=\"http://www.idpf.org/2007/opf\"", "");
        const Metadata mu = EpubMeta::parseOpfMetadata(undeclared);
        CHECK(mu.title == QStringLiteral("Dune"));
        CHECK(mu.author == QStringLiteral("Frank Herbert"));
        CHECK(mu.series == QStringLiteral("Dune"));
        CHECK(mu.coverHref == QStringLiteral("cover.png"));

        // Not a container / not a package: empty answers, never a crash.
        CHECK(EpubMeta::opfPathFromContainer(QByteArray()).isEmpty());
        CHECK(EpubMeta::opfPathFromContainer(QByteArrayLiteral("<nonsense/>")).isEmpty());
        CHECK(EpubMeta::parseOpfMetadata(QByteArrayLiteral("not xml at all")).isEmpty());
    }

    // ---- §2 A real .epub, end to end ------------------------------------------------------------------
    const QString epubFull = base + QStringLiteral("/lib/Asimov - Foundation.epub");
    const QString epubBare = base + QStringLiteral("/lib/an untitled manuscript.epub");
    {
        CHECK(writeEpub(epubFull, fullOpf("Foundation", "Isaac Asimov", "Foundation", "2"), true));
        CHECK(writeEpub(epubBare, bareOpf(), false));

        EpubMeta::Metadata m;
        QByteArray cover;
        CHECK(EpubMeta::readEpubFile(epubFull, &m, &cover));
        CHECK(m.title == QStringLiteral("Foundation"));
        CHECK(m.author == QStringLiteral("Isaac Asimov"));
        CHECK(!cover.isEmpty());
        QImage decoded;
        CHECK(decoded.loadFromData(cover));          // a REAL image, not just some bytes
        CHECK(decoded.width() == 40 && decoded.height() == 60);

        // ...and NOTHING was unpacked. The reader's temp-extraction path is what this deliberately avoids.
        CHECK(!QFile::exists(base + QStringLiteral("/lib/OEBPS")));

        // BookMeta is the same read, through the dispatch a scan uses.
        const BookMeta::Info info = BookMeta::read(epubFull);
        CHECK(info.title == QStringLiteral("Foundation"));
        CHECK(info.hasCover);
        CHECK(info.pageCount == 1);
        CHECK(BookMeta::read(epubBare).isEmpty());
        CHECK(!BookMeta::read(epubBare).hasCover);
        CHECK(BookMeta::coverBytes(epubBare).isEmpty());

        // A file that is not a zip at all yields an empty Info rather than a failure the scan would have to
        // handle.
        const QString junk = base + QStringLiteral("/lib/broken.epub");
        QFile jf(junk);
        CHECK(jf.open(QIODevice::WriteOnly));
        jf.write("this is not an epub");
        jf.close();
        CHECK(BookMeta::read(junk).isEmpty());
        CHECK(BookMeta::coverBytes(junk).isEmpty());
        CHECK(QFile::remove(junk));
    }

    // ---- §3 The comic filename rule -------------------------------------------------------------------
    {
        using ComicName::Evidence;

        // (a) A VOLUME marker is believed on its own.
        const ComicName::Parsed vol = ComicName::parse(QStringLiteral("Saga Vol. 3"));
        CHECK(vol.evidence == Evidence::Marked);
        CHECK(vol.series == QStringLiteral("Saga"));
        CHECK(vol.number == 3.0);

        // ...including the v01 spelling, and the title after it.
        const ComicName::Parsed v01 = ComicName::parse(QStringLiteral("Y The Last Man v01 - Unmanned"));
        CHECK(v01.evidence == Evidence::Marked);
        CHECK(v01.series == QStringLiteral("Y The Last Man"));
        CHECK(v01.number == 1.0);
        CHECK(v01.title == QStringLiteral("Unmanned"));

        // ...but NOT a lone "v" with a space, which is how a versus is spelled. Reading "Batman v Superman
        // 2" as volume 2 of "Batman" would file a film tie-in under the wrong series with a marker's
        // confidence.
        const ComicName::Parsed versus = ComicName::parse(QStringLiteral("Batman v Superman 2"));
        CHECK(versus.evidence == Evidence::Bare);
        CHECK(versus.series == QStringLiteral("Batman v Superman"));

        // ...and never a volume word buried inside another one.
        CHECK(ComicName::parse(QStringLiteral("Evolution 5")).series == QStringLiteral("Evolution"));
        CHECK(ComicName::parse(QStringLiteral("Evolution 5")).evidence == Evidence::Bare);

        // (b) A '#' is the least ambiguous marker there is.
        const ComicName::Parsed hash = ComicName::parse(QStringLiteral("Saga #12 - Some Title"));
        CHECK(hash.evidence == Evidence::Marked);
        CHECK(hash.series == QStringLiteral("Saga"));
        CHECK(hash.number == 12.0);
        CHECK(hash.title == QStringLiteral("Some Title"));

        // (c) A number standing alone as its own dash-delimited FIELD.
        const ComicName::Parsed field = ComicName::parse(QStringLiteral("Series - 012 - Title"));
        CHECK(field.evidence == Evidence::Marked);
        CHECK(field.series == QStringLiteral("Series"));
        CHECK(field.number == 12.0);
        CHECK(field.title == QStringLiteral("Title"));

        // (d) A BARE trailing number is graded, never believed on its own.
        const ComicName::Parsed bare = ComicName::parse(QStringLiteral("Series 012"));
        CHECK(bare.evidence == Evidence::Bare);
        CHECK(bare.series == QStringLiteral("Series"));
        CHECK(bare.number == 12.0);

        // (e) Trailing release furniture comes off; the name keeps everything else.
        const ComicName::Parsed noisy =
            ComicName::parse(QStringLiteral("Saga_012 (2013) (Digital) (Zone-Empire)"));
        CHECK(noisy.cleaned == QStringLiteral("Saga 012"));
        CHECK(noisy.series == QStringLiteral("Saga"));
        // ...but INTERIOR brackets are part of the title: stripping them would merge two different Batmen.
        CHECK(ComicName::parse(QStringLiteral("Batman (Earth-Two) and Friends")).cleaned
              == QStringLiteral("Batman (Earth-Two) and Friends"));
        // ...and a name that is ONLY a bracket group keeps itself rather than becoming empty.
        CHECK(ComicName::parse(QStringLiteral("(2013)")).cleaned == QStringLiteral("(2013)"));

        // (f) THE YEAR REFUSAL. A four-digit number in the publishing window is a year, and a year is not an
        // issue number — the whole name stays the title.
        const ComicName::Parsed year = ComicName::parse(QStringLiteral("Watchmen 1986"));
        CHECK(year.evidence == Evidence::None);
        CHECK(year.series.isEmpty());
        CHECK(year.cleaned == QStringLiteral("Watchmen 1986"));
        // ...while a four-digit number OUTSIDE it is read normally: 2000 AD really does run past #1500.
        CHECK(ComicName::parse(QStringLiteral("2000 AD 1500")).number == 1500.0);
        // A name with no number at all says so.
        CHECK(ComicName::parse(QStringLiteral("Batman - The Killing Joke")).evidence == Evidence::None);
        // A series name has to contain a letter, or a folder of "01 - 02.cbz" mints the series "01".
        CHECK(ComicName::parse(QStringLiteral("01 - 02")).evidence == Evidence::None);

        // (g) CORROBORATION, the grouping direction. Two files that agree ARE a series — this is the
        // assertion a mutant that RAISES the threshold (or drops the bare branch entirely) fails.
        const QVector<ComicName::Grouped> run =
            ComicName::group({ QStringLiteral("Saga 001"), QStringLiteral("Saga 002") });
        CHECK(run.size() == 2);
        CHECK(run.at(0).series == QStringLiteral("Saga"));
        CHECK(run.at(1).series == QStringLiteral("Saga"));
        CHECK(run.at(0).number == 1.0 && run.at(1).number == 2.0);
        // ...and a MARKED file needs no corroboration at all: this is what a mutant that drops the marked
        // branch fails.
        const QVector<ComicName::Grouped> lonelyVol = ComicName::group({ QStringLiteral("Saga Vol. 3") });
        CHECK(lonelyVol.at(0).series == QStringLiteral("Saga"));
        CHECK(lonelyVol.at(0).number == 3.0);

        // (h) CORROBORATION, the refusal direction. A lone numbered title is NOT a series — this is the
        // assertion a mutant that LOWERS the threshold fails, and the reason *Fahrenheit 451* does not
        // become the series "Fahrenheit".
        const QVector<ComicName::Grouped> lone = ComicName::group({ QStringLiteral("Fahrenheit 451"),
                                                                    QStringLiteral("Watchmen"),
                                                                    QStringLiteral("Akira 1") });
        CHECK(lone.at(0).series.isEmpty());
        CHECK(lone.at(0).number == 0.0);
        CHECK(lone.at(0).title == QStringLiteral("Fahrenheit 451"));   // the number is NOT dropped
        CHECK(lone.at(1).title == QStringLiteral("Watchmen"));
        CHECK(lone.at(2).series.isEmpty());                            // one Akira is not a run either

        // ...and a MARKED sibling does not vouch for a BARE one. "Saga Vol. 1" beside "Saga 451" must not
        // be enough to turn 451 into an issue, which is exactly one corroborator too few.
        const QVector<ComicName::Grouped> mixed = ComicName::group({ QStringLiteral("Saga Vol. 1"),
                                                                     QStringLiteral("Saga 451") });
        CHECK(mixed.at(0).series == QStringLiteral("Saga"));
        CHECK(mixed.at(1).series.isEmpty());

        // A grouped file's title is never empty, whatever shape it took.
        for (const ComicName::Grouped& g : run) CHECK(!g.title.isEmpty());
        for (const ComicName::Grouped& g : lone) CHECK(!g.title.isEmpty());

        // Only case and whitespace fold into a key. Folding punctuation would merge "Spider-Man" with
        // "Spider Man", and merging is the expensive failure.
        CHECK(ComicName::seriesKey(QStringLiteral("  Saga  ")) == ComicName::seriesKey(QStringLiteral("SAGA")));
        CHECK(ComicName::seriesKey(QStringLiteral("Spider-Man"))
              != ComicName::seriesKey(QStringLiteral("Spider Man")));
    }

    // ---- §5 PDFs, with and without a document dictionary ----------------------------------------------
    const QString pdfTagged = base + QStringLiteral("/lib/tagged.pdf");
    const QString pdfBare   = base + QStringLiteral("/lib/A Scanned Thing.pdf");
    {
        CHECK(writeMinimalPdf(pdfTagged, QStringLiteral("The Annotated Turing"),
                              QStringLiteral("Charles Petzold")));
        CHECK(writeMinimalPdf(pdfBare, QString(), QString()));

        const BookMeta::Info t = BookMeta::read(pdfTagged);
        CHECK(t.title == QStringLiteral("The Annotated Turing"));
        CHECK(t.author == QStringLiteral("Charles Petzold"));
        CHECK(t.pageCount == 1);
        CHECK(t.series.isEmpty());        // a PDF has no series field and none is invented
        CHECK(t.hasCover);
        // THE COVER HAS TO BE A PICTURE OF THE PAGE, not merely bytes that decode. PDFium paints only what
        // the page draws, so a text page comes back as black glyphs on a TRANSPARENT background - and the
        // cache writer re-encodes to JPEG, which has no alpha, turning every transparent pixel black. The
        // result decoded fine and was a solid black rectangle; a shelf of PDFs was a wall of them. Found on
        // the live drive, because "loadFromData succeeded" was the whole of what this used to assert.
        //
        // So: a page is mostly LIGHT (it is white paper) and has SOME dark pixels on it (that is the text).
        // The two halves together are what discriminate - light-only would pass a render that drew nothing,
        // and dark-only is the bug itself.
        QImage pdfCover;
        CHECK(pdfCover.loadFromData(BookMeta::coverBytes(pdfTagged)));
        long light = 0, dark = 0;
        for (int y = 0; y < pdfCover.height(); ++y)
            for (int x = 0; x < pdfCover.width(); ++x)
                (qGray(pdfCover.pixel(x, y)) > 200 ? light : dark) += 1;
        CHECK(light > dark * 4);   // overwhelmingly page, not ink
        CHECK(dark > 20);          // ...but the ink is actually there

        const BookMeta::Info b = BookMeta::read(pdfBare);
        CHECK(b.isEmpty());               // nothing said — the filename fallback is buildIndex's job
        CHECK(b.pageCount == 1);          // ...but the file is still perfectly readable
    }

    // ---- §7 The extension gate ------------------------------------------------------------------------
    {
        CHECK(BookLibrary::isReadingFile(QStringLiteral("/x/a.epub")));
        CHECK(BookLibrary::isReadingFile(QStringLiteral("/x/a.PDF")));
        CHECK(BookLibrary::isReadingFile(QStringLiteral("/x/a.cbz")));
        // Deliberate refusals, each with a cost behind it (BookLibrary.h).
        CHECK(!BookLibrary::isReadingFile(QStringLiteral("/x/a.mobi")));
        CHECK(!BookLibrary::isReadingFile(QStringLiteral("/x/a.cb7")));
        CHECK(!BookLibrary::isReadingFile(QStringLiteral("/x/a.cbt")));
        CHECK(!BookLibrary::isReadingFile(QStringLiteral("/x/a.zip")));
        CHECK(!BookLibrary::isReadingFile(QStringLiteral("/x/notes.txt")));
        // §11: no audio extension is ever claimed, whatever this root is pointed at.
        CHECK(!BookLibrary::isReadingFile(QStringLiteral("/x/track.mp3")));
        CHECK(!BookLibrary::isReadingFile(QStringLiteral("/x/book.m4b")));
        CHECK(!BookLibrary::isReadingFile(QStringLiteral("/x/rip.flac")));
        CHECK(BookLibrary::kindFor(QStringLiteral("/x/a.cbz")) == Kind::Comic);
        CHECK(BookLibrary::kindFor(QStringLiteral("/x/a.epub")) == Kind::Book);
        CHECK(BookLibrary::kindFor(QStringLiteral("/x/a.pdf")) == Kind::Book);
    }

    // ---- The library on disk --------------------------------------------------------------------------
    const QString lib = base + QStringLiteral("/lib");
    {
        // A run of comics that DOES corroborate, in its own folder.
        for (int i = 1; i <= 3; ++i)
            CHECK(writeCbz(lib + QStringLiteral("/comics/Saga %1.cbz").arg(i, 3, 10, QLatin1Char('0')), 2));
        // ...and one lone numbered title, in the SAME folder, which must NOT join it.
        CHECK(writeCbz(lib + QStringLiteral("/comics/Fahrenheit 451.cbz"), 4));
        // A marked one, alone in its own folder.
        CHECK(writeCbz(lib + QStringLiteral("/vols/Bone Vol. 2.cbz"), 2));
        CHECK(writeCbz(lib + QStringLiteral("/vols/Bone Vol. 10.cbz"), 2));

        // §8: two untagged PDFs whose names differ only by an UNPADDED number. Nothing about them carries a
        // series or an index, so the ONLY thing that can order them is the natural-title fallback - which is
        // exactly the case a plain QCollator gets silently wrong under the C locale (#205).
        CHECK(writeMinimalPdf(lib + QStringLiteral("/notes/part 2.pdf"), QString(), QString()));
        CHECK(writeMinimalPdf(lib + QStringLiteral("/notes/part 10.pdf"), QString(), QString()));

        // §7: things the scan must not pick up.
        QDir().mkpath(lib + QStringLiteral("/loose-pages"));
        for (int i = 1; i <= 3; ++i)
        {
            QFile f(lib + QStringLiteral("/loose-pages/scan%1.jpg").arg(i));
            CHECK(f.open(QIODevice::WriteOnly));
            f.write(pngBytes(QColor(10, 10, 10)));   // a loose folder of images is not a comic
        }
        for (const char* junk : { "/notes.txt", "/old.mobi", "/packed.cb7" })
        {
            QFile f(lib + QString::fromLatin1(junk));
            CHECK(f.open(QIODevice::WriteOnly));
            f.write("x");
        }
    }

    ScanStats s1;
    const QVector<BookLibrary::FileEntry> entries = BookLibrary::scanFolder(lib, {}, &s1);
    const Index idx = BookLibrary::buildIndex(entries);
    {
        // 2 epubs + 4 pdfs + 6 cbz = 12. The .txt/.mobi/.cb7 and the three loose .jpgs are not files this
        // library has ever heard of.
        CHECK(s1.files == 12);
        CHECK(s1.reread == 12);
        CHECK(s1.reused == 0);
        CHECK(entries.size() == 12);
        CHECK(idx.bookCount == 6);
        CHECK(idx.comicCount == 6);
        for (const BookLibrary::FileEntry& e : entries)
            CHECK(!e.path.endsWith(QStringLiteral(".jpg")) && !e.path.endsWith(QStringLiteral(".txt"))
                  && !e.path.endsWith(QStringLiteral(".mobi")) && !e.path.endsWith(QStringLiteral(".cb7")));
    }

    // ---- §2/§5/§6 What the index made of them ---------------------------------------------------------
    {
        const Book* foundation = findBook(idx, QStringLiteral("Foundation"));
        CHECK(foundation != nullptr);
        if (foundation)
        {
            CHECK(foundation->author == QStringLiteral("Isaac Asimov"));
            CHECK(foundation->series == QStringLiteral("Foundation"));
            CHECK(foundation->seriesIndex == 2.0);
            CHECK(foundation->kind == Kind::Book);
            CHECK(!foundation->titleFromFilename);
            CHECK(foundation->path == epubFull);
            CHECK(foundation->key == BookLibrary::bookKeyFor(epubFull));
        }

        // §6: THE UNTAGGED BOOK IS ON THE SHELF, under its own file name. This is the assertion that a
        // scanner which silently omitted its untagged files would fail.
        const Book* untitled = findBook(idx, QStringLiteral("an untitled manuscript"));
        CHECK(untitled != nullptr);
        if (untitled)
        {
            CHECK(untitled->titleFromFilename);
            CHECK(untitled->author.isEmpty());
            CHECK(untitled->series.isEmpty());
        }
        CHECK(findBook(idx, QStringLiteral("The Annotated Turing")) != nullptr);
        CHECK(findBook(idx, QStringLiteral("A Scanned Thing")) != nullptr);   // the bare PDF, by its name

        // The UNKNOWN-author bucket exists, holds the untagged files, and sorts LAST.
        CHECK(!idx.authors.isEmpty());
        CHECK(idx.authors.last().name.isEmpty());
        CHECK(idx.authors.last().books.size() >= 8);
        CHECK(BookLibrary::displayAuthor(idx.authors.last()) == QStringLiteral("Unknown Author"));
        CHECK(!BookLibrary::displayAuthor(idx.authors.first()).isEmpty());
        for (const BookLibrary::Author& a : idx.authors)
            for (const Book& b : a.books) CHECK(!b.title.isEmpty());   // NEVER a blank row
    }

    // ---- §3/§4/§8 The comic grouping, as the index applied it -----------------------------------------
    {
        const BookLibrary::Series* saga = idx.seriesFor(ComicName::seriesKey(QStringLiteral("Saga")));
        CHECK(saga != nullptr);
        if (saga)
        {
            CHECK(saga->books.size() == 3);          // the run, and NOT Fahrenheit 451 beside it
            CHECK(saga->books.at(0).seriesIndex == 1.0);
            CHECK(saga->books.at(2).seriesIndex == 3.0);
            for (const Book& b : saga->books) CHECK(b.kind == Kind::Comic);
        }
        // The lone numbered title is a book of its own, keeping its whole name, in no series at all.
        const Book* f451 = findBook(idx, QStringLiteral("Fahrenheit 451"));
        CHECK(f451 != nullptr);
        if (f451) CHECK(f451->series.isEmpty());
        CHECK(idx.seriesFor(ComicName::seriesKey(QStringLiteral("Fahrenheit"))) == nullptr);

        // §8: NATURAL ORDER, twice over. A series orders by its INDEX, so "Bone Vol. 2" leads "Bone Vol. 10"
        // on the number...
        const BookLibrary::Series* bone = idx.seriesFor(ComicName::seriesKey(QStringLiteral("Bone")));
        CHECK(bone != nullptr);
        if (bone)
        {
            CHECK(bone->books.size() == 2);
            CHECK(bone->books.at(0).seriesIndex == 2.0);
            CHECK(bone->books.at(1).seriesIndex == 10.0);
        }

        // ...and two books with NO series and NO index fall through to the natural TITLE, which is the rule
        // a plain QCollator gets silently wrong under the C locale: "part 10" sorts before "part 2" there,
        // on every machine with LANG/LC_ALL unset and on every CI runner, with nothing anywhere saying so
        // (#205, NaturalOrder.h — the trap that was FOUND in this app's comic page order).
        int at2 = -1, at10 = -1;
        const QVector<Book>& unknown = idx.authors.last().books;
        for (int i = 0; i < unknown.size(); ++i)
        {
            if (unknown.at(i).title == QStringLiteral("part 2"))  at2 = i;
            if (unknown.at(i).title == QStringLiteral("part 10")) at10 = i;
        }
        CHECK(at2 >= 0 && at10 >= 0);
        CHECK(at2 < at10);
    }

    // ---- §9 Incremental rescan + persistence ----------------------------------------------------------
    const QString indexFile = base + QStringLiteral("/bookindex.json");
    {
        CHECK(BookLibrary::saveIndexFile(indexFile, entries));
        QString rules;
        const QVector<BookLibrary::FileEntry> loaded = BookLibrary::loadIndexFile(indexFile, &rules);
        CHECK(loaded.size() == entries.size());
        CHECK(rules == BookLibrary::parseStamp());
        CHECK(!rules.isEmpty());

        ScanStats s2;
        const QVector<BookLibrary::FileEntry> again =
            BookLibrary::scanFolder(lib, BookLibrary::byPath(loaded), &s2);
        CHECK(s2.files == 12);
        CHECK(s2.reused == 12);     // NOT ONE FILE RE-OPENED: the whole point of the cache
        CHECK(s2.reread == 0);
        CHECK(s2.dropped == 0);
        // ...and the index built from the round-tripped entries is the same one.
        const Index idx2 = BookLibrary::buildIndex(again);
        CHECK(idx2.bookCount == idx.bookCount && idx2.comicCount == idx.comicCount);
        const Book* f = findBook(idx2, QStringLiteral("Foundation"));
        CHECK(f && f->series == QStringLiteral("Foundation") && f->seriesIndex == 2.0);

        // A dropped file is counted and gone.
        QVector<BookLibrary::FileEntry> plusGhost = loaded;
        BookLibrary::FileEntry ghost;
        ghost.path = lib + QStringLiteral("/gone.epub");
        ghost.mtime = 1; ghost.size = 1;
        plusGhost.append(ghost);
        ScanStats s3;
        BookLibrary::scanFolder(lib, BookLibrary::byPath(plusGhost), &s3);
        CHECK(s3.dropped == 1);
    }

    // ---- §10 The browse builders ----------------------------------------------------------------------
    {
        browse::BookEmptyNote none;
        const MediaCatalog root = browse::bookRootCatalog(idx, none);
        CHECK(!root.items.isEmpty());
        // The Series door leads, then one row per author.
        CHECK(root.items.first().type == QString::fromLatin1(browse::kBookSeriesListType));
        CHECK(root.items.first().mime == QString::fromLatin1(browse::kBookSeriesListPrefix));
        CHECK(root.items.at(1).type == QString::fromLatin1(browse::kBookAuthorType));
        CHECK(browse::bookKeyOf(root.items.at(1).mime, browse::kBookAuthorPrefix)
              == idx.authors.first().key);
        // Every synthetic row is expandable and carries its key in BOTH id and mime.
        for (const MediaItem& it : root.items)
            if (it.type.startsWith(QLatin1Char('_'))) { CHECK(it.expandable); CHECK(it.id == it.mime); }

        // An index with NO series offers no door — the compatibility rule the whole feature rests on.
        Index noSeries = idx;
        noSeries.series.clear();
        const MediaCatalog plain = browse::bookRootCatalog(noSeries, none);
        CHECK(!plain.items.isEmpty());
        CHECK(plain.items.first().type == QString::fromLatin1(browse::kBookAuthorType));

        // An author's shelf: real leaves, each carrying the FILE and the local-leaf mime both surfaces route
        // by. This is what makes a book row open its reader on the themed layout as well as the classic one.
        const MediaCatalog shelf = browse::bookAuthorCatalog(idx, idx.authors.first().key);
        CHECK(!shelf.items.isEmpty());
        CHECK(shelf.title == BookLibrary::displayAuthor(idx.authors.first()));
        for (const MediaItem& it : shelf.items)
        {
            CHECK(!it.expandable);
            CHECK(it.mime == QString::fromLatin1(browse::kLocalBookMime));
            CHECK(!it.url.isEmpty());
            CHECK(QFileInfo::exists(it.url));
            CHECK(!it.type.startsWith(QLatin1Char('_')));   // a leaf, so the themed Enter opens the chooser
            CHECK(it.type == QString::fromLatin1(browse::kBookLeafType)
                  || it.type == QString::fromLatin1(browse::kComicLeafType));
        }

        // A series' shelf is the same rows read from the other side, in the series' own order.
        const MediaCatalog seriesList = browse::bookSeriesListCatalog(idx);
        CHECK(!seriesList.items.isEmpty());
        CHECK(seriesList.items.first().type == QString::fromLatin1(browse::kBookSeriesType));
        const QString sagaKey = ComicName::seriesKey(QStringLiteral("Saga"));
        const MediaCatalog saga = browse::bookSeriesCatalog(idx, sagaKey);
        CHECK(saga.items.size() == 3);
        CHECK(saga.title == QStringLiteral("Saga"));
        CHECK(saga.items.first().subtitle.startsWith(QStringLiteral("#1")));

        // A STALE key is an empty, titled catalog — never a crash, on any level.
        CHECK(browse::bookAuthorCatalog(idx, QStringLiteral("nobody")).items.isEmpty());
        CHECK(!browse::bookAuthorCatalog(idx, QStringLiteral("nobody")).title.isEmpty());
        CHECK(browse::bookSeriesCatalog(idx, QStringLiteral("nothing")).items.isEmpty());
        CHECK(!browse::bookSeriesCatalog(idx, QStringLiteral("nothing")).title.isEmpty());

        // An EMPTY index says why, in one non-actionable row — and says nothing at all when the caller has
        // nothing to say, which is the other half of that parameter's contract.
        browse::BookEmptyNote note;
        note.text = QStringLiteral("No books folder yet.");
        const MediaCatalog emptyCat = browse::bookRootCatalog(Index{}, note);
        CHECK(emptyCat.items.size() == 1);
        CHECK(emptyCat.items.first().type == QStringLiteral("info"));
        CHECK(browse::bookRootCatalog(Index{}, none).items.isEmpty());

        // The cover resolver is INJECTED and defaults to nothing, so this whole unit is pure.
        for (const MediaItem& it : shelf.items) CHECK(it.thumbnailUrl.isEmpty());
        const MediaCatalog withArt = browse::bookAuthorCatalog(
            idx, idx.authors.first().key, [](const Book&) { return QStringLiteral("/art.jpg"); });
        CHECK(!withArt.items.isEmpty());
        CHECK(withArt.items.first().thumbnailUrl == QStringLiteral("/art.jpg"));
    }

    // ---- §11 The other libraries are untouched --------------------------------------------------------
    {
        // Three roots, three different defaults. Nothing about a file decides which library it belongs to;
        // WHICH FOLDER it is under does, and these are three different folders.
        CHECK(Settings::readingFolder() != Settings::musicFolder());
        CHECK(Settings::readingFolder() != Settings::audiobookFolder());
        CHECK(Settings::musicFolder() != Settings::audiobookFolder());
        CHECK(Settings::readingFolder().endsWith(QStringLiteral("/books")));
        // ...and three different persisted indexes, so no scan here can invalidate a cache over there.
        CHECK(BookLibrary::indexFilePath().endsWith(QStringLiteral("/bookindex.json")));
        CHECK(BookLibrary::root() == Settings::readingFolder());
    }

    // ---- §12 Dormant ----------------------------------------------------------------------------------
    {
        ScanStats s;
        CHECK(BookLibrary::scanFolder(QString(), {}, &s).isEmpty());
        CHECK(s.files == 0);
        CHECK(BookLibrary::scanFolder(base + QStringLiteral("/nope")).isEmpty());
        CHECK(BookLibrary::buildIndex({}).isEmpty());
        CHECK(BookLibrary::index().isEmpty());          // nothing installed in this process
        CHECK(!BookLibrary::indexReady());
        CHECK(BookLibrary::buildIndex({}).series.isEmpty());
        // A missing/corrupt persisted file loads as empty, which costs a full re-read and nothing else.
        QString r;
        CHECK(BookLibrary::loadIndexFile(base + QStringLiteral("/missing.json"), &r).isEmpty());
        CHECK(r.isEmpty());
    }

    QDir(base).removeRecursively();

    if (g_fails == 0)
        std::printf("BOOKS-OK\n");
    else
        std::printf("BOOKS had %d failure(s)\n", g_fails);
    return g_fails == 0 ? 0 : 1;
}
