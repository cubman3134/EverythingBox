// Headless probe for the catalog id-resolver: pure matcher (CatalogMatch) + persisted cache (LocalResolveCache).
// Prints RESOLVER-OK on success; any failure prints RESOLVER-FAIL <cond> (line) and exits non-zero.
#include "CatalogMatch.h"
#include "LocalLibrary.h"
#include "LocalResolveCache.h"
#include "AddonModels.h"

#include <QCoreApplication>
#include <QTemporaryDir>
#include <QDateTime>
#include <QFile>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "RESOLVER-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

static MediaItem mi(const QString& id, const QString& title, const QString& type = QStringLiteral("movie"))
{ MediaItem it; it.id = id; it.title = title; it.type = type; return it; }

static MediaItem miY(const QString& id, const QString& title, const QString& subtitle)
{ MediaItem it; it.id = id; it.title = title; it.type = QStringLiteral("movie"); it.subtitle = subtitle; return it; }

static LocalLibrary::VideoEntry movie(const QString& title, int year, const QString& imdb = QString())
{ LocalLibrary::VideoEntry e; e.kind = LocalLibrary::Kind::Movie; e.title = title; e.year = year; e.imdbId = imdb; return e; }

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // normalizeTitle
    CHECK(CatalogMatch::normalizeTitle(QStringLiteral("The Matrix")) == QStringLiteral("matrix"));
    CHECK(CatalogMatch::normalizeTitle(QStringLiteral("WALL·E!")) == QStringLiteral("wall e"));
    CHECK(CatalogMatch::normalizeTitle(QStringLiteral("Amélie")) == QStringLiteral("amelie"));
    CHECK(CatalogMatch::normalizeTitle(QStringLiteral("Pokémon")) == QStringLiteral("pokemon"));
    CHECK(CatalogMatch::normalizeTitle(QStringLiteral("WALL·E")) == QStringLiteral("wall e")); // · is punct → space

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
    // A contradicted "tt…" candidate (KNOWN imdb id ≠ NFO imdb) must never win on title.
    {
        QVector<MediaItem> c{ mi("tt9999999", "Inception") };
        CHECK(CatalogMatch::bestMatch(movie("Inception", 2010, "tt1375666"), c) == -1);
    }
    // Positive control: a non-tt candidate with the same title still matches (not contradicted).
    {
        QVector<MediaItem> c{ mi("tmdb:movie:27205", "Inception") };
        CHECK(CatalogMatch::bestMatch(movie("Inception", 2010, "tt1375666"), c) == 0);
    }
    // No candidates / empty title → -1.
    CHECK(CatalogMatch::bestMatch(movie("Inception", 2010), {}) == -1);
    CHECK(CatalogMatch::bestMatch(movie("", 0), { mi("tt1", "x") }) == -1);

    // subtitle-year disambiguation (movies): the year is in the aiocatalog search row's subtitle.
    {
        // Local Solaris (2002); catalog offers only the 1972 film → year disagrees → no match.
        QVector<MediaItem> c{ miY("tmdb:movie:1","Solaris","1972") };
        CHECK(CatalogMatch::bestMatch(movie("Solaris", 2002), c) == -1);
        // Both films present → the wrong year is skipped, the right one is the unique hit.
        QVector<MediaItem> c2{ miY("tmdb:movie:1","Solaris","1972"), miY("tmdb:movie:2","Solaris","2002") };
        CHECK(CatalogMatch::bestMatch(movie("Solaris", 2002), c2) == 1);
        // ±1 tolerance accepted.
        QVector<MediaItem> c3{ miY("tmdb:movie:9","Some Film","2001") };
        CHECK(CatalogMatch::bestMatch(movie("Some Film", 2002), c3) == 0);
        // No subtitle year on the candidate → falls back to title match (unchanged behavior).
        QVector<MediaItem> c4{ mi("tmdb:movie:3","Inception","movie") };
        CHECK(CatalogMatch::bestMatch(movie("Inception", 2010), c4) == 0);
        // Local year unknown (0) → year check inert, title match as before.
        QVector<MediaItem> c5{ miY("tmdb:movie:1","Solaris","1972") };
        CHECK(CatalogMatch::bestMatch(movie("Solaris", 0), c5) == 0);
    }

    // bestSeriesMatch: series/tv type filter + unique title + tt cross-check + contradicted-tt skip.
    {
        QVector<MediaItem> c{ mi("tmdb:tv:1396","Breaking Bad","series"), mi("tmdb:movie:1","Breaking Bad","movie") };
        CHECK(CatalogMatch::bestSeriesMatch("Breaking Bad", QString(), c) == 0);          // picks the series, not the movie
        QVector<MediaItem> c2{ mi("tt0903747","Breaking Bad","series") };
        CHECK(CatalogMatch::bestSeriesMatch("breaking bad", "tt0903747", c2) == 0);       // exact tt wins
        QVector<MediaItem> c3{ mi("tt9999999","Breaking Bad","series") };
        CHECK(CatalogMatch::bestSeriesMatch("Breaking Bad", "tt0903747", c3) == -1);      // contradicted tt skipped
        QVector<MediaItem> c4{ mi("tmdb:tv:1","The Office","series"), mi("tmdb:tv:2","The Office","series") };
        CHECK(CatalogMatch::bestSeriesMatch("The Office", QString(), c4) == -1);          // ambiguous → -1
        QVector<MediaItem> c5{ mi("tmdb:movie:1","Fargo","movie") };
        CHECK(CatalogMatch::bestSeriesMatch("Fargo", QString(), c5) == -1);               // no series candidate
    }

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
    // clear() empties the cache (the "Re-match online" path) and persists the empty file.
    {
        LocalResolveCache c(cachePath); c.load();
        CHECK(c.has("/movies/Inception.mkv"));                             // present before clear
        c.clear();
        CHECK(!c.has("/movies/Inception.mkv"));                            // gone after clear
        LocalResolveCache c2(cachePath); c2.load();
        CHECK(!c2.has("/movies/Inception.mkv"));                           // and the empty state persisted
    }
    // Show-level store: matched (never expires) vs nomatch (retry window), round-trip, and clear().
    {
        LocalResolveCache c(cachePath); c.load();
        CHECK(!c.isShowFresh("tt0903747", now));
        c.putShowMatched("tt0903747", { "tmdb:tv:1396", "tt0903747" }, now);
        c.putShowNoMatch("name:the wire", now);
        CHECK(c.isShowFresh("tt0903747", now));                                  // matched
        CHECK(c.isShowFresh("name:the wire", now));                              // nomatch within window
        CHECK(!c.isShowFresh("name:the wire", now + 15LL*86400));                // nomatch past 14d → stale
        c.save();
    }
    {
        LocalResolveCache c(cachePath); c.load();                               // round-trip
        CHECK(c.seriesIdsByShow().value("tt0903747").contains("tmdb:tv:1396"));
        CHECK(!c.seriesIdsByShow().contains("name:the wire"));                   // nomatch not in the snapshot
        c.clear();
        CHECK(!c.isShowFresh("tt0903747", now));                                // clear() drops shows too
    }
    // Backward-compat: a legacy FLAT movie cache (no "paths"/"shows" keys) still loads its path entries.
    {
        const QString legacyPath = tmp.path() + QStringLiteral("/legacy.json");
        QFile lf(legacyPath);
        CHECK(lf.open(QIODevice::WriteOnly | QIODevice::Truncate));
        lf.write("{\"/movies/Legacy.mkv\":{\"size\":100,\"mtime\":200,\"matched\":true,\"ts\":1000000,\"ids\":[\"tmdb:movie:42\",\"tt7777777\"]}}");
        lf.close();
        LocalResolveCache c(legacyPath); c.load();
        CHECK(c.has("/movies/Legacy.mkv"));                                     // legacy flat root treated as paths
        CHECK(c.matchedIdsByPath().value("/movies/Legacy.mkv").contains("tmdb:movie:42"));
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
    // A NFO-LESS movie (no imdbId) still gets indexed by its resolved catalog id (T2 review Minor).
    {
        LocalLibrary::VideoEntry e; e.kind = LocalLibrary::Kind::Movie; e.path = "/m/NoNfo.mkv";
        e.title = "No Nfo"; e.imdbId = QString();       // no NFO id at all
        QHash<QString, QStringList> extra; extra.insert(e.path, { "tmdb:movie:999" });
        const LocalLibrary::OwnedIndex idx = LocalLibrary::buildIndex({ e }, extra);
        CHECK(idx.ownsId("tmdb:movie:999"));            // resolved id indexed despite empty imdbId
        CHECK(idx.localPathFor("tmdb:movie:999") == e.path);
    }

    if (failures == 0) { std::puts("RESOLVER-OK"); return 0; }
    std::fprintf(stderr, "RESOLVER: %d check(s) failed\n", failures);
    return 1;
}
