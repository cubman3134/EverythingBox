// Headless check of the local AUDIOBOOK LIBRARY (issue #139, increment 1): the scan and the index
// (src/core/AudiobookLibrary), the browse builders over it (src/browse/AudiobookCatalogs), and the one
// change this feature made to the shared tag reader (src/media/AudioTags — narrator, series, chapters).
//
// THE FIXTURES ARE REAL TAGGED FILES, written to this process's own scratch dataDir() from the byte builders
// in tools/MusicFixtures.h — the same builders probe_musictags and probe_musiclibrary use. Nothing here
// fabricates a FileEntry by hand: every entry below came out of AudioTags::read() parsing an ID3v2 or MP4
// tag block off a disk, so what is proven is the path a user's folder takes.
//
// What it pins, in the order #139 cares about:
//   1. A SINGLE .m4b is one book, and its NERO chapter list (moov/udta/chpl) is read at scan time — count,
//      titles and start times, in 100-nanosecond units converted once.
//   2. A FOLDER OF NUMBERED MP3s is ONE book with one key, one tile and one ordered queue.
//   3. UNPADDED NUMBERING with no track tags at all orders 1, 2, 10 — the NaturalOrder case (#205), and the
//      one that is silently wrong on any machine with no locale if a plain QCollator is built.
//   4. A STRAY NON-AUDIO FILE in a book's folder is not in the book.
//   5. NESTED FOLDERS: a series directory holding one directory per book is one book per directory, and
//      loose files in the series directory itself are their own book.
//   6. A BOOK WITH NO TAGS AT ALL is named after its folder and files under the unknown-author bucket,
//      which sorts LAST.
//   7. NARRATOR: an explicit tag wins, COMPOSER is the fallback (the m4b convention), and a book with
//      neither mints no narrator bucket at all.
//   8. SERIES: the SERIES tag, else MOVEMENTNAME; the index orders a series by its book number.
//   9. A FILE IN A MUSIC ROOT IS UNAFFECTED. Three separate claims, because "untouched" is the requirement
//      most easily asserted vacuously: the music parse stamp did not move (so no library re-tags), the very
//      same file reads as a CLASSICAL COMPOSER through MusicLibrary and as a NARRATOR through this one, and
//      AudioTags::read with its default arguments returns NO chapters even for a file that has them.
//  10. The INCREMENTAL rescan does not re-read an unchanged file, and the persisted index round-trips —
//      chapters included — while a book with none writes no chapter key at all.
//  11. The BROWSE builders render what the index holds: doors only when their dimension exists, a stale key
//      yielding an empty titled catalog rather than a crash, and a book's part rows carrying the BOOK key so
//      the router queues the whole thing.
//  12. Nothing configured / a missing root are dormant and instant, not errors.
//
// Prints AUDIOBOOKS-OK on success; any failure prints AUDIOBOOKS-FAIL <cond> (line) and exits non-zero.
//
// Isolation: AppPaths::dataDir() is this process's own scratch dir (issue #42), so the whole fixture library
// is written under it and goes away at exit. Nothing is written beside the exe.
#include "AudiobookLibrary.h"
#include "AudiobookCatalogs.h"
#include "MusicLibrary.h"     // §9: the same file, read by the OTHER library
#include "AppPaths.h"
#include "MusicFixtures.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <cstdio>

static int g_fails = 0;

