#include "BookLibrary.h"
#include "AppPaths.h"
#include "BookMeta.h"
#include "NaturalOrder.h"
#include "Settings.h"
#include "../comic/ComicName.h"
#include "../ebook/Fb2Meta.h"   // isFb2Path: .fb2.zip is claimed by NAME, not by suffix (#144)

#include <QCollator>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QPair>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <limits>

namespace BookLibrary
{
namespace
{
    // The natural (numeric-aware, case-insensitive) collator, built ONCE and through NaturalOrder — never
    // inline. A plain `QCollator c; c.setNumericMode(true);` is INERT under the C locale, so "Volume 10"
    // sorts before "Volume 2" with nothing said (issue #205) — and that trap was FOUND in this app's comic
    // page order, which makes it exactly this feature's business.
    const QCollator& naturalCollator()
    {
        static QCollator coll = NaturalOrder::collator();
        return coll;
    }

    // Case- and whitespace-insensitive grouping. "Ursula K. Le Guin" and "ursula k. le guin " are ONE
    // author; the first spelling encountered is what gets displayed and only the key is folded.
    QString foldKey(const QString& s) { return s.simplified().toCaseFolded(); }

    // An unnumbered book sorts AFTER every numbered one inside a series, so a stray untagged volume does not
    // sit at the head of a shelf that is otherwise in order.
    double indexRank(double idx)
    {
        return idx > 0.0 ? idx : std::numeric_limits<double>::max();
    }

    // The order of a bucket's books: series name, then the book's place in that series, then its title. For
    // an author with no series anywhere — most of them — every book has an empty series name and this is
    // exactly title order; for an author of two series, each series groups together and reads in order.
    void sortBooks(QVector<Book>& books)
    {
        std::sort(books.begin(), books.end(), [](const Book& a, const Book& b) {
            const QString sa = foldKey(a.series), sb = foldKey(b.series);
            if (sa != sb) return naturalCollator().compare(sa, sb) < 0;
            if (indexRank(a.seriesIndex) != indexRank(b.seriesIndex))
                return indexRank(a.seriesIndex) < indexRank(b.seriesIndex);
            // TWO UNNUMBERED ISSUES ARE SEPARATED BY THE NUMBER THEY DO CARRY (issue #152). ComicInfo's
            // <Number> is free text: "Annual 1", "Special", "½" all have an indexRank of "last", and
            // without this they would fall through to the title — which for a comic is usually the same
            // string for every one of them, leaving "Annual 10" ahead of "Annual 2". Natural order, through
            // the one collator, for the same reason everything else here does (#205).
            if (a.number != b.number)
            {
                const int c = naturalCollator().compare(a.number, b.number);
                if (c != 0) return c < 0;
            }
            return naturalCollator().compare(a.title, b.title) < 0;
        });
    }

    // Buckets are sorted by display name with the UNKNOWN one LAST, so a pile of untagged files is not the
    // first thing the browse shows — and is never hidden either.
    void sortBuckets(QVector<Author>& buckets)
    {
        std::sort(buckets.begin(), buckets.end(), [](const Author& a, const Author& b) {
            if (a.name.isEmpty() != b.name.isEmpty()) return b.name.isEmpty();
            return naturalCollator().compare(a.name, b.name) < 0;
        });
        for (Author& a : buckets) sortBooks(a.books);
    }

