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

// #214: a MULTI-FILE release. It carries no url of its own -- a release is not a file -- so the bridge has
// to expand it, and the two parts are listed 10-before-2 with a cover and an .nfo between them, because
// "whatever the source returned first" is exactly what used to be played.
static const char* const kPartsTitle  = "A Tale of Two Cities - Charles Dickens [MP3]";
static const char* const kPartsWant   = "A Tale of Two Cities";
static const char* const kPartsQuery  = "A Tale of Two Cities Charles Dickens";
static const char* const kPart2Url    = "https://store-044.example.net/f/two-cities-2.mp3";
static const char* const kPart10Url   = "https://store-044.example.net/f/two-cities-10.mp3";
static const char* const kWholeUrl    = "https://store-044.example.net/dl/whole-release";

// #216: what the PROVIDER says when it has something to say about a part that would not link. A cause
// somebody established, as opposed to the one the app used to assert on its behalf.
static const char* const kProviderNotice = "Caching on TestBox: 42% of 1.2 GB";

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

    // A row with NO url: what a release (and what one of its files) actually looks like. Only a /stream
    // call turns one into a link, which is the whole point -- see #214.
    static QByteArray linkless(const char* id, const char* title, const char* type)
    {
        return QByteArray("{\"id\":\"") + id + "\",\"title\":\"" + title + "\",\"type\":\"" + type + "\"}";
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
        else if (scenario.startsWith(QStringLiteral("parts")))
        {
            // #214: one release, no url. It has to be expanded before anything can be played.
            items << linkless("book:ab:parts", kPartsTitle, t);
        }
        else // "silent"
        {
            items << item("book:ab:silent", kSilentTitle, t, kSilentUrl);
        }
        return QByteArray("{\"title\":\"Fixture\",\"items\":[") + items.join(',') + "]}";
    }

    // #214: what the release EXPANDS into. Three shapes, because the client owes a different answer to each:
    // a real multi-part book, a release with nothing playable in it, and a single-file recording.
    QByteArray detailBody() const
    {
        QByteArrayList items;
        if (scenario == QStringLiteral("partsnoaudio"))
        {
            // An ebook release that won an audiobook search -- #207's report, one level deeper. Refusing
            // with a sentence is the answer; staging a player over an EPUB is what used to happen.
            items << linkless("book:ab:parts~epub", "A Tale of Two Cities.epub", "audiobook")
                  << linkless("book:ab:parts~cover", "cover.jpg", "audiobook");
        }
        else if (scenario == QStringLiteral("partssingle"))
        {
            items << linkless("book:ab:parts~m4b", "A Tale of Two Cities.m4b", "audiobook")
                  << linkless("book:ab:parts~cover", "cover.jpg", "audiobook");
        }
        else
        {
            // 10 BEFORE 2, with a cover and an .nfo in between. Anything that reads this list without
            // filtering and natural-ordering it starts the listener at part ten -- the reported defect.
            items << linkless("book:ab:parts~p10", "10 - part.mp3", "audiobook")
                  << linkless("book:ab:parts~cover", "cover.jpg", "audiobook")
                  << linkless("book:ab:parts~p2", "2 - part.mp3", "audiobook")
                  << linkless("book:ab:parts~nfo", "info.nfo", "audiobook");
        }
        return QByteArray("{\"title\":\"A Tale of Two Cities\",\"items\":[") + items.join(',') + "]}";
    }

    // The link for whatever was asked for. A PART id is answered with that part's url; the release id
    // itself still answers with the one whole-release link, which is what a single-file recording takes.
    //
    // #216: two scenarios answer a PART with no link at all -- the shape the engine sends when a source
    // resolves to nothing ("streams":[], optionally with a notice). The release expands exactly as it does
    // in "parts", so what is being pinned is what the app SAYS when the listing worked and the link did
    // not: the enumeration is proof the release is there, and a sentence blaming the debrid cache is not
    // something anybody established.
    QByteArray streamBody(const QString& path) const
    {
        const bool part = path.contains(QStringLiteral("p10")) || path.contains(QStringLiteral("p2"));
        if (part && scenario == QStringLiteral("partsnolink"))
            return QByteArrayLiteral("{\"streams\":[]}");
        if (part && scenario == QStringLiteral("partsnolinknotice"))
            return QByteArray("{\"streams\":[],\"notice\":\"") + kProviderNotice + "\"}";

        const char* url = kWholeUrl;
        if (path.contains(QStringLiteral("p10"))) url = kPart10Url;
        else if (path.contains(QStringLiteral("p2"))) url = kPart2Url;
        else if (path.contains(QStringLiteral("m4b"))) url = kM4bUrl;
        return QByteArray("{\"url\":\"") + url + "\",\"mime\":\"audio/mpeg\"}";
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
                else if (path.startsWith(QStringLiteral("/detail/")))  body = detailBody();
                else if (path.startsWith(QStringLiteral("/stream/")))  body = streamBody(path);
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
struct Answer
{
    bool done = false; QString url, mime, err; bool noMatches = false;
    QVector<RemoteAudiobook::Part> parts;   // #214: the release's audio files, filtered and ordered
    bool noAudio = false;                   // #214: a release was chosen and there is nothing to play in it
    bool noPartLink = false;                // #216: it WAS expanded, and part one produced no link
    QString notice;                         // #216: what the provider said about that attempt, if anything
};

static Answer search(AddonManager& mgr, const QString& query, const QString& wantTitle, const QString& catType)
{
    auto a = std::make_shared<Answer>();
    mgr.resolveDocumentByQuery(query, wantTitle, catType, [a](const AddonManager::DocFind& found) {
        a->url = found.url; a->mime = found.mime; a->err = found.providerError;
        a->noMatches = found.noMatches; a->parts = found.parts; a->noAudio = found.noAudio;
        a->noPartLink = found.noPartLink; a->notice = found.notice;
        a->done = true;
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

    // ---- #214: a release is many files, and the book starts at part one --------------------------
    //
    // These four are the wiring no unit test of the ordering rule can reach. The defect was never that the
    // rule was wrong -- it is that NOBODY ASKED. Deleting the expansion leaves every pure assertion in
    // probe_remotebook green and puts the app straight back to playing chapter ten.

    // 5. THE REPORTED FAILURE, reproduced end to end. The release lists "10 - part.mp3" FIRST, with a cover
    //    and an .nfo among its files. What comes back must be two parts, in order, and the url must be the
    //    link for part TWO -- which is part ONE of the book.
    {
        prov.scenario = QStringLiteral("parts");
        const Answer a = search(mgr, QString::fromLatin1(kPartsQuery), QString::fromLatin1(kPartsWant),
                                QStringLiteral("audiobook"));
        CHECK(a.parts.size() == 2, "a multi-file release comes back as its two audio parts");
        if (a.parts.size() == 2)
        {
            CHECK(a.parts.at(0).fileName == QStringLiteral("2 - part.mp3"),
                  "the book starts at part 2 of 2/10 -- NOT at part 10, which is what a lexicographic order gives");
            CHECK(a.parts.at(1).fileName == QStringLiteral("10 - part.mp3"), "…and part 10 comes second");
            CHECK(!a.parts.at(0).id.isEmpty() && !a.parts.at(1).id.isEmpty(),
                  "every part carries the source id its link will be minted from");
        }
        for (const RemoteAudiobook::Part& part : a.parts)
        {
            CHECK(part.fileName != QStringLiteral("cover.jpg"), "the cover image is not one of the book's parts");
            CHECK(part.fileName != QStringLiteral("info.nfo"), "the .nfo is not one of the book's parts");
        }
        CHECK(a.url == QLatin1String(kPart2Url),
              "the link handed back is the FIRST PART'S, minted for it -- not the release's and not part ten's");
        CHECK(!a.noAudio, "…and the release is not reported as having no audio");
    }

    // 6. A release whose files are an EPUB and a cover. #207's report, one level deeper: there is a copy,
    //    and there is nothing in it to play. It must say so rather than hand back a url.
    {
        prov.scenario = QStringLiteral("partsnoaudio");
        const Answer a = search(mgr, QString::fromLatin1(kPartsQuery), QString::fromLatin1(kPartsWant),
                                QStringLiteral("audiobook"));
        CHECK(a.noAudio, "a release with no audio files in it is reported as exactly that");
        CHECK(a.url.isEmpty(), "…and hands back no url, so nothing can stage a player over it");
        CHECK(a.parts.isEmpty(), "…and no parts");
        CHECK(!a.noMatches, "…and it is NOT 'no copies were found' — a copy was found, and it is an ebook");
    }

    // 7. A SINGLE-FILE recording. The no-regression case: one .m4b with its chapters inside expands into
    //    one part, which the client reads as "this is a file", so it takes the untouched single-link path.
    {
        prov.scenario = QStringLiteral("partssingle");
        const Answer a = search(mgr, QString::fromLatin1(kPartsQuery), QString::fromLatin1(kPartsWant),
                                QStringLiteral("audiobook"));
        CHECK(a.parts.isEmpty(), "a single-file release yields no part list — nothing downstream builds a queue");
        CHECK(a.url == QLatin1String(kWholeUrl), "…and it resolves through the release's own /stream, exactly as before");
        CHECK(!a.noAudio, "…and is certainly not a release with no audio in it");
    }

    // 8. THE EXPANSION IS ASKED FOR, and asked of the release the picker chose. Read off the provider's own
    //    request log rather than inferred from the answer: "nobody asked" is the defect, and an answer that
    //    happens to look right is exactly how it hid for so long.
    {
        prov.requested.clear();
        prov.scenario = QStringLiteral("parts");
        (void)search(mgr, QString::fromLatin1(kPartsQuery), QString::fromLatin1(kPartsWant),
                     QStringLiteral("audiobook"));
        int detailAsks = 0, partStreamAsks = 0;
        for (const QString& r : std::as_const(prov.requested))
        {
            if (r.startsWith(QStringLiteral("/detail/"))) ++detailAsks;
            if (r.startsWith(QStringLiteral("/stream/")) && r.contains(QStringLiteral("p2"))) ++partStreamAsks;
        }
        CHECK(detailAsks == 1, "the bridge asked the source to expand the release exactly once");
        CHECK(partStreamAsks == 1, "…and minted exactly ONE part's link — part one's, not all of them");
        for (const QString& r : std::as_const(prov.requested))
            CHECK(!r.contains(QStringLiteral("p10")),
                  "part ten's link is NOT minted up front: a signed link is spent long before a listener reaches it");
    }

    // ---- #216: the release was listed, and part one still would not link -------------------------
    //
    // The reported failure was not that this happens — it will always be possible — but what the app said
    // when it did: "isn't ready yet, the source may still be caching", asserted whatever the cause, about a
    // release whose 57 files it had just finished listing. An enumeration that succeeded is proof the
    // release resolves, so that sentence was not merely unhelpful, it was contradicted by the line above it
    // in the same log. What is asserted here is the DISTINCTION the callers need to stop saying it.

    // 10. NO LINK, AND THE PROVIDER SAID NOTHING. The answer has to be distinguishable from "no copies were
    //     found" and from a provider that could not be reached, or there is no way to say anything else.
    {
        prov.scenario = QStringLiteral("partsnolink");
        const Answer a = search(mgr, QString::fromLatin1(kPartsQuery), QString::fromLatin1(kPartsWant),
                                QStringLiteral("audiobook"));
        CHECK(a.url.isEmpty(), "a part that would not link hands back no url");
        CHECK(a.noPartLink, "…and says the release WAS expanded and its first part produced no link");
        CHECK(!a.noMatches, "…and it is NOT 'no copies were found' — a copy was found, and listed");
        CHECK(a.err.isEmpty(), "…and not a provider error either: the provider answered every request");
        CHECK(!a.noAudio, "…and the release is not one with no audio in it: it had parts");
        CHECK(a.notice.isEmpty(), "…and invents no notice when the provider offered none");
    }

    // 11. NO LINK, AND THE PROVIDER DID SAY WHY. Its words are what the caller shows; the app's guess is
    //     what #216 is about, and a real notice is the thing that outranks it.
    {
        prov.scenario = QStringLiteral("partsnolinknotice");
        const Answer a = search(mgr, QString::fromLatin1(kPartsQuery), QString::fromLatin1(kPartsWant),
                                QStringLiteral("audiobook"));
        CHECK(a.url.isEmpty(), "a part that would not link hands back no url, notice or no notice");
        CHECK(a.noPartLink, "…and still reports which of the five empty-url answers this is");
        CHECK(a.notice == QLatin1String(kProviderNotice),
              "…and carries the PROVIDER's own words for it, which is the only established cause here");
    }

    // 12. A BOOK shelf release is still never expanded — one file IS the book there.
    {
        prov.requested.clear();
        prov.scenario = QStringLiteral("epubonly");
        (void)search(mgr, query, want, QStringLiteral("book"));
        for (const QString& r : std::as_const(prov.requested))
            CHECK(!r.startsWith(QStringLiteral("/detail/")), "a book search does not expand the release it picked");
    }

    if (failures == 0) { std::puts("DOCBRIDGE-OK"); return 0; }
    std::fprintf(stderr, "DOCBRIDGE: %d failure(s)\n", failures);
    return 1;
}
