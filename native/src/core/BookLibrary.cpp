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
        || e == QStringLiteral("mdown")|| e == QStringLiteral("mkd");
}

Kind kindFor(const QString& path)
{
    const QString e = QFileInfo(path).suffix().toLower();
    return (e == QStringLiteral("cbz") || e == QStringLiteral("cbr")) ? Kind::Comic : Kind::Book;
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

    QDirIterator it(root, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext())
    {
        it.next();
        const QFileInfo fi = it.fileInfo();
        // Extension-only, before anything is opened: a cover.jpg, a .nfo, a loose folder of scanned pages
        // that is not an archive at all — each costs one string compare and is not read.
        if (!isReadingFile(fi.filePath())) continue;
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

        if (e.kind == Kind::Comic)
        {
            // A comic knows nothing about itself but its name, and ComicName has already decided how much
            // of that name is a series. Its `title` is never empty by contract, so the "an untagged file
            // must still appear" rule holds without a second fallback here.
            const ComicName::Grouped& g = comicOf.at(i);
            b.title             = g.title;
            b.series            = g.series;
            b.seriesIndex       = g.number;
            b.titleFromFilename = true;
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
    const int kRules = 2;
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