    // Fold the books that name a series into buckets keyed by it. Written as the one-dimension version of
    // AudiobookLibrary::bucketBy, so that a second dimension later (language, publisher) is one more call
    // rather than a second idiom.
    QVector<Series> bucketBySeries(const QVector<Author>& authors)
    {
        QVector<Series> out;
        QHash<QString, int> at;
        for (const Author& a : authors)
            for (const Book& b : a.books)
            {
                const QString value = b.series.trimmed();
                const QString key   = seriesKeyFor(value);
                if (key.isEmpty()) continue;      // THE GATE: a file that names nothing mints no bucket
                int i = at.value(key, -1);
                if (i < 0)
                {
                    Series bucket;
                    bucket.key  = key;
                    bucket.name = value;          // display spelling: the first one seen
                    i = out.size();
                    out.push_back(bucket);
                    at.insert(key, i);
                }
                out[i].books.push_back(b);        // a COPY: the book still lives under its author
            }
        sortBuckets(out);
        return out;
    }
}

bool isReadingFile(const QString& path)
{
    // FB2 first, and by whole name: the zipped wire form is "book.fb2.zip", whose suffix() is "zip" — which
    // is NOT in the set below and must not be, because "a zip in a books folder is a comic" is a guess with
    // no marker behind it (the header says so at length).
    if (Fb2Meta::isFb2Path(path)) return true;
    const QString e = QFileInfo(path).suffix().toLower();
    // The whole extension set, in one place. See the header for why .cb7, .cbt and a bare .zip are still not
    // in it — each is a deliberate refusal with a cost behind it, not an oversight.
    return e == QStringLiteral("epub") || e == QStringLiteral("pdf")
        || e == QStringLiteral("cbz")  || e == QStringLiteral("cbr")
        || e == QStringLiteral("azw3") || e == QStringLiteral("azw") || e == QStringLiteral("mobi")
        || e == QStringLiteral("txt")  || e == QStringLiteral("text")
        || e == QStringLiteral("md")   || e == QStringLiteral("markdown")
        || e == QStringLiteral("mdown")|| e == QStringLiteral("mkd")
        || e == QStringLiteral("html") || e == QStringLiteral("htm");   // #259: a single-file HTML book
}

Kind kindFor(const QString& path)
{
    const QString e = QFileInfo(path).suffix().toLower();
    return (e == QStringLiteral("cbz") || e == QStringLiteral("cbr")) ? Kind::Comic : Kind::Book;
}

bool isSavedPageSupportFolder(const QString& dirPath, const QStringList& siblingNames)
{
    // The one suffix both browsers write today (the header cites where). A table of localised words would be
    // a list nobody here can source; see the header for what an unrecognised one costs.
    static const QLatin1String kSuffix("_files");
    static const QLatin1String kPageExts[] = { QLatin1String(".html"), QLatin1String(".htm") };

    // The folder's own name, and nothing above it. cleanPath drops a trailing separator; QFileInfo::fileName
    // is string work and reads no disk.
    const QString name = QFileInfo(QDir::cleanPath(dirPath)).fileName();
    if (name.size() <= kSuffix.size() || !name.endsWith(kSuffix, Qt::CaseInsensitive)) return false;
    const QString stem = name.left(name.size() - kSuffix.size());

    // THE PAIRING: a sibling that is exactly <stem> plus a page extension. Whole-name equality, so a stem
    // that matches only as a prefix ("notes-old.html", "my notes.html") pairs nothing.
    for (const QString& sib : siblingNames)
        for (const QLatin1String& ext : kPageExts)
            if (sib.size() == stem.size() + ext.size()
                && sib.endsWith(ext, Qt::CaseInsensitive)
                && sib.startsWith(stem, Qt::CaseInsensitive))
                return true;
    return false;
}

QString authorKeyFor(const QString& author) { return foldKey(author); }
QString seriesKeyFor(const QString& series) { return ComicName::seriesKey(series); }

QString bookKeyFor(const QString& path)
{
    // THE PATH IS THE IDENTITY. One file is one book (the header says why this library needs no folder key
    // at all), so there is nothing to compose and nothing that could collide short of two names for one
    // file. Folded because Windows filesystems are case-insensitive and a rescan that saw "C:/Books" once
    // and "c:/books" the next time must not mint a second copy of every book.
    return foldKey(path);
}

QVector<FileEntry> scanFolder(const QString& root, const QHash<QString, FileEntry>& known, ScanStats* stats)
{
    QVector<FileEntry> out;
    ScanStats s;
    if (root.isEmpty() || !QFileInfo::exists(root))
    {
        // Nothing configured, or the folder went away with the drive it was on. Dormant, instant, and NOT a
        // reason to forget what we knew: `known` is left alone, so plugging the drive back in re-uses it.
        if (stats) *stats = s;
        return out;
    }

    // A SAVED WEB PAGE'S FOLDER, AND EVERYTHING UNDER IT (#360). A directory counts as "inside" when it is
    // a `<stem>_files` folder paired with its page (isSavedPageSupportFolder) or sits anywhere below one, so
    // a nested pair inside a saved page's folder is covered by the outer one. Judged once per directory and
    // memoised, and only for directories that hold a reading file, so a library with no saved pages pays one
    // hash lookup per book. THE ROOT ITSELF IS NEVER JUDGED: a user who points the library at one of these
    // folders has said what they want read.
    const QString rootAbs = QDir::cleanPath(QFileInfo(root).absoluteFilePath());
    QHash<QString, bool> insideSavedPage;
    std::function<bool(const QString&)> isInsideSavedPage = [&](const QString& dir) -> bool {
        if (dir.size() <= rootAbs.size()) return false;
        const auto hit = insideSavedPage.constFind(dir);
        if (hit != insideSavedPage.constEnd()) return *hit;
        const QString parent = QFileInfo(dir).path();   // string work; "C:/Books" -> "C:/", never "C:"
        const bool inside = isInsideSavedPage(parent)
            || isSavedPageSupportFolder(dir, QDir(parent).entryList(QDir::Files | QDir::Hidden
                                                                    | QDir::NoDotAndDotDot));
        insideSavedPage.insert(dir, inside);
        return inside;
    };

    QDirIterator it(root, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext())
    {
        it.next();
        const QFileInfo fi = it.fileInfo();
        // Extension-only, before anything is opened: a cover.jpg, a .nfo, a loose folder of scanned pages
        // that is not an archive at all — each costs one string compare and is not read.
        if (!isReadingFile(fi.filePath())) continue;
        // BEFORE the cache is consulted, so an index written by an older build cannot bring a frame back:
        // it is simply not found, and counts as dropped below.
        if (isInsideSavedPage(QDir::cleanPath(fi.absolutePath())))
        {
            ++s.savedPageSkipped;
            continue;
        }
        const QString abs  = fi.absoluteFilePath();
        const qint64 mtime = fi.lastModified().toSecsSinceEpoch();
        const qint64 size  = fi.size();
        ++s.files;

        // THE INCREMENTAL DECISION, and the only one. Same path, same mtime, same size => the bytes we
        // already parsed are still the bytes on disk, so the file is not opened at all. Size is checked as
        // well as mtime because an editor that rewrites a file can preserve the timestamp (and archives
        // restored from backup routinely do), while almost nothing preserves the length too.
        const auto cached = known.constFind(abs);
        if (cached != known.constEnd() && cached->mtime == mtime && cached->size == size)
        {
            ++s.reused;
            out.push_back(*cached);
            continue;
        }

        const BookMeta::Info info = BookMeta::read(abs);
        ++s.reread;

        FileEntry e;
        e.path = abs; e.mtime = mtime; e.size = size;
        e.kind        = kindFor(abs);
        e.title       = info.title;
        e.author      = info.author;
        e.series      = info.series;
        e.seriesIndex = info.seriesIndex;
        e.language    = info.language;
        e.year        = info.year;
        e.pageCount   = info.pageCount;
        e.hasCover    = info.hasCover;
        e.untagged    = info.isEmpty();
        // ComicInfo.xml's fields (#152). Defaults for everything else the scan reads, so nothing but a comic
        // archive with a document in it can put a value in one of these.
        e.number      = info.number;
        e.volume      = info.volume;
        e.summary     = info.summary;
        e.month       = info.month;
        e.day         = info.day;
        e.creators    = info.creators;
        e.publisher   = info.publisher;
        e.genre       = info.genre;
        e.web         = info.web;
        e.rating      = info.rating;
        e.direction   = info.direction;
        out.push_back(e);
    }

    // Everything `known` held that the walk did not find is gone from the disk, and therefore gone from the
    // library — the scan is authoritative about what exists. Counted rather than acted on: the caller's next
    // save writes `out`, which already omits them.
    int kept = 0;
    for (const FileEntry& e : out)
        if (known.contains(e.path)) ++kept;
    s.dropped = int(known.size()) - kept;
    if (s.dropped < 0) s.dropped = 0;   // a `known` with paths outside this root is the caller's business

    if (stats) *stats = s;
    return out;
}

QHash<QString, FileEntry> byPath(const QVector<FileEntry>& entries)
{
    QHash<QString, FileEntry> out;
    out.reserve(entries.size());
    for (const FileEntry& e : entries) out.insert(e.path, e);
    return out;
}

Index buildIndex(const QVector<FileEntry>& entries)
{
    // Sort the input by natural path order FIRST, so everything decided by "first one seen" — an author's
    // display capitalisation, a series' spelling — is a property of the library rather than of whatever
    // order QDirIterator happened to hand back on this filesystem. Two runs must build the same index from
    // the same disk. It also fixes the order ComicName::group sees, which matters because a folder's
    // corroboration count is order-independent but its display spellings are not.
    QVector<FileEntry> sorted = entries;
    std::sort(sorted.begin(), sorted.end(), [](const FileEntry& a, const FileEntry& b) {
        return naturalCollator().compare(a.path, b.path) < 0;
    });

    // ---- COMIC SERIES, DERIVED PER FOLDER, HERE AND NEVER ON A CACHED ENTRY ----------------------------
    // The whole conservatism of the comic rule is that a bare trailing number only counts as an issue
    // number when a SIBLING agrees (ComicName.h). The evidence is therefore the folder's current contents,
    // and storing a verdict on a FileEntry would freeze it: drop a second issue of something into a folder
    // and the first one would go on standing alone until its mtime happened to change. So the grouping is
    // recomputed every time an index is built, which is per scan, over strings already in memory.
    QVector<ComicName::Grouped> comicOf(sorted.size());
    {
        QHash<QString, QVector<int>> byFolder;
        for (int i = 0; i < sorted.size(); ++i)
            if (sorted[i].kind == Kind::Comic)
                byFolder[foldKey(QFileInfo(sorted[i].path).absolutePath())].push_back(i);
        for (auto f = byFolder.constBegin(); f != byFolder.constEnd(); ++f)
        {
            QStringList names;
            names.reserve(f.value().size());
            for (int i : f.value()) names << QFileInfo(sorted[i].path).completeBaseName();
            const QVector<ComicName::Grouped> g = ComicName::group(names);
            for (int n = 0; n < f.value().size() && n < g.size(); ++n) comicOf[f.value().at(n)] = g.at(n);
        }
    }

    Index idx;
    QHash<QString, int> authorAt;   // author key -> position in idx.authors

    for (int i = 0; i < sorted.size(); ++i)
    {
        const FileEntry& e = sorted.at(i);

        Book b;
        b.key       = bookKeyFor(e.path);
        b.path      = e.path;
        b.kind      = e.kind;
        b.folder    = QFileInfo(e.path).absolutePath();
        b.year      = e.year;
        b.pageCount = e.pageCount;
        b.hasCover  = e.hasCover;
        b.author    = e.author.trimmed();
        // Carried for every kind, because they are all defaulted for anything that is not a comic with a
        // ComicInfo.xml in it (#152) — there is no branch to get wrong.
        b.number    = e.number;
        b.volume    = e.volume;
        b.summary   = e.summary;
        b.creators  = e.creators;
        b.publisher = e.publisher;
        b.genre     = e.genre;
        b.language  = e.language;
        b.web       = e.web;
        b.rating    = e.rating;
        b.direction = e.direction;

        if (e.kind == Kind::Comic)
        {
            // WHAT THE DOCUMENT SAID BEATS WHAT THE NAME SUGGESTS, dimension by dimension (see the header).
            // ComicName has already read the filename for the whole folder; each dimension below takes that
            // reading only where the archive's own ComicInfo.xml was silent.
            const ComicName::Grouped& g = comicOf.at(i);
            const QString embeddedTitle  = e.title.trimmed();
            const QString embeddedSeries = e.series.trimmed();

            // TITLE. Most issues carry no <Title> — the series and the number are the identification — so
            // the overwhelmingly common case here is the filename reading, unchanged. ComicName's `title`
            // is never empty by contract, so "an untagged file must still appear" holds with no second
            // fallback.
            b.title             = embeddedTitle.isEmpty() ? g.title : embeddedTitle;
            b.titleFromFilename = embeddedTitle.isEmpty();

            // SERIES + NUMBER, together or not at all: a series from the document and a number from the
            // filename would file an issue in the right shelf at the wrong place in it.
            if (!embeddedSeries.isEmpty() || !e.number.isEmpty())
            {
                b.series      = embeddedSeries;
                b.seriesIndex = e.seriesIndex;   // ComicInfo::numberAsIndex(<Number>), applied at scan time
            }
            else
            {
                b.series      = g.series;
                b.seriesIndex = g.number;
            }
        }
        else
        {
            b.title             = e.title.trimmed();
            b.titleFromFilename = b.title.isEmpty();
            // THE FALLBACK THAT MAKES AN UNTAGGED BOOK VISIBLE. completeBaseName keeps "Dune Part 2.rev3"
            // whole and drops only the final extension, which is what a person reading the folder would
            // call the file.
            if (b.title.isEmpty()) b.title = QFileInfo(e.path).completeBaseName();
            b.series      = e.series.trimmed();
            b.seriesIndex = e.seriesIndex;
        }

        if (e.kind == Kind::Comic) ++idx.comicCount; else ++idx.bookCount;

        const QString aKey = authorKeyFor(b.author);
        int ai = authorAt.value(aKey, -1);
        if (ai < 0)
        {
            Author a;
            a.key  = aKey;
            a.name = b.author;      // display spelling: the first one seen, in natural path order
            ai = idx.authors.size();
            idx.authors.push_back(a);
            authorAt.insert(aKey, ai);
        }
        idx.authors[ai].books.push_back(b);
    }

    sortBuckets(idx.authors);

    // THE VIEW, built after every book exists so a bucket holds finished copies. It is EMPTY for a library
    // whose files name no series, which is the compatibility gate the browse checks before offering the
    // dimension at all.
    idx.series = bucketBySeries(idx.authors);
    return idx;
}

Index filterForProfile(const Index& idx, bool restricted)
{
    if (!restricted) return idx;   // the ONLY thing an unrestricted profile pays is the copy it asked for

    Index out;
    for (const Author& a : idx.authors)
    {
        Author kept;
        kept.key  = a.key;
        kept.name = a.name;
        for (const Book& b : a.books)
        {
            if (ComicInfo::hiddenWhenRestricted(b.rating)) continue;
            kept.books.push_back(b);
            if (b.kind == Kind::Comic) ++out.comicCount; else ++out.bookCount;
        }
        // AN AUTHOR WITH NOTHING LEFT IS NOT A BUCKET. A shelf listing a name that opens onto an empty page
        // tells a child exactly what was taken away and who wrote it.
        if (!kept.books.isEmpty()) out.authors.push_back(kept);
    }
    // The series view is REBUILT rather than filtered, so a series whose every issue was hidden stops
    // existing and the door to the dimension closes with it — the same compatibility gate an untagged
    // library goes through.
    out.series = bucketBySeries(out.authors);
    return out;
}

const Author* Index::author(const QString& authorKey) const
{
    for (const Author& a : authors)
        if (a.key == authorKey) return &a;
    return nullptr;
}

const Series* Index::seriesFor(const QString& seriesKey) const
{
    for (const Series& s : series)
        if (s.key == seriesKey) return &s;
    return nullptr;
}

const Book* Index::book(const QString& bookKey) const
{
    // AUTHORS ONLY — the canonical home. Looking in the series copies as well would answer the same
    // question twice from two places that are only equal by construction.
    for (const Author& a : authors)
        for (const Book& b : a.books)
            if (b.key == bookKey) return &b;
    return nullptr;
}

QString displayAuthor(const Author& a)
{
    return a.name.trimmed().isEmpty() ? QObject::tr("Unknown Author") : a.name.trimmed();
}

// ---------------------------------------------------------------------------------------------------------
// Persistence. Its own file, its own version, its own stamp — nothing here can change what the music or the
// audiobook index holds or when it re-reads.
// ---------------------------------------------------------------------------------------------------------
namespace
{
    const int kIndexFileVersion = 1;
    // Bump when BookMeta starts reading something new, or when the scan starts making something new of what
    // it reads. 1 == issue #134, increment 1. NOTE that the COMIC grouping rule is deliberately NOT part of
    // this stamp: it is derived in buildIndex from data the cache already holds, so changing it takes effect
    // on the next index build with no re-read of anything.
    // 2 == issue #144: BookMeta learned .cbr, .fb2/.fb2.zip, .mobi/.azw/.azw3 and .txt/.md, so every
    //      library cached under rules 1 must be re-read to pick the new files (and the corrected MOBI
    //      title offset) up. Nothing else about the scan changed.
    // 3 == issue #152: BookMeta reads a comic archive's ComicInfo.xml, so every cached comic holds a
    //      series, a number, creators and a rating that were never looked for. Nothing else can supply
    //      them — the cache is keyed on mtime+size and those have not changed — so the whole cache is
    //      dropped once and every file re-read.
    //
    //      WHAT THAT COSTS, ONCE. One cold scan of the reading root: the same walk #134's first run makes,
    //      which opens each container for its metadata and its page count and inflates no page image (a CBZ
    //      is a central-directory read, a CBR a header walk, an EPUB one member, a PDF a PDFium load). It
    //      runs on the scan's worker thread with the previous index still installed, so the shelf stays up
    //      and in its old grouping until the new one replaces it, and the covers already in the art cache
    //      are keyed by book and are NOT re-extracted. A library of a few thousand files pays seconds, once,
    //      in the background — against re-reading nothing and never learning what half the collection says
    //      about itself.
    const int kRules = 3;

