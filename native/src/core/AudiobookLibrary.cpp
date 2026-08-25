#include "AudiobookLibrary.h"
#include "AppPaths.h"
#include "NaturalOrder.h"
#include "Settings.h"

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

namespace AudiobookLibrary
{
namespace
{
    // UNIT SEPARATOR joins the halves of a key, because it is the one byte a path or a tag value cannot
    // contain — a '/' or a '|' would let "A|B" + "C" collide with "A" + "B|C", which is a silent
    // two-books-become-one bug that only shows up on somebody's real disk.
    const QChar kSep = QChar(0x1F);

    // The natural (numeric-aware, case-insensitive) collator, built ONCE and through NaturalOrder — never
    // inline. A plain `QCollator c; c.setNumericMode(true);` is INERT under the C locale, so a book whose
    // parts are named 1..10 with no padding orders 10 before 2 with nothing said (issue #205), and a
    // multi-file book ordered by filename is exactly that shape.
    const QCollator& naturalCollator()
    {
        static QCollator coll = NaturalOrder::collator();
        return coll;
    }

    // Case- and whitespace-insensitive grouping. "Neil Gaiman" and "neil gaiman " are ONE author; the first
    // spelling encountered is what gets displayed and only the key is folded.
    QString foldKey(const QString& s) { return s.trimmed().toCaseFolded(); }

    // 0 means "untagged", and an untagged disc is disc 1 — otherwise a single-disc book whose files carry no
    // TPOS would sort into a phantom disc 0 ahead of a part that happens to say "disc 1".
    int discRank(int disc) { return disc > 0 ? disc : 1; }

    // 0 means "untagged", and an untagged part sorts AFTER every numbered one. When every file in a book is
    // untagged they all rank the same and the NATURAL FILENAME below decides, which is the case #139 calls
    // out: a folder of numbered mp3s with no track tags at all.
    int trackRank(int track) { return track > 0 ? track : std::numeric_limits<int>::max(); }

    // A book with no series index sorts after the numbered ones inside a series, for the same reason.
    int indexRank(int idx) { return idx > 0 ? idx : std::numeric_limits<int>::max(); }

    // One scanned entry -> the browse-facing file row. ONE builder, because a book's file list and the
    // book's own totals are both derived from it.
    BookFile bookFileFor(const FileEntry& e)
    {
        BookFile f;
        f.path = e.path;
        // Filename fallback for the title. completeBaseName() keeps "Part 2.disc1" intact and drops only the
        // final extension, which is what a person reading the folder would call the file.
        f.title        = e.title.trimmed().isEmpty() ? QFileInfo(e.path).completeBaseName() : e.title.trimmed();
        f.disc         = e.disc;
        f.track        = e.track;
        f.durationSec  = e.durationSec;
        f.chapterCount = int(e.chapters.size());
        f.hasCover     = e.hasCover;
        return f;
    }

    // The order of a book's files: disc, then track, then the NATURAL filename. The last one is not a
    // tiebreak — for an untagged folder it is the ONLY rule that fires. See the header.
    void sortFiles(QVector<BookFile>& files)
    {
        std::sort(files.begin(), files.end(), [](const BookFile& a, const BookFile& b) {
            if (discRank(a.disc) != discRank(b.disc))   return discRank(a.disc) < discRank(b.disc);
            if (trackRank(a.track) != trackRank(b.track)) return trackRank(a.track) < trackRank(b.track);
            return naturalCollator().compare(a.path, b.path) < 0;
        });
    }

    // The order of a bucket's books: series name, then the book's place in that series, then its title. For
    // an author with no series tags anywhere — most of them — every book has an empty series name and this
    // is exactly title order; for an author of two series, each series groups together and reads in order.
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
    // first thing the browse shows.
    void sortBuckets(QVector<Author>& buckets)
    {
        std::sort(buckets.begin(), buckets.end(), [](const Author& a, const Author& b) {
            if (a.name.isEmpty() != b.name.isEmpty()) return b.name.isEmpty();
            return naturalCollator().compare(a.name, b.name) < 0;
        });
        for (Author& a : buckets) sortBooks(a.books);
    }