#define CHECK(cond)                                                                          \
    do {                                                                                     \
        if (!(cond)) { std::printf("AUDIOBOOKS-FAIL %s (%d)\n", #cond, __LINE__); ++g_fails; } \
    } while (0)

using AudiobookLibrary::Author;
using AudiobookLibrary::Book;
using AudiobookLibrary::BookFile;
using AudiobookLibrary::FileEntry;
using AudiobookLibrary::Index;
using AudiobookLibrary::ScanStats;

// ---------------------------------------------------------------------------------------------------------
// Fixture writers. Each builds a complete, valid file of its container from MusicFixtures.h; the tag values
// passed in are the same literals the assertions are written against.
// ---------------------------------------------------------------------------------------------------------

// An mp3 with an ID3v2.4 tag. An empty QString means "do not write that frame at all", which is how a
// fixture expresses a MISSING field rather than an empty one.
struct Mp3Tags
{
    QString title, artist, albumArtist, album, composer, narrator, series, seriesPart, track, year;
    QByteArray cover;
    QList<QByteArray> chapters;   // pre-built CHAP frames
};

static bool writeMp3(const QString& path, const Mp3Tags& t)
{
    QByteArray frames;
    if (!t.title.isEmpty())       frames.append(id3TextFrame("TIT2", t.title));
    if (!t.artist.isEmpty())      frames.append(id3TextFrame("TPE1", t.artist));
    if (!t.albumArtist.isEmpty()) frames.append(id3TextFrame("TPE2", t.albumArtist));
    if (!t.album.isEmpty())       frames.append(id3TextFrame("TALB", t.album));
    if (!t.composer.isEmpty())    frames.append(id3TextFrame("TCOM", t.composer));
    if (!t.track.isEmpty())       frames.append(id3TextFrame("TRCK", t.track));
    if (!t.year.isEmpty())        frames.append(id3TextFrame("TDRC", t.year));
    // NARRATOR / SERIES have no standard ID3v2 frame, so every tagger that writes them uses a user-defined
    // TXXX whose DESCRIPTION is the field name — and TagLib turns that description into the property key.
    if (!t.narrator.isEmpty())    frames.append(id3TxxxFrame(QStringLiteral("NARRATOR"), t.narrator));
    if (!t.series.isEmpty())      frames.append(id3TxxxFrame(QStringLiteral("SERIES"), t.series));
    if (!t.seriesPart.isEmpty())  frames.append(id3TxxxFrame(QStringLiteral("SERIES-PART"), t.seriesPart));
    if (!t.cover.isEmpty())       frames.append(id3ApicFrame("image/jpeg", 0x03, t.cover));
    for (const QByteArray& c : t.chapters) frames.append(c);
    return writeFixture(path, mp3File(frames));
}

// An .m4b: an iTunes ilst plus a NERO chapter list in udta. No trak/mdhd, so its duration reads as 0 — the
// same deliberate property m4aFile has, and asserted as such below.
static bool writeM4b(const QString& path, const QString& title, const QString& artist,
                     const QString& albumArtist, const QString& album, const QString& composer,
                     const QList<QPair<quint64, QString>>& chapters,
                     const QString& narratorAtom = QString())
{
    QByteArray ilst;
    if (!title.isEmpty())       ilst.append(mp4TextItem(itunesName("nam"), title));
    if (!artist.isEmpty())      ilst.append(mp4TextItem(itunesName("ART"), artist));
    if (!albumArtist.isEmpty()) ilst.append(mp4TextItem("aART", albumArtist));
    if (!album.isEmpty())       ilst.append(mp4TextItem(itunesName("alb"), album));
    if (!composer.isEmpty())    ilst.append(mp4TextItem(itunesName("wrt"), composer));
    // ©nrt — the atom TagLib's property table does NOT know, which is why AudioTags reads the MP4 tag
    // directly for it. A fixture that only ever used the freeform spelling could not prove that.
    if (!narratorAtom.isEmpty()) ilst.append(mp4TextItem(itunesName("nrt"), narratorAtom));
    return writeFixture(path, m4bFile(ilst, chapters.isEmpty() ? QByteArray() : mp4ChplAtom(chapters)));
}

static bool setMtime(const QString& path, qint64 secs)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadWrite)) return false;
    return f.setFileTime(QDateTime::fromSecsSinceEpoch(secs), QFileDevice::FileModificationTime);
}

// The one book with this title, from anywhere in the index. Written as a search rather than by holding a key
// so the assertions read as questions about the LIBRARY rather than about a key this file computed.
static const Book* bookTitled(const Index& idx, const QString& title)
{
    for (const Author& a : idx.authors)
        for (const Book& b : a.books)
            if (b.title == title) return &b;
    return nullptr;
}

