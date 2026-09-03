// Headless integration probe for the SERIAL protocol resources — `chapters` and `pages` (issue #188).
//
// What this exists to hold. Manga used to work for exactly one provider, because the client contained that
// provider's chapter-page resolver: a source could not exist without a change to the app. The two resources
// below replace it, and the whole value of the replacement is in properties nothing else can assert:
//
//   1. the manifest DECLARES them, and an addon that has not declared one is never asked — no JS
//      invocation, no HTTP request. The never-ask rule is checked by watching a loopback server's request
//      log, not by trusting the return value: "answered empty" and "was never asked" look identical from
//      the caller and are completely different facts;
//   2. the ORDERING is the client's, once, for every source: a natural sort on `number`, stable, so "10"
//      comes after "9.5", un-numbered entries sort last, and two chapters of the same number keep the order
//      the source listed them in;
//   3. a page's `headers` survive parsing with the proxyHeaders hygiene rules applied (a Referer arrives,
//      a Range or a CRLF-carrying field does not) — this is what makes a gated image CDN readable at all;
//   4. an OUTDATED addon (bundled addons are copy-if-absent, so an upgraded install keeps its old script
//      forever) still lists chapters through the older /detail path, and says so once;
//   5. both transports answer identically — a local Duktape addon via getChapters/getPages, and a remote
//      HTTP addon via /chapters and /pages.
//
// Hermetic: an isolated EB_ADDONS_ROOT for the JsLocal fixtures and two loopback HTTP servers for the
// remote ones. No live service is contacted; nothing here names a real source.
//
// Prints CHAPTERS-OK; any failure prints CHAPTERS-FAIL <what> (line) and exits non-zero.
#include "AddonManager.h"
#include "AddonModels.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <cstdio>
#include <memory>

static int failures = 0;
#define CHECK(c, w) do { if (!(c)) { std::fprintf(stderr, "CHAPTERS-FAIL %s (line %d)\n", w, __LINE__); ++failures; } } while (0)

template <typename Pred>
static void pumpUntil(Pred done, int ms)
{
    QElapsedTimer t; t.start();
    while (t.elapsed() < ms && !done()) QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}

// ---------------------------------------------------------------------------------------------------
// The fixture chapter list, deliberately NOT in reading order and deliberately containing the three cases
// a naive sort gets wrong: a decimal chapter ("9.5" against "10", which a string compare inverts), a
// duplicate number (which only a STABLE sort keeps in source order), and an entry with no number at all
// (which must sort last rather than as chapter zero).
static QByteArray chaptersBody(bool hasMore)
{
    QByteArray b = QByteArrayLiteral(R"({"chapters":[
      {"id":"c-10",  "number":"10"},
      {"id":"c-9-5", "number":"9.5", "volume":"1"},
      {"id":"c-2",   "number":"2", "volume":"1", "title":"Two", "language":"en",
       "group":"Fan Scans", "published":"2019-01-02", "pageCount":18},
      {"id":"c-one", "number":"",  "title":"A oneshot"},
      {"id":"c-9",   "number":"9"},
      {"id":"c-10b", "number":"10"},
      {"id":"c-x",   "number":"Extra"},
      {"number":"7"}
    ],"hasMore":)");
    b += (hasMore ? "true" : "false");
    b += "}";
    return b;
}

// One chapter's pages. Page 1 carries headers: a lowercase `referer` that must canonicalise, a benign
// custom field that must survive, a `Range` that must be REFUSED (the reader issues its own), and a field
// whose value carries CRLF, which must be refused rather than repaired. Page 3 is a bare url STRING —
// the shortest thing a static addon can write.
static QByteArray pagesBody()
{
    return QByteArrayLiteral(R"({"pages":[
      {"url":"https://img.example.net/ch10/01.jpg","width":1114,"height":1600,
       "headers":{"referer":"https://reader.example.net/","X-Client":"fixture",
                  "Range":"bytes=0-1","X-Split":"a\r\nB: c"}},
      {"url":"https://img.example.net/ch10/02.jpg"},
      "https://img.example.net/ch10/03.jpg"
    ]})");
}

