#include "ComicInfo.h"
#include "Tar.h"                 // .cbt — the in-tree ustar reader (#144)
#include "RarComic.h"            // .cbr — the vendored unarr reader (#144)
#include "../core/SevenZip.h"    // .cb7 — the vendored LZMA SDK (#144)

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QTemporaryDir>
#include <QXmlStreamReader>
#include <QtGlobal>
#include <cstring>

#include "miniz.h"

namespace ComicInfo
{
namespace
{
    QString ext(const QString& path) { return QFileInfo(path).suffix().toLower(); }

    // The comparison every table below uses: case-folded and whitespace-collapsed, and otherwise EXACT.
    // simplified() also folds the inner runs, so "Mature  17+" and "mature 17+" are one value while
    // "Mature17+" — which is not a value anybody writes — is not silently made into one.
    QString foldValue(const QString& s) { return s.simplified().toCaseFolded(); }

    // One role field: "Alan Moore, Dave Gibbons" -> two names, in order, blanks dropped.
    QStringList splitNames(const QString& field)
    {
        QStringList out;
        const QStringList parts = field.split(QLatin1Char(','), Qt::SkipEmptyParts);
        for (const QString& p : parts)
        {
            const QString n = p.simplified();
            if (!n.isEmpty()) out.append(n);
        }
        return out;
    }

    // Append names, keeping the first spelling of each and dropping repeats. A writer who also inked the
    // issue is ONE credit, in the position their first role puts them.
    void addCreators(QStringList& into, const QStringList& names)
    {
        for (const QString& n : names)
        {
            bool seen = false;
            for (const QString& have : into)
                if (have.compare(n, Qt::CaseInsensitive) == 0) { seen = true; break; }
            if (!seen) into.append(n);
        }
    }

    int intOf(const QString& s)
    {
        bool ok = false;
        const int v = s.simplified().toInt(&ok);
        return ok ? v : 0;
    }

    // ---- The zip / tar member hunts ---------------------------------------------------------------------
    QByteArray xmlFromZip(const QString& path)
    {
        QByteArray out;
        mz_zip_archive zip;
        std::memset(&zip, 0, sizeof(zip));
        if (!mz_zip_reader_init_file(&zip, path.toUtf8().constData(), 0)) return out;
        const mz_uint count = mz_zip_reader_get_num_files(&zip);
        for (mz_uint i = 0; i < count; ++i)
        {
            if (mz_zip_reader_is_file_a_directory(&zip, i)) continue;
            mz_zip_archive_file_stat st;
            if (!mz_zip_reader_file_stat(&zip, i, &st)) continue;
            if (!isComicInfoName(QString::fromUtf8(st.m_filename))) continue;
            size_t sz = 0;
            void* p = mz_zip_reader_extract_to_heap(&zip, i, &sz, 0);
            if (p) { out = QByteArray(static_cast<const char*>(p), int(sz)); mz_free(p); }
            break;      // FIRST match at the root wins, in the archive's own order
        }
        mz_zip_reader_end(&zip);
        return out;
    }

    QByteArray xmlFromTar(const QString& path)
    {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) return QByteArray();
        // The whole file, because Tar.h reads a buffer — the same thing ComicView's .cbt branch does, and
        // one of the two reasons BookLibrary.h keeps .cbt out of the scan.
        const QByteArray bytes = f.readAll();
        f.close();
        const QVector<Tar::TarEntry> entries = Tar::listEntries(bytes);
        for (const Tar::TarEntry& e : entries)
        {
            if (!isComicInfoName(e.name)) continue;
            if (e.dataOffset < 0 || e.size <= 0 || e.dataOffset + e.size > bytes.size()) break;
            return bytes.mid(int(e.dataOffset), int(e.size));
        }
        return QByteArray();
    }
}

// A directory that already holds an archive's extracted contents (the .cb7 path, whose pages the reader
// unpacks to a temp dir anyway). Public so ComicView can read the document out of the extraction it ALREADY
// made instead of decoding the archive a second time.
QByteArray xmlFromDirectory(const QString& dir)
{
    const QDir d(dir);
    if (!d.exists()) return QByteArray();
    // Root only, exactly as in an archive: entryList of files, no recursion.
    const QStringList names = d.entryList(QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString& n : names)
    {
        if (!isComicInfoName(n)) continue;
        QFile f(d.filePath(n));
        if (!f.open(QIODevice::ReadOnly)) return QByteArray();
        return f.readAll();
    }
    return QByteArray();
}

bool isComicInfoName(const QString& memberName)
{
    QString n = memberName;
    n.replace(QLatin1Char('\\'), QLatin1Char('/'));     // some writers emit Windows separators
    while (n.startsWith(QStringLiteral("./"))) n = n.mid(2);
    if (n.contains(QLatin1Char('/'))) return false;      // NESTED: not the archive's own document
    return n.compare(QStringLiteral("ComicInfo.xml"), Qt::CaseInsensitive) == 0;
}