    // The persisted spellings of the two ComicInfo enums. WRITTEN AS EXPLICIT NUMBERS rather than as a cast
    // of the enumerator, so that inserting a rung into ComicInfo::Rating later cannot silently re-label
    // every stored index — a stored 4 has to go on meaning Mature whatever position Mature occupies in the
    // C++ enum. An unknown stored value reads as the neutral one, never as a rung.
    int storedRating(ComicInfo::Rating r)
    {
        switch (r)
        {
        case ComicInfo::Rating::Unrated:    return 0;
        case ComicInfo::Rating::Everyone:   return 1;
        case ComicInfo::Rating::Everyone10: return 2;
        case ComicInfo::Rating::Teen:       return 3;
        case ComicInfo::Rating::Mature:     return 4;
        case ComicInfo::Rating::Adults:     return 5;
        }
        return 0;
    }
    ComicInfo::Rating ratingFromStored(int v)
    {
        switch (v)
        {
        case 1: return ComicInfo::Rating::Everyone;
        case 2: return ComicInfo::Rating::Everyone10;
        case 3: return ComicInfo::Rating::Teen;
        case 4: return ComicInfo::Rating::Mature;
        case 5: return ComicInfo::Rating::Adults;
        default: return ComicInfo::Rating::Unrated;
        }
    }
    int storedDirection(ComicInfo::Direction d)
    {
        switch (d)
        {
        case ComicInfo::Direction::Unspecified: return 0;
        case ComicInfo::Direction::LeftToRight: return 1;
        case ComicInfo::Direction::RightToLeft: return 2;
        }
        return 0;
    }
    ComicInfo::Direction directionFromStored(int v)
    {
        if (v == 1) return ComicInfo::Direction::LeftToRight;
        if (v == 2) return ComicInfo::Direction::RightToLeft;
        return ComicInfo::Direction::Unspecified;
    }
}

QString parseStamp() { return QString::number(kRules); }

QVector<FileEntry> loadIndexFile(const QString& filePath, QString* rulesUsed)
{
    if (rulesUsed) rulesUsed->clear();
    QVector<FileEntry> out;
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) return out;
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    if (root.value(QStringLiteral("version")).toInt() != kIndexFileVersion) return out;
    if (rulesUsed) *rulesUsed = root.value(QStringLiteral("rules")).toString();

