// Headless coverage for the Stremio protocol translator, and for everything the app does with the
// behaviorHints.proxyHeaders a stream arrives with — parsing, the origin guard, the player assignment set,
// and (since #59) the requests the app makes itself.
//
// No addon is installed. Sections 17-18 send nothing at all: they build QNetworkRequests and a download
// queue but never run an event loop, so QNetworkAccessManager::get() queues a request that is inspected and
// dropped. Section 19 is the exception, and deliberately: the redirect DECISION cannot be reached without a
// server that answers 302, and the bug it now guards was in the consequence of that decision, not in the
// request. It talks only to two loopback servers it starts itself, on EPHEMERAL ports — the objection to a
// port-binding probe is that a fixed port fails on a busy CI box, and listen(…, 0) has no fixed port to be
// unlucky with. Anything needing a host we do not control (a gate that answers 403) is still proved out of
// tree.
//
// Prints STREMIO-OK on success; any failure prints STREMIO-FAIL <what> and exits non-zero.
#include "StremioTranslate.h"
#include "AppPaths.h"
#include "BingeStore.h"
#include "DownloadManager.h"
#include "NetHeaderApply.h"
#include "StreamHeaders.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <cstdio>
#include <functional>
#include <memory>

static int failures = 0;
#define CHECK(cond, what)                                                        \
    do { if (!(cond)) { std::fprintf(stderr, "STREMIO-FAIL %s\n", (what)); ++failures; } } while (0)

using namespace StremioTranslate;

static const Catalog* byId(const Manifest& m, const QString& id)
{
    for (const Catalog& c : m.catalogs) if (c.id == id) return &c;
    return nullptr;
}

// ---- the loopback half of section 18 ----------------------------------------------------------
// A minimal HTTP responder, one request per connection, used to put a REAL 302 in front of a real
// DownloadManager. Binds an EPHEMERAL port — listen(…, 0) — which is what makes this admissible under the
// rule at the top of this file: a port the OS chooses cannot collide with something already running on a
// busy CI box, so there is no fixed port to be unlucky with.
//
// It records the request's field NAMES, lower-cased, and never a value. That is not squeamishness: the
// assertions below are about which fields left the process, the values are fabricated three lines away, and
// a probe that keeps one in a variable is a probe someone later prints.
struct Loopback
{
    QTcpServer srv;
    int hits = 0;                 // connections accepted; 0 is the whole assertion for the cross-origin host
    QByteArray fieldNames;        // "\nhost\nuser-agent\nreferer\n…" of the last request
    std::function<QByteArray(const QByteArray& path)> answer;

    bool start()
    {
        if (!srv.listen(QHostAddress::LocalHost, 0)) return false;
        QObject::connect(&srv, &QTcpServer::newConnection, &srv, [this] {
            QTcpSocket* c = srv.nextPendingConnection();
            if (!c) return;
            ++hits;
            auto buf = std::make_shared<QByteArray>();
            QObject::connect(c, &QTcpSocket::readyRead, c, [this, c, buf] {
                buf->append(c->readAll());
                const int end = buf->indexOf("\r\n\r\n");
                if (end < 0) return;                       // request head not complete yet
                const QList<QByteArray> lines = buf->left(end).split('\n');
                fieldNames = "\n";
                for (int i = 1; i < lines.size(); ++i)     // [0] is the request line
                {
                    const int colon = lines.at(i).indexOf(':');
                    if (colon > 0) fieldNames += lines.at(i).left(colon).trimmed().toLower() + "\n";
                }
                const QList<QByteArray> req = lines.value(0).trimmed().split(' ');
                c->write(answer(req.value(1)));
                c->flush();
                c->disconnectFromHost();
            });
            QObject::connect(c, &QTcpSocket::disconnected, c, &QObject::deleteLater);
        });
        return true;
    }

    QString url(const QString& path) const
    { return QStringLiteral("http://127.0.0.1:%1%2").arg(srv.serverPort()).arg(path); }
};

static const DownloadJob* jobFor(const DownloadManager& dm, const QString& destSuffix)
{
    for (const DownloadJob& j : dm.jobs()) if (j.dest.endsWith(destSuffix)) return &j;
    return nullptr;
}