QByteArray xmlFromArchive(const QString& archivePath)
{
    const QString e = ext(archivePath);
    if (e == QStringLiteral("cbz") || e == QStringLiteral("zip")) return xmlFromZip(archivePath);
    if (e == QStringLiteral("cbt")) return xmlFromTar(archivePath);
    if (e == QStringLiteral("cbr"))
    {
        // THE HEADER WALK FIRST, and this ordering is the whole cost story for a .cbr. Listing decompresses
        // nothing, so an archive with no ComicInfo.xml pays a walk and stops; only one that HAS the member
        // pays the single sequential pass a solid RAR forces (RarComic.h).
        QStringList others;
        RarComic::Status st = RarComic::Status::Ok;
        RarComic::imageNames(archivePath, &st, &others);
        if (st != RarComic::Status::Ok && st != RarComic::Status::NoPages) return QByteArray();
        for (const QString& n : others)
            if (isComicInfoName(n)) return RarComic::memberBytes(archivePath, n);
        return QByteArray();
    }
    if (e == QStringLiteral("cb7"))
    {
        // A WHOLE-ARCHIVE DECODE, because 7z gives no cheaper way in. This is why .cb7 is not in the library
        // scan (BookLibrary.h) and why the reader, which has to unpack it anyway, calls xmlFromDirectory on
        // the extraction it already made rather than coming through here.
        QTemporaryDir tmp(QDir::tempPath() + QStringLiteral("/eb-cinfo-XXXXXX"));
        if (!tmp.isValid()) return QByteArray();
        if (!SevenZip::extractAllToDir(archivePath, tmp.path())) return QByteArray();
        return xmlFromDirectory(tmp.path());
    }
    return QByteArray();
}

Info readArchive(const QString& archivePath)
{
    const QByteArray xml = xmlFromArchive(archivePath);
    if (xml.isEmpty()) return Info();
    bool wellFormed = true;
    const Info i = parse(xml, &wellFormed);
    // IGNORED WHOLE when it will not parse — never applied as far as it got. One log line; the caller keeps
    // whatever the file name told it.
    if (!wellFormed)
    {
        qWarning("ComicInfo.xml in \"%s\" is malformed; ignoring it and using the file name.",
                 qPrintable(QFileInfo(archivePath).fileName()));
        return Info();
    }
    return i;
}

Info parse(const QByteArray& xml, bool* wellFormed)
{
    if (wellFormed) *wellFormed = false;
    Info info;
    if (xml.isEmpty()) return info;

    QXmlStreamReader r(xml);
    // The role fields, collected separately and folded at the end so ROLE ORDER decides the credit order
    // however the document happened to lay them out.
    QStringList writers, pencillers, inkers, colorists, letterers, coverArtists;
    bool inComicInfo = false;
    int depth = 0;

    while (!r.atEnd())
    {
        const QXmlStreamReader::TokenType t = r.readNext();
        if (r.hasError()) break;
        if (t == QXmlStreamReader::StartElement)
        {
            ++depth;
            const QString name = r.name().toString();
            if (depth == 1)
            {
                // NOT OUR FORMAT unless the root says so. An OPF, an .nfo's XML or a stray settings file
                // that happened to be called ComicInfo.xml yields an empty Info rather than fields scraped
                // out of somebody else's document.
                inComicInfo = (name.compare(QStringLiteral("ComicInfo"), Qt::CaseInsensitive) == 0);
                continue;
            }
            if (!inComicInfo || depth != 2) continue;

            // <Pages> IS READ PAST WITHOUT A WORD (issue #152 scope): per-page double-spread hints are a
            // reader feature this app does not have, and skipping the subtree here is what keeps its
            // <Page .../> children from being mistaken for fields.
            if (name.compare(QStringLiteral("Pages"), Qt::CaseInsensitive) == 0)
            {
                r.skipCurrentElement();
                --depth;
                continue;
            }

            const QString text = r.readElementText(QXmlStreamReader::IncludeChildElements).simplified();
            --depth;    // readElementText consumed this element's EndElement
            if (text.isEmpty()) continue;

            const QString n = name.toLower();
            if      (n == QStringLiteral("series"))      info.series = text;
            else if (n == QStringLiteral("number"))      info.number = text;
            else if (n == QStringLiteral("volume"))      info.volume = intOf(text);
            else if (n == QStringLiteral("title"))       info.title = text;
            else if (n == QStringLiteral("summary"))     info.summary = text;
            else if (n == QStringLiteral("year"))        info.year = intOf(text);
            else if (n == QStringLiteral("month"))       info.month = intOf(text);
            else if (n == QStringLiteral("day"))         info.day = intOf(text);
            else if (n == QStringLiteral("publisher"))   info.publisher = text;
            else if (n == QStringLiteral("genre"))       info.genre = text;
            else if (n == QStringLiteral("languageiso")) info.language = text;
            else if (n == QStringLiteral("web"))         info.web = text;
            else if (n == QStringLiteral("pagecount"))   info.pageCount = intOf(text);
            else if (n == QStringLiteral("agerating"))   info.rating = ratingFor(text);
            else if (n == QStringLiteral("manga"))       info.direction = directionFor(text);
            else if (n == QStringLiteral("writer"))      writers      += splitNames(text);
            else if (n == QStringLiteral("penciller"))   pencillers   += splitNames(text);
            else if (n == QStringLiteral("inker"))       inkers       += splitNames(text);
            else if (n == QStringLiteral("colorist"))    colorists    += splitNames(text);
            else if (n == QStringLiteral("letterer"))    letterers    += splitNames(text);
            else if (n == QStringLiteral("coverartist")) coverArtists += splitNames(text);
            // Every other field of the standard — Notes, Characters, StoryArc, AlternateSeries and the rest
            // — is read past on purpose. This app has nowhere to show them, and a field carried but never
            // rendered is an index that grew for nothing.
        }
        else if (t == QXmlStreamReader::EndElement)
        {
            --depth;
        }
    }

    if (wellFormed) *wellFormed = !r.hasError();

    addCreators(info.creators, writers);
    addCreators(info.creators, pencillers);
    addCreators(info.creators, inkers);
    addCreators(info.creators, colorists);
    addCreators(info.creators, letterers);
    addCreators(info.creators, coverArtists);
    // THE PRIMARY AUTHOR IS THE WRITER, which is the credit a comics shelf is grouped by everywhere else in
    // the world. An issue with no writer named has no author — never the penciller promoted into the slot,
    // because "Author: <artist>" is a statement about the book that the book did not make.
    if (!writers.isEmpty()) info.author = writers.first();

    return info;
}

