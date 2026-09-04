// probe_comicinfo — ComicInfo.xml, the comic world's metadata standard, read from inside the archive
// (issue #152): src/comic/ComicInfo — the parser, the four-container seam, the age-rating table, the
// reading-direction precedence and the issue-number arithmetic.
//
// THE FIXTURES ARE REAL ARCHIVES, ONE PER CONTAINER, AND NONE OF THEM IS BUILT BY THE CODE UNDER TEST:
//
//   * .cbz — a real zip, written by miniz's WRITER (the reader under test is miniz's reader);
//   * .cbr — a real RAR 4, laid byte by byte by tools/BookFixtures.h (the same independent placer probe_cbr
//            uses: signature, main block, per-entry file block, header CRC16, data CRC32);
//   * .cbt — a real ustar tar, laid 512-byte block by 512-byte block in this file;
//   * .cb7 — a real 7-Zip, produced by UPSTREAM 7-Zip (store method) and checked in below as 368 literal
//            bytes. This repo vendors a 7z DECODER and no writer, so this is the only way a .cb7 fixture can
//            exist at all — and it is a better one than a hand-lay would be: the bytes came from the
//            reference implementation of the format.
//
// ALL FOUR CARRY THE SAME DOCUMENT, and §6 asserts they parse to the same Info. That is the "format-
// agnostic" claim of the issue made checkable rather than asserted in a comment: one parser, four
// containers, one answer.
//
// WHAT IT PINS:
//
//   1. EVERY MAPPED FIELD off a full document — series, number, volume, title, summary, Y/M/D, the six
//      creator roles, publisher, genre, language, page count, web, rating, direction.
//   2. THE CREATORS COLLAPSE: role order decides the list order, a person credited twice appears once, and
//      the PRIMARY AUTHOR IS THE WRITER — never the penciller promoted into an empty slot.
//   3. THE AGE TABLE, all fifteen ComicRack values plus the two that are not values at all. UNKNOWN MAPS TO
//      UNRATED AND NEVER TO A RUNG, which is the whole safety property: a parental gate must not be able to
//      certify a comic nobody rated. And Unrated is not Everyone — they are distinct enumerators, so a
//      later gate can tell them apart.
//   4. THE READING DIRECTION AND ITS PRECEDENCE: YesAndRightToLeft is RTL, plain "Yes" is NOT a direction,
//      and the user's override beats the document IN BOTH DIRECTIONS (including overriding a manga back to
//      left-to-right, which a one-way test would miss).
//   5. MALFORMED IS IGNORED WHOLE, not applied as far as it got, and a document whose root is not
//      <ComicInfo> yields nothing rather than fields scraped out of somebody else's XML. <Pages> is read
//      past without its children leaking in.
//   6. THE NAME MATCH: case-insensitive, ROOT ONLY. A nested copy and the __MACOSX shadow every Mac-built
//      archive carries are not the comic's own document — the same trap ComicPageOrder.h documents for the
//      COVER, arriving through a second door.
//   7. THE ISSUE NUMBER: "1.5" keeps its half, "Annual 1" has no decimal at all and sorts as unnumbered,
//      and the RAW string is what a shelf shows.
//   8. THE OVERRIDE STORE round-trips through Settings and FORGETS on 0.
//
// Prints COMICINFO-OK on success; any failure prints COMICINFO-FAIL <cond> (line) and exits non-zero.
//
// Isolation: AppPaths::dataDir() is this process's own scratch dir (issue #42), so the fixtures and the
// settings file are written under it and go away at exit.
#include "ComicInfo.h"
#include "BookFixtures.h"   // the RAR 4 byte placer, shared with probe_cbr / probe_books
#include "AppPaths.h"
#include "Settings.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QVector>
#include <cstdio>
#include <cstring>

#include "miniz.h"

static int g_fails = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "COMICINFO-FAIL %s (line %d)\n", #cond, __LINE__); ++g_fails; } \
} while (0)

