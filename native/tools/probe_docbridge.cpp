// Headless integration probe for the DOC-BRIDGE PICKER (issue #207) — the wiring, not the rule.
//
// probe_resolver already pins CatalogMatch::formatMatchesRequest as a pure decision. What it cannot see is
// whether the picker ASKS it: the reported failure was not a rule that answered wrongly, it was a question
// nobody put. So this drives a REAL AddonManager against a REAL file provider (a loopback HTTP server
// speaking the remote media-source contract) and asserts what comes back out of resolveDocumentByQuery.
//
// Four searches, all for the same book, all against the same provider:
//
//   1. BOTH an EPUB release and an M4B one, EPUB listed first exactly as the live provider listed it. The
//      audiobook shelf must come back with the M4B. This is the reported failure, reproduced and refused.
//   2. ONLY the EPUB. The audiobook shelf must come back with NOTHING and say "reached, zero results" —
//      the "couldn't find it" the callers already say plainly — rather than hand over an ebook.
//   3. ONLY a FORMAT-SILENT release (most releases name no format at all). It must be CHOSEN. A gate that
//      demanded a positive audio signal would pass 1 and 2 and break the feature for everybody.
//   4. The same EPUB, asked for from the BOOK shelf. It must be chosen — the rule is about the shelf that
//      asked, not about EPUBs being bad.
//
// Prints DOCBRIDGE-OK; any failure prints DOCBRIDGE-FAIL <what> (line) and exits non-zero.
#include "AddonManager.h"
#include "AddonModels.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <cstdio>
#include <memory>

static int failures = 0;
#define CHECK(c, w) do { if (!(c)) { std::fprintf(stderr, "DOCBRIDGE-FAIL %s (line %d)\n", w, __LINE__); ++failures; } } while (0)

// The release names are the fixture. The EPUB one is the exact string the live log decoded to.
static const char* const kEpubTitle   = "The Poppy War by R. F. Kuang EPUB";
static const char* const kM4bTitle    = "The Poppy War - R. F. Kuang [M4B]";
static const char* const kSilentTitle = "The Poppy War (Unabridged) - R. F. Kuang";
static const char* const kOtherTitle  = "The Dragon Republic - R. F. Kuang M4B";   // right author, wrong book

// The url each release resolves to. The EPUB's is the shape the live one had: a debrid "zip the whole
// release" endpoint, no filename and no mime — which is why nothing downstream could tell what it was.
static const char* const kEpubUrl   = "https://store-044.example.net/zip/feb39992aa";
static const char* const kM4bUrl    = "https://store-044.example.net/f/poppy-war.m4b";
static const char* const kSilentUrl = "https://store-044.example.net/dl/9f2c1a";

struct Provider
{
    QTcpServer srv;
    QString scenario = QStringLiteral("both");   // which release list the catalog answers with
    QStringList requested;

    QByteArray manifest() const
    {
        return QByteArrayLiteral(R"({
          "id": "com.example.fixtureprovider", "name": "Fixture Provider", "version": "1.0.0",
          "type": "media-source",
          "catalogs": [
            { "id": "book:audiobooks", "name": "Audiobooks", "type": "audiobook" },
            { "id": "book:books",      "name": "Books",      "type": "book" }
          ]
        })");
    }

    static QByteArray item(const char* id, const char* title, const char* type, const char* url)
    {
        return QByteArray("{\"id\":\"") + id + "\",\"title\":\"" + title + "\",\"type\":\"" + type
             + "\",\"url\":\"" + url + "\"}";
    }

    QByteArray catalogBody(bool audiobookShelf) const
    {
        const char* t = audiobookShelf ? "audiobook" : "book";
        QByteArrayList items;
        if (scenario == QStringLiteral("both"))
        {
            // EPUB FIRST, as the provider listed it: "first" is not "right", and neither is "first that
            // passes the title gate".
            items << item("book:ab:epub", kEpubTitle, t, kEpubUrl)
                  << item("book:ab:m4b",  kM4bTitle,  t, kM4bUrl)
                  << item("book:ab:other", kOtherTitle, t, "https://store-044.example.net/f/dragon.m4b");
        }
        else if (scenario == QStringLiteral("epubonly"))
        {
            items << item("book:ab:epub", kEpubTitle, t, kEpubUrl)
                  << item("book:ab:other", kOtherTitle, t, "https://store-044.example.net/f/dragon.m4b");
        }
        else // "silent"
        {
            items << item("book:ab:silent", kSilentTitle, t, kSilentUrl);
        }
        return QByteArray("{\"title\":\"Fixture\",\"items\":[") + items.join(',') + "]}";
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
                if (path.endsWith(QStringLiteral("/manifest.json"))) body = manifest();
                else if (path.startsWith(QStringLiteral("/catalog/")))
                    // segEnc percent-encodes the catalog id, so the audiobook shelf arrives as
                    // "/catalog/book%3Aaudiobooks/search=….json".
                    body = catalogBody(path.contains(QStringLiteral("audiobooks")));
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
    QString base() const { return QStringLiteral("http://127.0.0.1:%1").arg(srv.serverPort()); }
};

