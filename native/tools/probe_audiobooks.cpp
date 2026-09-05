// Headless check of the local AUDIOBOOK LIBRARY (issue #139, increments 1 and 2): the scan and the index
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
//  13. PROGRESS (increment 2): the pure formula — finished parts' lengths plus the current part's position,
//      over the total — including the three answers that are NOT a number (unstarted, unknown length,
//      finished-by-mark), and the words the book page puts them in.
//  15. ENRICHMENT (issue #198): a provider's reply read by LABEL (and the series position taken from the
//      match rather than parsed out of a title that says "Book 3"), the confidence threshold, the merge when
//      two providers answer - and, above all, LOCAL TAGS ALWAYS WIN: a fully tagged book is unmoved by a
//      99%-confidence match that contradicts every field, while a tag that is PRESENT AND EMPTY is a blank
//      and fills. Plus: an enriched narrator mints a real Narrators bucket, a book with no match is exactly
//      what the scan found, and a REJECTION survives a re-scan and cannot be overwritten by the sweep that
//      stored it.
//  14. CHAPTERS (increment 2): the list for both book shapes — an .m4b's atoms and a folder's parts — the
//      row the listener is standing in, the natural order surviving into it, and the door that is not
//      offered for a book of one row.
//
// Prints AUDIOBOOKS-OK on success; any failure prints AUDIOBOOKS-FAIL <cond> (line) and exits non-zero.
//
// Isolation: AppPaths::dataDir() is this process's own scratch dir (issue #42), so the whole fixture library
// is written under it and goes away at exit. Nothing is written beside the exe.
#include "AudiobookLibrary.h"
#include "AudiobookCatalogs.h"
#include "AudiobookMeta.h"        // #198: the enrichment rule
#include "AudiobookMatchStore.h"  // #198: the match + rejection record
#include "MusicLibrary.h"     // §9: the same file, read by the OTHER library
#include "AppPaths.h"
#include "MusicFixtures.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
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

        // A book level: the Play row FIRST, then the Chapters door (§14), then the parts, each carrying the
        // BOOK key so the router queues the whole thing rather than opening one loose file.
        const Book* earthsea = bookTitled(idx, QStringLiteral("A Wizard of Earthsea"));
        CHECK(earthsea != nullptr);
        if (earthsea)
        {
            const MediaCatalog bookCat = browse::audiobookBookCatalog(idx, earthsea->key);
            CHECK(bookCat.title == QStringLiteral("A Wizard of Earthsea"));
            CHECK(bookCat.items.size() == 5);   // Play + Chapters + three parts
            CHECK(bookCat.items.first().type == QString::fromLatin1(browse::kAudiobookPlayType));
            CHECK(browse::audiobookKeyOf(bookCat.items.first().mime, browse::kAudiobookPlayPrefix)
                  == earthsea->key);
            const MediaItem& part = bookCat.items.at(2);
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

    // ---- §13 PROGRESS: the number a tile and a book page show (#139 increment 2) ----------------------
    //
    // A PURE function over the index and the marks the PLAYER already writes — no store is opened here and
    // none is opened by it. The fixtures' own parts are two seconds each (MusicFixtures.h), which is a real
    // length but not a book-scale one, so the arithmetic below runs on the REAL scanned book with its parts'
    // lengths set to an hour apiece: real paths, real order, real shape, and numbers large enough that a
    // rounding mistake is visible rather than absorbed.
    {
        const Book* real = bookTitled(idx, QStringLiteral("A Wizard of Earthsea"));
        CHECK(real != nullptr);
        if (real)
        {
            Book b = *real;
            for (BookFile& f : b.files) f.durationSec = 3600;   // three parts, an hour each

            // THE CASE #139 NAMES. Part one played to its end, so the player DROPPED its mark; part two is
            // 600 s in. The last part still carrying a position is where the listener is and everything
            // before it has been heard — openAudiobook's rule, as a number, over the same marks.
            const QString p2 = b.files.at(1).path;
            const AudiobookLibrary::PartPositionFn at600 =
                [p2](const QString& path) { return path == p2 ? 600.0 : 0.0; };

            const AudiobookLibrary::Progress p = AudiobookLibrary::progressFor(b, at600);
            CHECK(p.known);
            CHECK(p.started);
            CHECK(!p.finished);
            CHECK(p.partIndex == 1);
            CHECK(qRound(p.partPosSec) == 600);
            CHECK(p.listenedSec == 4200);            // 3600 heard, plus 600 into part two
            CHECK(p.remainingSec == 6600);           // 10800 - 4200
            CHECK(qAbs(p.fraction - 4200.0 / 10800.0) < 1e-9);

            // UNSTARTED. The lengths are known, so the ANSWER is known — and the answer is "show nothing",
            // which is a different state from "cannot tell" and has to stay different.
            const AudiobookLibrary::PartPositionFn nowhere = [](const QString&) { return 0.0; };
            const AudiobookLibrary::Progress none = AudiobookLibrary::progressFor(b, nowhere);
            CHECK(none.known);
            CHECK(!none.started);
            CHECK(!none.finished);
            CHECK(none.fraction == 0.0);
            CHECK(none.remainingSec == 0);
            CHECK(none.partIndex == -1);

            // A POSITION AT OR UNDER A SECOND IS NOT A MARK — the very threshold openAudiobook applies, so
            // the tile and the play verb cannot disagree about whether a book has been started at all.
            CHECK(!AudiobookLibrary::progressFor(b, [](const QString&) { return 0.9; }).started);
            // ...and no supplier at all is simply an unstarted book, not a crash.
            CHECK(!AudiobookLibrary::progressFor(b, AudiobookLibrary::PartPositionFn{}).started);

            // FINISHED IS THE COMPLETION MARK, and it needs no position: the player drops the last part's
            // mark at its end, so an unmarked book with no positions is "never opened" and must never be
            // congratulated for a book nobody has pressed.
            const AudiobookLibrary::Progress done =
                AudiobookLibrary::progressFor(b, nowhere, /*completed*/ true);
            CHECK(done.known);
            CHECK(done.started);
            CHECK(done.finished);
            CHECK(done.fraction == 1.0);
            CHECK(done.remainingSec == 0);
            CHECK(done.listenedSec == 10800);

            // ONE PART WITH NO LENGTH POISONS THE WHOLE BOOK, deliberately: the tile draws no bar and the
            // page says nothing, rather than a fraction of a total that had to invent a piece of itself.
            Book gap = b;
            gap.files[2].durationSec = 0;
            const AudiobookLibrary::Progress unknown = AudiobookLibrary::progressFor(gap, at600);
            CHECK(!unknown.known);
            CHECK(!unknown.started);
            CHECK(unknown.fraction == 0.0);
            CHECK(unknown.remainingSec == 0);
            // ...but a MARK still reads, because a mark is a statement rather than a measurement.
            CHECK(AudiobookLibrary::progressFor(gap, at600, /*completed*/ true).finished);

            // A SINGLE-FILE BOOK is the same formula with one term, not a second code path.
            Book solo = b;
            solo.files.resize(1);
            const QString only = solo.files.first().path;
            const AudiobookLibrary::Progress half = AudiobookLibrary::progressFor(
                solo, [only](const QString& path) { return path == only ? 1800.0 : 0.0; });
            CHECK(half.known);
            CHECK(half.started);
            CHECK(half.partIndex == 0);
            CHECK(half.remainingSec == 1800);
            CHECK(qAbs(half.fraction - 0.5) < 1e-9);

            // A position PAST the part's own length (mpv and a tag can disagree by a second) is clamped, so
            // a fraction can never leave 0..1 and "left" can never go negative.
            const AudiobookLibrary::Progress over = AudiobookLibrary::progressFor(
                solo, [only](const QString& path) { return path == only ? 99999.0 : 0.0; });
            CHECK(over.fraction <= 1.0);
            CHECK(over.remainingSec == 0);

            // A BOOK WITH NO FILES cannot be placed at all — a rescan can empty one out from under a row.
            Book empty = b; empty.files.clear();
            CHECK(!AudiobookLibrary::progressFor(empty, at600).known);

            // ---- ...and what the PAGE says, in words ---------------------------------------------------
            // Through the real builder, because the WORDING is the deliverable: "14h 20m left" is the line
            // #139 asks for, and a probe on the struct alone would not notice it going missing.
            const QString key = b.key;
            const browse::AudiobookProgressFn started = [&b, at600](const Book& x) {
                return x.key == b.key ? AudiobookLibrary::progressFor(b, at600)
                                      : AudiobookLibrary::Progress{};
            };
            const MediaCatalog page = browse::audiobookBookCatalog(idx, key, {}, started);
            CHECK(!page.items.isEmpty());
            if (!page.items.isEmpty())
            {
                // 6600 s left, rounded to the five minutes the line is honest to.
                CHECK(page.items.first().subtitle.startsWith(QStringLiteral("1h 50m left")));
            }
            const browse::AudiobookProgressFn finished = [&b](const Book& x) {
                return x.key == b.key
                           ? AudiobookLibrary::progressFor(b, [](const QString&) { return 0.0; }, true)
                           : AudiobookLibrary::Progress{};
            };
            CHECK(browse::audiobookBookCatalog(idx, key, {}, finished)
                      .items.first().subtitle.startsWith(QStringLiteral("Finished")));
            // NO SUPPLIER, NO LINE: the page reads exactly as it did before this increment.
            CHECK(!browse::audiobookBookCatalog(idx, key).items.first().subtitle
                       .contains(QStringLiteral("left")));

            // ---- ...and what the TILE carries -----------------------------------------------------------
            // The fraction rides the ROW (MediaItem::progress) because a book's position is filed under its
            // PARTS and not under the row's own id, so the surface's ordinary resume lookup finds nothing.
            const Author* ursula = bucketNamed(idx.authors, QStringLiteral("Ursula K. Le Guin"));
            CHECK(ursula != nullptr);
            if (ursula)
            {
                const MediaCatalog shelf = browse::audiobookAuthorCatalog(idx, ursula->key, {}, started);
                const MediaItem* tile = nullptr;
                for (const MediaItem& it : shelf.items)
                    if (browse::audiobookKeyOf(it.mime, browse::kAudiobookBookPrefix) == key) tile = &it;
                CHECK(tile != nullptr);
                if (tile) CHECK(qAbs(tile->progress - 4200.0 / 10800.0) < 1e-9);
                // ...and with no supplier the row keeps the -1 default, i.e. "look it up the usual way".
                const MediaCatalog plain = browse::audiobookAuthorCatalog(idx, ursula->key);
                CHECK(!plain.items.isEmpty());
                if (!plain.items.isEmpty()) CHECK(plain.items.first().progress < 0.0);
            }
        }
    }

    // ---- §14 CHAPTERS: one list, two book shapes ------------------------------------------------------
    {
        // (a) AN .m4b — the chapter atoms the scan already read, in order, all pointing at the one file.
        const Book* m4b = bookTitled(idx, QStringLiteral("The Ocean at the End of the Lane"));
        CHECK(m4b != nullptr);
        if (m4b && m4b->files.size() == 1)
        {
            // The atoms reached the BROWSE index, which is the widening this increment needed: increment 1
            // carried only the COUNT up here, and a list cannot be drawn from a count.
            CHECK(m4b->files.first().chapters.size() == 3);
            const QString file = m4b->files.first().path;
            // 100 s in: past "Chapter One" (90 s) and well short of "Chapter Two" (600 s).
            const AudiobookLibrary::PartPositionFn at100 =
                [file](const QString& p) { return p == file ? 100.0 : 0.0; };
            const QVector<AudiobookLibrary::ChapterRow> rows = AudiobookLibrary::chapterRows(*m4b, at100);
            CHECK(rows.size() == 3);
            if (rows.size() == 3)
            {
                CHECK(rows.at(0).title == QStringLiteral("Prologue"));
                CHECK(rows.at(1).title == QStringLiteral("Chapter One"));
                CHECK(rows.at(2).title == QStringLiteral("Chapter Two"));
                for (const AudiobookLibrary::ChapterRow& r : rows)
                {
                    CHECK(r.path == file);      // the queue entry openAudiobook will start on
                    CHECK(r.fileIndex == 0);
                }
                CHECK(rows.at(0).startSec == 0);
                CHECK(rows.at(1).startSec == 90);
                CHECK(rows.at(2).startSec == 600);
                // A chapter's length is the NEXT one's start. The last one's would need the file's own
                // length, which this fixture honestly does not carry, so it is 0 rather than guessed.
                CHECK(rows.at(0).durationSec == 90);
                CHECK(rows.at(1).durationSec == 510);
                CHECK(rows.at(2).durationSec == 0);
                // THE MARKER IS ON THE CHAPTER THE POSITION IS INSIDE, not on the nearest boundary.
                CHECK(!rows.at(0).current);
                CHECK(rows.at(1).current);
                CHECK(!rows.at(2).current);
                // And the wording the menu shows, marker included.
                const QStringList menu = browse::audiobookChapterMenuRows(rows);
                CHECK(menu.size() == 3);
                if (menu.size() == 3)
                {
                    CHECK(menu.at(1).startsWith(QString::fromUtf8("\xE2\x96\xB6")));
                    CHECK(!menu.at(0).startsWith(QString::fromUtf8("\xE2\x96\xB6")));
                    CHECK(menu.at(0).contains(QStringLiteral("Prologue")));
                    CHECK(menu.at(0).contains(QStringLiteral("1:30")));   // 90 s, as a part time
                    CHECK(!menu.at(2).contains(QLatin1Char(':')));        // no length, so no time at all
                }
            }
            // NO POSITION AT ALL marks nothing. "Chapter one by default" would tell somebody who has never
            // opened the book that they are standing in the middle of it.
            for (const AudiobookLibrary::ChapterRow& r :
                 AudiobookLibrary::chapterRows(*m4b, AudiobookLibrary::PartPositionFn{}))
                CHECK(!r.current);
        }

        // (b) A FOLDER OF PARTS, carrying no chapter atom anywhere: ONE ROW PER PART, each starting at the
        //     top of its own file, in the index's order (which the TRACK tag decided — the files are z/y/x).
        const Book* parts = bookTitled(idx, QStringLiteral("A Wizard of Earthsea"));
        CHECK(parts != nullptr);
        if (parts && parts->files.size() == 3)
        {
            const QString second = parts->files.at(1).path;
            const QVector<AudiobookLibrary::ChapterRow> rows = AudiobookLibrary::chapterRows(
                *parts, [second](const QString& p) { return p == second ? 1.5 : 0.0; });
            CHECK(rows.size() == 3);
            if (rows.size() == 3)
            {
                CHECK(rows.at(0).title == QStringLiteral("Part 1"));
                CHECK(rows.at(1).title == QStringLiteral("Part 2"));
                CHECK(rows.at(2).title == QStringLiteral("Part 3"));
                CHECK(rows.at(0).path.endsWith(QStringLiteral("z part.mp3")));
                for (int i = 0; i < 3; ++i)
                {
                    CHECK(rows.at(i).startSec == 0);    // a part row always opens its file at the top
                    CHECK(rows.at(i).fileIndex == i);
                    CHECK(rows.at(i).durationSec == 2); // the fixture's real length
                    // ACTIVATING ROW N PRODUCES THE QUEUE THE NORMAL OPEN PRODUCES, POSITIONED AT N. The
                    // queue openAudiobook builds IS Book::files in this order and it locates its start with
                    // queue.indexOf(startPath) — so the whole claim is that the row's path is that file.
                    CHECK(rows.at(i).path == parts->files.at(i).path);
                }
                CHECK(!rows.at(0).current);
                CHECK(rows.at(1).current);      // the LAST part carrying a position, per §13's rule
                CHECK(!rows.at(2).current);
            }
        }

        // (c) NATURAL ORDER survives into the list — 1, 2, 10 — because the rows follow Book::files and
        //     never re-sort. This is the ordering #205 found, one surface further on.
        const Book* nat = bookTitled(idx, QStringLiteral("unpadded"));
        CHECK(nat != nullptr);
        if (nat)
        {
            const QVector<AudiobookLibrary::ChapterRow> rows =
                AudiobookLibrary::chapterRows(*nat, AudiobookLibrary::PartPositionFn{});
            CHECK(rows.size() == 3);
            if (rows.size() == 3)
            {
                CHECK(rows.at(0).path.endsWith(QStringLiteral("1 - chapter.mp3")));
                CHECK(rows.at(1).path.endsWith(QStringLiteral("2 - chapter.mp3")));
                CHECK(rows.at(2).path.endsWith(QStringLiteral("10 - chapter.mp3")));
            }
        }

        // (d) A CHAPTERLESS SINGLE FILE is ONE row — a list of one, which is why no door is offered for it.
        const Book* one = bookTitled(idx, QStringLiteral("Book One"));
        CHECK(one != nullptr);
        if (one)
        {
            CHECK(AudiobookLibrary::chapterRows(*one, AudiobookLibrary::PartPositionFn{}).size() == 1);
            CHECK(!catalogHasType(browse::audiobookBookCatalog(idx, one->key),
                                  browse::kAudiobookChaptersType));
        }

        // ---- The DOOR, on the book page ---------------------------------------------------------------
        if (parts)
        {
            const MediaCatalog page = browse::audiobookBookCatalog(idx, parts->key);
            CHECK(catalogHasType(page, browse::kAudiobookChaptersType));
            CHECK(page.items.size() > 1);
            if (page.items.size() > 1)
            {
                // Directly under the play verb rather than below fifty parts, and carrying the BOOK key,
                // read the one way every reader in this feature reads one.
                const MediaItem& door = page.items.at(1);
                CHECK(door.type == QString::fromLatin1(browse::kAudiobookChaptersType));
                CHECK(browse::audiobookKeyOf(door.mime, browse::kAudiobookChaptersPrefix) == parts->key);
                CHECK(!door.expandable);
                CHECK(door.subtitle.contains(QStringLiteral("3 part")));   // three parts behind the door
            }
        }
        // A book that DOES carry atoms says chapters, not parts.
        if (m4b)
        {
            const MediaCatalog mp = browse::audiobookBookCatalog(idx, m4b->key);
            CHECK(catalogHasType(mp, browse::kAudiobookChaptersType));
            for (const MediaItem& it : mp.items)
                if (it.type == QString::fromLatin1(browse::kAudiobookChaptersType))
                    CHECK(it.subtitle.contains(QStringLiteral("3 chapter")));
        }
        // A stale route still yields an empty, titled catalog — no door, no play row.
        CHECK(!catalogHasType(browse::audiobookBookCatalog(idx, QStringLiteral("no-such-book")),
                              browse::kAudiobookChaptersType));
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

    // ---- §15 ENRICHMENT (issue #198): narrator, series position, cover, year — and what may NOT land ----
    //
    // A SECOND, SEPARATE fixture root, scanned on its own, because everything here is about the difference
    // between "what the tags said" and "what a provider said" and the §1-§14 library is deliberately
    // well-tagged. Three books:
    //   BARE      — an album title and an artist, and nothing else. The population the feature exists for.
    //   TAGGED    — a narrator, a series, a series place, a year and a cover, ALL of them tagged. The match
    //               below contradicts every one of them, and not one of them may move.
    //   BLANKTAG  — a NARRATOR and a SERIES frame that are present and hold only whitespace. The commonest
    //               shape a bulk converter leaves behind, and the case a naive "the file has the field"
    //               test reads as "the file said so" — which would make the feature dormant on exactly the
    //               libraries it is for, and leave a whitespace narrator no re-scan could ever clear.
    {
        using AudiobookMeta::Match;
        const QString eroot = base + QStringLiteral("/enrich");

        {
            Mp3Tags t;
            t.artist = QStringLiteral("J. R. R. Tolkien");
            t.album  = QStringLiteral("The Silmarillion");
            for (int i = 1; i <= 2; ++i)
            { t.title = QStringLiteral("Part %1").arg(i); t.track = QString::number(i);
              CHECK(writeMp3(eroot + QStringLiteral("/Bare/part%1.mp3").arg(i), t)); }
        }
        {
            Mp3Tags t;
            t.artist     = QStringLiteral("Frank Herbert");
            t.album      = QStringLiteral("Dune");
            t.title      = QStringLiteral("Dune");
            t.narrator   = QStringLiteral("Scott Brick");
            t.series     = QStringLiteral("Dune Chronicles");
            t.seriesPart = QStringLiteral("1");
            t.year       = QStringLiteral("1965");
            t.cover      = jpeg;
            CHECK(writeMp3(eroot + QStringLiteral("/Tagged/dune.mp3"), t));
        }
        {
            Mp3Tags t;
            t.artist   = QStringLiteral("William Gibson");
            t.album    = QStringLiteral("Neuromancer");
            t.title    = QStringLiteral("Neuromancer");
            t.narrator = QStringLiteral("   ");   // PRESENT, and empty. See the note above.
            t.series   = QStringLiteral("  ");
            CHECK(writeMp3(eroot + QStringLiteral("/Blank/neuro.mp3"), t));
        }

        const QVector<FileEntry> scanned = AudiobookLibrary::scanFolder(eroot);
        CHECK(scanned.size() == 4);
        const Index bare = AudiobookLibrary::buildIndex(scanned);
        CHECK(bare.bookCount == 3);

        // The three book keys, from the SCAN's own grouping rule rather than rebuilt here.
        QString keyBare, keyTagged, keyBlank;
        for (const FileEntry& e : scanned)
        {
            if (e.book == QStringLiteral("The Silmarillion")) keyBare   = AudiobookLibrary::bookKeyFor(e);
            if (e.book == QStringLiteral("Dune"))             keyTagged = AudiobookLibrary::bookKeyFor(e);
            if (e.book == QStringLiteral("Neuromancer"))      keyBlank  = AudiobookLibrary::bookKeyFor(e);
        }
        CHECK(!keyBare.isEmpty());
        CHECK(!keyTagged.isEmpty());
        CHECK(!keyBlank.isEmpty());

        // "Is this book filed under a narrator bucket of this name?" — the question the Narrators view is,
        // asked of an index. The TAGGED book already mints one bucket ("Scott Brick") off its own tag, so a
        // bare `narrators.isEmpty()` would be asserting the wrong thing entirely.
        const auto filedUnder = [](const Index& ix, const QString& who, const QString& bk) {
            for (const AudiobookLibrary::Narrator& n : ix.narrators)
                if (n.name == who)
                    for (const Book& nb : n.books) if (nb.key == bk) return true;
            return false;
        };

        // -- (a) THE PRECEDENCE RULE ITSELF, as three functions ------------------------------------------
        CHECK(AudiobookMeta::tagCarries(QStringLiteral("Rob Inglis")));
        CHECK(!AudiobookMeta::tagCarries(QString()));
        CHECK(!AudiobookMeta::tagCarries(QStringLiteral("   ")));      // present-and-empty IS a blank
        CHECK(AudiobookMeta::fill(QStringLiteral("Mine"), QStringLiteral("Theirs"))
              == QStringLiteral("Mine"));
        CHECK(AudiobookMeta::fill(QStringLiteral("   "), QStringLiteral("Theirs"))
              == QStringLiteral("Theirs"));
        CHECK(AudiobookMeta::fill(QString(), QStringLiteral("Theirs")) == QStringLiteral("Theirs"));
        CHECK(AudiobookMeta::fillInt(7, 3) == 7);
        CHECK(AudiobookMeta::fillInt(0, 3) == 3);
        CHECK(AudiobookMeta::fillInt(0, 0) == 0);

        // -- (b) READING A PROVIDER'S REPLY, by LABEL and never by title --------------------------------
        MediaDetail reply;
        reply.title    = QStringLiteral("The Silmarillion: Book 3 of the Middle-earth Chronicles");
        reply.overview = QStringLiteral("The elder days of Middle-earth.");
        reply.imageUrl = QStringLiteral("https://example.invalid/silm.jpg");
        reply.facts.push_back({ QStringLiteral("Author"),      QStringLiteral("J. R. R. Tolkien") });
        reply.facts.push_back({ QStringLiteral("Narrated by"), QStringLiteral("Martin Shaw") });
        reply.facts.push_back({ QStringLiteral("Series"),      QStringLiteral("Middle-earth") });
        reply.facts.push_back({ QStringLiteral("Published"),   QStringLiteral("1977-09-15") });
        reply.facts.push_back({ QStringLiteral("Runtime"),     QStringLiteral("14h 20m") });
        Match m = AudiobookMeta::fromDetail(reply, QStringLiteral("com.everythingbox.openlibrary"));
        CHECK(m.narrator == QStringLiteral("Martin Shaw"));
        CHECK(m.series == QStringLiteral("Middle-earth"));
        CHECK(m.year == 1977);
        CHECK(m.runtimeSec == 14 * 3600 + 20 * 60);
        CHECK(m.coverUrl == QStringLiteral("https://example.invalid/silm.jpg"));
        CHECK(m.matchAuthor == QStringLiteral("J. R. R. Tolkien"));
        CHECK(m.hasFields());
        // THE POSITION IS NOT PARSED OUT OF THE TITLE. The reply's title says "Book 3" and carries no
        // position field, so the position stays 0 — the book keeps the place the scan gave it, and a boxed-
        // set volume number never becomes a series index. This is the assertion #198 asks for by name.
        CHECK(m.seriesIndex == 0);
        // ...and WITH a position field it is taken, from that field.
        reply.facts.push_back({ QStringLiteral("Series position"), QStringLiteral("#2") });
        CHECK(AudiobookMeta::fromDetail(reply, QStringLiteral("p")).seriesIndex == 2);

        // The value parsers, each refusing rather than guessing.
        CHECK(AudiobookMeta::parseYear(QStringLiteral("1977")) == 1977);
        CHECK(AudiobookMeta::parseYear(QStringLiteral("April 1998")) == 1998);
        CHECK(AudiobookMeta::parseYear(QStringLiteral("no idea")) == 0);
        CHECK(AudiobookMeta::parseSeriesIndex(QStringLiteral("3.5")) == 3);
        CHECK(AudiobookMeta::parseSeriesIndex(QStringLiteral("Book 3")) == 3);
        CHECK(AudiobookMeta::parseSeriesIndex(QString()) == 0);
        CHECK(AudiobookMeta::parseRuntimeSec(QStringLiteral("14:20:00")) == 14 * 3600 + 20 * 60);
        CHECK(AudiobookMeta::parseRuntimeSec(QStringLiteral("860 min")) == 860 * 60);
        CHECK(AudiobookMeta::parseRuntimeSec(QStringLiteral("51600")) == 51600);
        CHECK(AudiobookMeta::parseRuntimeSec(QStringLiteral("ages")) == 0);

        // -- (c) CONFIDENCE, and the threshold that keeps a bad match off the shelf ----------------------
        const Book* bookBare = bare.book(keyBare);
        CHECK(bookBare != nullptr);
        if (bookBare)
        {
            Match good = AudiobookMeta::fromDetail(reply, QStringLiteral("com.everythingbox.openlibrary"));
            good.confidence = AudiobookMeta::confidenceFor(*bookBare, good);
            CHECK(good.confidence >= AudiobookMeta::kAcceptThreshold);

            Match wrong = good;
            wrong.matchTitle  = QStringLiteral("A Brief History of Time");
            wrong.matchAuthor = QStringLiteral("Stephen Hawking");
            CHECK(AudiobookMeta::confidenceFor(*bookBare, wrong) < AudiobookMeta::kAcceptThreshold);

            Match unnamed = good;
            unnamed.matchTitle = QString();
            CHECK(AudiobookMeta::confidenceFor(*bookBare, unnamed) == 0);
        }
        // Articles and punctuation are not a difference.
        CHECK(AudiobookMeta::normalizedName(QStringLiteral("The Hobbit"))
              == AudiobookMeta::normalizedName(QStringLiteral("Hobbit, The")));

        // -- (d) TAGS WIN, FIELD BY FIELD, and a blank is not a value -----------------------------------
        Match forBare = AudiobookMeta::fromDetail(reply, QStringLiteral("com.everythingbox.openlibrary"));
        forBare.seriesIndex = 2;
        forBare.confidence  = 99;
        // A match that CONTRADICTS every tag the well-tagged book carries.
        Match forTagged;
        forTagged.matchTitle  = QStringLiteral("Dune");
        forTagged.narrator    = QStringLiteral("Somebody Else");
        forTagged.series      = QStringLiteral("Another Series");
        forTagged.seriesIndex = 9;
        forTagged.year        = 2001;
        forTagged.runtimeSec  = 60;
        forTagged.confidence  = 99;
        Match forBlank;
        forBlank.matchTitle  = QStringLiteral("Neuromancer");
        forBlank.narrator    = QStringLiteral("Robertson Dean");
        forBlank.series      = QStringLiteral("Sprawl");
        forBlank.seriesIndex = 1;
        forBlank.year        = 1984;
        forBlank.confidence  = 99;

        QHash<QString, Match> matches;
        matches.insert(keyBare, forBare);
        matches.insert(keyTagged, forTagged);
        matches.insert(keyBlank, forBlank);

        QVector<FileEntry> enriched = scanned;
        const int touched = AudiobookMeta::applyToEntries(enriched, matches);
        CHECK(touched == 3);        // both parts of the bare book, and the blank-tagged one
        const Index eidx = AudiobookLibrary::buildIndex(enriched);
        CHECK(eidx.bookCount == 3);

        const Book* eb = eidx.book(keyBare);
        CHECK(eb != nullptr);
        if (eb)
        {
            CHECK(eb->narrator == QStringLiteral("Martin Shaw"));
            CHECK(eb->series == QStringLiteral("Middle-earth"));
            CHECK(eb->seriesIndex == 2);
            CHECK(eb->year == 1977);
        }
        // THE TAGGED BOOK IS UNTOUCHED, every field, at 99% confidence, from a provider that disagreed
        // about all of them. This is the whole safety of the feature.
        const Book* et = eidx.book(keyTagged);
        CHECK(et != nullptr);
        if (et)
        {
            CHECK(et->narrator == QStringLiteral("Scott Brick"));
            CHECK(et->series == QStringLiteral("Dune Chronicles"));
            CHECK(et->seriesIndex == 1);
            CHECK(et->year == 1965);
        }
        // ...and the PRESENT-AND-EMPTY tags are blanks, so they fill.
        const Book* ez = eidx.book(keyBlank);
        CHECK(ez != nullptr);
        if (ez)
        {
            CHECK(ez->narrator == QStringLiteral("Robertson Dean"));
            CHECK(ez->series == QStringLiteral("Sprawl"));
            CHECK(ez->seriesIndex == 1);
        }
        // A PART'S LENGTH IS NEVER TAKEN FROM A MATCH — the progress bar divides by it.
        for (int i = 0; i < scanned.size(); ++i)
            CHECK(enriched.at(i).durationSec == scanned.at(i).durationSec);

        // -- (e) NARRATOR IS A BROWSE DIMENSION FOR ENRICHED BOOKS TOO ----------------------------------
        // Before: only the TAGGED book names a narrator, so that is the only bucket there is and neither of
        // the other two books is reachable from the Narrators view at all.
        CHECK(bare.narrators.size() == 1);
        CHECK(filedUnder(bare, QStringLiteral("Scott Brick"), keyTagged));
        CHECK(!filedUnder(bare, QStringLiteral("Martin Shaw"), keyBare));
        // After: the bucket is minted by exactly the code that files a TAGGED narrator, so the enriched
        // book opens from either side and plays the same queue. THIS is #198's payoff.
        CHECK(eidx.narrators.size() == 3);
        CHECK(filedUnder(eidx, QStringLiteral("Martin Shaw"), keyBare));
        CHECK(filedUnder(eidx, QStringLiteral("Robertson Dean"), keyBlank));
        CHECK(filedUnder(eidx, QStringLiteral("Scott Brick"), keyTagged));   // ...and the tagged one is still there
        CHECK(catalogHasType(browse::audiobookRootCatalog(eidx, browse::AudiobookEmptyNote{}),
                             browse::kAudiobookNarratorsType));

        // -- (f) NO MATCH, A WEAK MATCH AND A REJECTED MATCH ALL LEAVE THE SCAN ALONE -------------------
        {
            QVector<FileEntry> untouched = scanned;
            CHECK(AudiobookMeta::applyToEntries(untouched, {}) == 0);
            const Index n = AudiobookLibrary::buildIndex(untouched);
            CHECK(n.narrators.size() == bare.narrators.size());   // no placeholder, no half-filled record
            CHECK(!filedUnder(n, QStringLiteral("Martin Shaw"), keyBare));
            CHECK(n.bookCount == bare.bookCount);
            const Book* nb = n.book(keyBare);
            CHECK(nb != nullptr);
            if (nb) { CHECK(nb->narrator.isEmpty()); CHECK(nb->series.isEmpty()); CHECK(nb->year == 0); }
        }
        {
            Match weak = forBare;
            weak.confidence = AudiobookMeta::kAcceptThreshold - 1;
            QVector<FileEntry> e2 = scanned;
            QHash<QString, Match> only; only.insert(keyBare, weak);
            CHECK(AudiobookMeta::applyToEntries(e2, only) == 0);
        }
        {
            Match no = forBare;
            no.rejected = true;
            QVector<FileEntry> e3 = scanned;
            QHash<QString, Match> only; only.insert(keyBare, no);
            CHECK(AudiobookMeta::applyToEntries(e3, only) == 0);
        }

        // -- (g) TWO PROVIDERS ANSWERED: role precedence, field by field --------------------------------
        CHECK(AudiobookMeta::providerPriority(QStringLiteral("com.everythingbox.openlibrary"))
              < AudiobookMeta::providerPriority(QStringLiteral("com.everythingbox.googlebooks")));
        CHECK(AudiobookMeta::providerPriority(QStringLiteral("com.everythingbox.googlebooks"))
              < AudiobookMeta::providerPriority(QStringLiteral("org.example.someaddon")));
        {
            Match hi;   // the leading provider: an identity, a narrator, no description, no cover
            hi.provider   = QStringLiteral("com.everythingbox.openlibrary");
            hi.matchId    = QStringLiteral("OL1W");
            hi.matchTitle = QStringLiteral("The Silmarillion");
            hi.narrator   = QStringLiteral("Martin Shaw");
            hi.year       = 1977;
            Match lo;   // the backfiller: disagrees about the narrator, has the description and the cover
            lo.provider    = QStringLiteral("com.everythingbox.googlebooks");
            lo.matchId     = QStringLiteral("GB9");
            lo.matchTitle  = QStringLiteral("Silmarillion (Illustrated)");
            lo.narrator    = QStringLiteral("Somebody Else");
            lo.description = QStringLiteral("The elder days.");
            lo.coverUrl    = QStringLiteral("https://example.invalid/gb.jpg");
            lo.year        = 1999;
            lo.seriesIndex = 4;
            const Match merged = AudiobookMeta::mergeLowerPriority(hi, lo);
            CHECK(merged.provider == QStringLiteral("com.everythingbox.openlibrary"));
            CHECK(merged.matchId == QStringLiteral("OL1W"));
            CHECK(merged.narrator == QStringLiteral("Martin Shaw"));   // the leader keeps what it has
            CHECK(merged.year == 1977);
            CHECK(merged.description == QStringLiteral("The elder days."));   // ...the other backfills
            CHECK(merged.coverUrl == QStringLiteral("https://example.invalid/gb.jpg"));
            CHECK(merged.seriesIndex == 4);
            // A leader that matched NOTHING hands the identity over, so "reject this match" can never name
            // a provider that supplied no fields.
            const Match onlyLo = AudiobookMeta::mergeLowerPriority(Match{}, lo);
            CHECK(onlyLo.provider == QStringLiteral("com.everythingbox.googlebooks"));
            CHECK(onlyLo.matchTitle == QStringLiteral("Silmarillion (Illustrated)"));
        }

        // -- (h) THE STORE: a match is remembered, and a REJECTION outlives a re-scan -------------------
        AudiobookMatches::clearAll();
        CHECK(!AudiobookMatches::has(keyBare));
        CHECK(AudiobookMatches::forBooks({ keyBare, keyTagged }).isEmpty());
        AudiobookMatches::set(keyBare, forBare);
        CHECK(AudiobookMatches::has(keyBare));
        CHECK(AudiobookMatches::count() == 1);
        CHECK(AudiobookMatches::get(keyBare).narrator == QStringLiteral("Martin Shaw"));
        CHECK(AudiobookMatches::get(keyBare).updatedAt > 0);
        CHECK(!AudiobookMatches::isRejected(keyBare));
        CHECK(AudiobookMatches::forBooks({ keyBare }).size() == 1);
        // The canonical JSON round-trips, omitting everything unset.
        {
            const QJsonObject j = AudiobookMeta::toJson(AudiobookMatches::get(keyBare));
            CHECK(!j.contains(QStringLiteral("rejected")));
            CHECK(AudiobookMeta::fromJson(j).narrator == QStringLiteral("Martin Shaw"));
            CHECK(AudiobookMeta::toJson(Match{}).isEmpty());
        }
        // ...and the whole point: reject it.
        AudiobookMatches::reject(keyBare);
        CHECK(AudiobookMatches::isRejected(keyBare));
        CHECK(AudiobookMatches::rejectedCount() == 1);
        CHECK(AudiobookMatches::count() == 0);
        CHECK(AudiobookMatches::get(keyBare).narrator.isEmpty());                // the fields are gone
        CHECK(AudiobookMatches::get(keyBare).matchTitle == forBare.matchTitle);  // ...what it WAS is not
        // A SWEEP CANNOT PUT IT BACK. This is the bit that makes the rejection permanent rather than a
        // pause: the very code path that stored it in the first place is refused.
        AudiobookMatches::set(keyBare, forBare);
        CHECK(AudiobookMatches::isRejected(keyBare));
        CHECK(AudiobookMatches::get(keyBare).narrator.isEmpty());
        // AND IT SURVIVES A RE-SCAN — a fresh scan of the same root, a fresh store read, a fresh index.
        AudiobookMatches::invalidate();
        const QVector<FileEntry> rescanned = AudiobookLibrary::scanFolder(eroot);
        CHECK(rescanned.size() == scanned.size());
        QVector<FileEntry> reapplied = rescanned;
        QStringList allKeys;
        for (const FileEntry& e : rescanned) allKeys << AudiobookLibrary::bookKeyFor(e);
        CHECK(AudiobookMeta::applyToEntries(reapplied, AudiobookMatches::forBooks(allKeys)) == 0);
        const Index ridx = AudiobookLibrary::buildIndex(reapplied);
        CHECK(ridx.narrators.size() == bare.narrators.size());   // still exactly what the tags said
        CHECK(!filedUnder(ridx, QStringLiteral("Martin Shaw"), keyBare));
        const Book* rb = ridx.book(keyBare);
        CHECK(rb != nullptr);
        if (rb) { CHECK(rb->narrator.isEmpty()); CHECK(rb->year == 0); }
        // Clearing the record is the deliberate act that makes it matchable again.
        AudiobookMatches::clear(keyBare);
        CHECK(!AudiobookMatches::has(keyBare));
        CHECK(AudiobookMatches::rejectedCount() == 0);

        // -- (i) THE MATCH IS SURFACED: the row on the book's own level ---------------------------------
        CHECK(!catalogHasType(browse::audiobookBookCatalog(eidx, keyBare), browse::kAudiobookMatchType));
        {
            const QString note = AudiobookMeta::matchSummary(forBare);
            CHECK(note.contains(QStringLiteral("Martin Shaw")));
            CHECK(note.contains(QStringLiteral("99%")));
            const MediaCatalog withRow = browse::audiobookBookCatalog(eidx, keyBare, {}, {}, note);
            CHECK(catalogHasType(withRow, browse::kAudiobookMatchType));
            for (const MediaItem& it : withRow.items)
                if (it.type == QString::fromLatin1(browse::kAudiobookMatchType))
                {
                    CHECK(browse::audiobookKeyOf(it.mime, browse::kAudiobookMatchPrefix) == keyBare);
                    CHECK(it.subtitle.contains(QStringLiteral("Martin Shaw")));
                    CHECK(!it.expandable);
                }
            // A stale route offers no match row either.
            CHECK(!catalogHasType(
                browse::audiobookBookCatalog(eidx, QStringLiteral("no-such-book"), {}, {}, note),
                browse::kAudiobookMatchType));
        }

        // -- (j) A WELL-TAGGED BOOK IS NEVER EVEN ASKED ABOUT ------------------------------------------
        if (bookBare) CHECK(AudiobookMeta::wantsEnrichment(*bookBare));
        {
            const Book* full = eidx.book(keyTagged);
            CHECK(full != nullptr);
            if (full) CHECK(!AudiobookMeta::wantsEnrichment(*full));
        }
    }

    if (g_fails == 0)
        std::printf("AUDIOBOOKS-OK\n");
    else
        std::printf("AUDIOBOOKS had %d failure(s)\n", g_fails);
    return g_fails == 0 ? 0 : 1;
}