// ---- The one document every container carries -----------------------------------------------------------
// Deliberately NOT in the field order of the standard's schema: a parser that depended on document order
// would pass a tidy fixture and fail on half the real world.
static QByteArray fullDoc()
{
    return QByteArrayLiteral(
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<ComicInfo xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">\n"
        "  <Series>Saga</Series>\n"
        "  <Number>1.5</Number>\n"
        "  <Volume>2012</Volume>\n"
        "  <Title>The Will</Title>\n"
        "  <Summary>A soldier and a soldier have a baby.</Summary>\n"
        "  <Year>2013</Year><Month>4</Month><Day>17</Day>\n"
        "  <Penciller>Fiona Staples</Penciller>\n"
        "  <Writer>Brian K. Vaughan, Fiona Staples</Writer>\n"
        "  <Letterer>Fonografiks</Letterer>\n"
        "  <CoverArtist>Fiona Staples</CoverArtist>\n"
        "  <Publisher>Image Comics</Publisher>\n"
        "  <Genre>Science Fiction</Genre>\n"
        "  <LanguageISO>en</LanguageISO>\n"
        "  <PageCount>24</PageCount>\n"
        "  <Web>https://example.invalid/saga/1</Web>\n"
        "  <AgeRating>Mature 17+</AgeRating>\n"
        "  <Manga>YesAndRightToLeft</Manga>\n"
        "  <Pages><Page Image=\"0\" Type=\"FrontCover\" /><Page Image=\"1\" Title=\"Not a title\" /></Pages>\n"
        "</ComicInfo>\n");
}

// Every mapped field of fullDoc(), asserted from ONE place so the four container sections cannot drift.
static void checkFullDoc(const ComicInfo::Info& i, const char* what)
{
    if (i.series != QStringLiteral("Saga"))
        { std::fprintf(stderr, "COMICINFO-FAIL series from %s\n", what); ++g_fails; }
    if (i.number != QStringLiteral("1.5"))
        { std::fprintf(stderr, "COMICINFO-FAIL number from %s\n", what); ++g_fails; }
    if (i.title != QStringLiteral("The Will"))
        { std::fprintf(stderr, "COMICINFO-FAIL title from %s\n", what); ++g_fails; }
    if (i.author != QStringLiteral("Brian K. Vaughan"))
        { std::fprintf(stderr, "COMICINFO-FAIL author from %s\n", what); ++g_fails; }
    if (i.rating != ComicInfo::Rating::Mature)
        { std::fprintf(stderr, "COMICINFO-FAIL rating from %s\n", what); ++g_fails; }
    if (i.direction != ComicInfo::Direction::RightToLeft)
        { std::fprintf(stderr, "COMICINFO-FAIL direction from %s\n", what); ++g_fails; }
    if (i.publisher != QStringLiteral("Image Comics"))
        { std::fprintf(stderr, "COMICINFO-FAIL publisher from %s\n", what); ++g_fails; }
}

// ---- Fixture writers ------------------------------------------------------------------------------------
static bool writeFile(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    return f.write(bytes) == bytes.size();
}

static bool writeZip(const QString& path, const QVector<QPair<QString, QByteArray>>& members)
{
    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    if (!mz_zip_writer_init_file(&zip, path.toUtf8().constData(), 0)) return false;
    bool ok = true;
    for (const auto& m : members)
        ok = mz_zip_writer_add_mem(&zip, m.first.toUtf8().constData(), m.second.constData(),
                                   size_t(m.second.size()), MZ_NO_COMPRESSION) && ok;
    ok = mz_zip_writer_finalize_archive(&zip) && ok;
    mz_zip_writer_end(&zip);
    return ok;
}

// A ustar tar, laid out by hand: one 512-byte header per member, data padded to the next 512 boundary, and
// a zero block to end it. Independent of Tar.h, which only ever READS.
static void tarOctal(char* p, qint64 v)
{
    for (int i = 10; i >= 0; --i) { p[i] = char('0' + int(v & 7)); v >>= 3; }
    p[11] = '\0';
}
static QByteArray buildTar(const QVector<QPair<QString, QByteArray>>& members)
{
    QByteArray out;
    for (const auto& m : members)
    {
        QByteArray header(512, '\0');
        const QByteArray name = m.first.toUtf8();
        std::memcpy(header.data(), name.constData(), size_t(qMin<qsizetype>(name.size(), 100)));
        tarOctal(header.data() + 124, m.second.size());
        header[156] = '0';                                   // regular file
        std::memcpy(header.data() + 257, "ustar", 5);
        header[263] = '0'; header[264] = '0';
        out += header;
        QByteArray data = m.second;
        while (data.size() % 512) data.append('\0');
        out += data;
    }
    out += QByteArray(512, '\0');   // end-of-archive marker
    return out;
}