static const Author* bucketNamed(const QVector<Author>& buckets, const QString& name)
{
    for (const Author& a : buckets)
        if (a.name == name) return &a;
    return nullptr;
}

static bool catalogHasType(const MediaCatalog& cat, const char* type)
{
    for (const MediaItem& it : cat.items)
        if (it.type == QString::fromLatin1(type)) return true;
    return false;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    const QString base = AppPaths::dataDir() + QStringLiteral("/audiobooklib");
    const QString root = base + QStringLiteral("/root");
    QDir().mkpath(root);

    const QByteArray jpeg = jpegBytes();

    // --- The fixture library ---------------------------------------------------------------------------

    // 1. A single .m4b in its own folder, with a real chpl. The chapter starts are written in milliseconds
    //    and converted by chplTime(), so the 100ns unit is stated once and asserted against the ms.
    const QString dirM4b = root + QStringLiteral("/Neil Gaiman/The Ocean at the End of the Lane");
    CHECK(writeM4b(dirM4b + QStringLiteral("/ocean.m4b"), QStringLiteral("The Ocean at the End of the Lane"),
                   QStringLiteral("Neil Gaiman"), QStringLiteral("Neil Gaiman"),
                   QStringLiteral("The Ocean at the End of the Lane"),
                   QStringLiteral("Neil Gaiman"),   // COMPOSER: the m4b narrator convention
                   { { chplTime(0),      QStringLiteral("Prologue") },
                     { chplTime(90000),  QStringLiteral("Chapter One") },
                     { chplTime(600000), QStringLiteral("Chapter Two") } }));

    // 2. A folder of numbered mp3s, tagged with the book's title and a track number each. The FILENAMES are
    //    chosen so that path order disagrees with track order — otherwise the ordering assertion would pass
    //    on a scan that never looked at a tag.
    const QString dirParts = root + QStringLiteral("/Ursula K. Le Guin/A Wizard of Earthsea");
    for (int i = 1; i <= 3; ++i)
    {
        Mp3Tags t;
        t.title  = QStringLiteral("Part %1").arg(i);
        t.artist = QStringLiteral("Ursula K. Le Guin");
        t.album  = QStringLiteral("A Wizard of Earthsea");
        t.composer = QStringLiteral("Rob Inglis");     // the narrator, by convention
        t.track  = QString::number(i);
        if (i == 1) t.cover = jpeg;
        // z-, y-, x- : reverse alphabetical, so only the TRACK tag can produce 1, 2, 3.
        const QChar name = QChar(QLatin1Char('z' - char(i - 1)));
        CHECK(writeMp3(dirParts + QStringLiteral("/%1 part.mp3").arg(name), t));
    }

    // 3. Unpadded numbering, NO tags at all: the NaturalOrder case. 10 must come after 2.
    const QString dirNat = root + QStringLiteral("/unpadded");
    for (int n : { 1, 2, 10 })
        CHECK(writeMp3(dirNat + QStringLiteral("/%1 - chapter.mp3").arg(n), Mp3Tags{}));

    // 4. A stray non-audio file beside a book's parts.
    CHECK(writeFixture(dirParts + QStringLiteral("/cover.jpg"), jpeg));
    CHECK(writeFixture(dirParts + QStringLiteral("/notes.txt"), QByteArray("not audio")));

    // 5. A SERIES directory holding one directory per book, plus a loose file in the series directory
    //    itself. Three books, and the loose one is its own.
    const QString dirSeries = root + QStringLiteral("/Terry Pratchett/Discworld");
    for (int n = 1; n <= 2; ++n)
    {
        Mp3Tags t;
        t.title       = QStringLiteral("Book %1").arg(n);
        t.artist      = QStringLiteral("Terry Pratchett");
        t.album       = QStringLiteral("Discworld %1").arg(n);
        t.composer    = QStringLiteral("Nigel Planer");
        t.series      = QStringLiteral("Discworld");
        t.seriesPart  = QString::number(3 - n);   // book 1 is #2 and book 2 is #1: only the tag can order them
        CHECK(writeMp3(dirSeries + QStringLiteral("/Book %1/only.mp3").arg(n), t));
    }
    {
        Mp3Tags t;
        t.title  = QStringLiteral("A Loose Reading");
        t.artist = QStringLiteral("Terry Pratchett");
        t.album  = QStringLiteral("Loose");
        CHECK(writeMp3(dirSeries + QStringLiteral("/loose.mp3"), t));
    }

    // 6. Two .m4b files in ONE folder with DIFFERENT album tags: two books, which is what stops a
    //    folder-only rule from merging a shelf of loose books into one.
    const QString dirTwo = root + QStringLiteral("/Loose Books");
    CHECK(writeM4b(dirTwo + QStringLiteral("/one.m4b"), QStringLiteral("One"), QStringLiteral("A Writer"),
                   QString(), QStringLiteral("Book One"), QString(), {}));
    CHECK(writeM4b(dirTwo + QStringLiteral("/two.m4b"), QStringLiteral("Two"), QStringLiteral("A Writer"),
                   QString(), QStringLiteral("Book Two"), QString(),
                   { { chplTime(0), QStringLiteral("Start") } },
                   QStringLiteral("An Explicit Narrator")));

    // ---- The scan -------------------------------------------------------------------------------------
    ScanStats stats;
    const QVector<FileEntry> entries = AudiobookLibrary::scanFolder(root, {}, &stats);
    // 12 audio files: 1 m4b + 3 parts + 3 unpadded + 2 series books + 1 loose + 2 loose m4b.
    CHECK(stats.files == 12);
    CHECK(stats.retagged == 12);
    CHECK(stats.reused == 0);
    CHECK(entries.size() == 12);
    // 4. The stray files were never opened — the extension filter runs before anything is read.
    for (const FileEntry& e : entries)
        CHECK(!e.path.endsWith(QStringLiteral(".jpg")) && !e.path.endsWith(QStringLiteral(".txt")));

    const Index idx = AudiobookLibrary::buildIndex(entries);
    CHECK(idx.fileCount == 12);
    CHECK(!idx.isEmpty());

    // ---- §1 A single .m4b, with its chapters --------------------------------------------------------
    {
        const Book* b = bookTitled(idx, QStringLiteral("The Ocean at the End of the Lane"));
        CHECK(b != nullptr);
        if (b)
        {
            CHECK(b->files.size() == 1);
            CHECK(!b->isMultiFile());
            CHECK(b->author == QStringLiteral("Neil Gaiman"));
            CHECK(b->chapterCount == 3);
            CHECK(b->files.first().chapterCount == 3);
            // The m4b fixture carries no trak/mdhd, so its length is honestly unknown rather than invented.
            CHECK(b->durationSec == 0);
            CHECK(b->coverSourcePath.isEmpty());   // no covr atom in this fixture
        }
        // The chapter VALUES, off the entry rather than the browse row, because the row only carries a count.
        const FileEntry* m4b = nullptr;
        for (const FileEntry& e : entries)
            if (e.path.endsWith(QStringLiteral("ocean.m4b"))) m4b = &e;
        CHECK(m4b != nullptr);
        if (m4b)
        {
            CHECK(m4b->chapters.size() == 3);
            CHECK(m4b->chapters.at(0).title == QStringLiteral("Prologue"));
            CHECK(m4b->chapters.at(0).startSec == 0);
            CHECK(m4b->chapters.at(1).title == QStringLiteral("Chapter One"));
            CHECK(m4b->chapters.at(1).startSec == 90);    // 90000 ms, via 900,000,000 hundred-nanoseconds
            CHECK(m4b->chapters.at(2).startSec == 600);
        }
    }

    // ---- §2 A folder of numbered mp3s is ONE book ---------------------------------------------------
    {
        const Book* b = bookTitled(idx, QStringLiteral("A Wizard of Earthsea"));
        CHECK(b != nullptr);
        if (b)
        {
            CHECK(b->files.size() == 3);
            CHECK(b->isMultiFile());
            // The TRACK tag decides, not the filename: the files are named z, y, x.
            CHECK(b->files.at(0).title == QStringLiteral("Part 1"));
            CHECK(b->files.at(1).title == QStringLiteral("Part 2"));
            CHECK(b->files.at(2).title == QStringLiteral("Part 3"));
            CHECK(b->files.at(0).path.endsWith(QStringLiteral("z part.mp3")));
            // Each fixture mp3 is 2 seconds by construction (MusicFixtures.h), so the book is 6.
            CHECK(b->durationSec == 6);
            CHECK(b->chapterCount == 0);            // no CHAP frames anywhere in it
            CHECK(b->narrator == QStringLiteral("Rob Inglis"));   // COMPOSER, read as the narrator
            CHECK(b->coverSourcePath.endsWith(QStringLiteral("z part.mp3")));  // the first part carries the art
        }
    }

    // ---- §3 Unpadded numbering, no tags: the NaturalOrder case --------------------------------------
    {
        const Book* b = bookTitled(idx, QStringLiteral("unpadded"));   // titled after its folder (§6)
        CHECK(b != nullptr);
        if (b)
        {
            CHECK(b->files.size() == 3);
            CHECK(b->titleFromFolder);
            // 10 AFTER 2. A plain QCollator with numeric mode set is inert under the C locale and would put
            // "10 - chapter" first here, on Linux CI and on any machine with LANG unset (issue #205).
            CHECK(b->files.at(0).path.endsWith(QStringLiteral("1 - chapter.mp3")));
            CHECK(b->files.at(1).path.endsWith(QStringLiteral("2 - chapter.mp3")));
            CHECK(b->files.at(2).path.endsWith(QStringLiteral("10 - chapter.mp3")));
            // Untagged: the titles fall back to the filename, never blank.
            CHECK(b->files.at(0).title == QStringLiteral("1 - chapter"));
        }
    }

    // ---- §5 Nested folders ---------------------------------------------------------------------------
    {
        const Book* one   = bookTitled(idx, QStringLiteral("Discworld 1"));
        const Book* two   = bookTitled(idx, QStringLiteral("Discworld 2"));
        const Book* loose = bookTitled(idx, QStringLiteral("Loose"));
        CHECK(one != nullptr && two != nullptr && loose != nullptr);
        if (one && two && loose)
        {
            CHECK(one->key != two->key && two->key != loose->key);
            CHECK(one->files.size() == 1 && two->files.size() == 1 && loose->files.size() == 1);
            // The loose file lives in the SERIES directory and is emphatically not part of either book.
            CHECK(loose->folder == dirSeries);
            CHECK(one->folder != loose->folder);
        }
        const Author* pratchett = bucketNamed(idx.authors, QStringLiteral("Terry Pratchett"));
        CHECK(pratchett != nullptr);
        if (pratchett) CHECK(pratchett->books.size() == 3);
    }

    // ---- §6 Untagged, and the unknown-author bucket --------------------------------------------------
    {
        const Author* unknown = bucketNamed(idx.authors, QString());
        CHECK(unknown != nullptr);
        // Sorted LAST, so a pile of untagged files is not the first thing the browse shows.
        CHECK(!idx.authors.isEmpty() && idx.authors.last().name.isEmpty());
        if (unknown)
        {
            CHECK(unknown->books.size() == 1);
            CHECK(AudiobookLibrary::displayAuthor(*unknown) == QStringLiteral("Unknown Author"));
        }
        // Two loose .m4b files in ONE folder, with different album tags, are TWO books.
        const Book* one = bookTitled(idx, QStringLiteral("Book One"));
        const Book* two = bookTitled(idx, QStringLiteral("Book Two"));
        CHECK(one != nullptr && two != nullptr);
        if (one && two) CHECK(one->key != two->key);
    }

    // ---- §7 Narrator -------------------------------------------------------------------------------
    {
        // The COMPOSER fallback, and the EXPLICIT tag winning over it.
        const Book* explicitNarrator = bookTitled(idx, QStringLiteral("Book Two"));
        CHECK(explicitNarrator != nullptr);
        if (explicitNarrator)
            CHECK(explicitNarrator->narrator == QStringLiteral("An Explicit Narrator"));
        // A book with neither mints NO narrator bucket — the compatibility gate every dimension follows.
        const Book* none = bookTitled(idx, QStringLiteral("Book One"));
        CHECK(none != nullptr);
        if (none) CHECK(none->narrator.isEmpty());

        CHECK(bucketNamed(idx.narrators, QStringLiteral("Rob Inglis")) != nullptr);
        CHECK(bucketNamed(idx.narrators, QStringLiteral("Nigel Planer")) != nullptr);
        CHECK(bucketNamed(idx.narrators, QStringLiteral("An Explicit Narrator")) != nullptr);
        // A narrator bucket holds COPIES of books that are already filed under their author, carrying the
        // very same key — so opening one from either side opens the same book.
        const Author* inglis = bucketNamed(idx.narrators, QStringLiteral("Rob Inglis"));
        const Book* earthsea = bookTitled(idx, QStringLiteral("A Wizard of Earthsea"));
        CHECK(inglis != nullptr && earthsea != nullptr);
        if (inglis && earthsea)
        {
            CHECK(inglis->books.size() == 1);
            CHECK(inglis->books.first().key == earthsea->key);
        }
    }

    // ---- §8 Series ---------------------------------------------------------------------------------
    {
        const Author* disc = bucketNamed(idx.series, QStringLiteral("Discworld"));
        CHECK(disc != nullptr);
        if (disc)
        {
            CHECK(disc->books.size() == 2);
            // Ordered by the SERIES-PART tag, not by title: "Discworld 2" is #1 and comes first.
            CHECK(disc->books.at(0).title == QStringLiteral("Discworld 2"));
            CHECK(disc->books.at(0).seriesIndex == 1);
            CHECK(disc->books.at(1).seriesIndex == 2);
        }
        // The loose reading names no series, so it is in no series bucket.
        CHECK(idx.series.size() == 1);
    }

    // ---- §9 A MUSIC-ONLY INSTALL IS UNTOUCHED --------------------------------------------------------
    {
        // 9a. THE MUSIC PARSE STAMP DID NOT MOVE. A bump here re-tags every music library on earth on the
        //     next launch; the literal is spelled out so a future edit to MusicLibrary's kTagRules has to
        //     come past this line deliberately (#194's bump to 4 is the current value).
        CHECK(MusicLibrary::parseStamp({}) == QStringLiteral("4 "));

        // 9b. THE SAME FILE, READ BY BOTH LIBRARIES. One COMPOSER tag; through MusicLibrary it is a
        //     classical composer with a Works list, through AudiobookLibrary it is the narrator. This is the
        //     "one tag means two things, and the ROOT decides which" claim, asserted in both directions
        //     rather than described.
        const QString musicRoot = base + QStringLiteral("/musicroot");
        Mp3Tags m;
        m.title    = QStringLiteral("Aria");
        m.artist   = QStringLiteral("Glenn Gould");
        m.album    = QStringLiteral("Goldberg Variations");
        m.composer = QStringLiteral("J. S. Bach");
        m.track    = QStringLiteral("1");
        CHECK(writeMp3(musicRoot + QStringLiteral("/aria.mp3"), m));

        const QVector<MusicLibrary::TrackEntry> mus = MusicLibrary::scanFolder(musicRoot);
        CHECK(mus.size() == 1);
        const MusicLibrary::Index midx = MusicLibrary::buildIndex(mus);
        CHECK(midx.composers.size() == 1);
        if (!midx.composers.isEmpty())
            CHECK(midx.composers.first().name == QStringLiteral("J. S. Bach"));
        // ...and the audiobook library, over the very same bytes under ITS root, calls it the narrator.
        const QVector<FileEntry> asBooks = AudiobookLibrary::scanFolder(musicRoot);
        CHECK(asBooks.size() == 1);
        if (!asBooks.isEmpty())
        {
            CHECK(asBooks.first().composer == QStringLiteral("J. S. Bach"));
            CHECK(asBooks.first().effectiveNarrator() == QStringLiteral("J. S. Bach"));
        }
        // ...while the AUDIOBOOK root's own scan never saw this file at all, because a root is a root.
        for (const FileEntry& e : entries)
            CHECK(!e.path.contains(QStringLiteral("musicroot")));

        // 9c. CHAPTERS ARE OPT-IN. The default read — which is the one the music scan makes — returns none
        //     even for a file that has three, so the music library pays for nothing this feature added.
        const QString chaptered = dirM4b + QStringLiteral("/ocean.m4b");
        CHECK(AudioTags::read(chaptered).chapters.isEmpty());
        CHECK(AudioTags::read(chaptered, {}, /*withChapters*/ true).chapters.size() == 3);
    }

    // ---- §10 Incremental rescan and persistence ------------------------------------------------------
    {
        ScanStats again;
        const QVector<FileEntry> second = AudiobookLibrary::scanFolder(root, AudiobookLibrary::byPath(entries),
                                                                       &again);
        CHECK(again.retagged == 0);
        CHECK(again.reused == 12);
        CHECK(again.dropped == 0);
        CHECK(second.size() == 12);

        // A file whose mtime advances IS re-read.
        CHECK(setMtime(dirNat + QStringLiteral("/1 - chapter.mp3"),
                       QDateTime::currentSecsSinceEpoch() + 120));
        ScanStats third;
        AudiobookLibrary::scanFolder(root, AudiobookLibrary::byPath(entries), &third);
        CHECK(third.retagged == 1);
        CHECK(third.reused == 11);

        // The persisted index round-trips, chapters included.
        const QString file = base + QStringLiteral("/index.json");
        CHECK(AudiobookLibrary::saveIndexFile(file, entries, {}));
        QString rules;
        const QVector<FileEntry> loaded = AudiobookLibrary::loadIndexFile(file, &rules);
        CHECK(loaded.size() == entries.size());
        CHECK(rules == AudiobookLibrary::parseStamp({}));
        const Index reloaded = AudiobookLibrary::buildIndex(loaded);
        const Book* rb = bookTitled(reloaded, QStringLiteral("The Ocean at the End of the Lane"));
        CHECK(rb != nullptr);
        if (rb) CHECK(rb->chapterCount == 3);

        // A DIFFERENT stamp drops the cache: that is the only thing that makes a settings change re-tag a
        // library whose files have not moved.
        CHECK(AudiobookLibrary::parseStamp({}) != AudiobookLibrary::parseStamp({ QStringLiteral(";") }));

        // A library with no chapters anywhere writes no chapter key at all — an ordinary collection's index
        // does not grow by a byte for a feature it does not use.
        const QVector<FileEntry> plain = AudiobookLibrary::scanFolder(dirParts);
        const QString plainFile = base + QStringLiteral("/plain.json");
        CHECK(AudiobookLibrary::saveIndexFile(plainFile, plain, {}));
        QFile pf(plainFile);
        CHECK(pf.open(QIODevice::ReadOnly));
        const QByteArray raw = pf.readAll();
        pf.close();
        CHECK(!raw.contains("\"ch\":"));
    }

    // ---- §11 The browse builders ---------------------------------------------------------------------
    {
        const browse::AudiobookEmptyNote none;
        const MediaCatalog rootCat = browse::audiobookRootCatalog(idx, none);
        CHECK(rootCat.title == QStringLiteral("Audiobooks"));
        // Both doors, because this library has narrators AND a series.
        CHECK(catalogHasType(rootCat, browse::kAudiobookNarratorsType));
        CHECK(catalogHasType(rootCat, browse::kAudiobookSeriesListType));
        // ...and one row per author after them.
        int authorRows = 0;
        for (const MediaItem& it : rootCat.items)
            if (it.type == QString::fromLatin1(browse::kAudiobookAuthorType)) ++authorRows;
        CHECK(authorRows == idx.authors.size());

        // A book level: the Play row FIRST, then the parts, each carrying the BOOK key so the router queues
        // the whole thing rather than opening one loose file.
        const Book* earthsea = bookTitled(idx, QStringLiteral("A Wizard of Earthsea"));
        CHECK(earthsea != nullptr);
        if (earthsea)
        {
            const MediaCatalog bookCat = browse::audiobookBookCatalog(idx, earthsea->key);
            CHECK(bookCat.title == QStringLiteral("A Wizard of Earthsea"));
            CHECK(bookCat.items.size() == 4);   // Play + three parts
            CHECK(bookCat.items.first().type == QString::fromLatin1(browse::kAudiobookPlayType));
            CHECK(browse::audiobookKeyOf(bookCat.items.first().mime, browse::kAudiobookPlayPrefix)
                  == earthsea->key);
            const MediaItem& part = bookCat.items.at(1);
            CHECK(part.type == QString::fromLatin1(browse::kAudiobookFileType));
            CHECK(part.url == earthsea->files.first().path);
            CHECK(browse::audiobookKeyOf(part.mime, browse::kAudiobookFilePrefix) == earthsea->key);
            // The key contains a folder path, so on Windows it contains a ':' — the case a section(':')
            // reader silently truncates.
            CHECK(!earthsea->key.isEmpty());
        }

        // A STALE key is an empty, titled catalog with no Play row — never a crash, and never an offer to
        // play something that is not there.
        const MediaCatalog gone = browse::audiobookBookCatalog(idx, QStringLiteral("no-such-book"));
        CHECK(gone.items.isEmpty());
        CHECK(!gone.title.isEmpty());
        CHECK(browse::audiobookAuthorCatalog(idx, QStringLiteral("nobody")).items.isEmpty());
        CHECK(browse::audiobookNarratorCatalog(idx, QStringLiteral("nobody")).items.isEmpty());
        CHECK(browse::audiobookSeriesCatalog(idx, QStringLiteral("nothing")).items.isEmpty());

        // NO DIMENSION, NO DOOR. An index built from books that name no narrator and no series gets a plain
        // list of authors, which is the compatibility claim the whole feature rests on.
        const Index bare = AudiobookLibrary::buildIndex(AudiobookLibrary::scanFolder(dirNat));
        CHECK(bare.narrators.isEmpty());
        CHECK(bare.series.isEmpty());
        const MediaCatalog bareCat = browse::audiobookRootCatalog(bare, none);
        CHECK(!catalogHasType(bareCat, browse::kAudiobookNarratorsType));
        CHECK(!catalogHasType(bareCat, browse::kAudiobookSeriesListType));

        // An EMPTY index says why rather than showing a blank shelf — and says nothing when the caller has
        // nothing to say, which is the other half of that parameter's contract.
        browse::AudiobookEmptyNote note;
        note.text = QStringLiteral("No audiobook folder yet.");
        const MediaCatalog emptyCat = browse::audiobookRootCatalog(Index{}, note);
        CHECK(emptyCat.items.size() == 1);
        CHECK(emptyCat.items.first().type == QStringLiteral("info"));
        CHECK(browse::audiobookRootCatalog(Index{}, none).items.isEmpty());
    }

    // ---- §12 Dormant ----------------------------------------------------------------------------------
    {
        ScanStats s;
        CHECK(AudiobookLibrary::scanFolder(QString(), {}, &s).isEmpty());
        CHECK(s.files == 0);
        CHECK(AudiobookLibrary::scanFolder(base + QStringLiteral("/nope")).isEmpty());
        CHECK(AudiobookLibrary::buildIndex({}).isEmpty());
        CHECK(AudiobookLibrary::index().isEmpty());          // nothing installed in this process
        CHECK(!AudiobookLibrary::indexReady());
        // A missing/corrupt persisted file loads as empty, which costs a full re-tag and nothing else.
        QString r;
        CHECK(AudiobookLibrary::loadIndexFile(base + QStringLiteral("/missing.json"), &r).isEmpty());
        CHECK(r.isEmpty());
    }

    if (g_fails == 0)
        std::printf("AUDIOBOOKS-OK\n");
    else
        std::printf("AUDIOBOOKS had %d failure(s)\n", g_fails);
    return g_fails == 0 ? 0 : 1;
}
