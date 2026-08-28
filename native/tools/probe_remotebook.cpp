// Headless probe for a REMOTE AUDIOBOOK RELEASE (issue #214): which of a release's files are a book's
// parts, in what order they play, and the token the queue files each part under.
//
// WHY THIS PROBE EXISTS. Three user reports — a player opened on an archive, a player silent forever, and
// a fifteen-hour book that started at chapter ten — were one bug: an audiobook release is many files and
// the app resolved it to a single link, so what played was whatever the source returned first. The fix is
// to take the release's file list, KEEP the audio, ORDER it and play it as one book. Both of those verbs
// are pure functions over file names, which means they can be pinned exactly, here, with no network, no
// provider and no player — and mutation-tested in both directions, which is the point: a mutant that
// mis-orders the parts must die, and so must one that stops filtering the non-audio files out.
//
// The ORDERING half is worth stating plainly, because getting it wrong reintroduces the reported defect
// with a different cause: "10 - part" must not sort before "2 - part". RemoteAudiobook orders through
// core/NaturalOrder, whose header explains why a hand-built QCollator is INERT under the C locale — and
// this probe runs on CI, which has exactly that locale. If NaturalOrder is ever bypassed here, this file
// is where it goes red first.
//
// Prints REMOTEBOOK-OK on success; any failure prints REMOTEBOOK-FAIL <cond> (line) and exits non-zero.
#include "RemoteAudiobook.h"

#include <QCoreApplication>
#include <QStringList>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "REMOTEBOOK-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

using RemoteAudiobook::Part;

// A release as a source lists it: an id per file (the source's own — never a link) and the file's name.
// The id is deliberately unlike the name, so that a test asserting on order cannot pass by accident from
// the ids happening to sort the same way.
static QVector<Part> release(const QStringList& fileNames)
{
    QVector<Part> parts;
    for (int i = 0; i < fileNames.size(); ++i)
        parts.push_back({ QStringLiteral("rel~f%1").arg(fileNames.size() - i), fileNames.at(i), QString() });
    return parts;
}