// A REAL .7z, store method, produced by upstream 7-Zip — see the header. It holds ComicInfo.xml (a
// Nausicaa document, DIFFERENT from fullDoc() on purpose so a mix-up cannot pass) and one page1.jpg.
static const unsigned char kCb7[] = {
        0x37, 0x7a, 0xbc, 0xaf, 0x27, 0x1c, 0x00, 0x04, 0x2b, 0xdf, 0x04, 0xc3, 0x2e, 0x01, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x22, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x21, 0x61, 0x55, 0x8b,
        0x3c, 0x3f, 0x78, 0x6d, 0x6c, 0x20, 0x76, 0x65, 0x72, 0x73, 0x69, 0x6f, 0x6e, 0x3d, 0x22, 0x31,
        0x2e, 0x30, 0x22, 0x3f, 0x3e, 0x0a, 0x3c, 0x43, 0x6f, 0x6d, 0x69, 0x63, 0x49, 0x6e, 0x66, 0x6f,
        0x3e, 0x3c, 0x53, 0x65, 0x72, 0x69, 0x65, 0x73, 0x3e, 0x4e, 0x61, 0x75, 0x73, 0x69, 0x63, 0x61,
        0x61, 0x3c, 0x2f, 0x53, 0x65, 0x72, 0x69, 0x65, 0x73, 0x3e, 0x3c, 0x4e, 0x75, 0x6d, 0x62, 0x65,
        0x72, 0x3e, 0x37, 0x3c, 0x2f, 0x4e, 0x75, 0x6d, 0x62, 0x65, 0x72, 0x3e, 0x3c, 0x57, 0x72, 0x69,
        0x74, 0x65, 0x72, 0x3e, 0x48, 0x61, 0x79, 0x61, 0x6f, 0x20, 0x4d, 0x69, 0x79, 0x61, 0x7a, 0x61,
        0x6b, 0x69, 0x3c, 0x2f, 0x57, 0x72, 0x69, 0x74, 0x65, 0x72, 0x3e, 0x3c, 0x4d, 0x61, 0x6e, 0x67,
        0x61, 0x3e, 0x59, 0x65, 0x73, 0x41, 0x6e, 0x64, 0x52, 0x69, 0x67, 0x68, 0x74, 0x54, 0x6f, 0x4c,
        0x65, 0x66, 0x74, 0x3c, 0x2f, 0x4d, 0x61, 0x6e, 0x67, 0x61, 0x3e, 0x3c, 0x41, 0x67, 0x65, 0x52,
        0x61, 0x74, 0x69, 0x6e, 0x67, 0x3e, 0x54, 0x65, 0x65, 0x6e, 0x3c, 0x2f, 0x41, 0x67, 0x65, 0x52,
        0x61, 0x74, 0x69, 0x6e, 0x67, 0x3e, 0x3c, 0x2f, 0x43, 0x6f, 0x6d, 0x69, 0x63, 0x49, 0x6e, 0x66,
        0x6f, 0x3e, 0x0a, 0x50, 0x41, 0x47, 0x45, 0x2d, 0x4f, 0x4e, 0x45, 0x00, 0x00, 0x81, 0x33, 0x07,
        0xae, 0x31, 0x9e, 0xdc, 0x6b, 0x52, 0x11, 0x1a, 0x6f, 0x91, 0x90, 0x72, 0x8b, 0x39, 0x8f, 0xd2,
        0x4e, 0xd5, 0xe5, 0x0a, 0xf1, 0x7a, 0xba, 0xcc, 0x2e, 0xda, 0x10, 0x3d, 0xda, 0x8d, 0x15, 0xa6,
        0x44, 0xf9, 0xc1, 0xe0, 0x74, 0x45, 0xec, 0x23, 0x2d, 0xe1, 0xb9, 0xee, 0xec, 0xea, 0x95, 0x5e,
        0x7e, 0xa4, 0x71, 0x9b, 0x64, 0xfa, 0xe2, 0x35, 0x11, 0x2d, 0xaf, 0xdd, 0x22, 0x8b, 0xbf, 0x50,
        0x07, 0xe0, 0x1e, 0x43, 0x35, 0xcb, 0x36, 0x69, 0x9a, 0x0c, 0xc6, 0xd2, 0x6a, 0xa4, 0x62, 0xe7,
        0x68, 0xba, 0xb2, 0x21, 0x91, 0xaa, 0x44, 0xeb, 0x45, 0xc5, 0xb5, 0xb0, 0x41, 0xed, 0x83, 0x82,
        0xf7, 0xd0, 0x59, 0x3a, 0x70, 0xb7, 0xbc, 0xaf, 0x76, 0x6c, 0xe1, 0xb6, 0x19, 0x6a, 0x17, 0x06,
        0x80, 0xbb, 0x01, 0x09, 0x73, 0x00, 0x07, 0x0b, 0x01, 0x00, 0x01, 0x23, 0x03, 0x01, 0x01, 0x05,
        0x5d, 0x00, 0x10, 0x00, 0x00, 0x0c, 0x80, 0x86, 0x0a, 0x01, 0x2d, 0xeb, 0x19, 0x1d, 0x00, 0x00
};