// A remote media-source addon over loopback. `declare` is the whole experiment: the SAME server, the same
// data, differing only in whether its manifest lists the two resources.
struct Provider
{
    QTcpServer srv;
    bool declare = true;
    QStringList requested;

    QByteArray manifest() const
    {
        QByteArray m = QByteArrayLiteral(R"({
          "id": ")") + id().toUtf8() + QByteArrayLiteral(R"(", "name": "Serial Fixture", "version": "1.0.0",
          "type": "media-source",
          "catalogs": [ { "id": "serials", "name": "Serials", "type": "manga" } ])");
        if (declare)
            m += QByteArrayLiteral(R"(,
          "resources": [ { "name": "chapters", "types": ["manga"] },
                         { "name": "pages", "types": ["manga"] } ])");
        m += "}";
        return m;
    }

    QString id() const
    { return declare ? QStringLiteral("net.example.serials") : QStringLiteral("net.example.legacyserials"); }

    // The OLD path: a container's children as ordinary catalog items. This is what an addon that predates
    // the resources answers with, and what the client must keep reading when nothing is declared.
    static QByteArray detailBody()
    {
        return QByteArrayLiteral(R"({"title":"Chapters","items":[
          {"id":"c-2","title":"Vol. 1 · Ch. 2","type":"manga_chapter","expandable":false},
          {"id":"c-9","title":"Vol. 1 · Ch. 9","type":"manga_chapter","expandable":false}
        ]})");
    }

    bool start()
    {
        if (!srv.listen(QHostAddress::LocalHost, 0)) return false;
        QObject::connect(&srv, &QTcpServer::newConnection, &srv, [this] {
            QTcpSocket* c = srv.nextPendingConnection();
            if (!c) return;
            auto buf = std::make_shared<QByteArray>();
            QObject::connect(c, &QTcpSocket::readyRead, c, [this, c, buf] {
                buf->append(c->readAll());
                const int end = buf->indexOf("\r\n\r\n");
                if (end < 0) return;
                const QByteArray reqLine = buf->left(end).split('\n').value(0).trimmed();
                const QString path = QString::fromLatin1(reqLine.split(' ').value(1));
                requested << path;
                QByteArray body;
                if (path.endsWith(QStringLiteral("/manifest.json")))     body = manifest();
                else if (path.startsWith(QStringLiteral("/chapters/")))  body = chaptersBody(true);
                else if (path.startsWith(QStringLiteral("/pages/")))     body = pagesBody();
                else if (path.startsWith(QStringLiteral("/detail/")))    body = detailBody();
                else if (path.startsWith(QStringLiteral("/catalog/")))
                    body = QByteArrayLiteral(R"({"title":"Serials","items":[
                      {"id":"the-long-walk","title":"The Long Walk","type":"manga","expandable":true}]})");
                else body = QByteArrayLiteral("{}");
                QByteArray resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: ";
                resp += QByteArray::number(body.size());
                resp += "\r\nConnection: close\r\n\r\n";
                resp += body;
                c->write(resp);
                c->flush();
                c->disconnectFromHost();
            });
            QObject::connect(c, &QTcpSocket::disconnected, c, &QObject::deleteLater);
        });
        return true;
    }

    int hits(const QString& prefix) const
    {
        int n = 0;
        for (const QString& p : requested) if (p.startsWith(prefix)) ++n;
        return n;
    }

    QString base() const { return QStringLiteral("http://127.0.0.1:%1").arg(srv.serverPort()); }
};

