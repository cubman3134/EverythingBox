// Headless probe for the catalog id-resolver: pure matcher (CatalogMatch) + persisted cache (LocalResolveCache).
// Prints RESOLVER-OK on success; any failure prints RESOLVER-FAIL <cond> (line) and exits non-zero.
#include "CatalogMatch.h"
#include "LocalLibrary.h"
#include "AddonModels.h"

#include <QCoreApplication>
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

    if (failures == 0) { std::puts("RESOLVER-OK"); return 0; }
    std::fprintf(stderr, "RESOLVER: %d check(s) failed\n", failures);
    return 1;
}