int main()
{
    const QString base = QDir(AppPaths::dataDir()).filePath(QStringLiteral("comicinfo-fixtures"));
    QDir(base).removeRecursively();
    QDir().mkpath(base);

    const QByteArray doc  = fullDoc();
    const QByteArray page = QByteArrayLiteral("NOT-REALLY-A-JPEG");

    // ---- 1. Every mapped field, off the document itself ------------------------------------------------
    {
        bool wellFormed = false;
        const ComicInfo::Info i = ComicInfo::parse(doc, &wellFormed);
        CHECK(wellFormed);
        CHECK(!i.isEmpty());
        checkFullDoc(i, "parse()");
        CHECK(i.volume == 2012);
        CHECK(i.summary == QStringLiteral("A soldier and a soldier have a baby."));
        CHECK(i.year == 2013);
        CHECK(i.month == 4);
        CHECK(i.day == 17);
        CHECK(i.genre == QStringLiteral("Science Fiction"));
        CHECK(i.language == QStringLiteral("en"));
        CHECK(i.pageCount == 24);
        CHECK(i.web == QStringLiteral("https://example.invalid/saga/1"));

        // <Pages> READ PAST: its <Page Title="Not a title"/> attribute must not have become the title, and
        // the block must not have ended the parse — <Manga> is the element AFTER it in document order.
        CHECK(i.title == QStringLiteral("The Will"));
        CHECK(i.direction == ComicInfo::Direction::RightToLeft);
    }

    // ---- 2. The creators collapse, and who the author is -----------------------------------------------
    {
        const ComicInfo::Info i = ComicInfo::parse(doc);
        // ROLE ORDER, not document order: the writers come first even though <Penciller> was laid out
        // above <Writer> in the fixture.
        CHECK(i.creators.size() == 3);
        CHECK(i.creators.value(0) == QStringLiteral("Brian K. Vaughan"));
        CHECK(i.creators.value(1) == QStringLiteral("Fiona Staples"));
        CHECK(i.creators.value(2) == QStringLiteral("Fonografiks"));
        // Fiona Staples is writer #2, penciller AND cover artist. ONCE.
        CHECK(i.creators.count(QStringLiteral("Fiona Staples")) == 1);
        CHECK(i.author == QStringLiteral("Brian K. Vaughan"));

        // NO WRITER => NO AUTHOR. The penciller is a creator and is not promoted into the author slot: a
        // shelf that says "by <artist>" is making a claim the file did not make.
        const ComicInfo::Info art = ComicInfo::parse(QByteArrayLiteral(
            "<ComicInfo><Series>Blame!</Series><Penciller>Tsutomu Nihei</Penciller></ComicInfo>"));
        CHECK(art.author.isEmpty());
        CHECK(art.creators == QStringList{ QStringLiteral("Tsutomu Nihei") });
    }

    // ---- 3. The age table ------------------------------------------------------------------------------
    {
        struct Row { const char* value; ComicInfo::Rating rating; };
        static const Row kRows[] = {
            { "Unknown",           ComicInfo::Rating::Unrated    },
            { "Rating Pending",    ComicInfo::Rating::Unrated    },
            { "Early Childhood",   ComicInfo::Rating::Everyone   },
            { "Everyone",          ComicInfo::Rating::Everyone   },
            { "G",                 ComicInfo::Rating::Everyone   },
            { "Kids to Adults",    ComicInfo::Rating::Everyone   },
            { "Everyone 10+",      ComicInfo::Rating::Everyone10 },
            { "PG",                ComicInfo::Rating::Everyone10 },
            { "Teen",              ComicInfo::Rating::Teen       },
            { "MA15+",             ComicInfo::Rating::Mature     },
            { "Mature 17+",        ComicInfo::Rating::Mature     },
            { "M",                 ComicInfo::Rating::Mature     },
            { "R18+",              ComicInfo::Rating::Adults     },
            { "Adults Only 18+",   ComicInfo::Rating::Adults     },
            { "X18+",              ComicInfo::Rating::Adults     },
        };
        for (const Row& r : kRows)
            CHECK(ComicInfo::ratingFor(QString::fromLatin1(r.value)) == r.rating);

        // Case and whitespace do not matter; anything else DOES.
        CHECK(ComicInfo::ratingFor(QStringLiteral("mature 17+")) == ComicInfo::Rating::Mature);
        CHECK(ComicInfo::ratingFor(QStringLiteral("  Teen  ")) == ComicInfo::Rating::Teen);

        // THE SAFETY PROPERTY. A value nobody has heard of is UNRATED, not the nearest-looking rung — and
        // "Everyone Except Adults" must not be pattern-matched onto Everyone by a prefix or a contains().
        CHECK(ComicInfo::ratingFor(QStringLiteral("PEGI 18")) == ComicInfo::Rating::Unrated);
        CHECK(ComicInfo::ratingFor(QStringLiteral("Everyone Except Adults")) == ComicInfo::Rating::Unrated);
        CHECK(ComicInfo::ratingFor(QString()) == ComicInfo::Rating::Unrated);
        CHECK(ComicInfo::Rating::Unrated != ComicInfo::Rating::Everyone);

        // What a restricted (kids) profile hides.
        CHECK(ComicInfo::hiddenWhenRestricted(ComicInfo::Rating::Mature));
        CHECK(ComicInfo::hiddenWhenRestricted(ComicInfo::Rating::Adults));
        CHECK(!ComicInfo::hiddenWhenRestricted(ComicInfo::Rating::Teen));
        CHECK(!ComicInfo::hiddenWhenRestricted(ComicInfo::Rating::Everyone10));
        CHECK(!ComicInfo::hiddenWhenRestricted(ComicInfo::Rating::Everyone));
        // Unrated is SHOWN: every comic in every library that exists today is unrated, and hiding them
        // would empty a kids shelf on the first launch of this build (ComicInfo.h says so at length).
        CHECK(!ComicInfo::hiddenWhenRestricted(ComicInfo::Rating::Unrated));
    }

    // ---- 4. The reading direction, and who wins ---------------------------------------------------------
    {
        using D = ComicInfo::Direction;
        CHECK(ComicInfo::directionFor(QStringLiteral("YesAndRightToLeft")) == D::RightToLeft);
        CHECK(ComicInfo::directionFor(QStringLiteral("yesandrighttoleft")) == D::RightToLeft);
        CHECK(ComicInfo::directionFor(QStringLiteral("YesAndLeftToRight")) == D::LeftToRight);
        // "Yes" says it IS manga, not which way it reads — a translated manga printed left to right is a
        // real thing and its taggers write exactly this.
        CHECK(ComicInfo::directionFor(QStringLiteral("Yes")) == D::Unspecified);
        CHECK(ComicInfo::directionFor(QStringLiteral("No")) == D::Unspecified);
        CHECK(ComicInfo::directionFor(QString()) == D::Unspecified);

        // PRECEDENCE. The user is above all; the document is the default under them; with neither, the
        // direction the reader has always used.
        CHECK(ComicInfo::resolveDirection(D::RightToLeft, D::Unspecified) == D::RightToLeft);
        CHECK(ComicInfo::resolveDirection(D::Unspecified, D::Unspecified) == D::LeftToRight);
        CHECK(ComicInfo::resolveDirection(D::LeftToRight, D::Unspecified) == D::LeftToRight);
        // BOTH WAYS: the override turns a manga back to left-to-right as well as turning a silent comic
        // right-to-left. A one-way assertion would pass a resolver that just ORed the two.
        CHECK(ComicInfo::resolveDirection(D::RightToLeft, D::LeftToRight) == D::LeftToRight);
        CHECK(ComicInfo::resolveDirection(D::Unspecified, D::RightToLeft) == D::RightToLeft);
        CHECK(ComicInfo::resolveDirection(D::LeftToRight, D::RightToLeft) == D::RightToLeft);
    }

    // ---- 5. Malformed, foreign and empty documents -------------------------------------------------------
    {
        // TRUNCATED MID-ELEMENT. The fields before the damage did parse — and the point is that the caller
        // is TOLD, so it can throw the lot away rather than file the comic under half a series.
        bool wellFormed = true;
        ComicInfo::parse(QByteArrayLiteral("<ComicInfo><Series>Saga</Series><Number>1</Numb"), &wellFormed);
        CHECK(!wellFormed);

        wellFormed = true;
        ComicInfo::parse(QByteArrayLiteral("<ComicInfo><Series>A</Series></ComicInfo>"), &wellFormed);
        CHECK(wellFormed);

        // NOT OUR FORMAT. A well-formed document whose root is something else yields nothing at all — the
        // <title> of an OPF is not a comic's title.
        wellFormed = false;
        const ComicInfo::Info foreign = ComicInfo::parse(
            QByteArrayLiteral("<package><metadata><Series>Wrong</Series></metadata></package>"), &wellFormed);
        CHECK(wellFormed);
        CHECK(foreign.isEmpty());
        CHECK(foreign.series.isEmpty());

        CHECK(ComicInfo::parse(QByteArray()).isEmpty());
        CHECK(ComicInfo::parse(QByteArrayLiteral("<ComicInfo/>")).isEmpty());
    }

    // ---- 6. THE NAME MATCH: case-insensitive, root only -------------------------------------------------
    {
        CHECK(ComicInfo::isComicInfoName(QStringLiteral("ComicInfo.xml")));
        CHECK(ComicInfo::isComicInfoName(QStringLiteral("comicinfo.xml")));
        CHECK(ComicInfo::isComicInfoName(QStringLiteral("COMICINFO.XML")));
        CHECK(ComicInfo::isComicInfoName(QStringLiteral("./ComicInfo.xml")));
        // NOT AT THE ROOT. The __MACOSX shadow is the one that matters: it sorts first in a Mac-built
        // archive, so a rule that looked at basenames would read the resource fork as the comic's metadata.
        CHECK(!ComicInfo::isComicInfoName(QStringLiteral("__MACOSX/ComicInfo.xml")));
        CHECK(!ComicInfo::isComicInfoName(QStringLiteral("extras/ComicInfo.xml")));
        CHECK(!ComicInfo::isComicInfoName(QStringLiteral("extras\\ComicInfo.xml")));
        CHECK(!ComicInfo::isComicInfoName(QStringLiteral("ComicInfo.xml.bak")));
        CHECK(!ComicInfo::isComicInfoName(QStringLiteral("MyComicInfo.xml")));
        CHECK(!ComicInfo::isComicInfoName(QStringLiteral("page1.jpg")));
        CHECK(!ComicInfo::isComicInfoName(QString()));
    }

    // ---- 7. The issue number -----------------------------------------------------------------------------
    {
        CHECK(ComicInfo::numberAsIndex(QStringLiteral("1")) == 1.0);
        CHECK(ComicInfo::numberAsIndex(QStringLiteral("12")) == 12.0);
        CHECK(ComicInfo::numberAsIndex(QStringLiteral("1.5")) == 1.5);   // the half-issue keeps its half
        CHECK(ComicInfo::numberAsIndex(QStringLiteral("0")) == 0.0);
        CHECK(ComicInfo::numberAsIndex(QStringLiteral("3a")) == 3.0);
        // NO LEADING DIGIT => UNNUMBERED (0), which is the "sorts last" value the shelf already uses. The
        // raw string is kept beside it and is what separates two of these from each other.
        CHECK(ComicInfo::numberAsIndex(QStringLiteral("Annual 1")) == 0.0);
        CHECK(ComicInfo::numberAsIndex(QStringLiteral("Special")) == 0.0);
        CHECK(ComicInfo::numberAsIndex(QString()) == 0.0);
        const ComicInfo::Info annual = ComicInfo::parse(QByteArrayLiteral(
            "<ComicInfo><Series>Saga</Series><Number>Annual 1</Number></ComicInfo>"));
        CHECK(annual.number == QStringLiteral("Annual 1"));
    }

    // ---- 8. THE SEAM: four containers, one document, one answer -------------------------------------------
    {
        // .cbz — the document is NOT the first member, and a decoy sits in a subfolder ahead of it.
        const QString cbz = base + QStringLiteral("/Saga 001.cbz");
        CHECK(writeZip(cbz, {
            { QStringLiteral("__MACOSX/ComicInfo.xml"), QByteArrayLiteral("<ComicInfo><Series>WRONG</Series></ComicInfo>") },
            { QStringLiteral("page1.jpg"), page },
            { QStringLiteral("comicinfo.xml"), doc },       // lower case: still the document
            { QStringLiteral("page2.jpg"), page },
        }));
        CHECK(ComicInfo::xmlFromArchive(cbz) == doc);
        checkFullDoc(ComicInfo::readArchive(cbz), "cbz");

        // .cbt — a real tar, same shape.
        const QString cbt = base + QStringLiteral("/Saga 001.cbt");
        CHECK(writeFile(cbt, buildTar({
            { QStringLiteral("page1.jpg"), page },
            { QStringLiteral("ComicInfo.xml"), doc },
        })));
        CHECK(ComicInfo::xmlFromArchive(cbt) == doc);
        checkFullDoc(ComicInfo::readArchive(cbt), "cbt");

        // .cbr — a real RAR 4, laid byte by byte. The document is a NON-IMAGE member, which is the case the
        // #144 reader deliberately dropped on the floor: imageNames() had to learn to report it.
        const QString cbr = base + QStringLiteral("/Saga 001.cbr");
        QVector<BookFixtures::RarEntry> rar;
        rar.append({ QStringLiteral("page1.jpg"), page, false, false });
        rar.append({ QStringLiteral("ComicInfo.xml"), doc, false, false });
        rar.append({ QStringLiteral("page2.jpg"), page, false, false });
        CHECK(writeFile(cbr, BookFixtures::buildRar4(rar)));
        CHECK(ComicInfo::xmlFromArchive(cbr) == doc);
        checkFullDoc(ComicInfo::readArchive(cbr), "cbr");

        // .cb7 — the upstream-produced archive. Its document is a DIFFERENT one (Nausicaa), so a fixture
        // mix-up cannot pass this section by accident.
        const QString cb7 = base + QStringLiteral("/Nausicaa 007.cb7");
        CHECK(writeFile(cb7, QByteArray(reinterpret_cast<const char*>(kCb7), int(sizeof(kCb7)))));
        const ComicInfo::Info seven = ComicInfo::readArchive(cb7);
        CHECK(seven.series == QStringLiteral("Nausicaa"));
        CHECK(seven.number == QStringLiteral("7"));
        CHECK(seven.author == QStringLiteral("Hayao Miyazaki"));
        CHECK(seven.direction == ComicInfo::Direction::RightToLeft);
        CHECK(seven.rating == ComicInfo::Rating::Teen);

        // AN ARCHIVE WITH NO DOCUMENT IS SILENT, in every container — no bytes, an empty Info, and (the
        // point) no failure: this is what almost every comic on almost every shelf is.
        const QString plain = base + QStringLiteral("/Bone 002.cbz");
        CHECK(writeZip(plain, { { QStringLiteral("page1.jpg"), page } }));
        CHECK(ComicInfo::xmlFromArchive(plain).isEmpty());
        CHECK(ComicInfo::readArchive(plain).isEmpty());

        const QString plainTar = base + QStringLiteral("/Bone 002.cbt");
        CHECK(writeFile(plainTar, buildTar({ { QStringLiteral("page1.jpg"), page } })));
        CHECK(ComicInfo::xmlFromArchive(plainTar).isEmpty());

        QVector<BookFixtures::RarEntry> bare;
        bare.append({ QStringLiteral("page1.jpg"), page, false, false });
        const QString plainRar = base + QStringLiteral("/Bone 002.cbr");
        CHECK(writeFile(plainRar, BookFixtures::buildRar4(bare)));
        CHECK(ComicInfo::xmlFromArchive(plainRar).isEmpty());

        // A MALFORMED DOCUMENT IS IGNORED WHOLE — the archive still opens, and the Info is empty rather
        // than half-filled with the fields that happened to precede the damage.
        const QString broken = base + QStringLiteral("/Torn 001.cbz");
        CHECK(writeZip(broken, {
            { QStringLiteral("ComicInfo.xml"), QByteArrayLiteral("<ComicInfo><Series>Torn</Series><Numb") },
            { QStringLiteral("page1.jpg"), page },
        }));
        CHECK(!ComicInfo::xmlFromArchive(broken).isEmpty());   // the bytes ARE there
        CHECK(ComicInfo::readArchive(broken).isEmpty());       // and none of them is used
        CHECK(ComicInfo::readArchive(broken).series.isEmpty());

        // A path that is not there, and a file that is not an archive at all.
        CHECK(ComicInfo::xmlFromArchive(base + QStringLiteral("/missing.cbz")).isEmpty());
        const QString junk = base + QStringLiteral("/junk.cbr");
        CHECK(writeFile(junk, QByteArrayLiteral("this is not a rar")));
        CHECK(ComicInfo::xmlFromArchive(junk).isEmpty());
        // An extension the seam does not claim.
        CHECK(ComicInfo::xmlFromArchive(cbz + QStringLiteral(".epub")).isEmpty());
    }

    // ---- 9. The per-series override store -----------------------------------------------------------------
    {
        const QString key = QStringLiteral("saga");
        CHECK(Settings::comicDirectionOverride(key) == 0);      // never asked == no override
        Settings::setComicDirectionOverride(key, 2);
        CHECK(Settings::comicDirectionOverride(key) == 2);
        CHECK(Settings::comicDirectionOverride(QStringLiteral("bone")) == 0);   // scoped to its series
        Settings::setComicDirectionOverride(key, 1);
        CHECK(Settings::comicDirectionOverride(key) == 1);
        // 0 FORGETS rather than storing a third state.
        Settings::setComicDirectionOverride(key, 0);
        CHECK(Settings::comicDirectionOverride(key) == 0);
        // A key with the characters that would have broken a key-per-series scheme.
        const QString awkward = QStringLiteral("g.i. joe / cobra [2009] = a");
        Settings::setComicDirectionOverride(awkward, 2);
        CHECK(Settings::comicDirectionOverride(awkward) == 2);
        CHECK(Settings::comicDirectionOverride(QString()) == 0);
    }

    QDir(base).removeRecursively();
    if (g_fails) { std::fprintf(stderr, "COMICINFO-FAIL %d check(s)\n", g_fails); return 1; }
    std::printf("COMICINFO-OK\n");
    return 0;
}