static QStringList namesOf(const QVector<Part>& parts)
{
    QStringList out;
    for (const Part& p : parts) out << p.fileName;
    return out;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ---- 1. the filter: which files are a book's parts ---------------------------------------------
    CHECK(RemoteAudiobook::isAudioFileName(QStringLiteral("01 - Chapter One.mp3")));
    CHECK(RemoteAudiobook::isAudioFileName(QStringLiteral("A Tale of Two Cities.m4b")));
    CHECK(RemoteAudiobook::isAudioFileName(QStringLiteral("part.FLAC")));            // case-insensitive
    CHECK(RemoteAudiobook::isAudioFileName(QStringLiteral("Disc 1/03 - part.mp3"))); // folder-qualified
    CHECK(RemoteAudiobook::isAudioFileName(QStringLiteral("Disc 1\\03 - part.mp3"))); // ...either separator
    CHECK(RemoteAudiobook::isAudioFileName(QStringLiteral(" 01.mp3 ")));             // surrounding space
    // The four things that actually sit beside the audio in a real release, and the one that started this.
    CHECK(!RemoteAudiobook::isAudioFileName(QStringLiteral("cover.jpg")));
    CHECK(!RemoteAudiobook::isAudioFileName(QStringLiteral("info.nfo")));
    CHECK(!RemoteAudiobook::isAudioFileName(QStringLiteral("chapters.txt")));
    CHECK(!RemoteAudiobook::isAudioFileName(QStringLiteral("The Poppy War.epub")));
    CHECK(!RemoteAudiobook::isAudioFileName(QStringLiteral("A Tale of Two Cities.zip")));  // the /zip/ report
    CHECK(!RemoteAudiobook::isAudioFileName(QStringLiteral("readme")));                    // no extension
    CHECK(!RemoteAudiobook::isAudioFileName(QStringLiteral("Disc 1/")));                   // a folder, not a file
    CHECK(!RemoteAudiobook::isAudioFileName(QString()));
    CHECK(!RemoteAudiobook::isAudioFileName(QStringLiteral("   ")));
    // A name whose FOLDER carries an audio extension but whose file does not: the answer is about the
    // FILE, which is the last path segment and not the longest dotted thing in the string.
    CHECK(!RemoteAudiobook::isAudioFileName(QStringLiteral("Book.mp3/cover.jpg")));

    // ---- 2. the order: 47 unpadded parts -----------------------------------------------------------
    // The case the whole issue turns on. Unpadded numbers are what a real rip carries, and a plain
    // lexicographic sort puts 10 before 2 — i.e. starts the listener at part ten, which is the reported
    // defect wearing a different hat.
    {
        QStringList names;
        for (int i = 1; i <= 47; ++i) names << QStringLiteral("%1 - part.mp3").arg(i);
        // Shuffled deterministically into an order no sort would produce by luck.
        QStringList listed;
        for (int i = 0; i < names.size(); ++i) listed << names.at((i * 17 + 5) % names.size());
        const QVector<Part> parts = RemoteAudiobook::playableParts(release(listed));
        CHECK(parts.size() == 47);
        CHECK(namesOf(parts) == names);
        CHECK(parts.first().fileName == QStringLiteral("1 - part.mp3"));
        CHECK(parts.at(1).fileName == QStringLiteral("2 - part.mp3"));   // NOT "10 - part.mp3"
        CHECK(parts.last().fileName == QStringLiteral("47 - part.mp3"));
    }

    // ---- 3. the order: "Chapter 1" … "Chapter 10" --------------------------------------------------
    {
        const QVector<Part> parts = RemoteAudiobook::playableParts(release({
            QStringLiteral("Chapter 10.mp3"), QStringLiteral("Chapter 2.mp3"), QStringLiteral("Chapter 1.mp3"),
            QStringLiteral("Chapter 9.mp3"),
        }));
        CHECK(namesOf(parts) == QStringList({ QStringLiteral("Chapter 1.mp3"), QStringLiteral("Chapter 2.mp3"),
                                              QStringLiteral("Chapter 9.mp3"), QStringLiteral("Chapter 10.mp3") }));
    }

    // ---- 4. a release with cover.jpg and an .nfo mixed in ------------------------------------------
    // Filtered THEN ordered, so a non-audio file that would have sorted FIRST cannot decide where the book
    // begins. "00 - readme.nfo" is exactly that file.
    {
        const QVector<Part> parts = RemoteAudiobook::playableParts(release({
            QStringLiteral("00 - readme.nfo"), QStringLiteral("cover.jpg"), QStringLiteral("02 - part.mp3"),
            QStringLiteral("01 - part.mp3"), QStringLiteral("folder.png"), QStringLiteral("chapters.txt"),
        }));
        CHECK(parts.size() == 2);
        CHECK(parts.first().fileName == QStringLiteral("01 - part.mp3"));
        CHECK(parts.last().fileName == QStringLiteral("02 - part.mp3"));
    }

    // ---- 5. a single-file .m4b -- one part, and it must still be one --------------------------------
    // The no-regression case: an .m4b with its chapters inside already worked, and the caller reads
    // "exactly one part" as "this is a single file, take the untouched path".
    {
        const QVector<Part> parts = RemoteAudiobook::playableParts(release({
            QStringLiteral("A Tale of Two Cities.m4b"), QStringLiteral("cover.jpg"),
        }));
        CHECK(parts.size() == 1);
        CHECK(parts.first().fileName == QStringLiteral("A Tale of Two Cities.m4b"));
    }

    // ---- 6. a release with only an .epub -- nothing to play ----------------------------------------
    // #207's report exactly: an ebook release won an audiobook search and a player was staged over it.
    // Empty here is what the caller turns into a sentence instead.
    {
        CHECK(RemoteAudiobook::playableParts(release({
            QStringLiteral("The Poppy War.epub"), QStringLiteral("cover.jpg"), QStringLiteral("info.nfo"),
        })).isEmpty());
    }

    // ---- 7. a part list of one, and of none --------------------------------------------------------
    {
        CHECK(RemoteAudiobook::playableParts(release({ QStringLiteral("book.mp3") })).size() == 1);
        CHECK(RemoteAudiobook::playableParts({}).isEmpty());
    }

    // ---- 8. a part with no id is not a part --------------------------------------------------------
    // Nothing can mint a link for it, and a queue row that can only fail is worse than one row fewer.
    {
        QVector<Part> listed = release({ QStringLiteral("01.mp3"), QStringLiteral("02.mp3") });
        listed[0].id.clear();
        const QVector<Part> parts = RemoteAudiobook::playableParts(listed);
        CHECK(parts.size() == 1);
        CHECK(parts.first().fileName == QStringLiteral("02.mp3"));
    }

    // ---- 9. equal names keep the source's order (stable) -------------------------------------------
    {
        QVector<Part> listed;
        listed.push_back({ QStringLiteral("id-second"), QStringLiteral("part.mp3"), QString() });
        listed.push_back({ QStringLiteral("id-third"),  QStringLiteral("part.mp3"), QString() });
        const QVector<Part> parts = RemoteAudiobook::playableParts(listed);
        CHECK(parts.size() == 2);
        CHECK(parts.first().id == QStringLiteral("id-second"));
    }

    // ---- 10. the part token: what the QUEUE holds ---------------------------------------------------
    // A part is its BOOK plus its FILE, and never a link. That is the rule the #200-#204 chain settled and
    // the reason this token exists at all, so it is asserted as a property rather than as a spelling:
    // nothing url-shaped survives into it, and it round-trips both halves.
    {
        const QString key = QStringLiteral("googlebooks:5EIPAAAAQAAJ");
        const QString file = QStringLiteral("Disc 1/03 - A Tale of Two Cities.mp3");
        const QString token = RemoteAudiobook::partToken(key, file);
        CHECK(!token.isEmpty());
        CHECK(RemoteAudiobook::isPartToken(token));
        CHECK(RemoteAudiobook::bookKeyOfToken(token) == key);
        CHECK(RemoteAudiobook::fileNameOfToken(token) == file);
        // The two properties that make it safe to write down.
        CHECK(!token.contains(QStringLiteral("http")));
        CHECK(!token.contains(QLatin1Char('?')));
        // ...and the two properties that make it a name rather than a location: it is not a url, so the
        // stores that scrub playback paths leave it exactly as it is.
        CHECK(token.startsWith(QLatin1String(RemoteAudiobook::kPartScheme)));
    }
    // Two books, same file name -> two different tokens. A shared token would file one book's position
    // against another's, which is the resume bug #194 and #203 each fixed one instance of.
    CHECK(RemoteAudiobook::partToken(QStringLiteral("bookA"), QStringLiteral("01.mp3"))
          != RemoteAudiobook::partToken(QStringLiteral("bookB"), QStringLiteral("01.mp3")));
    // ...and the same book+file is the SAME token every time, which is what makes resume survive a
    // re-search that picked a differently-encoded release.
    CHECK(RemoteAudiobook::partToken(QStringLiteral("bookA"), QStringLiteral("01.mp3"))
          == RemoteAudiobook::partToken(QStringLiteral("bookA"), QStringLiteral("01.mp3")));
    // Anything that is not a token reads as one nowhere — every caller sees most play paths, and most
    // play paths are ordinary files and urls.
    CHECK(!RemoteAudiobook::isPartToken(QStringLiteral("https://host/x.mp3?token=abc")));
    CHECK(!RemoteAudiobook::isPartToken(QStringLiteral("C:/Books/01.mp3")));
    CHECK(!RemoteAudiobook::isPartToken(QString()));
    CHECK(RemoteAudiobook::bookKeyOfToken(QStringLiteral("C:/Books/01.mp3")).isEmpty());
    CHECK(RemoteAudiobook::fileNameOfToken(QStringLiteral("C:/Books/01.mp3")).isEmpty());
    CHECK(RemoteAudiobook::partToken(QString(), QStringLiteral("01.mp3")).isEmpty());
    CHECK(RemoteAudiobook::partToken(QStringLiteral("bookA"), QString()).isEmpty());

    // ---- 11. the display title a queue row shows ----------------------------------------------------
    CHECK(RemoteAudiobook::partTitle(QStringLiteral("01 - Chapter One.mp3")) == QStringLiteral("01 - Chapter One"));
    CHECK(RemoteAudiobook::partTitle(QStringLiteral("Disc 1/02 - part.mp3")) == QStringLiteral("02 - part"));
    CHECK(RemoteAudiobook::partTitle(QStringLiteral("Disc 1\\02 - part.mp3")) == QStringLiteral("02 - part"));
    // A name that is ALL extension (a dotfile) has no base name at all; the row still has to say
    // something, so it falls back to the name itself rather than rendering a blank line.
    CHECK(RemoteAudiobook::partTitle(QStringLiteral(".mp3")) == QStringLiteral(".mp3"));
    CHECK(!RemoteAudiobook::partTitle(QStringLiteral("part.mp3")).isEmpty());   // never blank

    if (failures == 0) { std::puts("REMOTEBOOK-OK"); return 0; }
    std::fprintf(stderr, "REMOTEBOOK: %d check(s) failed\n", failures);
    return 1;
}