    const QJsonArray files = root.value(QStringLiteral("files")).toArray();
    out.reserve(files.size());
    for (const QJsonValue& v : files)
    {
        const QJsonObject o = v.toObject();
        FileEntry e;
        e.path = o.value(QStringLiteral("p")).toString();
        if (e.path.isEmpty()) continue;                  // an entry with no path can key nothing
        e.mtime = qint64(o.value(QStringLiteral("m")).toDouble());
        e.size  = qint64(o.value(QStringLiteral("s")).toDouble());
        // The KIND is re-derived from the path rather than trusted from the file: it is a pure function of
        // the extension, so a stored one could only ever disagree with the truth.
        e.kind        = kindFor(e.path);
        e.title       = o.value(QStringLiteral("ti")).toString();
        e.author      = o.value(QStringLiteral("au")).toString();
        e.series      = o.value(QStringLiteral("se")).toString();
        e.seriesIndex = o.value(QStringLiteral("si")).toDouble();
        e.language    = o.value(QStringLiteral("lg")).toString();
        e.year        = o.value(QStringLiteral("yr")).toInt();
        e.pageCount   = o.value(QStringLiteral("pc")).toInt();
        e.hasCover    = o.value(QStringLiteral("cv")).toBool();
        e.untagged    = o.value(QStringLiteral("nt")).toBool();
        // ComicInfo.xml (#152). Absent for every entry written by an older build — which is exactly what
        // the parse stamp below drops the whole cache over, so none of these is ever half-populated.
        e.number      = o.value(QStringLiteral("nm")).toString();
        e.volume      = o.value(QStringLiteral("vo")).toInt();
        e.summary     = o.value(QStringLiteral("sm")).toString();
        e.month       = o.value(QStringLiteral("mo")).toInt();
        e.day         = o.value(QStringLiteral("dy")).toInt();
        e.publisher   = o.value(QStringLiteral("pb")).toString();
        e.genre       = o.value(QStringLiteral("gn")).toString();
        e.web         = o.value(QStringLiteral("wb")).toString();
        e.rating      = ratingFromStored(o.value(QStringLiteral("ar")).toInt());
        e.direction   = directionFromStored(o.value(QStringLiteral("dr")).toInt());
        const QJsonArray cr = o.value(QStringLiteral("cs")).toArray();
        for (const QJsonValue& c : cr)
        {
            const QString name = c.toString().trimmed();
            if (!name.isEmpty()) e.creators.append(name);
        }
        out.push_back(e);
    }
    return out;
}