    // Fold the books that name this dimension into buckets keyed by it. ONE function for narrators and
    // series, because they are the same operation over a different field — and because a copy of it per
    // dimension is how the two would come to disagree about ordering or about what an empty value means.
    QVector<Author> bucketBy(const QVector<Author>& authors,
                             const std::function<QString(const Book&)>& valueOf)
    {
        QVector<Author> out;
        QHash<QString, int> at;
        for (const Author& a : authors)
            for (const Book& b : a.books)
            {
                const QString value = valueOf(b).trimmed();
                const QString key   = foldKey(value);
                if (key.isEmpty()) continue;      // THE GATE: a file that names nothing mints no bucket
                int i = at.value(key, -1);
                if (i < 0)
                {
                    Author bucket;
                    bucket.key  = key;
                    bucket.name = value;          // display spelling: the first one seen
                    i = out.size();
                    out.push_back(bucket);
                    at.insert(key, i);
                }
                out[i].books.push_back(b);        // a COPY: the book still lives under its author
                out[i].durationSec += b.durationSec;
            }
        sortBuckets(out);
        return out;
    }
}

bool isAudioFile(const QString& path) { return AudioTags::isSupportedFile(path); }

QString authorKeyFor(const FileEntry& e) { return foldKey(e.effectiveAuthor()); }

QString bookKeyFor(const FileEntry& e)
{
    // THE FOLDER IS ALWAYS HALF THE KEY, and the author is never part of it (the header says why at
    // length). The other half is the ALBUM tag when the files carry one, and a bare discriminator when they
    // do not, so a folder titled after a book can never be confused with a book literally named after that
    // folder.
    const QString folder = foldKey(QFileInfo(e.path).absolutePath());
    if (!e.book.trimmed().isEmpty())
        return folder + kSep + QLatin1String("t") + kSep + foldKey(e.book);
    return folder + kSep + QLatin1String("d");
}

QString narratorKeyFor(const FileEntry& e) { return foldKey(e.effectiveNarrator()); }
QString seriesKeyFor(const FileEntry& e)   { return foldKey(e.series); }

QVector<FileEntry> scanFolder(const QString& root, const QHash<QString, FileEntry>& known, ScanStats* stats,
                              const QStringList& separators)
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
        if (!isAudioFile(fi.filePath())) continue;   // extension-only, before anything is opened: a cover.jpg,
                                                     // a .nfo, a stray .txt costs one string compare
        const QString abs  = fi.absoluteFilePath();
        const qint64 mtime = fi.lastModified().toSecsSinceEpoch();
        const qint64 size  = fi.size();
        ++s.files;

        // THE INCREMENTAL DECISION, and the only one. Same path, same mtime, same size => the bytes we
        // already parsed are still the bytes on disk, so the file is not opened at all. Size is checked as
        // well as mtime because a tag editor that rewrites a file can preserve the timestamp (and archives
        // restored from backup routinely do), while almost nothing preserves the length too.
        const auto cached = known.constFind(abs);
        if (cached != known.constEnd() && cached->mtime == mtime && cached->size == size)
        {
            ++s.reused;
            out.push_back(*cached);
            continue;
        }

        // withChapters TRUE: this is the one caller in the tree that asks. See AudioTags.h.
        const AudioTags::Tags t = AudioTags::read(abs, separators, /*withChapters*/ true);
        ++s.retagged;

        FileEntry e;
        e.path = abs; e.mtime = mtime; e.size = size;
        e.title       = t.title;
        e.artist      = t.artist;
        e.albumArtist = t.albumArtist;
        e.book        = t.album;
        e.narrator    = t.narrator;
        e.composer    = t.composer;
        e.series      = t.series;
        e.seriesIndex = t.seriesIndex;
        e.track       = t.track;
        e.disc        = t.disc;
        e.year        = t.year;
        e.durationSec = t.durationSec;
        e.hasCover    = !t.cover.isNull();
        e.untagged    = t.isEmpty();
        for (const AudioTags::Chapter& c : t.chapters)
        {
            Chapter ch;
            ch.title    = c.title;
            ch.startSec = c.startMs / 1000;
            e.chapters.push_back(ch);
        }
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
    // display capitalisation, a book's folder, its year, which file supplies its cover — is a property of
    // the library rather than of whatever order QDirIterator happened to hand back on this filesystem. Two
    // runs must build the same index from the same disk.
    QVector<FileEntry> sorted = entries;
    std::sort(sorted.begin(), sorted.end(), [](const FileEntry& a, const FileEntry& b) {
        return naturalCollator().compare(a.path, b.path) < 0;
    });

    Index idx;
    QHash<QString, int> authorAt;                // author key -> position in idx.authors
    QHash<QString, QPair<int, int>> bookAt;      // book key   -> (author position, book position)