// ---- The tables ---------------------------------------------------------------------------------------------

Rating ratingFor(const QString& ageRating)
{
    // ComicRack's vocabulary, all fifteen values, collapsed onto five rungs. EXACT matches only: a value not
    // in this table is Unrated, never the nearest-looking rung.
    static const struct { const char* value; Rating rating; } kTable[] = {
        { "unknown",            Rating::Unrated    },
        { "rating pending",     Rating::Unrated    },
        { "early childhood",    Rating::Everyone   },
        { "everyone",           Rating::Everyone   },
        { "g",                  Rating::Everyone   },
        { "kids to adults",     Rating::Everyone   },
        { "everyone 10+",       Rating::Everyone10 },
        { "pg",                 Rating::Everyone10 },
        { "teen",               Rating::Teen       },
        { "ma15+",              Rating::Mature     },   // higher rung of the two it straddles: see the header
        { "mature 17+",         Rating::Mature     },
        { "m",                  Rating::Mature     },
        { "r18+",               Rating::Adults     },
        { "adults only 18+",    Rating::Adults     },
        { "x18+",               Rating::Adults     },
    };
    const QString v = foldValue(ageRating);
    if (v.isEmpty()) return Rating::Unrated;
    for (const auto& row : kTable)
        if (v == QLatin1String(row.value)) return row.rating;
    return Rating::Unrated;
}

Direction directionFor(const QString& manga)
{
    const QString v = foldValue(manga);
    if (v == QStringLiteral("yesandrighttoleft")) return Direction::RightToLeft;
    if (v == QStringLiteral("yesandlefttoright")) return Direction::LeftToRight;
    // "Yes" / "No" / "Unknown" say whether it IS manga, not which way it reads. A manga published in
    // translation reads left to right and its taggers write exactly "Yes", so reading a direction out of
    // that value would flip a shelf of them.
    return Direction::Unspecified;
}

bool hiddenWhenRestricted(Rating r)
{
    return r == Rating::Mature || r == Rating::Adults;
}

Direction resolveDirection(Direction embedded, Direction userOverride)
{
    if (userOverride != Direction::Unspecified) return userOverride;   // the user is above all
    if (embedded != Direction::Unspecified) return embedded;
    return Direction::LeftToRight;   // the direction the reader has always used
}

double numberAsIndex(const QString& number)
{
    const QString s = number.simplified();
    int i = 0;
    while (i < s.size() && s.at(i).isDigit()) ++i;
    if (i == 0) return 0.0;                 // "Annual 1", "Special", "½": unnumbered, and sorts last
    int end = i;
    if (end < s.size() && s.at(end) == QLatin1Char('.'))
    {
        int j = end + 1;
        while (j < s.size() && s.at(j).isDigit()) ++j;
        if (j > end + 1) end = j;           // "1.5" — Calibre's half-issue, and the reason this is a double
    }
    bool ok = false;
    const double v = s.left(end).toDouble(&ok);
    return ok ? v : 0.0;
}

} // namespace ComicInfo