bool saveIndexFile(const QString& filePath, const QVector<FileEntry>& entries)
{
    QJsonArray files;
    for (const FileEntry& e : entries)
    {
        QJsonObject o;
        o.insert(QStringLiteral("p"), e.path);
        o.insert(QStringLiteral("m"), double(e.mtime));
        o.insert(QStringLiteral("s"), double(e.size));
        if (!e.title.isEmpty())    o.insert(QStringLiteral("ti"), e.title);
        if (!e.author.isEmpty())   o.insert(QStringLiteral("au"), e.author);
        if (!e.series.isEmpty())   o.insert(QStringLiteral("se"), e.series);
        if (e.seriesIndex > 0.0)   o.insert(QStringLiteral("si"), e.seriesIndex);
        if (!e.language.isEmpty()) o.insert(QStringLiteral("lg"), e.language);
        if (e.year)                o.insert(QStringLiteral("yr"), e.year);
        if (e.pageCount)           o.insert(QStringLiteral("pc"), e.pageCount);
        if (e.hasCover)            o.insert(QStringLiteral("cv"), true);
        if (e.untagged)            o.insert(QStringLiteral("nt"), true);
        // ComicInfo.xml (#152), default-valued fields omitted exactly as above — a library with no tagged
        // comic in it writes a file byte-identical to the one the previous build wrote.
        if (!e.number.isEmpty())    o.insert(QStringLiteral("nm"), e.number);
        if (e.volume)               o.insert(QStringLiteral("vo"), e.volume);
        if (!e.summary.isEmpty())   o.insert(QStringLiteral("sm"), e.summary);
        if (e.month)                o.insert(QStringLiteral("mo"), e.month);
        if (e.day)                  o.insert(QStringLiteral("dy"), e.day);
        if (!e.publisher.isEmpty()) o.insert(QStringLiteral("pb"), e.publisher);
        if (!e.genre.isEmpty())     o.insert(QStringLiteral("gn"), e.genre);
        if (!e.web.isEmpty())       o.insert(QStringLiteral("wb"), e.web);
        if (e.rating != ComicInfo::Rating::Unrated)
            o.insert(QStringLiteral("ar"), storedRating(e.rating));
        if (e.direction != ComicInfo::Direction::Unspecified)
            o.insert(QStringLiteral("dr"), storedDirection(e.direction));
        if (!e.creators.isEmpty())
        {
            QJsonArray cr;
            for (const QString& c : e.creators) cr.append(c);
            o.insert(QStringLiteral("cs"), cr);
        }
        files.append(o);
    }
    QJsonObject root;
    root.insert(QStringLiteral("version"), kIndexFileVersion);
    root.insert(QStringLiteral("rules"), parseStamp());
    root.insert(QStringLiteral("files"), files);

    QDir().mkpath(QFileInfo(filePath).absolutePath());
    QFile f(filePath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    return f.write(QJsonDocument(root).toJson(QJsonDocument::Compact)) >= 0;
}

// ---- WHERE A PERSON IS IN A BOOK (#134 increment 2) ---------------------------------------------------
//
// Pure, and deliberately short: the argument for every line of it is in the header, and the value of having
// it in ONE function is that a tile's badge, the Continue-reading shelf and anything added later are the
// same answer read three ways rather than three re-derivations that agree until one of them is edited.

Progress progressFor(const Book& b, const ReadState& st)
{
    Progress p;
    p.page     = qMax(0, st.furthestPage);
    p.lastRead = st.lastRead;

    // THE AUTOMATIC ANSWER. Reaching the last page finishes a book; getting PAST the first page starts one.
    // Page one alone is not a start — it is what opening a file looks like, and calling it progress is how a
    // "continue reading" shelf fills up with books nobody is reading (RecentStore is where an open belongs).
    ItemMarks::Completion autoC = ItemMarks::Completion::None;
    if (b.pageCount > 0 && p.page >= b.pageCount) autoC = ItemMarks::Completion::Finished;
    else if (p.page >= 2)                         autoC = ItemMarks::Completion::InProgress;

    // ...AND THE PERSON BEATS IT, in both directions and without exception. See the header: nothing in this
    // feature ever writes ItemMarks, so a non-None mark here can only be one somebody set.
    const bool said = st.userMark != ItemMarks::Completion::None;
    p.completion = said ? st.userMark : autoC;
    p.fromUser   = said;

    p.finished = p.completion == ItemMarks::Completion::Finished;
    p.started  = p.finished || p.completion == ItemMarks::Completion::InProgress || p.page >= 2;

    // THE FRACTION, and the one case that must not produce one. A finished book is at 1.0 whichever way it
    // got there — a bar that stops at 94% under the word "Finished" is a disagreement between two things
    // that are supposed to be one answer.
    if (p.finished)
    {
        p.known = true;
        p.fraction = 1.0;
    }
    else if (b.pageCount > 0 && p.page > 0)
    {
        p.known = true;
        p.fraction = qBound(0.0, double(p.page) / double(b.pageCount), 1.0);
    }
    else
    {
        p.known = false;      // no denominator, or nothing read: a state may still be shown, a number may not
        p.fraction = 0.0;
    }
    return p;
}

bool continueReading(const Progress& p)
{
    return p.completion == ItemMarks::Completion::InProgress;
}

QVector<Book> continueReadingBooks(const Index& idx, const std::function<Progress(const Book&)>& progressOf)
{
    QVector<Book> out;
    if (!progressOf) return out;
    QVector<QPair<qint64, Book>> keyed;
    for (const Author& a : idx.authors)          // authors only: series holds copies of these same books
        for (const Book& b : a.books)
        {
            if (b.key.isEmpty()) continue;
            const Progress p = progressOf(b);
            if (!continueReading(p)) continue;
            keyed.push_back({ p.lastRead, b });
        }
    // Most recently read first. A book somebody marked "In progress" by hand and never opened carries no
    // time at all, so it lands at the end in natural title order rather than at an arbitrary place.
    std::stable_sort(keyed.begin(), keyed.end(),
                     [](const QPair<qint64, Book>& x, const QPair<qint64, Book>& y) {
                         if (x.first != y.first) return x.first > y.first;
                         return naturalCollator().compare(x.second.title, y.second.title) < 0;
                     });
    out.reserve(keyed.size());
    for (const QPair<qint64, Book>& k : keyed) out.push_back(k.second);
    return out;
}

// ---- ONLINE BLANK-FILLING (#134 increment 2) ----------------------------------------------------------

QVector<Book> enrichmentTargets(const Index& idx, const HasCoverFn& hasCover, bool enabled)
{
    QVector<Book> out;
    // THE WHOLE OPT-IN, in one line and before anything is walked. With the setting off there is no list, so
    // there is nothing for a caller to ask about — which is the shape that makes "zero requests" assertable.
    if (!enabled) return out;
    for (const Author& a : idx.authors)          // authors only, for the reason above: no book asked twice
        for (const Book& b : a.books)
        {
            if (b.key.isEmpty()) continue;
            const bool haveCover  = hasCover ? hasCover(b) : b.hasCover;
            const bool haveAuthor = !b.author.trimmed().isEmpty();
            if (haveCover && haveAuthor) continue;   // it already says everything this could fill
            out.push_back(b);
        }
    return out;
}

namespace
{
    // Case, punctuation and spacing dropped; everything else kept. Deliberately NOT a fuzzy distance: a
    // threshold that admits "close enough" is a threshold somebody has to defend, and the wrong answers a
    // book search returns are not near-misses — they are different books.
    QString matchFold(const QString& s)
    {
        QString out;
        out.reserve(s.size());
        for (const QChar& c : s)
        {
            if (c.isLetterOrNumber()) out.append(c.toCaseFolded());
            else if (!out.isEmpty() && !out.endsWith(QLatin1Char(' '))) out.append(QLatin1Char(' '));
        }
        return out.trimmed();
    }

    // One is the other, or one starts the other AT A WORD BOUNDARY. The boundary is the whole point: it
    // matches "Dune" to "Dune Chronicles 1" and refuses it for "Duneland Folk".
    bool prefixAtWordBoundary(const QString& shortOne, const QString& longOne)
    {
        if (!longOne.startsWith(shortOne)) return false;
        return longOne.size() == shortOne.size() || longOne.at(shortOne.size()) == QLatin1Char(' ');
    }
}

bool titleCorroborates(const QString& bookTitle, const QString& answerTitle)
{
    const QString a = matchFold(bookTitle), b = matchFold(answerTitle);
    if (a.isEmpty() || b.isEmpty()) return false;   // an answer that names nothing is evidence of nothing
    if (a == b) return true;
    return a.size() < b.size() ? prefixAtWordBoundary(a, b) : prefixAtWordBoundary(b, a);
}

int fillConfidence(const Book& b, const Fill& f)
{
    // THE #134 GATE, KEPT, and kept FIRST: an answer whose title does not corroborate the book's is not
    // evidence about it at any score. Every invention that live run caught fails here and scores 0.
    if (!titleCorroborates(b.title, f.title)) return 0;

    const QString mine = matchFold(b.title), theirs = matchFold(f.title);
    int score = (mine == theirs) ? 50 : 35;    // exact, else one is a whole-word prefix of the other

    // A DISTINCTIVE TITLE IS ITS OWN CORROBORATION. A catalogue answers "Alpha Chronicle" with a book called
    // something else; what it does not do is produce an exact five-word match for a title it does not hold.
    // Two words or fewer earns nothing — "The Gift" is the case this issue is named after.
    if (mine.split(QLatin1Char(' '), Qt::SkipEmptyParts).size() >= 3) score += 15;

    // THE AUTHOR, when BOTH sides state one. This is the term that separates two real books sharing a name,
    // so a disagreement is disqualifying by itself rather than merely discouraging. A book whose file names
    // no author says nothing here — an untagged library is the population this feature serves.
    const QString mineAuthor = b.author.trimmed(), theirAuthor = f.author.trimmed();
    if (!mineAuthor.isEmpty() && !theirAuthor.isEmpty())
    {
        const QString a = matchFold(mineAuthor), c = matchFold(theirAuthor);
        const bool agrees = !a.isEmpty() && !c.isEmpty()
                            && (a == c || (a.size() < c.size() ? prefixAtWordBoundary(a, c)
                                                               : prefixAtWordBoundary(c, a)));
        score += agrees ? 35 : -60;
    }

    // THE YEAR, when both sides state one. A year of slack, because an EPUB stamps the edition it was made
    // from and a catalogue answers with first publication; beyond that they are two different books far more
    // often than they are two printings.
    if (b.year > 0 && f.year > 0)
    {
        const int drift = std::abs(b.year - f.year);
        score += drift == 0 ? 25 : (drift <= 1 ? 10 : -25);
    }

    // THE PAGE COUNT: a nudge when it agrees, NOTHING when it does not. Our count is chapters for an EPUB
    // and page images for a comic, so a disagreement usually means two units rather than two books — and a
    // term that can only be evidence in one direction is honest only if it never subtracts.
    if (b.pageCount >= kComparablePages && f.pageCount >= kComparablePages)
    {
        const int slack = std::max(1, b.pageCount / 10);
        if (std::abs(b.pageCount - f.pageCount) <= slack) score += 10;
    }

    return std::max(0, std::min(100, score));
}

Fill acceptedFill(const Book& b, bool hasCover, const Fill& f)
{
    Fill out;
    // GATE 1: is this answer even about this book? All or nothing — half of a wrong answer is a wrong
    // answer, and a cover from one book over the title of another is the most confusing form it can take.
    // A SCORE rather than a title comparison since #294: a title alone cannot separate two real books that
    // share a name, and below the threshold nothing at all is applied.
    if (fillConfidence(b, f) < kFillAcceptThreshold) return out;
    // GATE 2: only a blank is ever filled.
    if (b.author.trimmed().isEmpty())  out.author      = f.author.trimmed();
    if (!hasCover)                     out.coverUrl    = f.coverUrl.trimmed();
    if (b.summary.trimmed().isEmpty()) out.description = f.description.trimmed();
    return out;
}

// Cached process-wide index (main-thread only): the async scan installs it, browse reads it.
namespace { Index g_index; bool g_indexReady = false; }

QString root() { return Settings::readingFolder(); }
QString indexFilePath() { return AppPaths::dataDir() + QStringLiteral("/bookindex.json"); }
void installIndex(Index idx) { g_index = std::move(idx); g_indexReady = true; }
const Index& index() { return g_index; }
bool indexReady() { return g_indexReady; }

bool hasLibrary()
{
    if (!g_index.isEmpty()) return true;
    const QString r = root();
    return !r.isEmpty() && QFileInfo::exists(r);
}

} // namespace BookLibrary
