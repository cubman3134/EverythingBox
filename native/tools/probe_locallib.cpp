// Headless probe for the local video library core (filename parse + NFO read + scan + OwnedIndex).
// Builds a hermetic QTemporaryDir fixture and asserts the parse/index matrix.
// Prints LOCALLIB-OK on success; any failure prints LOCALLIB-FAIL <cond> (line) and exits non-zero.
#include "LocalLibrary.h"
#include "SyntheticCatalogs.h"

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

    writeFile(root + "/Blade Runner 2049/Blade Runner 2049.mkv");   // space-delimited trailing number = title, not year
    writeFile(root + "/2001 A Space Odyssey (1968)/2001 A Space Odyssey (1968).mkv");
    writeFile(root + "/Dune (2021)/Dune (2021).mkv");
    writeFile(root + "/Dune (2021)/movie.nfo",                       // folder movie.nfo fallback (no sidecar)
              "<movie><uniqueid type=\"imdb\">1160419</uniqueid></movie>");  // non-tt id -> normalized to tt1160419
    writeFile(root + "/Heat (1995)/Heat (1995).mkv");
    writeFile(root + "/Heat (1995)/Heat (1995).nfo",                 // uniqueid(imdb) must WIN over <imdbid>
              "<movie><imdbid>tt0000001</imdbid><uniqueid type=\"imdb\">tt0113277</uniqueid></movie>");

    writeFile(root + "/Broken (2015)/Broken (2015).mkv");
    writeFile(root + "/Broken (2015)/Broken (2015).nfo", "<movie><uniqueid type=\"imd");  // truncated/garbage
    writeFile(root + "/Show2/Season 02/Show2.2x05.mkv");             // NxNN episode form (reX regex)
    writeFile(root + "/Show2/tvshow.nfo", "<tvshow><uniqueid type=\"imdb\">tt3000000</uniqueid></tvshow>");
    writeFile(root + "/Poster Movie (2018)/Poster Movie (2018).mkv");
    writeFile(root + "/Poster Movie (2018)/Poster Movie (2018).nfo",
              "<movie><uniqueid type=\"imdb\">tt4000000</uniqueid><thumb>poster.jpg</thumb></movie>");

    const QVector<LocalLibrary::VideoEntry> scanned = LocalLibrary::scanFolder(root);
    CHECK(scanned.size() == 11);  // 4 existing + 4 (Blade/Odyssey/Dune/Heat) + 3 (Broken/Show2 ep/Poster); non-video ignored

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

    const auto* br = findByPathSuffix(scanned, "Blade Runner 2049.mkv");
    CHECK(br != nullptr);
    if (br) { CHECK(br->kind == LocalLibrary::Kind::Movie); CHECK(br->title == QStringLiteral("Blade Runner 2049")); CHECK(br->year == 0); }

    const auto* od = findByPathSuffix(scanned, "2001 A Space Odyssey (1968).mkv");
    CHECK(od != nullptr);
    if (od) { CHECK(od->title == QStringLiteral("2001 A Space Odyssey")); CHECK(od->year == 1968); }

    const auto* dune = findByPathSuffix(scanned, "Dune (2021).mkv");
    CHECK(dune != nullptr);
    if (dune) { CHECK(dune->year == 2021); CHECK(dune->imdbId == QStringLiteral("tt1160419")); }  // movie.nfo fallback + non-tt normalization

    const auto* heat = findByPathSuffix(scanned, "Heat (1995).mkv");
    CHECK(heat != nullptr);
    if (heat) { CHECK(heat->imdbId == QStringLiteral("tt0113277")); }  // uniqueid(imdb) wins over <imdbid>

    // Malformed/garbage .nfo: the scan must not crash; the entry is present with an EMPTY imdbId and a
    // filename-derived title/year (the truncated XML yields nothing useful, so readNfo leaves the entry).
    const auto* broken = findByPathSuffix(scanned, "Broken (2015).mkv");
    CHECK(broken != nullptr);
    if (broken) {
        CHECK(broken->kind == LocalLibrary::Kind::Movie);
        CHECK(broken->title == QStringLiteral("Broken"));
        CHECK(broken->year == 2015);
        CHECK(broken->imdbId.isEmpty());
    }

    // NxNN episode form (the reX regex, previously uncovered): "Show2.2x05" -> season 2, episode 5, and the
    // series imdb id from Show2/tvshow.nfo one level up from the "Season 02" folder.
    const auto* ep2 = findByPathSuffix(scanned, "Show2.2x05.mkv");
    CHECK(ep2 != nullptr);
    if (ep2) {
        CHECK(ep2->kind == LocalLibrary::Kind::Episode);
        CHECK(ep2->season == 2);
        CHECK(ep2->episode == 5);
        CHECK(ep2->seriesImdbId == QStringLiteral("tt3000000"));
    }

    // Relative <thumb> resolved against the nfo's directory -> an ABSOLUTE path ending in "poster.jpg".
    const auto* poster = findByPathSuffix(scanned, "Poster Movie (2018).mkv");
    CHECK(poster != nullptr);
    if (poster) {
        CHECK(poster->imdbId == QStringLiteral("tt4000000"));
        CHECK(QFileInfo(poster->thumbPath).isAbsolute());
        CHECK(poster->thumbPath.endsWith(QStringLiteral("poster.jpg")));
    }

    const LocalLibrary::OwnedIndex idx = LocalLibrary::buildIndex(scanned);
    CHECK(idx.all().size() == 11);
    CHECK(idx.ownsId(QStringLiteral("tt1375666")));                       // movie
    if (inc) CHECK(idx.localPathFor(QStringLiteral("tt1375666")) == inc->path);
    CHECK(idx.ownsId(QStringLiteral("tt2000000")));                       // series (by count)
    CHECK(idx.ownedEpisodes(QStringLiteral("tt2000000")) == 1);
    if (ep) CHECK(idx.localPathFor(QStringLiteral("tt2000000:1:2")) == ep->path);
    CHECK(!idx.ownsId(QStringLiteral("tt9999999")));                      // not owned

    // Seam A/B decision surface (pure): movie id owned -> local path; series id owned -> episode count;
    // episode key owned -> its file; unknown -> not owned. Pins the OwnedIndex contract Seam A (HomeView
    // browse-row decoration) and Seam B (MainWindow prefer-local redirect) consume, so an index regression
    // fails this probe, not just the UI.
    CHECK(idx.ownsId(QStringLiteral("tt1375666")) == true);
    CHECK(!idx.localPathFor(QStringLiteral("tt1375666")).isEmpty());
    CHECK(idx.ownedEpisodes(QStringLiteral("tt2000000")) == 1);
    CHECK(!idx.localPathFor(QStringLiteral("tt2000000:1:2")).isEmpty());
    CHECK(idx.ownsId(QStringLiteral("tt0000001")) == false);
    CHECK(idx.localPathFor(QStringLiteral("tt0000001")).isEmpty());

    if (inc) CHECK(LocalLibrary::displayTitle(*inc) == QStringLiteral("Inception (2010)"));
    if (ep)  CHECK(LocalLibrary::displayTitle(*ep)  == QStringLiteral("Show S01E02"));

    // Browse builder: each VideoEntry -> a playable MediaItem (url=path, mime=local:video), stable id.
    const MediaCatalog cat = browse::localLibraryCatalog(scanned);
    CHECK(cat.items.size() == scanned.size());
    {
        const MediaItem* mi = nullptr;
        for (const auto& x : cat.items) if (x.url.endsWith("Inception (2010).mkv")) mi = &x;
        CHECK(mi != nullptr);
        if (mi) {
            CHECK(mi->mime == QStringLiteral("local:video"));
            CHECK(mi->id == QStringLiteral("tt1375666"));          // known imdb id
            CHECK(mi->title == QStringLiteral("Inception (2010)"));
        }
        const MediaItem* rnd = nullptr;
        for (const auto& x : cat.items) if (x.url.endsWith("random.mkv")) rnd = &x;
        CHECK(rnd != nullptr);
        if (rnd) CHECK(rnd->id.startsWith(QStringLiteral("local:")));   // no imdb id -> local:<path> key
    }

    // Empty / missing root => empty scan (feature-dormant contract).
    CHECK(LocalLibrary::scanFolder(QString()).isEmpty());
    CHECK(LocalLibrary::scanFolder(root + "/does-not-exist").isEmpty());

    if (failures == 0) { std::puts("LOCALLIB-OK"); return 0; }
    std::fprintf(stderr, "LOCALLIB: %d check(s) failed\n", failures);
    return 1;
}