    for (const FileEntry& e : sorted)
    {
        const QString bKey = bookKeyFor(e);

        // THE BOOK IS LOOKED UP BEFORE THE AUTHOR IS, and that ordering is the anti-shatter rule made
        // executable: the author is not part of the book key, so a second file in the same folder joins the
        // book that is already there even when its own ARTIST tag says somebody else. The FIRST file's
        // author owns the book. Audiobook rips disagree about that tag constantly — narrator in ARTIST on
        // one file, author on the next — and fourteen one-part books is a far worse outcome than one book
        // filed under the first of two spellings.
        int ai = -1, bi = -1;
        const auto found = bookAt.constFind(bKey);
        if (found != bookAt.constEnd())
        {
            ai = found->first;
            bi = found->second;
        }
        else
        {
            const QString aKey = authorKeyFor(e);
            ai = authorAt.value(aKey, -1);
            if (ai < 0)
            {
                Author a;
                a.key  = aKey;
                a.name = e.effectiveAuthor().trimmed();   // display spelling: the first one seen
                ai = idx.authors.size();
                idx.authors.push_back(a);
                authorAt.insert(aKey, ai);
            }

            Book b;
            b.key    = bKey;
            b.author = idx.authors[ai].name;
            b.folder = QFileInfo(e.path).absolutePath();
            if (!e.book.trimmed().isEmpty())
            {
                b.title = e.book.trimmed();
            }
            else
            {
                // Untagged: named after its directory, which is what a person means by a folder of parts.
                // QDir::dirName() of a drive root is empty and the title then stays empty rather than being
                // invented — displayBook() says "Unknown Book", which is honest.
                b.title = QDir(b.folder).dirName();
                b.titleFromFolder = true;
            }
            bi = idx.authors[ai].books.size();
            idx.authors[ai].books.push_back(b);
            bookAt.insert(bKey, qMakePair(ai, bi));
            ++idx.bookCount;
        }

        Book& book = idx.authors[ai].books[bi];
        // FIRST NON-EMPTY WINS for everything a book carries but a file might not. A book whose first part
        // was tagged without a narrator and whose second part names one still shows the narrator: the
        // alternative — only ever reading file one — would drop a whole dimension over one badly tagged
        // part.
        if (book.narrator.isEmpty()) book.narrator = e.effectiveNarrator().trimmed();
        if (book.series.isEmpty())   book.series   = e.series.trimmed();
        if (book.seriesIndex == 0)   book.seriesIndex = e.seriesIndex;
        if (book.year == 0 && e.year > 0) book.year = e.year;
        if (book.coverSourcePath.isEmpty() && e.hasCover) book.coverSourcePath = e.path;

        book.durationSec  += e.durationSec;
        book.chapterCount += int(e.chapters.size());
        book.files.push_back(bookFileFor(e));
        ++idx.fileCount;
    }

    for (Author& a : idx.authors)
    {
        for (Book& b : a.books)
        {
            sortFiles(b.files);
            a.durationSec += b.durationSec;
        }
    }
    sortBuckets(idx.authors);

    // THE TWO VIEWS, built after every book exists so a bucket holds finished copies. Each is EMPTY for a
    // library whose files carry no such tag, which is the compatibility gate every surface checks before
    // offering the dimension at all.
    idx.narrators = bucketBy(idx.authors, [](const Book& b) { return b.narrator; });
    idx.series    = bucketBy(idx.authors, [](const Book& b) { return b.series; });
    return idx;
}

const Author* Index::author(const QString& authorKey) const
{
    for (const Author& a : authors)
        if (a.key == authorKey) return &a;
    return nullptr;
}

const Narrator* Index::narrator(const QString& narratorKey) const
{
    for (const Narrator& n : narrators)
        if (n.key == narratorKey) return &n;
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
    // AUTHORS ONLY — the canonical home. Looking in the narrator or series copies as well would answer the
    // same question twice from two places that are only equal by construction.
    for (const Author& a : authors)
        for (const Book& b : a.books)
            if (b.key == bookKey) return &b;
    return nullptr;
}

QString displayAuthor(const Author& a)
{
    return a.name.trimmed().isEmpty() ? QObject::tr("Unknown Author") : a.name.trimmed();
}

QString displayBook(const Book& b)
{
    return b.title.trimmed().isEmpty() ? QObject::tr("Unknown Book") : b.title.trimmed();
}

// ---------------------------------------------------------------------------------------------------------
// Persistence. Its own file, its own version, its own stamp — nothing here can change what the music index
// holds or when it re-reads.
// ---------------------------------------------------------------------------------------------------------
namespace
{
    const int kIndexFileVersion = 1;
    // Bump when AudioTags starts reading something new that a BOOK cares about, or when the scan starts
    // making something new of what it reads. 1 == issue #139, increment 1.
    const int kTagRules = 1;
}

