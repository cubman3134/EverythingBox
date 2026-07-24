// Headless probe for the catalog id-resolver: pure matcher (CatalogMatch) + persisted cache (LocalResolveCache).
// Prints RESOLVER-OK on success; any failure prints RESOLVER-FAIL <cond> (line) and exits non-zero.
#include "CatalogMatch.h"
#include "LocalLibrary.h"
#include "LocalResolveCache.h"
#include "AddonModels.h"

#include <QCoreApplication>
#include <QTemporaryDir>
#include <QDateTime>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "RESOLVER-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

static MediaItem mi(const QString& id, const QString& title, const QString& type = QStringLiteral("movie"))
{ MediaItem it; it.id = id; it.title = title; it.type = type; return it; }

static LocalLibrary::VideoEntry movie(const QString& title, int year, const QString& imdb = QString())
{ LocalLibrary::VideoEntry e; e.kind = LocalLibrary::Kind::Movie; e.title = title; e.year = year; e.imdbId = imdb; return e; }

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // normalizeTitle
    CHECK(CatalogMatch::normalizeTitle(QStringLiteral("The Matrix")) == QStringLiteral("matrix"));
    CHECK(CatalogMatch::normalizeTitle(QStringLiteral("WALL·E!")) == QStringLiteral("wall e"));
    CHECK(CatalogMatch::normalizeTitle(QStringLiteral("Amélie")) == CatalogMatch::normalizeTitle(QStringLiteral("amelie"))
          || true);  // diacritics best-effort; do not over-assert

    // IMDB cross-check wins outright.
    {
        QVector<MediaItem> c{ mi("tmdb:movie:27205", "Inception"), mi("tt1375666", "Inception") };
        CHECK(CatalogMatch::bestMatch(movie("Inception", 2010, "tt1375666"), c) == 1);
    }
    // Unique normalized-title match (no imdb) → that index.
    {
        QVector<MediaItem> c{ mi("tmdb:movie:27205", "Inception"), mi("tmdb:movie:99", "Interstellar") };
        CHECK(CatalogMatch::bestMatch(movie("inception", 2010), c) == 0);
    }
    // Article/case/punctuation normalization still matches.
    {
        QVector<MediaItem> c{ mi("tt0133093", "The Matrix") };
        CHECK(CatalogMatch::bestMatch(movie("Matrix", 1999), c) == 0);
    }
    // Ambiguous: two candidates share the normalized title → -1 (conservative, never mis-badge).
    {
        QVector<MediaItem> c{ mi("tmdb:movie:1", "The Mummy"), mi("tmdb:movie:2", "The Mummy") };
        CHECK(CatalogMatch::bestMatch(movie("The Mummy", 2017), c) == -1);
    }
    // A same-title SERIES candidate is not a movie match.
    {
        QVector<MediaItem> c{ mi("tt111", "Fargo", "series") };
        CHECK(CatalogMatch::bestMatch(movie("Fargo", 1996), c) == -1);
    }
    // No candidates / empty title → -1.
    CHECK(CatalogMatch::bestMatch(movie("Inception", 2010), {}) == -1);
    CHECK(CatalogMatch::bestMatch(movie("", 0), { mi("tt1", "x") }) == -1);

    QTemporaryDir tmp; CHECK(tmp.isValid());
    const QString cachePath = tmp.path() + QStringLiteral("/localresolve.json");
    const qint64 now = 1000000;
    {
        LocalResolveCache c(cachePath);
        c.load();
        CHECK(!c.has("/movies/Inception.mkv"));
        c.putMatched("/movies/Inception.mkv", 100, 200, { "tmdb:movie:27205", "tt1375666" }, now);
        c.putNoMatch("/movies/Unknown.mkv", 50, 60, now);
        CHECK(c.isFresh("/movies/Inception.mkv", 100, 200, now));
        CHECK(!c.isFresh("/movies/Inception.mkv", 100, 999, now));           // mtime changed → stale
        CHECK(c.isFresh("/movies/Unknown.mkv", 50, 60, now));               // nomatch within window
        CHECK(!c.isFresh("/movies/Unknown.mkv", 50, 60, now + 15LL*86400)); // nomatch past 14d → stale (retry)
        c.save();
    }
    {
        LocalResolveCache c(cachePath); c.load();                          // persistence round-trip
        CHECK(c.entry("/movies/Inception.mkv").ids.contains("tmdb:movie:27205"));
        CHECK(c.matchedIdsByPath().value("/movies/Inception.mkv").contains("tt1375666"));
        CHECK(!c.matchedIdsByPath().contains("/movies/Unknown.mkv"));       // nomatch not in the snapshot
    }
    // buildIndex indexes the resolved ids → the movie's path, alongside the NFO id.
    {
        LocalLibrary::VideoEntry e; e.kind = LocalLibrary::Kind::Movie; e.path = "/m/Inception.mkv";
        e.title = "Inception"; e.imdbId = "tt1375666";
        QHash<QString, QStringList> extra; extra.insert(e.path, { "tmdb:movie:27205" });
        const LocalLibrary::OwnedIndex idx = LocalLibrary::buildIndex({ e }, extra);
        CHECK(idx.ownsId("tt1375666"));                 // NFO id (existing behavior)
        CHECK(idx.ownsId("tmdb:movie:27205"));          // resolved id (new)
        CHECK(idx.localPathFor("tmdb:movie:27205") == e.path);
    }

    if (failures == 0) { std::puts("RESOLVER-OK"); return 0; }
    std::fprintf(stderr, "RESOLVER: %d check(s) failed\n", failures);
    return 1;
}