// Run the event loop until the named job leaves Queued/Active. Everything here is loopback with no DNS, so
// the deadline is enormously generous; it exists so a broken build reports a FAILED assertion instead of
// hanging the suite, which is the failure mode a probe must never have.
static bool settle(const DownloadManager& dm, const QString& destSuffix, int ms = 15000)
{
    QElapsedTimer t; t.start();
    while (t.elapsed() < ms)
    {
        const DownloadJob* j = jobFor(dm, destSuffix);
        if (j && j->state != DownloadJob::Queued && j->state != DownloadJob::Active) return true;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    return false;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ------------------------------------------------- 1. not a Stremio manifest
    {
        CHECK(!parseManifest(QByteArray("{\"id\":\"x\",\"name\":\"y\"}")).isValid(),
              "a manifest with no resources is not Stremio");
        CHECK(!parseManifest(QByteArray("not json at all")).isValid(), "garbage is not Stremio");
        // Detection needs BOTH keys, so each one has to be pinned WITHOUT the other — a body carrying
        // neither would still be rejected by half a guard and proves nothing about the other half.
        CHECK(!parseManifest(QByteArray(R"({"id":"a","resources":["stream"]})")).isValid(),
              "resources without types is not Stremio");
        CHECK(!parseManifest(QByteArray(R"({"id":"a","types":["movie"]})")).isValid(),
              "types without resources is not Stremio");
    }

    // ------------------------------------------------- 2. resources: strings, objects, MIXED
    {
        // Torrentio's shape: a plain string alongside an object carrying per-resource scoping.
        const QByteArray body = R"({
          "id": "com.stremio.torrentio.addon",
          "name": "Torrentio",
          "version": "0.0.15",
          "types": ["movie", "series"],
          "idPrefixes": ["tt"],
          "resources": [
            "catalog",
            { "name": "stream", "types": ["movie"], "idPrefixes": ["tt", "kitsu:"] }
          ],
          "catalogs": []
        })";
        const Manifest m = parseManifest(body);
        CHECK(m.isValid(), "a mixed resources array parses");
        CHECK(m.resources.contains(QStringLiteral("catalog")), "the string entry yields its name");
        CHECK(m.resources.contains(QStringLiteral("stream")), "the object entry yields its name");
        CHECK(m.idPrefixes == QStringList{ QStringLiteral("tt") }, "manifest-level idPrefixes");
        CHECK(m.resourceIdPrefixes.value(QStringLiteral("stream"))
                  == (QStringList{ QStringLiteral("tt"), QStringLiteral("kitsu:") }),
              "the object's own idPrefixes are kept, not discarded");
        CHECK(m.resourceTypes.value(QStringLiteral("stream")) == QStringList{ QStringLiteral("movie") },
              "the object's own types are kept");
        CHECK(!m.resourceIdPrefixes.contains(QStringLiteral("catalog")),
              "a string resource records no per-resource override");
    }

    // ------------------------------------------------- 3. catalog classification
    {
        const QByteArray body = R"({
          "id": "org.test.catalogs",
          "types": ["movie"],
          "resources": ["catalog"],
          "catalogs": [
            { "type": "movie", "id": "plain", "name": "Popular" },
            { "type": "movie", "id": "searchonly", "name": "Search",
              "extra": [ { "name": "search", "isRequired": true } ] },
            { "type": "movie", "id": "genrereq", "name": "By Genre",
              "extra": [ { "name": "genre", "isRequired": true,
                           "options": ["Action", "Comedy", "Drama"], "optionsLimit": 2 } ] },
            { "type": "movie", "id": "opaque", "name": "Needs Something",
              "extra": [ { "name": "mystery", "isRequired": true } ] },
            { "type": "movie", "id": "optional", "name": "Discover",
              "extra": [ { "name": "genre", "options": ["Action", "Comedy"] },
                         { "name": "skip" } ] }
          ]
        })";
        const Manifest m = parseManifest(body);
        CHECK(m.catalogs.size() == 5, "no catalog is dropped at parse time — classification decides use");

        CHECK(byId(m, "plain")->use == CatalogUse::Browse, "no extras -> Browse");

        CHECK(byId(m, "searchonly")->use == CatalogUse::SearchOnly, "required search -> SearchOnly");

        const Catalog* g = byId(m, "genrereq");
        CHECK(g->use == CatalogUse::Browse, "a required extra WITH options is browsable");
        CHECK(g->presets.value(QStringLiteral("genre")) == QStringLiteral("Action"),
              "…preselecting its FIRST option");
        CHECK(g->extras.first().optionsLimit == 2, "optionsLimit is carried");

        const Catalog* o = byId(m, "opaque");
        CHECK(o->use == CatalogUse::Unsatisfiable, "a required extra with no options is unsatisfiable");
        CHECK(!o->skipReason.isEmpty(), "…and says why, so it is never silently invisible");

        const Catalog* opt = byId(m, "optional");
        CHECK(opt->use == CatalogUse::Browse, "optional extras stay browsable");
        CHECK(opt->presets.isEmpty(), "…with nothing preselected");

        CHECK(byId(m, "genrereq")->routeId() == QStringLiteral("movie/genrereq"), "routeId shape");
    }

    // ------------------------------------------------- 4. the LEGACY extra form
    {
        // extraRequired/extraSupported are plain string arrays with no options anywhere.
        const QByteArray body = R"({
          "id": "org.test.legacy",
          "types": ["series"],
          "resources": ["catalog"],
          "catalogs": [
            { "type": "series", "id": "legacysearch", "name": "Old Search",
              "extraRequired": ["search"], "extraSupported": ["search", "skip"] },
            { "type": "series", "id": "legacyopt", "name": "Old Discover",
              "extraSupported": ["genre", "skip"] }
          ]
        })";
        const Manifest m = parseManifest(body);
        const Catalog* s = byId(m, "legacysearch");
        CHECK(s->use == CatalogUse::SearchOnly, "legacy extraRequired:[search] behaves like the modern form");
        bool sawSkip = false;
        for (const Extra& e : s->extras) if (e.name == QStringLiteral("skip")) sawSkip = true;
        CHECK(sawSkip, "extraSupported entries become non-required Extras");
        CHECK(byId(m, "legacyopt")->use == CatalogUse::Browse, "extraSupported alone is not required");
    }

    // ------------------------------------------------- 5. BOTH extra forms on ONE catalog: MERGE
    {
        // Neither fixture above carries both forms, so a replace-style legacy block would pass them all.
        // A manifest may declare the modern objects AND the legacy arrays; the legacy names must merge
        // INTO the parsed objects, never stand in for them.
        const QByteArray body = R"({
          "id": "org.test.bothforms",
          "types": ["movie"],
          "resources": ["catalog"],
          "catalogs": [
            { "type": "movie", "id": "upgrade", "name": "By Genre",
              "extra": [ { "name": "genre", "options": ["Action", "Comedy"] } ],
              "extraRequired": ["genre"] },
            { "type": "movie", "id": "adds", "name": "By Genre and Studio",
              "extra": [ { "name": "genre", "options": ["Action", "Comedy"] } ],
              "extraRequired": ["studio"] }
          ]
        })";
        const Manifest m = parseManifest(body);

        // Case 1: legacy names the SAME extra — it upgrades isRequired, the object's options survive.
        const Catalog* u = byId(m, "upgrade");
        CHECK(u->extras.size() == 1, "the legacy name merges into the modern entry, it does not duplicate it");
        CHECK(u->extras.first().isRequired, "extraRequired upgrades the modern entry to required");
        CHECK(u->use == CatalogUse::Browse, "…and the modern options survive, so it is still browsable");
        CHECK(u->presets.value(QStringLiteral("genre")) == QStringLiteral("Action"),
              "…preselecting the first option the object declared");

        // Case 2: legacy names a DIFFERENT extra — it is added, option-less, so the catalog dies.
        const Catalog* a = byId(m, "adds");
        CHECK(a->extras.size() == 2, "a legacy name not in extra[] is ADDED alongside it");
        CHECK(a->use == CatalogUse::Unsatisfiable,
              "…and being required with no options makes the catalog unsatisfiable");
        CHECK(!a->skipReason.isEmpty(), "…with a reason naming what is missing");
    }

    // ------------------------------------------------- 6. MULTIPLE required extras
    {
        const QByteArray body = R"({
          "id": "org.test.multireq",
          "types": ["movie"],
          "resources": ["catalog"],
          "catalogs": [
            { "type": "movie", "id": "searchandgenre", "name": "Search by Genre",
              "extra": [ { "name": "search", "isRequired": true },
                         { "name": "genre", "isRequired": true, "options": ["Action", "Comedy"] } ] },
            { "type": "movie", "id": "genrefirst", "name": "Genre then Mystery",
              "extra": [ { "name": "genre", "isRequired": true, "options": ["Action", "Comedy"] },
                         { "name": "mystery", "isRequired": true } ] },
            { "type": "movie", "id": "mysteryfirst", "name": "Mystery then Genre",
              "extra": [ { "name": "mystery", "isRequired": true },
                         { "name": "genre", "isRequired": true, "options": ["Action", "Comedy"] } ] }
          ]
        })";
        const Manifest m = parseManifest(body);

        // Search dominates a defaultable extra — and dominating means the default is DROPPED. Left in
        // place, presets["genre"] would seed the query map and quietly filter every search to Action.
        const Catalog* sg = byId(m, "searchandgenre");
        CHECK(sg->use == CatalogUse::SearchOnly,
              "required search wins over a required extra we could have defaulted");
        CHECK(sg->presets.isEmpty(),
              "…and the default is dropped, so a search is never silently filtered by it");

        // Same manifest, extras reordered: the classifier returns on the first extra it cannot supply,
        // so the struct must be scrubbed on that path or its presets would depend on declaration order.
        const Catalog* gf = byId(m, "genrefirst");
        const Catalog* mf = byId(m, "mysteryfirst");
        CHECK(gf->use == CatalogUse::Unsatisfiable && mf->use == CatalogUse::Unsatisfiable,
              "an option-less required extra is unsatisfiable in either declaration order");
        CHECK(!gf->skipReason.isEmpty() && !mf->skipReason.isEmpty(), "…both saying why");
        CHECK(gf->presets.isEmpty(),
              "an unsatisfiable catalog keeps NO presets, even ones recorded before the verdict");
        CHECK(gf->presets == mf->presets, "…so reordering the extras cannot change the result");
    }

    // ------------------------------------------------- 7. duplicate names inside extra[]
    {
        const QByteArray body = R"({
          "id": "org.test.dupes",
          "types": ["movie"],
          "resources": ["catalog"],
          "catalogs": [
            { "type": "movie", "id": "dupe", "name": "Twice",
              "extra": [ { "name": "genre", "options": ["Action", "Comedy"] },
                         { "name": "genre", "options": ["Sci-Fi"] } ] }
          ]
        })";
        const Manifest m = parseManifest(body);
        const Catalog* d = byId(m, "dupe");
        CHECK(d->extras.size() == 1, "a name declared twice in extra[] yields ONE Extra, not a doubled control");
        CHECK(d->extras.first().options == (QStringList{ QStringLiteral("Action"), QStringLiteral("Comedy") }),
              "…the FIRST declaration wins, as on the legacy path");
    }

    // ------------------------------------------------- 8. behaviorHints
    {
        const QByteArray body = R"({
          "id": "org.test.cfg", "types": ["movie"], "resources": ["stream"],
          "behaviorHints": { "configurable": true, "configurationRequired": true }
        })";
        const Manifest m = parseManifest(body);
        CHECK(m.configurable && m.configurationRequired, "manifest behaviorHints are read");

        const Manifest none = parseManifest(QByteArray(
            R"({"id":"a","types":["movie"],"resources":["stream"]})"));
        CHECK(!none.configurable && !none.configurationRequired, "absent behaviorHints default to false");
    }

    // ------------------------------------------------- 9. catalogPath
    {
        Catalog c;
        c.type = QStringLiteral("movie");
        c.id   = QStringLiteral("top");
        CHECK(catalogPath(c, {}) == QStringLiteral("/catalog/movie/top.json"), "no extras -> bare path");

        // Sorted keys, so the same request is always the same string (the result cache keys on it).
        QMap<QString, QString> two;
        two.insert(QStringLiteral("skip"), QStringLiteral("100"));
        two.insert(QStringLiteral("genre"), QStringLiteral("Action"));
        CHECK(catalogPath(c, two) == QStringLiteral("/catalog/movie/top/genre=Action&skip=100.json"),
              "extras are emitted in sorted key order");

        // A value containing the separators must not be able to forge extra params.
        QMap<QString, QString> nasty;
        nasty.insert(QStringLiteral("search"), QStringLiteral("a b&c=d"));
        CHECK(catalogPath(c, nasty) == QStringLiteral("/catalog/movie/top/search=a%20b%26c%3Dd.json"),
              "spaces, & and = in a value are percent-encoded");

        // Only VALUES were exercised above, and every key in this file is unreserved — so the encoder
        // could be dropped from the key path and from type/id entirely without a single check moving.
        // A catalog id and an extra KEY that both need escaping pin those two call sites.
        Catalog wild;
        wild.type = QStringLiteral("movie");
        wild.id   = QStringLiteral("top rated&x");
        QMap<QString, QString> wildkey;
        wildkey.insert(QStringLiteral("release year"), QStringLiteral("2019"));
        CHECK(catalogPath(wild, wildkey)
                  == QStringLiteral("/catalog/movie/top%20rated%26x/release%20year=2019.json"),
              "the catalog id AND the extra key are percent-encoded, not just the value");

        // An empty value carries no information and must not become "key=" — some addons 400 on it.
        QMap<QString, QString> blank;
        blank.insert(QStringLiteral("search"), QString());
        CHECK(catalogPath(c, blank) == QStringLiteral("/catalog/movie/top.json"),
              "an empty value is dropped rather than emitted as a bare key=");

        // Presets fill gaps; a caller value REPLACES rather than joins.
        Catalog g = c;
        g.presets.insert(QStringLiteral("genre"), QStringLiteral("Action"));
        CHECK(catalogPath(g, {}) == QStringLiteral("/catalog/movie/top/genre=Action.json"),
              "a preset supplies the default");
        QMap<QString, QString> chosen;
        chosen.insert(QStringLiteral("genre"), QStringLiteral("Comedy"));
        CHECK(catalogPath(g, chosen) == QStringLiteral("/catalog/movie/top/genre=Comedy.json"),
              "the caller's value replaces the preset, never appends");
    }

    // ------------------------------------------------- 10. handlesId
    {
        Manifest m;
        m.resources << QStringLiteral("stream") << QStringLiteral("meta");
        m.idPrefixes << QStringLiteral("tt");
        m.resourceIdPrefixes.insert(QStringLiteral("stream"),
                                    { QStringLiteral("kitsu:") });

        CHECK(handlesId(m, QStringLiteral("stream"), QStringLiteral("kitsu:1234")),
              "the per-resource prefix matches");
        CHECK(!handlesId(m, QStringLiteral("stream"), QStringLiteral("tt0903747")),
              "the per-resource list OVERRIDES the manifest-level one for that resource");
        CHECK(handlesId(m, QStringLiteral("meta"), QStringLiteral("tt0903747")),
              "a resource with no override falls back to manifest-level");
        CHECK(!handlesId(m, QStringLiteral("meta"), QStringLiteral("kitsu:1")),
              "…and is filtered by it");

        Manifest open;
        open.resources << QStringLiteral("stream");
        CHECK(handlesId(open, QStringLiteral("stream"), QStringLiteral("anything")),
              "an addon declaring NO prefixes answers for everything");
    }

    // ------------------------------------------------- 11. parseStreams
    {
        // The torrent row's title deliberately carries NO size token, and its videoSize renders as
        // "3.0 GB" — a string that appears nowhere in the fixture. describe() copies the title verbatim,
        // so a size that merely echoes the title (the old "2.1 GB") asserted nothing: delete the size
        // branch and the copied title still satisfied it.
        const QByteArray body = R"({"streams":[
          { "name": "Torrentio\n1080p", "title": "Movie.2019.1080p.x265\n👤 42",
            "infoHash": "0123456789abcdef0123456789abcdef01234567", "fileIdx": 0,
            "behaviorHints": { "bingeGroup": "torrentio|1080p|x265", "videoSize": 3221225472 } },
          { "name": "Direct", "url": "https://example.com/a.mkv", "mime": "video/x-matroska",
            "behaviorHints": { "notWebReady": true } },
          { "name": "DescOnly", "description": "Fallback.Release.720p 👤 7 💾 1.5 GB",
            "infoHash": "89abcdef0123456789abcdef0123456789abcdef",
            "behaviorHints": { "videoSize": 3221225472 } },
          { "name": "Broken", "infoHash": "#", "title": "please report this issue" },
          { "name": "Nothing useful", "title": "no url and no hash" }
        ]})";
        const QVector<StreamCandidate> v = parseStreams(body);
        CHECK(v.size() == 3, "rows with neither a usable url nor a valid infoHash are dropped");

        // Sorted: direct http before torrent.
        CHECK(v[0].isDirect(), "a direct url sorts ahead of a torrent");
        CHECK(v[0].notWebReady, "stream-level behaviorHints.notWebReady is kept");
        CHECK(v[0].mime == QStringLiteral("video/x-matroska"), "mime is kept");

        const StreamCandidate& t = v[1];
        CHECK(t.name.contains(QStringLiteral("1080p")), "name is kept (it carries the quality)");
        CHECK(t.title.contains(QStringLiteral("x265")), "title is kept (it carries the release)");
        CHECK(t.bingeGroup == QStringLiteral("torrentio|1080p|x265"), "bingeGroup is kept");
        CHECK(t.videoSize == 3221225472LL, "videoSize is kept");
        CHECK(t.seeders == 42, "seeders are scraped out of the title");
        CHECK(t.fileIdx == 0, "fileIdx is kept");

        // The size comes from behaviorHints, so it is genuinely additive here — and "3.0 GB" appears
        // nowhere in the copied title, which is what makes this fail if the size branch is deleted.
        CHECK(describe(t).contains(QStringLiteral("3.0 GB")),
              "describe appends the behaviorHints size when the title carries none");
        // Seeders are scraped FROM the title being displayed, so appending them can only duplicate it.
        CHECK(!describe(t).contains(QStringLiteral("seeders")),
              "describe never appends the seeder count — the title it renders already shows it");
        // describe must be ONE line — NavMenu word-wraps, and a row with embedded newlines reads as junk.
        CHECK(!describe(t).contains(QLatin1Char('\n')), "describe collapses the addon's newlines");

        // An addon that puts its release line in `description` instead of `title`. Without the fallback
        // it loses ALL picker text and — because the scrape reads `title` — its whole catalogue lands at
        // -1 seeders and orders by size alone.
        const StreamCandidate& d = v[2];
        CHECK(d.title.startsWith(QStringLiteral("Fallback.Release.720p")),
              "an empty title falls back to `description`");
        CHECK(d.seeders == 7, "…and the fallen-back text is scraped for seeders like any other title");
        // fileIdx is read behind a contains() guard. Drop it and an absent field becomes 0, which
        // AddonManager reads as "play files[0]" — the sample or the NFO in a season pack — instead of
        // falling back to the largest video file.
        CHECK(d.fileIdx == -1, "an absent fileIdx stays -1, it does not collapse to file 0");
        // This row's own text already carries "1.5 GB", so the behaviorHints size must NOT be appended.
        CHECK(!describe(d).contains(QStringLiteral("3.0 GB")),
              "the size is suppressed when the title already carries one");
    }

    // ------------------------------------------------- 11a. describe() is capped to ONE picker row
    {
        // A real raw-torrent release line. Flattening newlines keeps a row one PARAGRAPH; only the length cap
        // keeps it one ROW — NavMenu word-wraps, so an over-long candidate silently occupies two rows and the
        // list stops being scannable. Asserted against kMaxDescribeChars (not a literal) so the cap and the
        // contract move together.
        StreamCandidate longRow;
        longRow.name  = QStringLiteral("Torrentio\n1080p");
        longRow.title = QStringLiteral(
            "The.Very.Long.Release.Name.2019.1080p.BluRay.REMUX.AVC.DTS-HD.MA.7.1-GROUPNAME.Extended."
            "Directors.Cut.With.Commentary.And.A.Great.Many.Further.Tokens.Nobody.Reads 👤 42");
        longRow.url = QStringLiteral("https://example.com/a.mkv");
        CHECK(longRow.title.size() > kMaxDescribeChars, "the fixture is actually longer than the cap");

        const QString row = describe(longRow);
        CHECK(row.size() <= kMaxDescribeChars,
              "describe elides to kMaxDescribeChars so one candidate stays one row");
        CHECK(row.endsWith(QChar(0x2026)), "an elided row says so with an ellipsis");
        CHECK(!row.contains(QLatin1Char('\n')), "an elided row is still one line");

        // A row that already fits is untouched — the cap must not trim or ellipsise short candidates.
        StreamCandidate shortRow;
        shortRow.name = QStringLiteral("1080p");
        shortRow.title = QStringLiteral("Short.Release.x265");
        shortRow.url = QStringLiteral("https://example.com/b.mkv");
        CHECK(describe(shortRow) == QStringLiteral("1080p · Short.Release.x265"),
              "a row within the cap is rendered verbatim");
    }

    // ------------------------------------------------- 11b. seeder scraping is PRIORITY, not leftmost
    {
        // One combined alternation is leftmost-wins, so a season token pre-empts the real marker whenever
        // it sits further left — and a release that reads as 2 seeders sorts below every unknown (-1)
        // row, so auto-play silently lands on a different, possibly dead torrent.
        auto seedersOf = [](const QByteArray& title) {
            const QByteArray b = QByteArrayLiteral("{\"streams\":[{\"name\":\"x\",\"title\":\"") + title
                + "\",\"infoHash\":\"0123456789abcdef0123456789abcdef01234567\"}]}";
            const QVector<StreamCandidate> got = parseStreams(b);
            return got.size() == 1 ? got.at(0).seeders : -2;
        };
        const QByteArray person = QByteArrayLiteral("\xF0\x9F\x91\xA4");   // the seeder emoji
        const QByteArray disk   = QByteArrayLiteral("\xF0\x9F\x92\xBE");   // the size emoji

        CHECK(seedersOf(QByteArrayLiteral("Show S:2 E:5 ") + person + " 500 " + disk + " 12 GB") == 500,
              "the seeder emoji wins over an S: season token sitting to its LEFT");
        CHECK(seedersOf(QByteArrayLiteral("1080p S:2 E:5")) == 2,
              "with no stronger marker present the bare s: form is still read");
        CHECK(seedersOf(QByteArrayLiteral("S: 42 | L: 3")) == 42,
              "the legitimate bare s: form — the reason that alternative exists at all");
        CHECK(seedersOf(QByteArrayLiteral("Seeders: 9 | Peers: 3")) == 9, "the spelled-out form");

        // Verified-clean shapes that must keep reading as unknown: the (?<![a-z]) lookbehind and the
        // required colon are what protect them.
        CHECK(seedersOf(QByteArrayLiteral("Movie.2019 2.1 GB")) == -1, "a size is not a seeder count");
        CHECK(seedersOf(QByteArrayLiteral("Movie.2019.1080p")) == -1, "a resolution is not one either");
        CHECK(seedersOf(QByteArrayLiteral("Show.S01E05.2160p")) == -1, "nor a scene episode token");
        CHECK(seedersOf(QByteArrayLiteral("Sens8.S01 1080p")) == -1,
              "…nor a season token glued to a title that ends in s");
    }

    // ------------------------------------------------- 12. sort order, cap, and pickAuto
    {
        QByteArray body = QByteArrayLiteral("{\"streams\":[");
        for (int i = 0; i < 50; ++i)
            body += QByteArray("{\"name\":\"t") + QByteArray::number(i)
                  + "\",\"title\":\"\xF0\x9F\x91\xA4 " + QByteArray::number(i)
                  + "\",\"infoHash\":\"0123456789abcdef0123456789abcdef0123456" + (i % 10 ? "7" : "8")
                  + "\"},";
        body.chop(1);
        body += "]}";
        const QVector<StreamCandidate> v = parseStreams(body);
        CHECK(v.size() == kMaxStreamRows, "the candidate list is capped");
        CHECK(v[0].seeders >= v[1].seeders, "higher seeders sort first");
        // The cap must be applied AFTER the sort. Capping first also leaves a descending list — so the
        // check above passes on that mutation, and only the actual best row's value catches it.
        CHECK(v[0].seeders == 49 && v[v.size() - 1].seeders == 20,
              "the cap keeps the BEST rows, not the first 30 the addon happened to list");

        // pickAuto: a remembered bingeGroup wins over sort order; a miss falls back to the first row.
        QVector<StreamCandidate> three;
        StreamCandidate a; a.url = QStringLiteral("https://a"); a.bingeGroup = QStringLiteral("g1");
        StreamCandidate b; b.url = QStringLiteral("https://b"); b.bingeGroup = QStringLiteral("g2");
        // The third carries NO bingeGroup, and sits LAST. That is what gives the empty-preferGroup guard
        // teeth: without the guard the match loop runs with preferGroup == "", finds this row's empty
        // bingeGroup and returns 2 — handing "no preference" the worst row instead of the best one.
        // With both candidates grouped (as this fixture used to be) the guard could be deleted unnoticed.
        StreamCandidate c; c.url = QStringLiteral("https://c");
        three << a << b << c;
        CHECK(pickAuto(three, QStringLiteral("g2")) == 1, "a remembered bingeGroup is chosen");
        CHECK(pickAuto(three, QStringLiteral("nope")) == 0, "an unmatched group falls back to the first");
        CHECK(pickAuto(three, QString()) == 0,
              "no preference -> the BEST row, not the first one that happens to have no group");
        CHECK(pickAuto({}, QStringLiteral("g1")) == -1, "nothing playable -> -1");
    }

    // ------------------------------------------------- 13. routeProviders: the id filter and its fallback
    {
        auto mf = [](const QString& res, const QStringList& types, const QStringList& prefixes) {
            Manifest m;
            m.resources = QStringList{res};
            m.types = types;
            m.idPrefixes = prefixes;
            return m;
        };
        const QStringList movie{QStringLiteral("movie")};
        QVector<Manifest> ms;
        ms << mf(QStringLiteral("stream"), movie, {QStringLiteral("tt")})        // 0: imdb only
           << mf(QStringLiteral("stream"), movie, {QStringLiteral("kitsu:")})    // 1: anime only
           << mf(QStringLiteral("catalog"), movie, {QStringLiteral("tt")})       // 2: wrong resource
           << mf(QStringLiteral("stream"), {QStringLiteral("series")}, {QStringLiteral("tt")}); // 3: wrong type

        bool fell = true;
        QVector<int> r = routeProviders(ms, QStringLiteral("stream"), QStringLiteral("movie"),
                                        QStringLiteral("tt0133093"), &fell);
        CHECK(r == (QVector<int>{0}), "only the stream addon of this type that claims the id space");
        CHECK(!fell, "…and that is a real match, not the fallback");

        r = routeProviders(ms, QStringLiteral("stream"), QStringLiteral("movie"),
                           QStringLiteral("kitsu:1"), &fell);
        CHECK(r == (QVector<int>{1}), "a different id space selects a different addon");
        CHECK(!fell, "still a real match");

        // THE FALLBACK, the reason routing is allowed to exist at all: an id no manifest claims must still
        // reach every stream provider. Without it a mis-declared idPrefixes makes an item silently unplayable.
        r = routeProviders(ms, QStringLiteral("stream"), QStringLiteral("movie"),
                           QStringLiteral("weird:99"), &fell);
        CHECK(r == (QVector<int>{0, 1}), "an unclaimed id falls back to ASKING EVERY stream provider");
        CHECK(fell, "and says so, so the log can explain it");
        // The fallback widens the ID filter ONLY. A resource/type mismatch is not a routing guess, it is the
        // addon saying it does not serve this at all — indices 2 and 3 must stay out even here.
        CHECK(!r.contains(2) && !r.contains(3),
              "the fallback does not resurrect the wrong resource or the wrong type");

        // An addon declaring NO prefixes claims everything, so it absorbs the unclaimed id on its own merits.
        // That is a match, not a fallback — and the addons that DID declare a mismatching prefix stay out.
        QVector<Manifest> withOpen = ms;
        withOpen << mf(QStringLiteral("stream"), movie, {});                      // 4: claims everything
        r = routeProviders(withOpen, QStringLiteral("stream"), QStringLiteral("movie"),
                           QStringLiteral("weird:99"), &fell);
        CHECK(r == (QVector<int>{4}) && !fell,
              "an addon claiming no prefixes takes the unclaimed id WITHOUT triggering the fallback");

        // Nothing offers the resource -> empty, and NOT flagged as a fallback: there was nothing to widen to.
        QVector<Manifest> none;
        none << mf(QStringLiteral("catalog"), movie, {});
        r = routeProviders(none, QStringLiteral("stream"), QStringLiteral("movie"), QStringLiteral("tt1"), &fell);
        CHECK(r.isEmpty() && !fell, "no provider offers streams at all -> empty, and not a fallback");
        CHECK(routeProviders({}, QStringLiteral("stream"), QStringLiteral("movie"), QStringLiteral("tt1")).isEmpty(),
              "an empty roster is handled without the out-param");

        // A per-resource idPrefixes narrows what the manifest-level list claims (handlesId's rule, reached
        // through the router — the seam AddonManager actually calls).
        Manifest perRes = mf(QStringLiteral("stream"), movie, {QStringLiteral("tt")});
        perRes.resourceIdPrefixes.insert(QStringLiteral("stream"), {QStringLiteral("kitsu:")});
        QVector<Manifest> two; two << perRes;
        CHECK(routeProviders(two, QStringLiteral("stream"), QStringLiteral("movie"), QStringLiteral("kitsu:1"))
                  == (QVector<int>{0}), "a per-resource prefix list routes on its own terms");
    }

    // ------------------------------------------------- 14. mergeCandidates: the AGGREGATE across addons
    {
        // Two addons' blocks, EACH ALREADY SORTED, exactly as listStremioStreams hands them over: addon A's
        // torrents (90, 40) and addon B's instant http url ahead of its own torrent (70). Both blocks are
        // internally correct, so a plain concatenation reads as sorted — and puts A's cold torrent first.
        // Merging is what stops auto-play taking it over B's instant stream.
        QVector<StreamCandidate> a, b;
        StreamCandidate a1; a1.infoHash = QStringLiteral("a"); a1.seeders = 90;
        StreamCandidate a2; a2.infoHash = QStringLiteral("b"); a2.seeders = 40;
        StreamCandidate b1; b1.url = QStringLiteral("https://b/direct");
        StreamCandidate b2; b2.infoHash = QStringLiteral("c"); b2.seeders = 70;
        a << a1 << a2;
        b << b1 << b2;
        // Sanity: each block IS sorted on its own, so the failure this pins is the merge's, not the fixture's.
        QVector<StreamCandidate> aSorted = a, bSorted = b;
        sortCandidates(aSorted); sortCandidates(bSorted);
        CHECK(aSorted[0].infoHash == a[0].infoHash && bSorted[0].url == b[0].url,
              "both fixture blocks are already individually sorted");

        const QVector<StreamCandidate> agg = mergeCandidates({a, b});
        CHECK(agg.size() == 4, "the merge keeps every row from every addon");
        // THE assertion: delete the sort inside mergeCandidates and this is the line that fails.
        CHECK(agg[0].url == QStringLiteral("https://b/direct"),
              "the second addon's instant url outranks the first addon's best torrent");
        CHECK(agg[1].seeders == 90 && agg[2].seeders == 70 && agg[3].seeders == 40,
              "and the torrents interleave by seeders ACROSS addons");
        CHECK(pickAuto(agg, QString()) == 0, "so auto-play lands on the instant url");

        // Degenerate shapes the aggregator really does hand it: no providers, and providers that answered
        // with nothing (a failed request leaves an empty slot in place).
        CHECK(mergeCandidates({}).isEmpty(), "no providers -> nothing");
        CHECK(mergeCandidates({{}, {}}).isEmpty(), "providers that all answered empty -> nothing");
        CHECK(mergeCandidates({{}, a, {}}).size() == 2, "empty slots do not disturb the rows that arrived");
    }

    // -------------------------------------- 14b. the per-response cap is a PARAMETER, not the debrid bound
    {
        // 50 rows from ONE addon — the common single-stream-addon setup. The picker's bound must still be 30,
        // but the resolution path asks for more so the batch cached-check can reach a release ranked 31-60.
        // With the cap hardcoded to kMaxStreamRows, a user whose only cached torrent sat at row 31 went from
        // playing fine to "nothing cached".
        QByteArray body = QByteArrayLiteral("{\"streams\":[");
        for (int i = 0; i < 50; ++i)
            body += QByteArray("{\"name\":\"t") + QByteArray::number(i)
                  + "\",\"title\":\"\xF0\x9F\x91\xA4 " + QByteArray::number(i)
                  + "\",\"infoHash\":\"0123456789abcdef0123456789abcdef0123456" + (i % 10 ? "7" : "8")
                  + "\"},";
        body.chop(1);
        body += "]}";
        CHECK(parseStreams(body).size() == kMaxStreamRows, "the default is still the picker's bound");
        const QVector<StreamCandidate> deep = parseStreams(body, 60);
        CHECK(deep.size() == 50, "a larger bound keeps every row the addon offered");
        CHECK(deep[0].seeders == 49 && deep[30].seeders == 19,
              "…including the ones past row 30, still in rank order");
        CHECK(parseStreams(body, 5).size() == 5, "and a smaller bound is honoured too");
    }

    // ---------------------------------- 14c. behaviorHints.proxyHeaders: parsing (#43)
    {
        const QByteArray body = R"({"streams":[{
          "url": "https://cdn.example.test/a.mp4",
          "name": "Direct",
          "behaviorHints": {
            "proxyHeaders": {
              "request":  { "Referer": "https://embed.example.test/watch",
                            "user-agent": "EB-Probe/1.0",
                            "X-Token": "abc,def" },
              "response": { "Content-Type": "video/mp4" }
            }
          }
        }]})";
        const QVector<StreamCandidate> v = parseStreams(body);
        CHECK(v.size() == 1, "the header-gated stream parses");
        const StreamHeaders::Headers h = v.value(0).requestHeaders;
        CHECK(h.size() == 3, "all three request headers are carried onto the candidate");
        CHECK(h.value(QStringLiteral("Referer")) == QStringLiteral("https://embed.example.test/watch"),
              "Referer survives verbatim");
        // Name canonicalisation is not cosmetic: everything downstream (the mpv split, the field list, the
        // log summary) looks these up by name, so "user-agent" and "User-Agent" MUST be one key.
        CHECK(h.value(QStringLiteral("User-Agent")) == QStringLiteral("EB-Probe/1.0"),
              "a lowercase name is canonicalised, not carried as a second key");
        CHECK(h.value(QStringLiteral("X-Token")) == QStringLiteral("abc,def"),
              "a value containing a comma is carried whole");
        // The `response` half describes what the addon expects BACK. Carrying it would only create a second
        // thing to accidentally send.
        CHECK(!h.contains(QStringLiteral("Content-Type")), "the response half is not carried");

        // A stream with no behaviorHints at all carries no headers — the overwhelmingly common case, and the
        // one that has to stay empty rather than picking up a default.
        CHECK(parseStreams(R"({"streams":[{"url":"https://h.test/b.mp4"}]})").value(0).requestHeaders.isEmpty(),
              "a stream without proxyHeaders carries none");

        // canonicalName: mpv's option is spelled `referrer`, HTTP's header is `Referer`. Folding them means a
        // stream cannot end up with two spellings holding two different values.
        CHECK(StreamHeaders::canonicalName(QStringLiteral("referrer")) == QStringLiteral("Referer"),
              "the two-r spelling folds onto the HTTP one");
        CHECK(StreamHeaders::canonicalName(QStringLiteral("X-FORWARDED-for")) == QStringLiteral("X-Forwarded-For"),
              "each dash-separated part is capitalised");

        // The RFC 7230 `token` charset, asserted on canonicalName directly because that is where the rule
        // lives and every consumer of a header name goes through it. The parse-level fixture in 14d pins
        // the consequence; these pin the rule, one refused character class at a time.
        CHECK(StreamHeaders::canonicalName(QStringLiteral("X-Ok_1.2*")) == QStringLiteral("X-Ok_1.2*"),
              "the punctuation RFC 7230 actually permits is not refused");
        CHECK(StreamHeaders::canonicalName(QStringLiteral("X-A\r\nRange")).isEmpty(),
              "a name carrying CRLF is refused outright — the request-smuggling primitive");
        CHECK(StreamHeaders::canonicalName(QStringLiteral("X A")).isEmpty(), "…as is one containing a space");
        CHECK(StreamHeaders::canonicalName(QStringLiteral("X:A")).isEmpty(), "…or its own colon");
        CHECK(StreamHeaders::canonicalName(QStringLiteral("X-Café")).isEmpty(),
              "…or a non-ASCII letter, which QChar::isLetterOrNumber would have waved through");
        CHECK(StreamHeaders::canonicalName(QString(QChar(u'\0'))).isEmpty(),
              "…or a lone NUL, which strchr's terminator match would otherwise accept");
        CHECK(StreamHeaders::canonicalName(QStringLiteral("  Referer  ")) == QStringLiteral("Referer"),
              "surrounding whitespace is still tolerated — the charset rule applies to the trimmed name");
    }

    // ---------------------------------- 14d. proxyHeaders: what is REFUSED on the way in (#43)
    {
        const QByteArray body = R"({"streams":[{
          "url": "https://cdn.example.test/a.mp4",
          "behaviorHints": { "proxyHeaders": { "request": {
            "Referer":   "https://ok.example.test/",
            "Host":      "victim.example.test",
            "Range":     "bytes=0-99",
            "Content-Length": "12",
            "Connection": "keep-alive",
            "Transfer-Encoding": "chunked",
            "Upgrade":   "websocket",
            "X-Inject":  "v\r\nX-Evil: 1",
            "X-Empty":   "",
            "":          "nameless",
            "X-Number":  7,
            "X-Smuggle\r\nRange": "bytes=0-1",
            "X Spaced":  "v",
            "X-Colon: Y": "v",
            "X-é":  "v"
          } } }
        }]})";
        const StreamHeaders::Headers h = parseStreams(body).value(0).requestHeaders;
        CHECK(h.keys() == QStringList{ QStringLiteral("Referer") },
              "exactly one header survives the filter — every other entry above is refused");
        // Named individually, because a single blanket assertion above would still pass if the filter kept
        // the WRONG one of them, and each of these is refused for its own reason.
        CHECK(!h.contains(QStringLiteral("Host")), "Host is refused: it re-points the request");
        CHECK(!h.contains(QStringLiteral("Range")),
              "Range is refused: the player issues its own per seek, and a pinned one freezes playback");
        CHECK(!h.contains(QStringLiteral("Content-Length")) && !h.contains(QStringLiteral("Connection"))
                  && !h.contains(QStringLiteral("Transfer-Encoding")) && !h.contains(QStringLiteral("Upgrade")),
              "hop-by-hop / body-shaping fields are refused");
        CHECK(!h.contains(QStringLiteral("X-Inject")),
              "a value containing CRLF is refused — one field must not become two");
        // The NAME half of the same guard, and the sharper one. trimmed() strips only leading/trailing
        // whitespace, so an EMBEDDED CRLF used to survive canonicalisation, miss the blocklist (the key is
        // not spelled `range`) and reach mpv's field list verbatim — which puts the bytes on the socket.
        // Asserted three ways because each defeats the filter differently.
        // Matched case-INSENSITIVELY and by substring, deliberately. Asserting `!h.contains("X-Spaced")`
        // would be inert: canonicalName only capitalises after a dash, so a surviving mangled key is
        // spelled "X spaced" / "X-Smuggle\r\nrange" and an exact-spelling assertion would pass while the
        // field sat in the map. (Both of these WERE inert until mutation C1-a showed them staying green.)
        CHECK(h.keys().filter(QStringLiteral("smuggle"), Qt::CaseInsensitive).isEmpty()
                  && h.keys().filter(QStringLiteral("range"), Qt::CaseInsensitive).isEmpty(),
              "a blocked field smuggled as a CRLF continuation of a permitted name does not survive — "
              "under any spelling");
        CHECK(h.keys().filter(QStringLiteral("spaced"), Qt::CaseInsensitive).isEmpty(),
              "a name containing a space is refused, not repaired into a legal one");
        CHECK(h.keys().filter(QStringLiteral("colon"), Qt::CaseInsensitive).isEmpty(),
              "a name carrying its own colon is refused — it would write two fields");
        CHECK(h.keys().filter(QStringLiteral("x-"), Qt::CaseInsensitive).isEmpty(),
              "…so no X- name from this fixture reaches the map at all");
        CHECK(!h.contains(QStringLiteral("X-Empty")), "an empty value is refused");
        // A number/array/object value stringifies to "" and is refused by that same rule — asserted because
        // it is the OBSERVABLE behaviour an addon can trip, not because a separate guard implements it.
        CHECK(!h.contains(QStringLiteral("X-Number")), "a non-string value is refused too");
    }

    // ---------------------------------- 14e. forPlayUrl: headers belong to ONE origin (#43)
    {
        StreamHeaders::Headers h;
        h.insert(QStringLiteral("Referer"), QStringLiteral("https://a.test/watch"));
        const QString declared = QStringLiteral("https://cdn.a.test:8443/movie.mp4");

        CHECK(StreamHeaders::forPlayUrl(h, declared, declared) == h,
              "the url they were declared for keeps them");
        CHECK(StreamHeaders::forPlayUrl(h, declared, QStringLiteral("https://cdn.a.test:8443/other.mp4")) == h,
              "…as does another path on the same origin");
        // THE hygiene assertion. A debrid/CDN substitute is a different host, and sending host A's Referer to
        // host B both leaks where the user came from and gets refused for a reason nobody would guess.
        CHECK(StreamHeaders::forPlayUrl(h, declared, QStringLiteral("https://cdn.b.test:8443/movie.mp4")).isEmpty(),
              "a DIFFERENT host gets none of them");
        CHECK(StreamHeaders::forPlayUrl(h, declared, QStringLiteral("https://cdn.a.test/movie.mp4")).isEmpty(),
              "a different PORT on the same host gets none of them");
        CHECK(StreamHeaders::forPlayUrl(h, declared, QStringLiteral("http://cdn.a.test:8443/movie.mp4")).isEmpty(),
              "a downgrade to http gets none of them");
        // Default ports are made explicit on both sides, so the two spellings of one origin still match.
        CHECK(StreamHeaders::forPlayUrl(h, QStringLiteral("https://a.test/x"),
                                        QStringLiteral("https://a.test:443/x")) == h,
              "an implicit and an explicit default port are the same origin");
        // Two ways to be originless, pinned separately because ONE of them alone leaves the other's guard
        // untestable: a magnet has no host at all, while an ftp url has a perfectly good host and differs
        // only by being a scheme we do not speak. (The second case was added after mutation testing showed
        // the magnet assertion was being satisfied by the empty-host branch, leaving the scheme check inert.)
        CHECK(StreamHeaders::forPlayUrl(h, QStringLiteral("magnet:?xt=urn:btih:0"),
                                        QStringLiteral("magnet:?xt=urn:btih:0")).isEmpty(),
              "a hostless url can never match — not even itself");
        CHECK(StreamHeaders::forPlayUrl(h, QStringLiteral("ftp://cdn.a.test/movie.mp4"),
                                        QStringLiteral("ftp://cdn.a.test/movie.mp4")).isEmpty(),
              "a non-http scheme can never match either — not even itself");
        // …and a third: an http url whose host is missing. Without this the empty-host guard is unreachable
        // (the magnet above is refused by the scheme check first) and two malformed urls would compare equal.
        CHECK(StreamHeaders::forPlayUrl(h, QStringLiteral("http:///movie.mp4"),
                                        QStringLiteral("http:///movie.mp4")).isEmpty(),
              "an http url with no host is not an origin either");
        // There is deliberately NO "no headers in, none out" assertion here. It existed, and it was inert:
        // with `declared` empty, deleting the early return, inverting it or replacing it with anything at all
        // still yields an empty map, because every other branch returns either {} or the empty input. It
        // pinned nothing and read as coverage. The guard it was aimed at has been removed too — same
        // reasoning as the isString() guard in parseProxyHeaders.
    }

    // ---------------------------------- 14f. applyTo: the player assignment set, and its CLEARS (#43)
    {
        // Record what a player would be told. MpvWidget::play drives the identical call with an mpv-writing
        // sink, so what is asserted here is what reaches libmpv.
        struct Assign { QString property; QStringList values; };
        auto record = [](const StreamHeaders::Headers& h) {
            QVector<Assign> out;
            StreamHeaders::applyTo(h, [&out](const QString& p, const QStringList& v) { out.push_back({ p, v }); });
            return out;
        };
        // Every index below goes through this. CHECK is non-fatal by design — it counts a failure and carries
        // on — so a size assertion does NOT protect the indexing that follows it: the exact regression these
        // assertions exist to catch (applyTo stopping emitting one of the three) would run off the end of the
        // vector and crash the probe instead of printing STREMIO-FAIL. Out of range yields a default Assign,
        // which matches no expectation, so the failure stays a failure and stays readable.
        auto at = [](const QVector<Assign>& v, int i) { return (i >= 0 && i < v.size()) ? v.at(i) : Assign{}; };

        StreamHeaders::Headers h;
        h.insert(QStringLiteral("User-Agent"), QStringLiteral("EB-Probe/1.0"));
        h.insert(QStringLiteral("Referer"), QStringLiteral("https://embed.a.test/watch"));
        h.insert(QStringLiteral("X-Token"), QStringLiteral("abc,def"));
        const QVector<Assign> got = record(h);
        CHECK(got.size() == 3, "exactly three properties are written");
        CHECK(at(got, 0).property == QStringLiteral("user-agent")
                  && at(got, 0).values == QStringList{ QStringLiteral("EB-Probe/1.0") },
              "User-Agent goes to mpv's dedicated user-agent property");
        CHECK(at(got, 1).property == QStringLiteral("referrer")
                  && at(got, 1).values == QStringList{ QStringLiteral("https://embed.a.test/watch") },
              "Referer goes to mpv's dedicated referrer property");
        CHECK(at(got, 2).property == QStringLiteral("http-header-fields"),
              "everything else goes to the header field list");
        CHECK(at(got, 2).values == QStringList{ QStringLiteral("X-Token: abc,def") },
              "…as one 'Name: value' entry, comma in the value and all");
        // Lifting the two out of the field list is what stops mpv sending each of them twice.
        CHECK(!at(got, 2).values.join(QLatin1Char('|')).contains(QStringLiteral("User-Agent"))
                  && !at(got, 2).values.join(QLatin1Char('|')).contains(QStringLiteral("Referer")),
              "the two dedicated ones are NOT also in the field list");

        // THE clear-between-streams assertion. A stream that needs no headers must still produce all three
        // assignments, EMPTY — because "emit nothing when there is nothing" is exactly the bug where the
        // previous source's Referer stays live for the next stream, on a different host.
        const QVector<Assign> cleared = record({});
        CHECK(cleared.size() == 3, "a headerless stream still writes all three properties");
        CHECK(at(cleared, 0).property == QStringLiteral("user-agent") && at(cleared, 0).values.isEmpty(),
              "…user-agent cleared");
        CHECK(at(cleared, 1).property == QStringLiteral("referrer") && at(cleared, 1).values.isEmpty(),
              "…referrer cleared");
        CHECK(at(cleared, 2).property == QStringLiteral("http-header-fields") && at(cleared, 2).values.isEmpty(),
              "…and the field list cleared");
        // The order is fixed, so a caller can never write a stale value after a fresh one.
        CHECK(at(cleared, 0).property == at(got, 0).property && at(cleared, 1).property == at(got, 1).property
                  && at(cleared, 2).property == at(got, 2).property,
              "the same properties in the same order, headers or not");

        // A stream carrying ONLY a User-Agent still clears the referrer the previous one set.
        StreamHeaders::Headers uaOnly;
        uaOnly.insert(QStringLiteral("User-Agent"), QStringLiteral("EB-Probe/2.0"));
        const QVector<Assign> partial = record(uaOnly);
        CHECK(partial.size() == 3, "a partial header set still writes all three properties");
        // The property NAMES are checked alongside the emptiness, and that is not decoration: an out-of-range
        // at() yields a default Assign whose values are also empty, so "cleared" and "never emitted" would be
        // indistinguishable if only emptiness were asserted — the assertion would pass on exactly the
        // regression it exists to catch.
        CHECK(at(partial, 1).property == QStringLiteral("referrer") && at(partial, 1).values.isEmpty(),
              "a partial header set clears the referrer it does not use");
        CHECK(at(partial, 2).property == QStringLiteral("http-header-fields")
                  && at(partial, 2).values.isEmpty(),
              "…and the field list it does not use");
    }

    // ---------------------------------- 14g. logSummary is names-only, and the external-player call (#43)
    {
        StreamHeaders::Headers h;
        h.insert(QStringLiteral("Referer"), QStringLiteral("https://embed.a.test/watch?token=SECRET"));
        h.insert(QStringLiteral("Authorization"), QStringLiteral("Bearer SECRET"));
        const QString s = StreamHeaders::logSummary(h);
        // These values are the whole reason the rule exists: proxyHeaders routinely carry a signed-URL token
        // or a session cookie, and stream_debug.log is a file users paste into bug reports.
        CHECK(!s.contains(QStringLiteral("SECRET")), "no header VALUE reaches the log line");
        CHECK(!s.contains(QStringLiteral("Bearer")), "…not even the scheme half of one");
        CHECK(s.contains(QStringLiteral("Referer")) && s.contains(QStringLiteral("Authorization")),
              "the header NAMES do — that is the diagnosable part");
        CHECK(s.contains(QStringLiteral("2")), "and how many there are");
        CHECK(StreamHeaders::logSummary({}).isEmpty(), "nothing to say about a stream with no headers");

        // The external-player decision. Headers cannot follow a URL out to VLC/MPC-HC/an Android intent, so a
        // gated stream stays in the built-in player (which can satisfy the gate) and the user is told why.
        CHECK(StreamHeaders::externalRoute({}) == StreamHeaders::ExternalRoute::HandOff,
              "an ordinary stream still goes to the configured external player");
        CHECK(StreamHeaders::externalRoute(h) == StreamHeaders::ExternalRoute::FallBackToBuiltin,
              "a header-gated stream does not");
    }

    // ------------------------------------------------- 15. BingeStore
    // (the brief numbered this 10; 10-12 were already taken by handlesId/parseStreams/sort, so it lands later)
    {
        CHECK(BingeStore::seriesKeyFor(QStringLiteral("tt0903747:2:7")) == QStringLiteral("tt0903747"),
              "an episode id yields its series key");
        CHECK(BingeStore::seriesKeyFor(QStringLiteral("tt0133093")).isEmpty(),
              "a MOVIE id yields no key — binge memory is episodes-only");
        CHECK(BingeStore::seriesKeyFor(QString()).isEmpty(), "empty in, empty out");
        // SHAPE, not just arity — the same landmine MediaSegments::keyFor documents. Without the "tt" test
        // every TMDB-catalogued show keys as "tmdb" and one show's remembered release is handed to another.
        CHECK(BingeStore::seriesKeyFor(QStringLiteral("tmdb:tv:1396")).isEmpty(),
              "a 3-part id that is not an imdb episode is NOT a series key");
        CHECK(BingeStore::seriesKeyFor(QStringLiteral("tt1:2:7:extra")).isEmpty(),
              "and a longer id is not one either");

        QTemporaryDir tmp;
        CHECK(tmp.isValid(), "temp dir");
        const QString path = QDir(tmp.path()).filePath(QStringLiteral("binge.json"));

        BingeStore st(path);
        st.load();
        CHECK(st.lookup(QStringLiteral("tt1")).isEmpty(), "an empty store has nothing");
        CHECK(st.put(QStringLiteral("tt1"), QStringLiteral("torrentio|1080p")), "put succeeds");
        CHECK(st.lookup(QStringLiteral("tt1")) == QStringLiteral("torrentio|1080p"),
              "and the value is visible on the SAME instance, without a reload");
        CHECK(st.lookup(QStringLiteral("tt-other")).isEmpty(), "an unrelated series is still unknown");

        BingeStore re(path);
        re.load();
        CHECK(re.lookup(QStringLiteral("tt1")) == QStringLiteral("torrentio|1080p"), "it round-trips");
        re.put(QStringLiteral("tt1"), QStringLiteral("torrentio|4k"));
        CHECK(re.lookup(QStringLiteral("tt1")) == QStringLiteral("torrentio|4k"), "the newest choice wins");
        CHECK(!re.put(QString(), QStringLiteral("g")), "an empty key is refused");
        CHECK(!re.put(QStringLiteral("tt2"), QString()), "and so is an empty group");
        CHECK(re.lookup(QStringLiteral("tt2")).isEmpty(), "a refused put stores nothing");

        // load() must REPLACE what is in memory, not merge into it: a store whose file vanished must forget,
        // otherwise a cleared/rewritten file leaves the old choice quietly in effect for the rest of the run.
        CHECK(QFile::remove(path), "remove the backing file");
        re.load();
        CHECK(re.lookup(QStringLiteral("tt1")).isEmpty(), "load() clears what was there before");

        BingeStore missing(QDir(tmp.path()).filePath(QStringLiteral("nope.json")));
        missing.load();
        CHECK(missing.lookup(QStringLiteral("tt1")).isEmpty(), "a missing file loads as empty");

        // A hand-written file carrying values load() cannot use (a number, an empty string) alongside a good
        // one. The unusable keys sort FIRST — QJsonObject iterates by key — so a load() that BAILS on the
        // first value it cannot use, rather than skipping it, loses the entry that follows.
        //
        // That reachability is the only assertable part: a value skipped versus stored as "" is invisible
        // through lookup(), which cannot tell a stored "" from "never chosen". Asserting lookup("bEmpty")
        // is empty would pass either way, so it is deliberately not asserted here.
        const QString hand = QDir(tmp.path()).filePath(QStringLiteral("hand.json"));
        {
            QFile f(hand);
            CHECK(f.open(QIODevice::WriteOnly), "write a hand-made store");
            f.write("{\"aBad\":5,\"bEmpty\":\"\",\"ttA\":\"g1\"}");
        }
        BingeStore h(hand);
        h.load();
        CHECK(h.lookup(QStringLiteral("ttA")) == QStringLiteral("g1"),
              "a usable entry is read even when unusable ones precede it");
        CHECK(h.lookup(QStringLiteral("ttZ")).isEmpty(), "a key the file never had stays unknown");

        // A put whose WRITE fails must not leave memory ahead of disk. The store points into a directory that
        // does not exist, so QSaveFile cannot open and save() returns false; put() has to undo its insert.
        // Otherwise put() reports false while lookup() reports the new value for the rest of the run — and
        // the first later put that DOES succeed persists this one too, silently.
        BingeStore ro(QDir(tmp.path()).filePath(QStringLiteral("no-such-dir/binge.json")));
        ro.load();
        CHECK(!ro.put(QStringLiteral("ttA"), QStringLiteral("g1")), "a put that cannot be written fails");
        CHECK(ro.lookup(QStringLiteral("ttA")).isEmpty(), "…and leaves nothing behind in memory");
        // Same, but OVERWRITING an existing value: the rollback must restore the previous one, not erase it.
        BingeStore ov(QDir(tmp.path()).filePath(QStringLiteral("ov.json")));
        ov.load();
        CHECK(ov.put(QStringLiteral("ttA"), QStringLiteral("good")), "seed a value that did write");
        CHECK(QFile::remove(QDir(tmp.path()).filePath(QStringLiteral("ov.json"))), "drop its file");
        CHECK(QDir().mkpath(QDir(tmp.path()).filePath(QStringLiteral("ov.json"))),
              "…and put a DIRECTORY in its place so the next write cannot succeed");
        CHECK(!ov.put(QStringLiteral("ttA"), QStringLiteral("bad")), "the overwrite fails to write");
        CHECK(ov.lookup(QStringLiteral("ttA")) == QStringLiteral("good"),
              "and the previous choice is restored, not erased");
    }

    // ---------------------------------- 16. the download queue: a start that FAILS must not strand the rest
    // One download runs at a time and pump() is the only thing that picks the next, so every path that takes
    // a job out of the running has to call it. The ones that end a live job do (finishActive, cancel, retry);
    // the ones that refuse a job BEFORE there is a request are the ones to watch, because there is no reply
    // and therefore no finished() coming later to pump on their behalf.
    //
    // A restored queue is the case with teeth: the constructor pumps exactly once, so one job whose folder
    // cannot be written would otherwise hold every job behind it — with no error against them and nothing
    // running — until the user happened to touch the panel and pump it by hand.
    //
    // Driven through queue.json rather than enqueue(), because enqueue() pumps per job: a queue that is
    // Queued all the way down with only ONE pump against it is what a restart produces and nothing else does.
    //
    // No event loop runs here, so the job that does start sends nothing: get() queues the request and returns.
    {
        const QString downloads = AppPaths::dataDir() + QStringLiteral("/downloads");
        CHECK(QDir().mkpath(downloads), "the probe's own downloads folder");

        // An ordinary FILE standing where the failing jobs' folder would have to be. mkpath() cannot make a
        // directory under it and QFile cannot create the .part inside it — on every platform, and without
        // needing a permission the CI box may not be able to set.
        const QString blocked = downloads + QStringLiteral("/blocked");
        {
            QFile b(blocked);
            CHECK(b.open(QIODevice::WriteOnly), "a file standing where a folder is needed");
            b.write("x");
        }
        // Port 1 on loopback: nothing listens, and with no event loop nothing is even attempted.
        const QString url = QStringLiteral("http://127.0.0.1:1/x.bin");
        const QString bad1 = blocked + QStringLiteral("/first.bin");
        const QString bad2 = blocked + QStringLiteral("/second.bin");
        const QString good = downloads + QStringLiteral("/third.bin");

        QJsonArray restored;
        for (const QString& dest : { bad1, bad2, good })
            restored.append(QJsonObject{
                { QStringLiteral("id"), QFileInfo(dest).fileName() },
                { QStringLiteral("title"), QFileInfo(dest).fileName() },
                { QStringLiteral("url"), url },
                { QStringLiteral("dest"), dest },
                { QStringLiteral("kind"), QStringLiteral("video") },
                { QStringLiteral("state"), int(DownloadJob::Queued) } });
        {
            QFile q(downloads + QStringLiteral("/queue.json"));
            CHECK(q.open(QIODevice::WriteOnly), "write the queue a restart would find");
            q.write(QJsonDocument(restored).toJson(QJsonDocument::Compact));
        }

        DownloadManager dm;    // load() + one pump(): the restart
        const QVector<DownloadJob>& jobs = dm.jobs();
        CHECK(jobs.size() == 3, "all three queued jobs came back");
        if (jobs.size() == 3)
        {
            CHECK(jobs.at(0).dest == bad1, "the unwritable one is first, so it is the one that pump picks");
            CHECK(jobs.at(0).state == DownloadJob::Failed, "…and it fails rather than pretending to run");
            CHECK(!jobs.at(0).error.isEmpty(), "…saying so, since nothing else will");
            // THE assertion, twice over. Queued here would mean the queue stopped dead behind a job that is
            // not even running. Two failures in a row also pin that the retry is not a loop: each one is
            // Failed before the next pump, so pump keeps finding a DIFFERENT job and eventually none.
            CHECK(jobs.at(1).state == DownloadJob::Failed,
                  "the second unwritable job is reached too — a refused start does not strand the queue");
            // The control that makes those mean something: a job that CAN be written still starts. Without
            // it, a start() that refused everything would satisfy both assertions above while having broken
            // every download in the app.
            CHECK(jobs.at(2).state == DownloadJob::Active,
                  "…and the startable job behind them runs, so the failures are about the folder alone");
            // Close the job we just started. Not tidiness for its own sake: the manager holds its .part open
            // until the request ends, and on Windows an open handle keeps the probe's scratch directory from
            // being removed when the process exits — one leaked directory per hand-run of this probe.
            const QString startedId = jobs.at(2).id;   // a COPY: cancel() removes the job this points into
            dm.cancel(startedId);
        }
    }

    // ---------------------------------- 17. NetHeaderApply: what goes onto OUR OWN requests (#59)
    // #43 taught the PLAYER to send a stream's proxyHeaders. Three places in the app also fetch the stream
    // URL themselves — the playlist read, the download-for-keeps queue and the remote document/ROM pull —
    // and they share NetHeaderApply. What is assertable without a server is the REQUEST it builds: which
    // fields are on it, and which redirect policy it chose. The redirect DECISION needs a real 302 and is
    // proved out of tree against loopback servers, as #43's was.
    //
    // No event loop runs in this probe, so nothing is ever sent: QNetworkAccessManager::get() queues the
    // request and returns. The request object is ours and is inspected after the call.
    {
        QNetworkAccessManager nam;
        // Values chosen to be greppable: they are fabricated here, and every assertion below is about
        // whether they are PRESENT, never about printing them.
        StreamHeaders::Headers declaredHeaders;
        declaredHeaders.insert(QStringLiteral("Referer"), QStringLiteral("https://a.test/watch"));
        declaredHeaders.insert(QStringLiteral("X-Token"), QStringLiteral("PROBE-TOKEN"));
        const QString declared = QStringLiteral("https://cdn.a.test/movie.mp4");

        QNetworkRequest same{ QUrl(declared) };
        same.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("EB-Probe/1.0"));
        NetHeaderApply::get(&nam, same, declaredHeaders, declared);
        CHECK(same.rawHeader("Referer") == QByteArray("https://a.test/watch"),
              "the url they were declared for is sent the Referer");
        CHECK(same.rawHeader("X-Token") == QByteArray("PROBE-TOKEN"),
              "…and every other field the stream declared");
        CHECK(same.rawHeader("User-Agent") == QByteArray("EB-Probe/1.0"),
              "…and the caller's own User-Agent survives when the stream declares none");
        // The policy is not decoration: NoLessSafe would follow a cross-origin 302 and Qt would re-send
        // every raw header above to the new host. UserVerified holds the hop for the gate.
        CHECK(same.attribute(QNetworkRequest::RedirectPolicyAttribute).toInt()
                  == int(QNetworkRequest::UserVerifiedRedirectPolicy),
              "a request carrying headers does not follow a redirect unasked");

        // A stream that declares its own User-Agent REPLACES the caller's rather than being appended to it.
        // Two User-Agent fields on one request is not a cosmetic problem: it is a malformed request, and the
        // CDN this feature exists for is the one that will reject it.
        StreamHeaders::Headers uaHeaders;
        uaHeaders.insert(QStringLiteral("User-Agent"), QStringLiteral("EB-Probe-Gated/9"));
        QNetworkRequest ua{ QUrl(declared) };
        ua.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("EB-Probe/1.0"));
        NetHeaderApply::get(&nam, ua, uaHeaders, declared);
        CHECK(ua.rawHeader("User-Agent") == QByteArray("EB-Probe-Gated/9"),
              "a stream's own User-Agent wins over the caller's default");
        // Counted case-INSENSITIVELY. Qt 6.7+ stores field names folded to lower case, so the obvious
        // `rawHeaderList().count("User-Agent")` is 0 no matter what the code does — an assertion that fails
        // on correct code, and one that would have passed forever had the expectation been 0 instead of 1.
        // (It was written the obvious way first and failed on the first run, which is the only reason this
        // reads as a lesson rather than as a comment about nothing.)
        int uaFields = 0;
        for (const QByteArray& name : ua.rawHeaderList())
            if (name.compare("user-agent", Qt::CaseInsensitive) == 0) ++uaFields;
        CHECK(uaFields == 1, "…by replacing it, not by being a second User-Agent field");

        // THE hygiene assertion, on the request rather than on forPlayUrl's return value: a debrid/CDN
        // substitute is a different host and this request must leave without any of them.
        QNetworkRequest other{ QUrl(QStringLiteral("https://cdn.b.test/movie.mp4")) };
        other.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("EB-Probe/1.0"));
        NetHeaderApply::get(&nam, other, declaredHeaders, declared);
        CHECK(other.rawHeader("Referer").isEmpty(), "a DIFFERENT host is sent no Referer");
        CHECK(other.rawHeader("X-Token").isEmpty(), "…and none of the other declared fields");
        CHECK(other.rawHeader("User-Agent") == QByteArray("EB-Probe/1.0"),
              "…while OUR default User-Agent still goes, as it always has");
        // …and it goes back to the ordinary policy. Asserted because the two are one decision: leaving a
        // header-free request on UserVerified would hang it (nothing gates it), which is how a
        // policy-always-UserVerified mutation would show up in the field rather than here.
        CHECK(other.attribute(QNetworkRequest::RedirectPolicyAttribute).toInt()
                  == int(QNetworkRequest::NoLessSafeRedirectPolicy),
              "a request carrying nothing keeps the redirect policy it always had");

        QNetworkRequest bare{ QUrl(declared) };
        NetHeaderApply::get(&nam, bare, {}, declared);
        CHECK(bare.attribute(QNetworkRequest::RedirectPolicyAttribute).toInt()
                  == int(QNetworkRequest::NoLessSafeRedirectPolicy),
              "…and so does a stream that declared no headers at all");
    }

    // ---------------------------------- 18. the download queue's half of it (#59)
    // A download is the plain-HTTP fetch of the very URL the player would have played, so it needs the same
    // headers — and it is the one consumer that PERSISTS, which is where the interesting rule is: the values
    // must not be written to disk. queue.json is an ordinary file in the app folder, and a proxyHeader is
    // routinely a signed-URL token; a persisted one is a secret at rest that outlives its own download.
    //
    // Runs against the probe's isolated data dir (issue #42), so this touches no real queue.
    {
        const QString downloads = AppPaths::dataDir() + QStringLiteral("/downloads");

        // Section 16 leaves its own queue.json behind — jobs pointing at a folder it deliberately made
        // unwritable. A DownloadManager constructed here would load() them, so every jobs() count below
        // would be off by however many that section queued. Per-process isolation (#42) says nothing about
        // what one section leaves the next; that is what an inter-section reset is for (#48).
        QFile::remove(downloads + QStringLiteral("/queue.json"));
        StreamHeaders::Headers declaredHeaders;
        declaredHeaders.insert(QStringLiteral("Referer"), QStringLiteral("https://gated.test/watch"));
        declaredHeaders.insert(QStringLiteral("X-Token"), QStringLiteral("PROBE-TOKEN"));
        // Port 1 on loopback: nothing listens, and with no event loop nothing is even attempted.
        const QString url = QStringLiteral("http://127.0.0.1:1/gated.bin");
        QString jobId;
        {
            DownloadManager dm;
            DownloadJob j;
            j.title = QStringLiteral("Gated");
            j.url = url;
            j.dest = downloads + QStringLiteral("/gated.bin");
            j.kind = QStringLiteral("video");
            j.requestHeaders = declaredHeaders;
            dm.enqueue(j);
            CHECK(dm.jobs().size() == 1, "the gated job is queued");
            CHECK(dm.jobs().at(0).requestHeaders == declaredHeaders,
                  "…carrying its OWN headers, per job — not on the manager, where they would outlive it");
            CHECK(dm.jobs().at(0).headerGated, "…and flagged as needing them");
            jobId = dm.jobs().at(0).id;
        }
        // What actually landed on disk.
        QFile qf(downloads + QStringLiteral("/queue.json"));
        CHECK(qf.open(QIODevice::ReadOnly), "the queue was written");
        const QByteArray onDisk = qf.readAll();
        qf.close();
        CHECK(!onDisk.contains("PROBE-TOKEN"), "no header VALUE is persisted");
        CHECK(!onDisk.contains("gated.test"), "…not the Referer's either");
        CHECK(!onDisk.contains("X-Token") && !onDisk.contains("Referer"),
              "…and not even the NAMES, which would say which gate a source uses");
        // The positive half. Written as the JSON the flag actually serialises to, NOT as the bare substring
        // "gated": the fixture's url and dest are both …/gated.bin, so `onDisk.contains("gated")` is
        // satisfied by the FILENAME whether or not save() writes the flag at all — delete
        // {"gated", j.headerGated} from save() and the old spelling still passed. The property was pinned
        // only by the restore-side check further down, and this line named something it did not test.
        CHECK(onDisk.contains("\"gated\":true"),
              "the value-free flag IS written, as a flag and not merely as part of the file name");
        CHECK(onDisk.contains("127.0.0.1"), "…and the url is — the two of them are the whole record");

        {
            DownloadManager restored;    // a restart: load() from the queue.json above
            CHECK(restored.jobs().size() == 1, "the job comes back");
            CHECK(restored.jobs().at(0).headerGated, "…still flagged");
            CHECK(restored.jobs().at(0).requestHeaders.isEmpty(),
                  "…and with no headers, because they were never written");
            // The point of the flag. Without it this retry re-requests the source BARE and takes a 403 the
            // user reads as the download being broken — the exact failure #59 exists to remove.
            restored.retry(jobId);
            CHECK(restored.jobs().at(0).state == DownloadJob::Failed,
                  "retrying it does not silently re-request it without the headers");
            CHECK(restored.jobs().at(0).error.contains(QStringLiteral("headers")),
                  "…and says that is why, rather than reporting an HTTP status");

            // The control that makes the two assertions above mean something: an UNGATED job retried through
            // the identical path must actually start. Without this, a start() that refused everything would
            // satisfy both, and the refusal would read as coverage while having broken all downloads.
            DownloadJob plain;
            plain.title = QStringLiteral("Plain");
            plain.url = url;
            plain.dest = downloads + QStringLiteral("/plain.bin");
            plain.kind = QStringLiteral("video");
            restored.enqueue(plain);
            const QVector<DownloadJob>& jobs = restored.jobs();
            int plainIdx = -1;
            for (int i = 0; i < jobs.size(); ++i)
                if (jobs.at(i).dest.endsWith(QStringLiteral("plain.bin"))) plainIdx = i;
            CHECK(plainIdx >= 0, "the ungated job is queued too");
            CHECK(plainIdx >= 0 && !jobs.at(plainIdx).headerGated, "…and is not flagged as gated");
            CHECK(plainIdx >= 0 && jobs.at(plainIdx).state == DownloadJob::Active,
                  "…and starts, so the refusal above is about the missing headers and nothing else");
        }
    }

    // ---------------------------------- 19. the gate on the wire, and what a refusal TELLS the user (#59)
    // Everything above builds requests and inspects them. The redirect DECISION cannot be reached that way —
    // it needs a server that actually answers 302 — and the consequence of the decision was where the bug
    // was: NetHeaderApply aborts a cross-origin hop, DownloadManager sees OperationCanceledError, and
    // finishActive recorded Qt's string for it, "Operation canceled". A message that says the USER stopped
    // this download, on a source that plays perfectly, with nothing in any log. Undiagnosable from the
    // field, and not theoretical: these hosts 302 cross-origin, which is exactly why playback works — mpv
    // follows the hop.
    //
    // So this section runs the real DownloadManager against two loopback origins on ephemeral ports, over a
    // real event loop, and asserts on the reason the user is given.
    {
        const QString downloads = AppPaths::dataDir() + QStringLiteral("/downloads");
        // Section 17's queue.json is still on disk and its jobs point at a port where nothing listens. A
        // DownloadManager constructed here would load() them and spend its single slot on them.
        QFile::remove(downloads + QStringLiteral("/queue.json"));

        // The two `is listening` lines below are HARNESS PRECONDITIONS, not assertions about the code under
        // test — no mutation of the app can kill them, and they are here so that a box which cannot bind
        // loopback reports that instead of letting every assertion in this section pass vacuously.
        //
        // Origin B: the host a cross-origin hop would land on. It answers, so that "B was not contacted" is
        // a fact about the gate and not about B being unreachable.
        Loopback b;
        b.answer = [](const QByteArray&) {
            return QByteArray("HTTP/1.1 200 OK\r\nContent-Length: 12\r\nConnection: close\r\n\r\nB-SHOULD-NOT");
        };
        CHECK(b.start(), "origin B is listening");

        // Origin A: the origin the headers are declared for. Same host as B, DIFFERENT port — which is a
        // different origin, because StreamHeaders::origin() makes the port explicit precisely so that this
        // comparison cannot be fooled by a shared hostname.
        const QByteArray aBody = "A-CONTENT-OK";
        Loopback a;
        CHECK(a.start(), "origin A is listening");
        a.answer = [&a, &b, &aBody](const QByteArray& path) -> QByteArray {
            if (path.startsWith("/cross"))
                return QByteArray("HTTP/1.1 302 Found\r\nLocation: ") + b.url(QStringLiteral("/moved.bin")).toUtf8()
                     + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            if (path.startsWith("/same"))
                return QByteArray("HTTP/1.1 302 Found\r\nLocation: ") + a.url(QStringLiteral("/final.bin")).toUtf8()
                     + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            return QByteArray("HTTP/1.1 200 OK\r\nContent-Length: ") + QByteArray::number(aBody.size())
                 + "\r\nConnection: close\r\n\r\n" + aBody;
        };

        StreamHeaders::Headers wireHeaders;
        wireHeaders.insert(QStringLiteral("Referer"), a.url(QStringLiteral("/watch")));
        wireHeaders.insert(QStringLiteral("X-Token"), QStringLiteral("PROBE-WIRE-TOKEN"));

        DownloadManager dm;

        // --- the refusal.
        DownloadJob cross;
        cross.title = QStringLiteral("Cross");
        cross.url   = a.url(QStringLiteral("/cross.bin"));
        cross.dest  = downloads + QStringLiteral("/cross.bin");
        cross.kind  = QStringLiteral("video");
        cross.requestHeaders = wireHeaders;
        dm.enqueue(cross);
        CHECK(settle(dm, QStringLiteral("cross.bin")), "the refused download reaches a terminal state");
        const DownloadJob* cj = jobFor(dm, QStringLiteral("cross.bin"));
        CHECK(cj && cj->state == DownloadJob::Failed, "…as a failure");
        CHECK(b.hits == 0,
              "origin B is never contacted, so there is nothing of A's for it to have been handed");
        // THE assertion this section exists for. Qt's errorString for the abort is "Operation canceled"; the
        // job must not report that, because nobody cancelled anything.
        CHECK(cj && !cj->error.contains(QStringLiteral("cancel"), Qt::CaseInsensitive),
              "…and the reason is not the generic cancellation string, which blames the user");
        CHECK(cj && cj->error.contains(QStringLiteral("different site"))
                 && cj->error.contains(QStringLiteral("headers")),
              "…it names the hop and the headers, so this is diagnosable from a bug report");
        // (A `!cj->error.isEmpty()` line was here and was deleted before it ever shipped: any string that
        // satisfies the check above is non-empty, so it could not fail while that one passed. No mutation
        // could kill one without the other — an assertion that only ever agrees with its neighbour is a
        // count in the kill matrix and nothing else.)

        // --- the control that makes all of the above mean something. Same manager, same headers, same
        // server: a SAME-origin redirect must be followed and the file must land. Without this, a
        // NetHeaderApply that refused every redirect — or a DownloadManager that failed every job — would
        // satisfy every assertion above, and a totally broken download path would read as coverage.
        DownloadJob same;
        same.title = QStringLiteral("Same");
        same.url   = a.url(QStringLiteral("/same.bin"));
        same.dest  = downloads + QStringLiteral("/same.bin");
        same.kind  = QStringLiteral("video");
        same.requestHeaders = wireHeaders;
        dm.enqueue(same);
        CHECK(settle(dm, QStringLiteral("same.bin")), "the same-origin download reaches a terminal state");
        const DownloadJob* sj = jobFor(dm, QStringLiteral("same.bin"));
        CHECK(sj && sj->state == DownloadJob::Done,
              "a SAME-origin redirect is followed — the refusal above is about the origin CHANGE, not about "
              "redirects and not about carrying headers at all");
        QFile got(downloads + QStringLiteral("/same.bin"));
        CHECK(got.open(QIODevice::ReadOnly) && got.readAll() == aBody,
              "…and the bytes on disk are the ones the redirect TARGET served");
        // …and the headers really did leave the process, which is what makes b.hits == 0 a statement about
        // the gate rather than about a request that carried nothing in the first place. Names only.
        CHECK(a.fieldNames.contains("\nx-token\n") && a.fieldNames.contains("\nreferer\n"),
              "origin A received the declared fields on the wire, across the hop it was allowed");
    }

    // ------------------------------------------------- 20. subtitlesPath (#79)
    {
        // Bare route: no extras -> "/subtitles/{type}/{id}.json", built like catalogPath's bare form. Pins the
        // "/subtitles/" literal — a copy of catalogPath that forgot to change "/catalog/" fails right here.
        CHECK(subtitlesPath(QStringLiteral("movie"), QStringLiteral("tt0133093"))
                  == QStringLiteral("/subtitles/movie/tt0133093.json"),
              "no extras -> bare /subtitles route");

        // An EPISODE id carries colons ("tt:season:episode"); they are percent-encoded, exactly as the stream
        // and meta routes encode an id (segEnc == QUrl::toPercentEncoding). Expected value hand-computed: ':'
        // is not unreserved, so each becomes %3A. This pins the id going through the encoder, not raw.
        CHECK(subtitlesPath(QStringLiteral("series"), QStringLiteral("tt0944947:1:1"))
                  == QStringLiteral("/subtitles/series/tt0944947%3A1%3A1.json"),
              "an episode id's colons are percent-encoded, like the stream route");

        // The OSDb extras. videoHash sorts before videoSize (…H < …S), and the values are hex/digits — all
        // unreserved, so nothing in them encodes. INSERTED in reverse of the sorted order to prove the output
        // is key-sorted (the result cache keys on the string), not merely echoing insertion order.
        QMap<QString, QString> osdb;
        osdb.insert(QStringLiteral("videoSize"), QStringLiteral("734003200"));
        osdb.insert(QStringLiteral("videoHash"), QStringLiteral("8e245d9679d31e12"));
        CHECK(subtitlesPath(QStringLiteral("movie"), QStringLiteral("tt0133093"), osdb)
                  == QStringLiteral("/subtitles/movie/tt0133093/videoHash=8e245d9679d31e12&videoSize=734003200.json"),
              "videoHash/videoSize are emitted in sorted key order in a path segment");

        // An empty value carries no information and must be DROPPED, not emitted as a bare "key=" — the same
        // rule catalogPath holds. With videoSize empty, only videoHash survives.
        QMap<QString, QString> oneEmpty;
        oneEmpty.insert(QStringLiteral("videoHash"), QStringLiteral("abc"));
        oneEmpty.insert(QStringLiteral("videoSize"), QString());
        CHECK(subtitlesPath(QStringLiteral("movie"), QStringLiteral("tt1"), oneEmpty)
                  == QStringLiteral("/subtitles/movie/tt1/videoHash=abc.json"),
              "an empty extra value is dropped, not emitted as a bare key=");

        // A KEY and a VALUE that both need escaping — the value must not be able to forge extra params, and the
        // key path must go through the encoder too (catalogPath's own wild-key assertion, mirrored). Expected
        // hand-computed: ' '->%20, '&'->%26, '='->%3D.
        QMap<QString, QString> nasty;
        nasty.insert(QStringLiteral("v h"), QStringLiteral("a b&c=d"));
        CHECK(subtitlesPath(QStringLiteral("movie"), QStringLiteral("tt1"), nasty)
                  == QStringLiteral("/subtitles/movie/tt1/v%20h=a%20b%26c%3Dd.json"),
              "the extra key AND value are percent-encoded, so a value can't forge a second param");
    }

    // ------------------------------------------------- 21. parseSubtitlesResponse (#79)
    {
        // The response shape a Stremio subtitles add-on returns: {"subtitles":[{id,url,lang}...]}. Rows keep
        // their input order (there is no ranking rule for subtitles), and every field is carried verbatim.
        const QByteArray body = R"({"subtitles":[
          { "id": "sub-1", "url": "https://subs.test/a.srt", "lang": "eng" },
          { "id": "sub-2", "url": "https://subs.test/b.vtt", "lang": "spa" },
          { "id": "nourl", "lang": "fre" },
          { "url": "https://subs.test/c.srt" }
        ]})";
        const QVector<SubtitleAddonResult> v = parseSubtitlesResponse(body);
        CHECK(v.size() == 3, "a row with no url is dropped; the three with a url are kept");
        CHECK(v[0].url == QStringLiteral("https://subs.test/a.srt") && v[0].lang == QStringLiteral("eng")
                  && v[0].id == QStringLiteral("sub-1"),
              "url, lang and id are extracted from the first row");
        CHECK(v[1].url == QStringLiteral("https://subs.test/b.vtt") && v[1].lang == QStringLiteral("spa")
                  && v[1].id == QStringLiteral("sub-2"),
              "…and the second, in input order");
        // The url-less row ("nourl") must be the one gone: the survivor at index 2 is the LAST row, which had a
        // url but no id/lang. If the drop were on the wrong field, this row (or its position) would differ.
        CHECK(v[2].url == QStringLiteral("https://subs.test/c.srt") && v[2].id.isEmpty()
                  && v[2].lang.isEmpty(),
              "a row with a url but no id/lang is KEPT, with those fields empty — only a missing url drops a row");

        // Malformed / hostile bodies -> empty, never a throw (parseManifest's discipline). Each shape defeats a
        // naive parse differently, so each is pinned on its own.
        CHECK(parseSubtitlesResponse(QByteArray("not json at all")).isEmpty(), "garbage -> empty");
        CHECK(parseSubtitlesResponse(QByteArray("{}")).isEmpty(), "no subtitles key -> empty");
        CHECK(parseSubtitlesResponse(QByteArray(R"({"subtitles":"nope"})")).isEmpty(),
              "a non-array subtitles value -> empty, not a crash");
        CHECK(parseSubtitlesResponse(QByteArray(R"({"subtitles":[3,"x",null]})")).isEmpty(),
              "rows that are not objects contribute nothing");
    }

    // ------------------------------------------------- 22. routeProviders over the SUBTITLES resource (#79)
    {
        // The fan-out routes on "subtitles" through the same router the stream path uses. Pin that it selects
        // the subtitle-declaring add-ons of this type/id-space, and that an unclaimed id falls back to all of
        // them (never zero — the same never-cost-a-result rule).
        auto mf = [](const QString& res, const QStringList& types, const QStringList& prefixes) {
            Manifest m;
            m.resources = QStringList{res};
            m.types = types;
            m.idPrefixes = prefixes;
            return m;
        };
        const QStringList movie{QStringLiteral("movie")};
        QVector<Manifest> ms;
        ms << mf(QStringLiteral("subtitles"), movie, {QStringLiteral("tt")})       // 0: subtitles, imdb
           << mf(QStringLiteral("stream"),    movie, {QStringLiteral("tt")})       // 1: wrong resource
           << mf(QStringLiteral("subtitles"), {QStringLiteral("series")}, {QStringLiteral("tt")}) // 2: wrong type
           << mf(QStringLiteral("subtitles"), movie, {QStringLiteral("kitsu:")});  // 3: subtitles, anime only

        bool fell = true;
        QVector<int> r = routeProviders(ms, QStringLiteral("subtitles"), QStringLiteral("movie"),
                                        QStringLiteral("tt0133093"), &fell);
        CHECK(r == (QVector<int>{0}),
              "only the subtitles add-on of this type that claims the id space — the stream/wrong-type/wrong-"
              "prefix ones stay out");
        CHECK(!fell, "…and that is a real match, not the fallback");

        // An id no subtitles add-on claims still reaches every subtitles OFFERER (0 and 3), and says so. The
        // stream add-on (1) and the wrong-type one (2) are NOT resurrected — the fallback widens the id filter
        // only, exactly as it does for streams.
        r = routeProviders(ms, QStringLiteral("subtitles"), QStringLiteral("movie"),
                           QStringLiteral("weird:99"), &fell);
        CHECK(r == (QVector<int>{0, 3}), "an unclaimed id falls back to asking every subtitles provider");
        CHECK(fell, "…and flags the fallback so the log can explain it");
        CHECK(!r.contains(1) && !r.contains(2),
              "the fallback does not resurrect the wrong resource or the wrong type");
    }

    if (failures) { std::fprintf(stderr, "STREMIO-FAIL %d check(s) failed\n", failures); return 1; }
    std::printf("STREMIO-OK\n");
    return 0;
}