QString parseStamp(const QStringList& separators)
{
    return QString::number(kTagRules) + QChar(' ') + separators.join(QChar(' '));
}

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
        e.title       = o.value(QStringLiteral("ti")).toString();
        e.artist      = o.value(QStringLiteral("ar")).toString();
        e.albumArtist = o.value(QStringLiteral("aa")).toString();
        e.book        = o.value(QStringLiteral("bk")).toString();
        e.narrator    = o.value(QStringLiteral("nr")).toString();
        e.composer    = o.value(QStringLiteral("cm")).toString();
        e.series      = o.value(QStringLiteral("se")).toString();
        e.seriesIndex = o.value(QStringLiteral("si")).toInt();
        e.track       = o.value(QStringLiteral("tn")).toInt();
        e.disc        = o.value(QStringLiteral("dn")).toInt();
        e.year        = o.value(QStringLiteral("yr")).toInt();
        e.durationSec = o.value(QStringLiteral("du")).toInt();
        e.hasCover    = o.value(QStringLiteral("cv")).toBool();
        e.untagged    = o.value(QStringLiteral("nt")).toBool();
        for (const QJsonValue& cv : o.value(QStringLiteral("ch")).toArray())
        {
            const QJsonObject co = cv.toObject();
            Chapter c;
            c.title    = co.value(QStringLiteral("t")).toString();
            c.startSec = co.value(QStringLiteral("s")).toInt();
            e.chapters.push_back(c);
        }
        out.push_back(e);
    }
    return out;
}

bool saveIndexFile(const QString& filePath, const QVector<FileEntry>& entries, const QStringList& separators)
{
    QJsonArray files;
    for (const FileEntry& e : entries)
    {
        QJsonObject o;
        o.insert(QStringLiteral("p"), e.path);
        o.insert(QStringLiteral("m"), double(e.mtime));
        o.insert(QStringLiteral("s"), double(e.size));
        if (!e.title.isEmpty())       o.insert(QStringLiteral("ti"), e.title);
        if (!e.artist.isEmpty())      o.insert(QStringLiteral("ar"), e.artist);
        if (!e.albumArtist.isEmpty()) o.insert(QStringLiteral("aa"), e.albumArtist);
        if (!e.book.isEmpty())        o.insert(QStringLiteral("bk"), e.book);
        if (!e.narrator.isEmpty())    o.insert(QStringLiteral("nr"), e.narrator);
        if (!e.composer.isEmpty())    o.insert(QStringLiteral("cm"), e.composer);
        if (!e.series.isEmpty())      o.insert(QStringLiteral("se"), e.series);
        if (e.seriesIndex) o.insert(QStringLiteral("si"), e.seriesIndex);
        if (e.track)       o.insert(QStringLiteral("tn"), e.track);
        if (e.disc)        o.insert(QStringLiteral("dn"), e.disc);
        if (e.year)        o.insert(QStringLiteral("yr"), e.year);
        if (e.durationSec) o.insert(QStringLiteral("du"), e.durationSec);
        if (e.hasCover)    o.insert(QStringLiteral("cv"), true);
        if (e.untagged)    o.insert(QStringLiteral("nt"), true);
        if (!e.chapters.isEmpty())
        {
            QJsonArray ch;
            for (const Chapter& c : e.chapters)
            {
                QJsonObject co;
                if (!c.title.isEmpty()) co.insert(QStringLiteral("t"), c.title);
                co.insert(QStringLiteral("s"), c.startSec);
                ch.append(co);
            }
            o.insert(QStringLiteral("ch"), ch);
        }
        files.append(o);
    }
    QJsonObject root;
    root.insert(QStringLiteral("version"), kIndexFileVersion);
    root.insert(QStringLiteral("rules"), parseStamp(separators));
    root.insert(QStringLiteral("files"), files);

    QDir().mkpath(QFileInfo(filePath).absolutePath());
    QFile f(filePath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    return f.write(QJsonDocument(root).toJson(QJsonDocument::Compact)) >= 0;
}

// Cached process-wide index (main-thread only): the async scan installs it, browse reads it.
namespace { Index g_index; bool g_indexReady = false; }

QString root() { return Settings::audiobookFolder(); }
QString indexFilePath() { return AppPaths::dataDir() + QStringLiteral("/audiobookindex.json"); }
void installIndex(Index idx) { g_index = std::move(idx); g_indexReady = true; }
const Index& index() { return g_index; }
bool indexReady() { return g_indexReady; }

bool hasLibrary()
{
    if (!g_index.isEmpty()) return true;
    const QString r = root();
    return !r.isEmpty() && QFileInfo::exists(r);
}

} // namespace AudiobookLibrary