// ---------------------------------------------------------------------------------------------------
// A JsLocal fixture: the same two resources, answered by a Duktape script. `implement` off writes an addon
// that DECLARES the resources and then does not define the functions — a real mistake an author can make,
// and one that must produce an empty answer rather than a crash.
static bool writeJsFixture(const QString& root, const QString& id, bool implement)
{
    const QString dir = root + QStringLiteral("/") + id;
    if (!QDir().mkpath(dir)) return false;
    const QByteArray manifest =
        "{\n"
        "  \"id\": \"" + id.toUtf8() + "\",\n"
        "  \"name\": \"Serial JS Fixture\",\n"
        "  \"version\": \"1.0.0\",\n"
        "  \"type\": \"media-source\",\n"
        "  \"entry\": \"main.js\",\n"
        "  \"permissions\": [],\n"
        "  \"catalogs\": [ { \"id\": \"serials\", \"name\": \"Serials\", \"type\": \"manga\" } ],\n"
        "  \"resources\": [ { \"name\": \"chapters\", \"types\": [\"manga\"] },\n"
        "                   { \"name\": \"pages\", \"types\": [\"manga\"] } ]\n"
        "}\n";
    static const char* JS =
        "function getCatalog(a){return JSON.stringify({title:'Serials',items:[]});}\n"
        // Same out-of-order list as the remote fixture: the ordering rule is the CLIENT'S, so both
        // transports must come back in the same order without either addon sorting anything.
        "function getChapters(argJson){\n"
        "  var a=JSON.parse(argJson);\n"
        "  if(a.type!=='manga') return JSON.stringify({chapters:[]});\n"
        "  return JSON.stringify({chapters:[\n"
        "    {id:'c-10',number:'10'},{id:'c-9-5',number:'9.5'},{id:'c-2',number:'2'},\n"
        "    {id:'c-one',number:''},{id:'c-9',number:'9'},{id:'c-10b',number:'10'},\n"
        "    {id:'c-x',number:'Extra'}],hasMore:false});\n"
        "}\n"
        "function getPages(argJson){\n"
        "  var a=JSON.parse(argJson);\n"
        "  if(a.id!=='c-10') return JSON.stringify({pages:[]});\n"
        "  return JSON.stringify({pages:[\n"
        "    {url:'https://img.example.net/js/01.jpg',headers:{referer:'https://reader.example.net/'}},\n"
        "    'https://img.example.net/js/02.jpg']});\n"
        "}\n";
    static const char* JS_BARE = "function getCatalog(a){return JSON.stringify({title:'Serials',items:[]});}\n";

    QFile mf(dir + QStringLiteral("/manifest.json"));
    if (!mf.open(QIODevice::WriteOnly)) return false;
    mf.write(manifest);
    mf.close();
    QFile jf(dir + QStringLiteral("/main.js"));
    if (!jf.open(QIODevice::WriteOnly)) return false;
    jf.write(implement ? JS : JS_BARE);
    jf.close();
    return true;
}

