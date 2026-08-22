// Headless probe for the catalog id-resolver: pure matcher (CatalogMatch) + persisted cache (LocalResolveCache).
// Prints RESOLVER-OK on success; any failure prints RESOLVER-FAIL <cond> (line) and exits non-zero.
#include "CatalogMatch.h"
#include "LocalLibrary.h"
#include "LocalResolveCache.h"
#include "LocalMetaMerge.h"
#include "MetaOverrides.h"
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

    // ----------------------------------------------------------------------------------------------------
    // #73: the metadata layer persisted for a resolved local file. The precedence a surface ultimately sees is
    //      override (#24)  >  .nfo sidecar  >  scraped-fills-blank.
    // baseDetail() produces the lower two layers; MetaOverrides::applyTo() adds the top one (as MetaCache does
    // at read time). Fixtures are hand-authored so an expected value never comes from running the merge.
    // ----------------------------------------------------------------------------------------------------
    {
        // A movie entry whose ONLY .nfo-sourced fields are plot + thumb (title/year come from the filename).
        auto local = [](const QString& title, int year, const QString& plot, const QString& thumb) {
            LocalLibrary::VideoEntry e; e.kind = LocalLibrary::Kind::Movie;
            e.title = title; e.year = year; e.plot = plot; e.thumbPath = thumb; return e;
        };
        // A scraped card with a poster role (as getMeta's fromJson registers imageUrl under "poster").
        auto scrapedCard = [](const QString& title, const QString& sub, const QString& overview, const QString& img) {
            MediaDetail d; d.title = title; d.subtitle = sub; d.overview = overview; d.imageUrl = img;
            if (!img.isEmpty()) d.art.addImage(QStringLiteral("poster"), img);
            d.valid = true; return d;
        };

        const MediaDetail scraped = scrapedCard(QStringLiteral("Inception"), QStringLiteral("2010"),
            QStringLiteral("A thief who steals corporate secrets."), QStringLiteral("http://s/poster.jpg"));

        // --- .nfo plot is authoritative; the scrape fills it only when the .nfo is silent. ---
        {
            const MediaDetail base = LocalMeta::baseDetail(
                local(QStringLiteral("Inception"), 2010, QStringLiteral("Sidecar synopsis."), QString()), scraped);
            CHECK(base.overview == QStringLiteral("Sidecar synopsis."));   // .nfo plot wins over scraped overview
        }
        {
            const MediaDetail base = LocalMeta::baseDetail(
                local(QStringLiteral("Inception"), 2010, QString(), QString()), scraped);
            CHECK(base.overview == QStringLiteral("A thief who steals corporate secrets.")); // no .nfo plot → scrape
        }

        // --- .nfo thumb wins on EVERY surface: imageUrl points at it AND the scraped poster/thumb roles go. ---
        {
            const MediaDetail base = LocalMeta::baseDetail(
                local(QStringLiteral("Inception"), 2010, QString(), QStringLiteral("/lib/poster.jpg")), scraped);
            CHECK(base.imageUrl == QStringLiteral("/lib/poster.jpg"));                 // sidecar poster wins
            CHECK(!base.art.images.contains(QStringLiteral("poster")));               // scraped poster role dropped
            CHECK(!base.art.images.contains(QStringLiteral("thumb")));
        }
        {
            const MediaDetail base = LocalMeta::baseDetail(
                local(QStringLiteral("Inception"), 2010, QString(), QString()), scraped);
            CHECK(base.imageUrl == QStringLiteral("http://s/poster.jpg"));            // no sidecar → scraped poster
            CHECK(base.art.images.contains(QStringLiteral("poster")));               // …and its role is KEPT
        }

        // --- title/year are filename FALLBACKS, not .nfo data: the scrape outranks them, filling only a blank. ---
        {
            const MediaDetail base = LocalMeta::baseDetail(
                local(QStringLiteral("Inception 2010 1080p"), 2010, QString(), QString()), scraped);
            CHECK(base.title == QStringLiteral("Inception"));            // scraped title wins over the filename
            CHECK(base.subtitle == QStringLiteral("2010"));             // scraped subtitle present → kept
        }
        {
            MediaDetail bare; bare.valid = true;                        // scrape with no title/subtitle
            const MediaDetail base = LocalMeta::baseDetail(
                local(QStringLiteral("Filename Title"), 1999, QString(), QString()), bare);
            CHECK(base.title == QStringLiteral("Filename Title"));       // blank scrape title → filename fills it
            CHECK(base.subtitle == QStringLiteral("1999"));            // blank scrape subtitle → filename year fills it
        }

        // --- full precedence: override > .nfo > scraped, field by field, over the SAME base. ---
        {
            const MediaDetail base = LocalMeta::baseDetail(
                local(QStringLiteral("Inception"), 2010, QStringLiteral("Sidecar synopsis."),
                      QStringLiteral("/lib/poster.jpg")), scraped);
            // overview: override set → override; image: override set → override; title: no override → base(scraped).
            MetaOverrides::Override ov;
            ov.overview = QStringLiteral("My own summary.");
            ov.image    = QStringLiteral("http://user/fixed.jpg");
            MediaDetail shown = base;
            MetaOverrides::applyTo(ov, shown);
            CHECK(shown.overview == QStringLiteral("My own summary."));      // override beats the .nfo plot
            CHECK(shown.imageUrl == QStringLiteral("http://user/fixed.jpg")); // override beats the .nfo thumb
            CHECK(shown.title == QStringLiteral("Inception"));               // no override here → falls to base

            // With NO override, the same base falls through to the .nfo layer (proves the override is doing work).
            MediaDetail plain = base;
            MetaOverrides::applyTo(MetaOverrides::Override{}, plain);
            CHECK(plain.overview == QStringLiteral("Sidecar synopsis."));    // empty override → .nfo plot stands
            CHECK(plain.imageUrl == QStringLiteral("/lib/poster.jpg"));      // empty override → .nfo thumb stands
        }
    }

    // ---- refusing a search result that is not what was asked for -------------------------------------
    // The gate exists because taking the first result played an entirely different audiobook — and then the
    // wrong item owned the resume key, so the NEXT book opened resumed the first one's position and looked
    // like it played nothing. Refusing is the safe answer; the callers already say "couldn't find it".
    {
        using CatalogMatch::titleMatchesRequest;

        // Two spellings of one work. The provider's title is routinely longer than the catalog's.
        CHECK(titleMatchesRequest(QStringLiteral("Hemingway"),
                                  QStringLiteral("Hemingway: A Life Without Consequences")));
        CHECK(titleMatchesRequest(QStringLiteral("Persuasion"), QStringLiteral("Persuasion (Unabridged)")));
        CHECK(titleMatchesRequest(QStringLiteral("The Maltese Falcon"),
                                  QStringLiteral("Maltese Falcon")));          // leading article dropped
        CHECK(titleMatchesRequest(QStringLiteral("Amelie"), QStringLiteral("Amélie")));  // diacritics folded
        CHECK(titleMatchesRequest(QStringLiteral("A B C"), QStringLiteral("a  b   c")));       // punctuation/space

        // The failure this was written for: nine results came back and the first was somebody else's book.
        CHECK(!titleMatchesRequest(QStringLiteral("Hemingway"),
                                   QStringLiteral("My Journey to the World Cup - Sam Kerr")));

        // A DERIVATIVE that names the original inside its own subtitle. Containment alone reads this as the
        // book asked for, because the wanted title is genuinely present in the candidate's text — a parody, a
        // study guide, an "inspired by" all trip it. The candidate's own title is what it is called BEFORE its
        // subtitle, and that is what has to match.
        CHECK(!titleMatchesRequest(
                  QStringLiteral("Alice's Adventures in Wonderland"),
                  QStringLiteral("Alice in Zombieland: Lewis Carroll's 'Alice's Adventures in Wonderland' "
                                 "with Undead Madness")));
        // ...and the edition that IS the book still passes, subtitle and all.
        CHECK(titleMatchesRequest(QStringLiteral("Alice's Adventures in Wonderland"),
                                  QStringLiteral("Alice's Adventures in Wonderland: An Illustrated Edition")));
        // A provider that leads with the AUTHOR has no subtitle to split on, so the whole string still counts.
        CHECK(titleMatchesRequest(QStringLiteral("Jane Eyre"),
                                  QStringLiteral("Charlotte Bronte - Jane Eyre (BBC)")));

        // Whole tokens only. A two-letter title is exactly where a substring rule does the most damage.
        CHECK(!titleMatchesRequest(QStringLiteral("It"), QStringLiteral("Commitment")));
        CHECK(titleMatchesRequest(QStringLiteral("It"), QStringLiteral("It")));

        // Either side may be the longer one: a catalog title can carry the subtitle the provider omits, as
        // well as the other way round.
        CHECK(titleMatchesRequest(QStringLiteral("Hemingway: A Life Without Consequences"),
                                  QStringLiteral("Hemingway")));

        // ---- a copy already on disk -------------------------------------------------------------------
        // The failure: a book was downloaded, closed, and opened again — and the app went back to the network
        // to "find" it, which looks exactly like downloading it a second time.
        {
            using CatalogMatch::LocalCopy;
            using CatalogMatch::localCopyFor;
            const QVector<LocalCopy> have = {
                { QStringLiteral("C:/dl/alice.epub"),  QStringLiteral("Alice's Adventures in Wonderland"),
                  QStringLiteral("document"), QStringLiteral("gb:alice-1") },
                { QStringLiteral("C:/dl/alice.m4b"),   QStringLiteral("Alice's Adventures in Wonderland"),
                  QStringLiteral("audio"),    QString() },
                { QStringLiteral("C:/dl/jane.epub"),   QStringLiteral("Jane Eyre"),
                  QStringLiteral("document"), QString() },
            };

            // The saved key identifies the work exactly, and stands on its own — a catalog row whose title has
            // since changed still finds its own file.
            CHECK(localCopyFor(QStringLiteral("gb:alice-1"), QStringLiteral("Something Else Entirely"),
                               QStringLiteral("document"), have) == QStringLiteral("C:/dl/alice.epub"));

            // Title is the fallback for a copy saved before any id was known.
            CHECK(localCopyFor(QString(), QStringLiteral("Jane Eyre"), QStringLiteral("document"), have)
                  == QStringLiteral("C:/dl/jane.epub"));

            // NEVER across kinds: the audiobook and the book of one work are different things, and by this
            // point there is no network step left that would notice the wrong one being opened.
            CHECK(localCopyFor(QString(), QStringLiteral("Jane Eyre"), QStringLiteral("audio"), have).isEmpty());
            CHECK(localCopyFor(QString(), QStringLiteral("Alice's Adventures in Wonderland"),
                               QStringLiteral("audio"), have) == QStringLiteral("C:/dl/alice.m4b"));

            // EXACT titles only. A near-miss falls through to the ordinary search, which costs a wait; opening
            // the wrong book costs someone reading it. The conservative side is the cheap one.
            CHECK(localCopyFor(QString(), QStringLiteral("Alice's Adventures in Wonderland: Annotated"),
                               QStringLiteral("document"), have).isEmpty());
            CHECK(localCopyFor(QString(), QStringLiteral("Alice"), QStringLiteral("document"), have).isEmpty());

            // Nothing to offer is an empty answer, not a wrong one.
            CHECK(localCopyFor(QStringLiteral("gb:nope"), QStringLiteral("Unknown Book"),
                               QStringLiteral("document"), have).isEmpty());
            CHECK(localCopyFor(QString(), QString(), QStringLiteral("document"), have).isEmpty());
            CHECK(localCopyFor(QString(), QStringLiteral("Jane Eyre"), QString(), have).isEmpty());
            CHECK(localCopyFor(QStringLiteral("x"), QStringLiteral("y"), QStringLiteral("document"), {})
                      .isEmpty());
        }

        // Fails CLOSED: nothing to compare is a refusal, never a pass. BOTH sides empty especially — two
        // blanks are trivially "contained" in each other, which without the guard reads as a match.
        CHECK(!titleMatchesRequest(QString(), QString()));
        CHECK(!titleMatchesRequest(QString(), QStringLiteral("Anything At All")));
        CHECK(!titleMatchesRequest(QStringLiteral("Anything At All"), QString()));
        CHECK(!titleMatchesRequest(QStringLiteral("!!!"), QStringLiteral("Something")));  // normalizes to empty
    }

    // ---- document catalog siblings (comic ↔ manga only) ---------------------------------------------
    // The doc-bridge falls back to the sibling shelf when a title is not in the catalog it was filed under:
    // a manga classified as a comic_issue searches Comics, comes up empty, and is retried against Manga.
    CHECK(CatalogMatch::docCatalogSibling(QStringLiteral("comic")) == QStringLiteral("manga"));
    CHECK(CatalogMatch::docCatalogSibling(QStringLiteral("manga")) == QStringLiteral("comic"));
    CHECK(CatalogMatch::docCatalogSibling(QStringLiteral("book")).isEmpty());        // no sibling
    CHECK(CatalogMatch::docCatalogSibling(QStringLiteral("audiobook")).isEmpty());
    CHECK(CatalogMatch::docCatalogSibling(QStringLiteral("game")).isEmpty());
    CHECK(CatalogMatch::docCatalogSibling(QString()).isEmpty());

    // ---- drilling a matched manga SERIES into the requested chapter ---------------------------------
    // Manga is filed as series→chapters, so a chapter search answers with the series container (expandable),
    // not a readable leaf. The doc-bridge extracts the requested chapter number from the trailing number of
    // the query, then matches it against each chapter item's parsed number.
    CHECK(CatalogMatch::requestedChapterNumber(QStringLiteral("Doubutsu Ningen 1")) == QStringLiteral("1"));
    CHECK(CatalogMatch::requestedChapterNumber(QStringLiteral("One Piece 1052.5")) == QStringLiteral("1052.5"));
    CHECK(CatalogMatch::requestedChapterNumber(QStringLiteral("Berserk")).isEmpty()); // no trailing number
    CHECK(CatalogMatch::requestedChapterNumber(QStringLiteral("Vinland Saga 24 ")) == QStringLiteral("24")); // trailing space tolerated
    CHECK(CatalogMatch::requestedChapterNumber(QString()).isEmpty());

    // chapter-item number matching: parse the item's number and compare NUMERICALLY to the request.
    CHECK(CatalogMatch::chapterNumberMatches(QStringLiteral("Chapter 1"), QStringLiteral("1")));      // exact
    CHECK(!CatalogMatch::chapterNumberMatches(QStringLiteral("Chapter 12"), QStringLiteral("1")));    // 12 != 1
    CHECK(CatalogMatch::chapterNumberMatches(QStringLiteral("Chapter 1.0"), QStringLiteral("1")));    // 1.0 == 1
    CHECK(CatalogMatch::chapterNumberMatches(QStringLiteral("Ch. 1052.5"), QStringLiteral("1052.5"))); // fractional
    CHECK(!CatalogMatch::chapterNumberMatches(QStringLiteral("Prologue"), QStringLiteral("1")));       // no number in item
    CHECK(!CatalogMatch::chapterNumberMatches(QStringLiteral("Chapter 1"), QString()));                // no number wanted

    if (failures == 0) { std::puts("RESOLVER-OK"); return 0; }
    std::fprintf(stderr, "RESOLVER: %d check(s) failed\n", failures);
    return 1;
}
