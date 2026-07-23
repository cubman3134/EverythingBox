// Headless probe for the local video library core (filename parse + NFO read + scan + OwnedIndex).
// Builds a hermetic QTemporaryDir fixture and asserts the parse/index matrix.
// Prints LOCALLIB-OK on success; any failure prints LOCALLIB-FAIL <cond> (line) and exits non-zero.
#include "LocalLibrary.h"

#include <QCoreApplication>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "LOCALLIB-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

static void writeFile(const QString& path, const QByteArray& bytes = QByteArray())
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path); f.open(QIODevice::WriteOnly); f.write(bytes); f.close();
}

static const LocalLibrary::VideoEntry* findByPathSuffix(const QVector<LocalLibrary::VideoEntry>& v, const QString& suffix)
{
    for (const auto& e : v) if (e.path.endsWith(suffix)) return &e;
    return nullptr;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    const QString root = tmp.path();

    // Fixture tree.
    writeFile(root + "/Inception (2010)/Inception (2010).mkv");
    writeFile(root + "/Inception (2010)/Inception (2010).nfo",
              "<movie><title>Inception</title><year>2010</year>"
              "<uniqueid type=\"imdb\">tt1375666</uniqueid>"
              "<plot>A thief.</plot></movie>");
    writeFile(root + "/Movies/The Matrix.1999.mp4");                 // no nfo
    writeFile(root + "/Show/Season 01/Show - S01E02.mkv");
    writeFile(root + "/Show/tvshow.nfo",
              "<tvshow><title>Show</title><uniqueid type=\"imdb\">tt2000000</uniqueid></tvshow>");
    writeFile(root + "/junk/random.mkv");                            // no year, no SxxEyy
    writeFile(root + "/Show/notes.txt", "ignore me");               // non-video

    const QVector<LocalLibrary::VideoEntry> scanned = LocalLibrary::scanFolder(root);
    CHECK(scanned.size() == 4);   // 3 mkv + 1 mp4; txt ignored

    const auto* inc = findByPathSuffix(scanned, "Inception (2010).mkv");
    CHECK(inc != nullptr);
    if (inc) {
        CHECK(inc->kind == LocalLibrary::Kind::Movie);
        CHECK(inc->title == QStringLiteral("Inception"));
        CHECK(inc->year == 2010);
        CHECK(inc->imdbId == QStringLiteral("tt1375666"));
    }

    const auto* mtx = findByPathSuffix(scanned, "The Matrix.1999.mp4");
    CHECK(mtx != nullptr);
    if (mtx) {
        CHECK(mtx->kind == LocalLibrary::Kind::Movie);
        CHECK(mtx->title == QStringLiteral("The Matrix"));
        CHECK(mtx->year == 1999);
        CHECK(mtx->imdbId.isEmpty());
    }

    const auto* ep = findByPathSuffix(scanned, "Show - S01E02.mkv");
    CHECK(ep != nullptr);
    if (ep) {
        CHECK(ep->kind == LocalLibrary::Kind::Episode);
        CHECK(ep->season == 1);
        CHECK(ep->episode == 2);
        CHECK(ep->show == QStringLiteral("Show"));
        CHECK(ep->seriesImdbId == QStringLiteral("tt2000000"));
    }

    const auto* rnd = findByPathSuffix(scanned, "random.mkv");
    CHECK(rnd != nullptr);
    if (rnd) {
        CHECK(rnd->kind == LocalLibrary::Kind::Movie);
        CHECK(rnd->title == QStringLiteral("random"));
        CHECK(rnd->year == 0);
    }

    const LocalLibrary::OwnedIndex idx = LocalLibrary::buildIndex(scanned);
    CHECK(idx.all().size() == 4);
    CHECK(idx.ownsId(QStringLiteral("tt1375666")));                       // movie
    if (inc) CHECK(idx.localPathFor(QStringLiteral("tt1375666")) == inc->path);
    CHECK(idx.ownsId(QStringLiteral("tt2000000")));                       // series (by count)
    CHECK(idx.ownedEpisodes(QStringLiteral("tt2000000")) == 1);
    if (ep) CHECK(idx.localPathFor(QStringLiteral("tt2000000:1:2")) == ep->path);
    CHECK(!idx.ownsId(QStringLiteral("tt9999999")));                      // not owned

    if (inc) CHECK(LocalLibrary::displayTitle(*inc) == QStringLiteral("Inception (2010)"));
    if (ep)  CHECK(LocalLibrary::displayTitle(*ep)  == QStringLiteral("Show S01E02"));

    // Empty / missing root => empty scan (feature-dormant contract).
    CHECK(LocalLibrary::scanFolder(QString()).isEmpty());
    CHECK(LocalLibrary::scanFolder(root + "/does-not-exist").isEmpty());

    if (failures == 0) { std::puts("LOCALLIB-OK"); return 0; }
    std::fprintf(stderr, "LOCALLIB: %d check(s) failed\n", failures);
    return 1;
}