template <typename Pred>
static void pumpUntil(Pred done, int ms)
{
    QElapsedTimer t; t.start();
    while (t.elapsed() < ms && !done()) QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}

// One search, run to completion. Reports everything the callback carries, because "no url" has three
// different meanings on this path (error / still caching / reached-and-empty) and only one of them is the
// refusal being asserted.
struct Answer { bool done = false; QString url, mime, err; bool noMatches = false; };

static Answer search(AddonManager& mgr, const QString& query, const QString& wantTitle, const QString& catType)
{
    auto a = std::make_shared<Answer>();
    mgr.resolveDocumentByQuery(query, wantTitle, catType,
        [a](const QString& url, const QString& mime, const QString& err, bool noMatches) {
            a->url = url; a->mime = mime; a->err = err; a->noMatches = noMatches; a->done = true;
        });
    pumpUntil([a] { return a->done; }, 20000);
    return *a;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    Provider prov;
    if (!prov.start()) { std::fprintf(stderr, "DOCBRIDGE-FAIL loopback server did not listen\n"); return 1; }

    AddonManager mgr;
    bool added = false, addOk = false;
    QObject::connect(&mgr, &AddonManager::remoteSourceResult, &mgr, [&](bool ok, const QString&) {
        added = true; addOk = ok;
    });
    mgr.addRemoteSource(prov.base());
    pumpUntil([&] { return added; }, 20000);
    CHECK(addOk, "the fixture provider was accepted as a remote media source");
    CHECK(mgr.sources().size() >= 1, "the fixture provider is in the source list");
    if (failures) { std::fprintf(stderr, "DOCBRIDGE-FAIL provider setup\n"); return 1; }

    const QString query = QStringLiteral("The Poppy War R. F. Kuang");
    const QString want  = QStringLiteral("The Poppy War");

    // 1. THE REPORTED FAILURE, reproduced. The EPUB is listed first and passes the title gate word for word;
    //    the audiobook shelf must still come back with the M4B.
    {
        prov.scenario = QStringLiteral("both");
        const Answer a = search(mgr, query, want, QStringLiteral("audiobook"));
        CHECK(a.url == QLatin1String(kM4bUrl), "an audiobook search picks the M4B release, not the EPUB one");
        CHECK(a.url != QLatin1String(kEpubUrl), "the EPUB release cannot win an audiobook search");
    }

    // 2. ONLY the EPUB. Refusing is the answer: the callers turn this into "No copies of … were found."
    {
        prov.scenario = QStringLiteral("epubonly");
        const Answer a = search(mgr, query, want, QStringLiteral("audiobook"));
        CHECK(a.url.isEmpty(), "an audiobook search with only an EPUB on offer resolves nothing");
        CHECK(a.err.isEmpty(), "…and reports no provider error — the provider answered fine");
        CHECK(a.noMatches, "…and says it reached the provider and found no match, not 'still caching'");
    }

    // 3. A FORMAT-SILENT release. The one that would break the feature if silence read as a mismatch.
    {
        prov.scenario = QStringLiteral("silent");
        const Answer a = search(mgr, query, want, QStringLiteral("audiobook"));
        CHECK(a.url == QLatin1String(kSilentUrl), "a release that names no format at all is still opened");
    }

    // 4. The BOOK shelf, same EPUB. The gate is about what was asked for, not about a format being bad.
    {
        prov.scenario = QStringLiteral("epubonly");
        const Answer a = search(mgr, query, want, QStringLiteral("book"));
        CHECK(a.url == QLatin1String(kEpubUrl), "a book search still gets the EPUB release");
    }

    if (failures == 0) { std::puts("DOCBRIDGE-OK"); return 0; }
    std::fprintf(stderr, "DOCBRIDGE: %d failure(s)\n", failures);
    return 1;
}