static QStringList idsOf(const QVector<AddonChapter>& v)
{
    QStringList out;
    for (const AddonChapter& c : v) out << c.id;
    return out;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ---- 1. The manifest declaration, and the family-type rule --------------------------------------
    {
        bool ok = false;
        const AddonManifest m = AddonManifest::fromJson(QByteArrayLiteral(R"({
            "id":"x","type":"media-source",
            "resources":[ {"name":"chapters","types":["manga"]},
                          {"name":"pages","types":["manga","novel"]},
                          {"name":"anything"},
                          {"types":["manga"]} ]})"), &ok);
        CHECK(ok, "a manifest carrying resources still parses");
        CHECK(m.resources.size() == 3, "three named resources parse and the nameless one is dropped");
        CHECK(m.declares(QStringLiteral("chapters"), QStringLiteral("manga")),
              "chapters is declared for the type it lists");
        CHECK(!m.declares(QStringLiteral("chapters"), QStringLiteral("comic")),
              "chapters is NOT declared for a type it does not list");
        CHECK(m.declares(QStringLiteral("pages"), QStringLiteral("novel")),
              "a resource declared for two types answers for both");
        CHECK(!m.declares(QStringLiteral("stream"), QStringLiteral("manga")),
              "a resource nobody declared is not declared");
        CHECK(m.declares(QStringLiteral("anything"), QStringLiteral("whatever")),
              "a resource with no types answers for every type");

        // The family rule: both routes are keyed by the SERIES type, so one declaration covers the leaves.
        CHECK(AddonManager::familyType(QStringLiteral("manga_chapter")) == QStringLiteral("manga"),
              "a chapter leaf's family is the series type");
        CHECK(AddonManager::familyType(QStringLiteral("manga")) == QStringLiteral("manga"),
              "a family type is its own family");
        CHECK(AddonManager::familyType(QStringLiteral("novel_chapter")) == QStringLiteral("novel"),
              "the family rule is a shape, not a list of known types");
    }

    // ---- 2. The ordering rule, as a pure decision ---------------------------------------------------
    {
        QVector<AddonChapter> v;
        auto add = [&v](const char* id, const char* n) { AddonChapter c; c.id = id; c.number = n; v << c; };
        add("c-10", "10"); add("c-9-5", "9.5"); add("c-2", "2"); add("c-one", "");
        add("c-9", "9"); add("c-10b", "10"); add("c-x", "Extra");
        AddonChapters::sortNaturally(v);
        const QStringList got = idsOf(v);
        const QStringList want = { QStringLiteral("c-2"), QStringLiteral("c-9"), QStringLiteral("c-9-5"),
                                   QStringLiteral("c-10"), QStringLiteral("c-10b"), QStringLiteral("c-x"),
                                   QStringLiteral("c-one") };
        CHECK(got == want, "chapters sort naturally: 9.5 before 10, ties stable, un-numbered last");
        if (got != want)
            std::fprintf(stderr, "  order was: %s\n", got.join(QLatin1Char(',')).toUtf8().constData());

        CHECK(AddonChapters::numberLess(QStringLiteral("9.5"), QStringLiteral("10")),
              "\"9.5\" sorts before \"10\" (the case a string compare inverts)");
        CHECK(!AddonChapters::numberLess(QStringLiteral("10"), QStringLiteral("9.5")),
              "...and not the other way round");
        CHECK(AddonChapters::numberLess(QStringLiteral("007"), QStringLiteral("8")),
              "leading zeros do not change a number's value");
        CHECK(AddonChapters::numberLess(QStringLiteral("12"), QStringLiteral("12a")),
              "a number is a prefix of the same number with a suffix");
        CHECK(AddonChapters::numberLess(QStringLiteral("99999999999999999999"),
                                        QStringLiteral("100000000000000000000")),
              "a number past every integer type still orders by magnitude");
    }

    // ---- 3. Payload parsing, including the header hygiene -------------------------------------------
    {
        const AddonChapterList l = AddonChapterList::fromJson(chaptersBody(true));
        CHECK(l.hasMore, "hasMore is read from the chapters payload");
        CHECK(l.chapters.size() == 7, "an entry with no id is dropped (it could never be opened)");
        const AddonChapter* two = nullptr;
        for (const AddonChapter& c : l.chapters) if (c.id == QLatin1String("c-2")) two = &c;
        CHECK(two != nullptr, "the fully populated chapter parsed");
        if (two)
        {
            CHECK(two->volume == QLatin1String("1"), "volume");
            CHECK(two->title == QLatin1String("Two"), "title");
            CHECK(two->language == QLatin1String("en"), "language");
            CHECK(two->group == QLatin1String("Fan Scans"), "group");
            CHECK(two->published == QLatin1String("2019-01-02"), "published");
            CHECK(two->pageCount == 18, "pageCount");
        }
        for (const AddonChapter& c : l.chapters)
            if (c.id == QLatin1String("c-10")) CHECK(c.pageCount == -1, "an absent pageCount reads as -1");

        const AddonPageList p = AddonPageList::fromJson(pagesBody());
        CHECK(p.pages.size() == 3, "three pages, one of them written as a bare url string");
        if (p.pages.size() == 3)
        {
            CHECK(p.pages[0].width == 1114 && p.pages[0].height == 1600, "a page's own dimensions");
            CHECK(p.pages[2].url == QLatin1String("https://img.example.net/ch10/03.jpg"),
                  "a bare url string is a page");
            const StreamHeaders::Headers h = p.pages[0].headers;
            CHECK(h.contains(QStringLiteral("Referer")), "a lowercase referer canonicalises");
            CHECK(h.contains(QStringLiteral("X-Client")), "a benign custom header survives");
            CHECK(!h.contains(QStringLiteral("Range")), "Range is refused: the reader issues its own");
            CHECK(!h.contains(QStringLiteral("X-Split")), "a value carrying CRLF is refused, not repaired");
            CHECK(p.pages[1].headers.isEmpty(), "a page that declares no headers has none");
        }
    }

    // ---- fixtures ------------------------------------------------------------------------------------
    const QString root = QDir::tempPath() + QStringLiteral("/eb-serial-fixture-")
                       + QString::number(QCoreApplication::applicationPid());
    QDir(root).removeRecursively();
    const QString jsId = QStringLiteral("probe.serial.js");
    const QString jsBareId = QStringLiteral("probe.serial.declared-not-implemented");
    if (!writeJsFixture(root, jsId, true) || !writeJsFixture(root, jsBareId, false))
    { std::fprintf(stderr, "CHAPTERS-FAIL could not write the JS fixtures\n"); return 2; }
    qputenv("EB_ADDONS_ROOT", root.toUtf8());

    Provider modern, legacy;
    legacy.declare = false;
    if (!modern.start() || !legacy.start())
    { std::fprintf(stderr, "CHAPTERS-FAIL loopback servers did not listen\n"); return 1; }

    AddonManager mgr;
    for (Provider* p : { &modern, &legacy })
    {
        bool added = false, addOk = false;
        auto conn = QObject::connect(&mgr, &AddonManager::remoteSourceResult, &mgr,
                                     [&](bool ok, const QString&) { added = true; addOk = ok; });
        mgr.addRemoteSource(p->base());
        pumpUntil([&] { return added; }, 20000);
        QObject::disconnect(conn);
        CHECK(addOk, "the fixture provider was accepted as a remote media source");
    }
    LoadedAddon* remote = mgr.sourceById(modern.id());
    LoadedAddon* old    = mgr.sourceById(legacy.id());
    LoadedAddon* local  = mgr.sourceById(jsId);
    LoadedAddon* bare   = mgr.sourceById(jsBareId);
    CHECK(remote && old && local && bare, "all four fixtures are loaded sources");
    if (!remote || !old || !local || !bare) { std::fprintf(stderr, "CHAPTERS-FAIL fixture setup\n"); return 1; }

    // ---- 4. The capability gate ---------------------------------------------------------------------
    CHECK(mgr.supportsChapters(remote, QStringLiteral("manga")), "a declaring remote addon supports chapters");
    CHECK(mgr.supportsPages(remote, QStringLiteral("manga_chapter")),
          "...and supports pages when asked with the LEAF type (the family rule)");
    CHECK(!mgr.supportsChapters(remote, QStringLiteral("comic")), "...but not for an undeclared family");
    CHECK(!mgr.supportsChapters(old, QStringLiteral("manga")), "a legacy addon declares no chapters");
    CHECK(!mgr.supportsPages(old, QStringLiteral("manga_chapter")), "a legacy addon declares no pages");
    CHECK(!mgr.supportsChapters(nullptr, QStringLiteral("manga")), "no source supports nothing");

    // ---- 5. THE NEVER-ASK RULE ----------------------------------------------------------------------
    // Asserted on the SERVER'S request log, not on the answer. An addon that answered empty and an addon
    // that was never asked are indistinguishable from the callback, and only one of them is the rule.
    {
        const int before = legacy.requested.size();
        bool done = false;
        QVector<AddonChapter> got;
        mgr.requestChapters(old, QStringLiteral("manga"), QStringLiteral("the-long-walk"), 1,
                            [&](const QVector<AddonChapter>& c, bool) { got = c; done = true; });
        pumpUntil([&] { return done; }, 5000);
        CHECK(done, "requestChapters answers an undeclaring addon immediately");
        CHECK(got.isEmpty(), "...with an empty list");

        bool pdone = false;
        QVector<AddonPage> pgot;
        mgr.requestPages(old, QStringLiteral("manga_chapter"), QStringLiteral("c-10"),
                         [&](const QVector<AddonPage>& p) { pgot = p; pdone = true; });
        pumpUntil([&] { return pdone; }, 5000);
        CHECK(pdone && pgot.isEmpty(), "requestPages answers an undeclaring addon with an empty list");

        // Give a stray request time to arrive before concluding none was made.
        pumpUntil([] { return false; }, 400);
        CHECK(legacy.requested.size() == before,
              "NOT ONE request left the process for an addon that declared neither resource");
        CHECK(legacy.hits(QStringLiteral("/chapters/")) == 0, "no /chapters request was made");
        CHECK(legacy.hits(QStringLiteral("/pages/")) == 0, "no /pages request was made");
    }

    // ---- 6. The remote transport --------------------------------------------------------------------
    {
        bool done = false, more = false;
        QVector<AddonChapter> got;
        mgr.requestChapters(remote, QStringLiteral("manga"), QStringLiteral("the-long-walk"), 1,
                            [&](const QVector<AddonChapter>& c, bool m) { got = c; more = m; done = true; });
        pumpUntil([&] { return done; }, 20000);
        CHECK(done, "the remote chapters request completed");
        CHECK(more, "hasMore reaches the caller");
        CHECK(modern.hits(QStringLiteral("/chapters/manga/the-long-walk")) == 1,
              "the request went to /chapters/{family}/{seriesId}.json");
        const QStringList want = { QStringLiteral("c-2"), QStringLiteral("c-9"), QStringLiteral("c-9-5"),
                                   QStringLiteral("c-10"), QStringLiteral("c-10b"), QStringLiteral("c-x"),
                                   QStringLiteral("c-one") };
        CHECK(idsOf(got) == want, "a remote chapter list arrives in the client's natural order");

        bool pdone = false;
        QVector<AddonPage> pages;
        mgr.requestPages(remote, QStringLiteral("manga_chapter"), QStringLiteral("c-10"),
                         [&](const QVector<AddonPage>& p) { pages = p; pdone = true; });
        pumpUntil([&] { return pdone; }, 20000);
        CHECK(pdone && pages.size() == 3, "the remote pages request returned the chapter's pages");
        CHECK(modern.hits(QStringLiteral("/pages/manga/c-10")) == 1,
              "the pages request is keyed by the FAMILY type, not the leaf's");
        if (pages.size() == 3)
            CHECK(pages[0].headers.contains(QStringLiteral("Referer")),
                  "a page's request headers survive the transport");
    }

    // ---- 7. The chapters resource IS the browse list -------------------------------------------------
    // The one-seam rule: a declaring container answers /detail's callers through catalogReady, so the
    // browse grid, the reading run and the volume crossing are reached by the path they always were.
    {
        MediaItem series;
        series.id = QStringLiteral("the-long-walk");
        series.type = QStringLiteral("manga");
        series.expandable = true;
        int gotId = -1;
        MediaCatalog cat;
        auto conn = QObject::connect(&mgr, &AddonManager::catalogReady, &mgr,
                                     [&](int id, const MediaCatalog& c) { gotId = id; cat = c; });
        const int req = mgr.requestDetail(remote, series, 1);
        pumpUntil([&] { return gotId == req; }, 20000);
        QObject::disconnect(conn);
        CHECK(gotId == req, "requestDetail on a declaring container answered on catalogReady");
        CHECK(cat.items.size() == 7, "every chapter became a browse row");
        CHECK(cat.hasMore, "hasMore drives infinite scroll as it always did");
        if (cat.items.size() == 7)
        {
            CHECK(cat.items[0].id == QLatin1String("c-2"), "the rows are in the client's chapter order");
            CHECK(cat.items[0].type == QLatin1String("manga_chapter"),
                  "a chapter row is typed as its family's leaf");
            CHECK(!cat.items[0].expandable, "a chapter is a leaf, not a container");
            // The label has to carry the number in a form ChapterOrder::chapterNumber reads back, or the
            // reading run that drives the volume crossing is built in the wrong direction.
            CHECK(cat.items[0].title.contains(QStringLiteral("Ch. 2")), "the row names its chapter number");
            CHECK(cat.items[0].title.startsWith(QStringLiteral("Vol. 1")), "...and its volume, when there is one");
            CHECK(cat.items[0].subtitle.contains(QStringLiteral("Fan Scans")), "the group reaches the row");
            CHECK(cat.items[6].title.contains(QStringLiteral("A oneshot")),
                  "an un-numbered chapter is named by its title, last");
        }
    }

    // ---- 8. THE OUTDATED-ADDON FALLBACK --------------------------------------------------------------
    // Bundled addons are copy-if-absent, so an upgraded install keeps its old script. Its containers must
    // still list their chapters — through the /detail path they always used.
    {
        MediaItem series;
        series.id = QStringLiteral("the-long-walk");
        series.type = QStringLiteral("manga");
        series.expandable = true;
        int gotId = -1;
        MediaCatalog cat;
        auto conn = QObject::connect(&mgr, &AddonManager::catalogReady, &mgr,
                                     [&](int id, const MediaCatalog& c) { gotId = id; cat = c; });
        const int req = mgr.requestDetail(old, series, 1);
        pumpUntil([&] { return gotId == req; }, 20000);
        QObject::disconnect(conn);
        CHECK(gotId == req, "an outdated addon's container still answers");
        CHECK(legacy.hits(QStringLiteral("/detail/manga/the-long-walk")) == 1,
              "...through the OLDER /detail route, which is the fallback");
        CHECK(legacy.hits(QStringLiteral("/chapters/")) == 0,
              "...and still never through /chapters");
        CHECK(cat.items.size() == 2, "the old path's chapter children arrive unchanged");
    }

    // ---- 9. The local (Duktape) transport ------------------------------------------------------------
    {
        bool more = true;
        const QVector<AddonChapter> got =
            mgr.chaptersSync(local, QStringLiteral("manga"), QStringLiteral("the-long-walk"), 1, &more);
        const QStringList want = { QStringLiteral("c-2"), QStringLiteral("c-9"), QStringLiteral("c-9-5"),
                                   QStringLiteral("c-10"), QStringLiteral("c-10b"), QStringLiteral("c-x"),
                                   QStringLiteral("c-one") };
        CHECK(idsOf(got) == want, "a local JS addon's chapters arrive in the SAME order as a remote one's");
        CHECK(!more, "...with its own hasMore");

        const QVector<AddonPage> pages = mgr.pagesSync(local, QStringLiteral("manga_chapter"),
                                                       QStringLiteral("c-10"));
        CHECK(pages.size() == 2, "a local JS addon supplies pages");
        if (pages.size() == 2)
            CHECK(pages[0].headers.contains(QStringLiteral("Referer")),
                  "a local addon's page headers go through the same hygiene");

        bool pdone = false;
        QVector<AddonPage> apages;
        mgr.requestPages(local, QStringLiteral("manga_chapter"), QStringLiteral("c-10"),
                         [&](const QVector<AddonPage>& p) { apages = p; pdone = true; });
        pumpUntil([&] { return pdone; }, 20000);
        CHECK(pdone && apages.size() == 2, "the async local path answers identically to the sync one");

        // Declared but not implemented: an empty answer, not a crash and not a hang.
        CHECK(mgr.supportsPages(bare, QStringLiteral("manga_chapter")),
              "an addon that declares a resource it never wrote is still asked");
        CHECK(mgr.pagesSync(bare, QStringLiteral("manga_chapter"), QStringLiteral("c-10")).isEmpty(),
              "...and answers empty rather than failing");
        CHECK(mgr.chaptersSync(bare, QStringLiteral("manga"), QStringLiteral("s")).isEmpty(),
              "...for both resources");
    }

    // The remote fixtures are persisted device-locally by addRemoteSource; take them back out so a later
    // run of any probe does not start with two dead loopback URLs in its source list.
    mgr.removeRemoteSource(modern.base());
    mgr.removeRemoteSource(legacy.base());
    QDir(root).removeRecursively();

    if (failures) { std::fprintf(stderr, "CHAPTERS-FAIL %d check(s) failed\n", failures); return 1; }
    std::printf("CHAPTERS-OK\n");
    return 0;
}
