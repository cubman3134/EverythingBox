// probe_lookup — in-book lookup (issue #137): the three verbs' request shapes, every failure sentence, the
// language seam, the vocabulary store and its sync, and the cancel a page turn fires.
//
// WHAT IS DRIVEN AGAINST A REAL SOCKET, AND WHY IT IS NOT ALL OF IT. Define and Wikipedia go to fixed
// Wikimedia hosts on purpose — the endpoints are zero-config, and giving them an overridable base URL would
// be a test hook in shipping code that a hand-edited ini could then point at somebody else's server. So:
//
//   * TRANSLATE is driven END TO END through the real LookupClient, because its endpoint IS a setting: the
//     fixture server below is a perfectly legitimate LibreTranslate-class instance as far as the client is
//     concerned. That covers the client's own plumbing — the POST, the JSON body, the User-Agent, the
//     timeout, the transport failure, and the cancel.
//   * DEFINE and WIKIPEDIA have their request shapes asserted PURELY (the URL LookupRequest builds) and then
//     replayed against the same fixture server with the host swapped to loopback, so the PATH, the language
//     and the header that actually reach a server are asserted against server bytes rather than against a
//     string this file wrote. Their readers are then driven over the server's real response bytes.
//
// NO CREDENTIAL IS INVOLVED anywhere in this feature — there is no key, no token and no account. The only
// user text that leaves the process is the selected word, and the last section asserts that a fixture server
// sees ZERO bytes until a verb is explicitly started, which is the headless form of "never on mere selection".
//
// Sections:
//   1. The sanitisers — the language code (including the host-injection refusal), the term, the vocabulary
//      key, and what is short enough to be worth asking about.
//   2. The request shapes — the three URLs, the translate body, the header.
//   3. Against the fixture server — all three shapes arrive as built; all three readers over real bytes.
//   4. Every failure is a SENTENCE — offline, 404, 5xx, a malformed body, a disambiguation page, and an
//      unset translation endpoint (the verb is ABSENT, not broken).
//   5. The language seam — the book's declaration seeds it, the system locale is the stated fallback, and an
//      override beats both.
//   6. The vocabulary store — the round trip, a duplicate UPDATING rather than duplicating, the delete
//      tombstone and the sync classification. (The CloudMerge round trip is probe_cloudmerge section 24e,
//      where #136 put the highlights one, for the reason the CMake comment gives.)
//   7. Cancel — a lookup dropped mid-flight produces no callback at all.
//   8. Nothing is requested until a verb is pressed.
#include "AppBrand.h"
#include "AppPaths.h"
#include "CloudSync.h"
#include "LookupClient.h"
#include "LookupRequest.h"
#include "ProfileStore.h"
#include "Tombstones.h"
#include "VocabularyStore.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>

#include <cstdio>

static int g_fail = 0;
#define CHECK(cond)                                                                     \
    do {                                                                                \
        if (!(cond)) { std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_fail; } \
    } while (0)

// ---------------------------------------------------------------------------------------------------------
// The fixture server. It answers three routes — a Wiktionary definition page, a Wikipedia summary and a
// LibreTranslate /translate — and records every request so the probe can assert what actually left the
// process. Routes are switchable so the failure paths (404, 5xx, a body that is not JSON) are driven over a
// real socket rather than hand-fed to the readers.
class LookupStub : public QTcpServer
{
public:
    struct Seen { QString method; QString target; QString agent; QByteArray body; };
    QVector<Seen> seen;

    int     status = 200;            // what every route answers with
    QByteArray override_;            // when non-empty, the body every route answers with
    int     delayMs = 0;             // hold the answer back (the cancel + timeout cases)

    explicit LookupStub(QObject* parent = nullptr) : QTcpServer(parent) {}

    const Seen* lastOf(const QString& method) const
    {
        for (int i = seen.size() - 1; i >= 0; --i)
            if (seen[i].method == method) return &seen[i];
        return nullptr;
    }

protected:
    void incomingConnection(qintptr handle) override
    {
        auto* sock = new QTcpSocket(this);
        sock->setSocketDescriptor(handle);
        connect(sock, &QTcpSocket::readyRead, this, [this, sock] {
            sock->setProperty("buf", sock->property("buf").toByteArray() + sock->readAll());
            QByteArray buf = sock->property("buf").toByteArray();
            const int headEnd = buf.indexOf("\r\n\r\n");
            if (headEnd < 0) return;
            const QByteArray head = buf.left(headEnd);
            const QList<QByteArray> lines = head.split('\n');
            const QList<QByteArray> reqLine = lines.value(0).trimmed().split(' ');
            QString agent;
            int wantBody = 0;
            for (int i = 1; i < lines.size(); ++i)
            {
                // Case-insensitively: Qt 6 lower-cases the header names it puts on the wire, and a stub that
                // matched "User-Agent:" would report the header as ABSENT from every request.
                const QByteArray l = lines.at(i).trimmed();
                if (l.toLower().startsWith("user-agent:"))     agent = QString::fromUtf8(l.mid(11)).trimmed();
                if (l.toLower().startsWith("content-length:")) wantBody = l.mid(15).trimmed().toInt();
            }
            const QByteArray body = buf.mid(headEnd + 4);
            if (body.size() < wantBody) return;   // wait for the rest
            const QString method = QString::fromUtf8(reqLine.value(0));
            const QString target = QString::fromUtf8(reqLine.value(1));
            seen.push_back({ method, target, agent, body.left(wantBody) });
            if (delayMs > 0) QTimer::singleShot(delayMs, sock, [this, sock, target] { route(sock, target); });
            else             route(sock, target);
        });
    }

private:
    void send(QTcpSocket* sock, int code, const QByteArray& body)
    {
        QByteArray out = "HTTP/1.1 " + QByteArray::number(code) + (code == 200 ? " OK" : " ERR")
                       + "\r\nContent-Type: application/json"
                       + "\r\nContent-Length: " + QByteArray::number(body.size())
                       + "\r\nConnection: close\r\n\r\n" + body;
        sock->write(out);
        sock->flush();
        sock->disconnectFromHost();
    }

    void route(QTcpSocket* sock, const QString& target)
    {
        if (!override_.isEmpty()) { send(sock, status, override_); return; }
        const QString path = QUrl(target).path();

        if (path.contains(QLatin1String("/page/definition/")))
        {
            send(sock, status,
                 "{\"en\":[{\"partOfSpeech\":\"Noun\",\"language\":\"English\",\"definitions\":["
                 "{\"definition\":\"A <a href=\\\"x\\\">quality</a> of being too great to be expressed.\"},"
                 "{\"definition\":\"Second sense.\"},{\"definition\":\"Third sense.\"},"
                 "{\"definition\":\"Fourth sense, past the cap.\"}]}],"
                 "\"la\":[{\"partOfSpeech\":\"Verb\",\"definitions\":[{\"definition\":\"A Latin entry.\"}]}]}");
            return;
        }
        if (path.contains(QLatin1String("/page/summary/")))
        {
            send(sock, status, "{\"type\":\"standard\",\"title\":\"Ineffable\","
                               "\"extract\":\"Ineffable   is  an  encyclopedia summary.\"}");
            return;
        }
        if (path.endsWith(QLatin1String("/translate")))
        {
            send(sock, status, "{\"translatedText\":\"indicible\"}");
            return;
        }
        send(sock, 404, "{\"title\":\"Not found\"}");
    }
};

// Swap a LookupRequest URL's host+port for the fixture's, keeping the PATH and the query exactly as built —
// so what the server sees is the shape the production code produced, not one this file rewrote.
static QUrl toFixture(const QUrl& built, quint16 port)
{
    QUrl u = built;
    u.setScheme(QStringLiteral("http"));
    u.setHost(QStringLiteral("127.0.0.1"));
    u.setPort(port);
    return u;
}

// One blocking GET through a plain QNetworkAccessManager, returning the status and body. Used only to replay
// the two zero-config verbs' shapes at the fixture; the translate verb goes through the real client.
static void fetch(QNetworkAccessManager& nam, const QUrl& url, int* status, QByteArray* body)
{
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(LookupRequest::userAgent()));
    QNetworkReply* r = nam.get(req);
    QEventLoop loop;
    QObject::connect(r, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    *status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    *body = r->readAll();
    r->deleteLater();
}

// The tombstone timestamp for a word id, or 0 when there is none. Tombstones::all is the only reader, so the
// probe asks the same way CloudMerge does rather than reading the ini rows behind it.
static qint64 tombTs(const QString& id)
{
    for (const Tombstones::Entry& e : Tombstones::all(VocabularyStore::tombstoneStore()))
        if (e.key == id) return e.ts;
    return 0;
}

// Run the event loop until `done` or the deadline. Never a bare sleep: the answers here arrive on the loop.
static bool spin(const bool& done, int ms = 6000)
{
    QDeadlineTimer dl(ms);
    while (!done && !dl.hasExpired())
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return done;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // ---- 1. the sanitisers ---------------------------------------------------------------------------
    std::printf("== 1. the sanitisers ==\n");
    {
        CHECK(LookupRequest::normalizeLang(QStringLiteral("en_US")) == QLatin1String("en"));
        CHECK(LookupRequest::normalizeLang(QStringLiteral("pt-BR")) == QLatin1String("pt"));
        CHECK(LookupRequest::normalizeLang(QStringLiteral("FR"))    == QLatin1String("fr"));
        CHECK(LookupRequest::normalizeLang(QString())               == QLatin1String("en"));
        // The code becomes part of a HOST NAME. Anything that is not plain ASCII letters is refused outright
        // and falls back to English, so a hand-edited ini cannot steer a lookup at another server.
        CHECK(LookupRequest::normalizeLang(QStringLiteral("evil.example.com")) == QLatin1String("en"));
        CHECK(LookupRequest::normalizeLang(QStringLiteral("e"))    == QLatin1String("en"));
        CHECK(LookupRequest::normalizeLang(QStringLiteral("en/x")) == QLatin1String("en"));
        CHECK(LookupRequest::normalizeLang(QStringLiteral("e2"))   == QLatin1String("en"));

        // The punctuation a selection drags off the page comes off the ENDS; the inside is left alone.
        CHECK(LookupRequest::normalizeTerm(QStringLiteral("  “ineffable,”  ")) == QLatin1String("ineffable"));
        CHECK(LookupRequest::normalizeTerm(QStringLiteral("don't")) == QLatin1String("don't"));
        CHECK(LookupRequest::normalizeTerm(QStringLiteral("New   York")) == QLatin1String("New York"));
        CHECK(LookupRequest::normalizeTerm(QStringLiteral("...")).isEmpty());

        // The vocabulary key is case-folded, so the same word met twice is ONE row.
        CHECK(LookupRequest::vocabKey(QStringLiteral("Ineffable"), QStringLiteral("en_GB"))
              == LookupRequest::vocabKey(QStringLiteral("ineffable,"), QStringLiteral("en")));

        // A word or a short phrase is a lookup; a paragraph is not — which is what stops a mis-committed
        // selection from posting a page of a book to a translator.
        CHECK(LookupRequest::isLookupable(QStringLiteral("ineffable")));
        CHECK(LookupRequest::isLookupable(QStringLiteral("a very short phrase")));
        CHECK(!LookupRequest::isLookupable(QStringLiteral("one two three four five")));
        CHECK(!LookupRequest::isLookupable(QString()));
        CHECK(!LookupRequest::isLookupable(QStringLiteral("   ,,,  ")));
    }

    // ---- 2. the request shapes ------------------------------------------------------------------------
    std::printf("== 2. the request shapes ==\n");
    {
        const QUrl def = LookupRequest::defineUrl(QStringLiteral("ineffable"), QStringLiteral("en_US"));
        CHECK(def.host() == QLatin1String("en.wiktionary.org"));
        CHECK(def.scheme() == QLatin1String("https"));
        CHECK(def.path() == QLatin1String("/api/rest_v1/page/definition/ineffable"));
        // The LANGUAGE is the edition: a French book asks fr.wiktionary, not en's.
        CHECK(LookupRequest::defineUrl(QStringLiteral("mot"), QStringLiteral("fr")).host()
              == QLatin1String("fr.wiktionary.org"));

        const QUrl sum = LookupRequest::summaryUrl(QStringLiteral("New York"), QStringLiteral("en"));
        CHECK(sum.host() == QLatin1String("en.wikipedia.org"));
        // A space is the canonical underscore, and the whole title is percent-encoded: a '/' in a selection
        // would otherwise reshape the path into a different request.
        CHECK(sum.path() == QLatin1String("/api/rest_v1/page/summary/New_York"));
        CHECK(LookupRequest::summaryUrl(QStringLiteral("a/b"), QStringLiteral("en")).toString()
                  .contains(QLatin1String("%2F")));

        // An unset instance means the verb is ABSENT, and a non-http setting is not an instance.
        CHECK(!LookupRequest::translateConfigured(QString()));
        CHECK(!LookupRequest::translateConfigured(QStringLiteral("   ")));
        CHECK(!LookupRequest::translateConfigured(QStringLiteral("file:///etc/passwd")));
        CHECK(!LookupRequest::translateConfigured(QStringLiteral("not a url")));
        CHECK(LookupRequest::translateConfigured(QStringLiteral("https://lt.example")));
        // Both spellings of the setting reach the same route.
        CHECK(LookupRequest::translateUrl(QStringLiteral("https://lt.example")).toString()
              == QLatin1String("https://lt.example/translate"));
        CHECK(LookupRequest::translateUrl(QStringLiteral("https://lt.example/")).toString()
              == QLatin1String("https://lt.example/translate"));
        CHECK(LookupRequest::translateUrl(QStringLiteral("https://lt.example/translate")).toString()
              == QLatin1String("https://lt.example/translate"));

        const QJsonObject body = QJsonDocument::fromJson(
            LookupRequest::translateBody(QStringLiteral("ineffable"), QStringLiteral("en-GB"),
                                         QStringLiteral("fr_FR"))).object();
        CHECK(body.value(QStringLiteral("q")).toString() == QLatin1String("ineffable"));
        CHECK(body.value(QStringLiteral("source")).toString() == QLatin1String("en"));
        CHECK(body.value(QStringLiteral("target")).toString() == QLatin1String("fr"));
        CHECK(body.value(QStringLiteral("format")).toString() == QLatin1String("text"));
        // A book that declared NO language asks the endpoint to detect it, rather than this app guessing.
        CHECK(QJsonDocument::fromJson(LookupRequest::translateBody(QStringLiteral("x"), QString(),
                                                                   QStringLiteral("en")))
                  .object().value(QStringLiteral("source")).toString() == QLatin1String("auto"));

        // Wikimedia refuses or throttles a client that does not identify itself.
        CHECK(!LookupRequest::userAgent().isEmpty());
        CHECK(LookupRequest::userAgent().contains("EverythingBox"));
    }

    // ---- the fixture server ----------------------------------------------------------------------------
    LookupStub stub;
    if (!stub.listen(QHostAddress::LocalHost, 0))
    {
        std::printf("  FAIL could not start the fixture server\n");
        std::printf("PROBE-LOOKUP-FAILED\n");
        return 1;
    }
    const quint16 port = stub.serverPort();
    const QString endpoint = QStringLiteral("http://127.0.0.1:%1").arg(port);
    QNetworkAccessManager nam;

    // ---- 3. against the fixture server -----------------------------------------------------------------
    std::printf("== 3. against the fixture server ==\n");
    {
        int status = 0; QByteArray body;

        // DEFINE. The path and the header that reach a server are the ones LookupRequest built.
        fetch(nam, toFixture(LookupRequest::defineUrl(QStringLiteral("ineffable"), QStringLiteral("en")), port),
              &status, &body);
        CHECK(status == 200);
        const LookupStub::Seen* d = stub.lastOf(QStringLiteral("GET"));
        CHECK(d != nullptr);
        if (d)
        {
            CHECK(d->target == QLatin1String("/api/rest_v1/page/definition/ineffable"));
            CHECK(d->agent.contains(QLatin1String("EverythingBox")));
        }
        LookupRequest::Outcome o = LookupRequest::readDefinition(body, status, false,
                                                                QStringLiteral("ineffable"),
                                                                QStringLiteral("en"));
        CHECK(o.ok);
        // The HTML fragments Wiktionary serves come off — a card is a plain label, and an unstripped <a> is
        // rendered as angle brackets in the middle of a sentence.
        CHECK(!o.text.contains(QLatin1Char('<')));
        CHECK(o.text.contains(QLatin1String("quality of being too great")));
        CHECK(o.text.contains(QLatin1String("Noun 1.")));
        // Three senses per part of speech: the fourth is past the cap.
        CHECK(!o.text.contains(QLatin1String("past the cap")));
        // The LANGUAGE picks the section: the Latin entry is not the answer to an English word.
        CHECK(!o.text.contains(QLatin1String("A Latin entry")));
        LookupRequest::Outcome la = LookupRequest::readDefinition(body, status, false,
                                                                  QStringLiteral("ineffable"),
                                                                  QStringLiteral("la"));
        CHECK(la.ok);
        CHECK(la.text.contains(QLatin1String("A Latin entry")));

        // WIKIPEDIA.
        fetch(nam, toFixture(LookupRequest::summaryUrl(QStringLiteral("Ineffable"), QStringLiteral("en")), port),
              &status, &body);
        const LookupStub::Seen* w = stub.lastOf(QStringLiteral("GET"));
        CHECK(w != nullptr);
        if (w) CHECK(w->target == QLatin1String("/api/rest_v1/page/summary/Ineffable"));
        LookupRequest::Outcome s = LookupRequest::readSummary(body, status, false, QStringLiteral("Ineffable"));
        CHECK(s.ok);
        // simplified(): the extract arrives with the run of spaces a wiki article carries, and a card is
        // one flowing paragraph.
        CHECK(s.text == QLatin1String("Ineffable is an encyclopedia summary."));

        // TRANSLATE, end to end through the REAL client — its endpoint is a setting, so the fixture is a
        // legitimate instance as far as it is concerned.
        LookupClient client;
        bool done = false;
        LookupRequest::Outcome got;
        client.start(LookupRequest::Verb::Translate, QStringLiteral("ineffable"), QStringLiteral("en"),
                     QStringLiteral("fr"), endpoint,
                     [&](LookupRequest::Outcome out) { got = out; done = true; });
        CHECK(spin(done));
        CHECK(got.ok);
        CHECK(got.text == QLatin1String("indicible"));
        const LookupStub::Seen* t = stub.lastOf(QStringLiteral("POST"));
        CHECK(t != nullptr);
        if (t)
        {
            CHECK(t->target == QLatin1String("/translate"));
            CHECK(t->agent.contains(QLatin1String("EverythingBox")));
            const QJsonObject sent = QJsonDocument::fromJson(t->body).object();
            CHECK(sent.value(QStringLiteral("q")).toString() == QLatin1String("ineffable"));
            CHECK(sent.value(QStringLiteral("source")).toString() == QLatin1String("en"));
            CHECK(sent.value(QStringLiteral("target")).toString() == QLatin1String("fr"));
        }
    }

    // ---- 4. every failure is a SENTENCE ----------------------------------------------------------------
    std::printf("== 4. every failure is a sentence ==\n");
    {
        // 404 — the server ANSWERED, so this is "no entry", not "we could not reach it". The two are
        // different sentences on purpose: one is the reader's problem, the other is the network's.
        LookupRequest::Outcome o = LookupRequest::readDefinition("{}", 404, false,
                                                                QStringLiteral("frabjous"), QStringLiteral("en"));
        CHECK(!o.ok);
        CHECK(o.text.contains(QLatin1String("No dictionary entry")));
        CHECK(o.text.contains(QLatin1String("frabjous")));
        CHECK(!o.text.contains(QLatin1String("Check your connection")));

        o = LookupRequest::readSummary("{}", 404, false, QStringLiteral("frabjous"));
        CHECK(!o.ok);
        CHECK(o.text.contains(QLatin1String("no article")));

        // 5xx.
        o = LookupRequest::readDefinition(QByteArray(), 503, false, QStringLiteral("x"), QStringLiteral("en"));
        CHECK(!o.ok);
        CHECK(o.text.contains(QLatin1String("503")));
        CHECK(!o.text.isEmpty());

        // A body that is not what the endpoint documents.
        o = LookupRequest::readDefinition("<html>not json</html>", 200, false, QStringLiteral("x"),
                                          QStringLiteral("en"));
        CHECK(!o.ok);
        CHECK(o.text.contains(QLatin1String("couldn't read")));
        o = LookupRequest::readSummary("{{{", 200, false, QStringLiteral("x"));
        CHECK(!o.ok);
        CHECK(!o.text.isEmpty());
        // Well-formed JSON that simply has no senses is still "no entry", never an empty card.
        o = LookupRequest::readDefinition("{\"en\":[]}", 200, false, QStringLiteral("x"), QStringLiteral("en"));
        CHECK(!o.ok);
        CHECK(o.text.contains(QLatin1String("No dictionary entry")));

        // A disambiguation page is not an answer, and saying so is better than pasting a list of unrelated
        // things into a card that claims to be about one of them.
        o = LookupRequest::readSummary("{\"type\":\"disambiguation\",\"extract\":\"Mercury may refer to...\"}",
                                       200, false, QStringLiteral("Mercury"));
        CHECK(!o.ok);
        CHECK(o.text.contains(QLatin1String("several things")));

        // The translation service's own refusal is passed through — a wrong target language or a rate limit
        // is the most useful thing there is to show.
        o = LookupRequest::readTranslation("{\"error\":\"target language not supported\"}", 400, false);
        CHECK(!o.ok);
        CHECK(o.text.contains(QLatin1String("target language not supported")));
        // A 200 with an empty translation is still a failure with a sentence, never a blank card.
        o = LookupRequest::readTranslation("{\"translatedText\":\"\"}", 200, false);
        CHECK(!o.ok);
        CHECK(!o.text.isEmpty());

        // OFFLINE, over a real socket: a port with nothing listening. The client must produce a sentence
        // rather than a card that waits for ever.
        QTcpServer dead;
        CHECK(dead.listen(QHostAddress::LocalHost, 0));
        const quint16 deadPort = dead.serverPort();
        dead.close();
        LookupClient client;
        bool done = false;
        LookupRequest::Outcome got;
        client.start(LookupRequest::Verb::Translate, QStringLiteral("ineffable"), QStringLiteral("en"),
                     QStringLiteral("fr"), QStringLiteral("http://127.0.0.1:%1").arg(deadPort),
                     [&](LookupRequest::Outcome out) { got = out; done = true; });
        CHECK(spin(done));
        CHECK(!got.ok);
        CHECK(got.text.contains(QLatin1String("Check your connection")));

        // An UNSET endpoint: the verb is not offered at all (translateConfigured, §2) — and a caller that
        // reaches the client anyway still gets an Outcome with a sentence rather than a card that never
        // resolves.
        bool done2 = false;
        LookupRequest::Outcome got2;
        client.start(LookupRequest::Verb::Translate, QStringLiteral("x"), QStringLiteral("en"),
                     QStringLiteral("fr"), QString(),
                     [&](LookupRequest::Outcome out) { got2 = out; done2 = true; });
        CHECK(done2);            // synchronous: no socket was opened at all
        CHECK(!got2.ok);
        CHECK(got2.text.contains(QLatin1String("Settings")));

        // A SERVER THAT ACCEPTS AND SAYS NOTHING is the failure a lookup panel usually gets wrong: no error
        // ever arrives, so the card sits on "Looking up…". The transfer timeout is what turns it into the
        // same readable sentence being offline produces.
        LookupClient::setTimeoutMs(600);
        stub.delayMs = 5000;
        bool done3 = false;
        LookupRequest::Outcome got3;
        QElapsedTimer clock; clock.start();
        client.start(LookupRequest::Verb::Translate, QStringLiteral("x"), QStringLiteral("en"),
                     QStringLiteral("fr"), endpoint,
                     [&](LookupRequest::Outcome out) { got3 = out; done3 = true; });
        CHECK(spin(done3, 4000));
        CHECK(!got3.ok);
        CHECK(!got3.text.isEmpty());
        CHECK(clock.elapsed() < 4000);      // it resolved on the timeout, not on the server finally answering
        stub.delayMs = 0;
        LookupClient::setTimeoutMs(12000);
    }

    // ---- 5. the language seam --------------------------------------------------------------------------
    std::printf("== 5. the language seam ==\n");
    {
        // The BOOK's declaration seeds the dictionary edition. EbookView::bookLanguage() is the seam
        // (EbookSource::language() -> the system locale when the format declared none); what is asserted here
        // is the arithmetic that seam feeds, over both of its answers.
        const QString declared = QStringLiteral("fr-CA");     // what an EPUB package said
        const QString fallback = QStringLiteral("en_US");     // what a format that says nothing falls back to
        CHECK(LookupRequest::defineUrl(QStringLiteral("mot"), declared).host()
              == QLatin1String("fr.wiktionary.org"));
        CHECK(LookupRequest::summaryUrl(QStringLiteral("Mot"), declared).host()
              == QLatin1String("fr.wikipedia.org"));
        CHECK(LookupRequest::defineUrl(QStringLiteral("word"), fallback).host()
              == QLatin1String("en.wiktionary.org"));
        // The OVERRIDE wins over the book: an English novel quoting French is one press from the right
        // dictionary. (The card's Language… row calls runLookup with the chosen code in place of the seed.)
        const QString override_ = QStringLiteral("de");
        CHECK(LookupRequest::defineUrl(QStringLiteral("wort"), override_).host()
              == QLatin1String("de.wiktionary.org"));
        CHECK(LookupRequest::defineUrl(QStringLiteral("wort"), override_).host()
              != LookupRequest::defineUrl(QStringLiteral("wort"), declared).host());
        // The translation SOURCE is the book's; the TARGET is the reader's own locale.
        const QJsonObject b = QJsonDocument::fromJson(
            LookupRequest::translateBody(QStringLiteral("mot"), declared, fallback)).object();
        CHECK(b.value(QStringLiteral("source")).toString() == QLatin1String("fr"));
        CHECK(b.value(QStringLiteral("target")).toString() == QLatin1String("en"));

        // The context the vocabulary row keeps: one line, elided, taken from the chapter around the anchor.
        const QString chapter = QStringLiteral("It was a truth universally acknowledged, and had been for a "
                                               "very long while indeed, that a single man in possession of a\n"
                                               "large fortune must be in want of a wife, and that the word "
                                               "ineffable was, in itself, quite ineffable indeed and so on "
                                               "and on and on for many more words after that.");
        const int at = chapter.indexOf(QStringLiteral("ineffable"));
        const QString ctx = LookupRequest::contextAround(chapter, at, at + 9);
        CHECK(ctx.contains(QLatin1String("ineffable")));
        // A panel row is one line. QChar(0x0A) rather than '\n': CHECK stringifies its condition, and a
        // backslash escape inside a stringified condition is the GCC trap that has already reached main once.
        CHECK(!ctx.contains(QChar(0x0A)));
        CHECK(ctx.startsWith(QStringLiteral("…")));
        CHECK(ctx.endsWith(QStringLiteral("…")));
        CHECK(LookupRequest::contextAround(chapter, 5, 5).isEmpty());   // an empty range has no context
    }

    // ---- 6. the vocabulary store -----------------------------------------------------------------------
    std::printf("== 6. the vocabulary store ==\n");
    {
        // An isolated data dir, so the probe never touches a real library (the CMake data-dir isolation the
        // other store probes run under).
        QSettings ini(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                      QSettings::IniFormat);
        ini.remove(VocabularyStore::itemsKey());
        ini.sync();

        CHECK(VocabularyStore::count() == 0);

        VocabularyStore::Word w;
        w.word       = QStringLiteral("Ineffable");
        w.lang       = QStringLiteral("en_GB");
        w.source     = QStringLiteral("Define");
        w.definition = QStringLiteral("Too great to be expressed.");
        w.bookKey    = QStringLiteral("/books/austen.epub");
        w.bookTitle  = QStringLiteral("Pride and Prejudice");
        w.context    = QStringLiteral("…quite ineffable indeed…");
        w.spine      = 3;
        w.offset     = 1200;
        const VocabularyStore::Word a = VocabularyStore::add(w);

        CHECK(!a.id.isEmpty());
        CHECK(a.lang == QLatin1String("en"));           // normalised on the way in
        CHECK(VocabularyStore::count() == 1);
        const VocabularyStore::Word back = VocabularyStore::byId(a.id);
        CHECK(back.word == QLatin1String("Ineffable"));
        CHECK(back.definition == QLatin1String("Too great to be expressed."));
        CHECK(back.bookTitle == QLatin1String("Pride and Prejudice"));
        CHECK(back.context == QStringLiteral("…quite ineffable indeed…"));
        CHECK(back.spine == 3);
        CHECK(back.offset == 1200);
        CHECK(back.source == QLatin1String("Define"));

        // A DUPLICATE UPDATES rather than duplicates — the identity is the WORD, not the occurrence, which is
        // what makes this a vocabulary and not a log. Met again in another book, in another case, through
        // another verb: still one row, carrying where it was met LAST.
        VocabularyStore::Word again;
        again.word       = QStringLiteral("ineffable,");
        again.lang       = QStringLiteral("en");
        again.source     = QStringLiteral("Wikipedia");
        again.definition = QStringLiteral("An encyclopedia summary.");
        again.bookKey    = QStringLiteral("/books/other.epub");
        again.bookTitle  = QStringLiteral("Another Book");
        again.spine      = 9;
        again.offset     = 42;
        const VocabularyStore::Word a2 = VocabularyStore::add(again);
        CHECK(a2.id == a.id);
        CHECK(VocabularyStore::count() == 1);
        const VocabularyStore::Word back2 = VocabularyStore::byId(a.id);
        CHECK(back2.source == QLatin1String("Wikipedia"));
        CHECK(back2.bookTitle == QLatin1String("Another Book"));
        CHECK(back2.spine == 9);

        // A DIFFERENT LANGUAGE is a different word: "sensible" in English and in French are not one row.
        VocabularyStore::Word fr;
        fr.word = QStringLiteral("sensible");
        fr.lang = QStringLiteral("fr");
        fr.definition = QStringLiteral("Sensitive.");
        const VocabularyStore::Word afr = VocabularyStore::add(fr);
        VocabularyStore::Word en;
        en.word = QStringLiteral("sensible");
        en.lang = QStringLiteral("en");
        en.definition = QStringLiteral("Reasonable.");
        const VocabularyStore::Word aen = VocabularyStore::add(en);
        CHECK(afr.id != aen.id);
        CHECK(VocabularyStore::count() == 3);

        // An empty word is a no-op, not a blank row.
        CHECK(VocabularyStore::add(VocabularyStore::Word()).id.isEmpty());
        CHECK(VocabularyStore::count() == 3);

        // Most recent first — the order a list of what you just met wants to be in.
        const QVector<VocabularyStore::Word> list = VocabularyStore::all();
        CHECK(list.size() == 3);
        if (list.size() == 3)
        {
            CHECK(list.at(0).ts >= list.at(1).ts);
            CHECK(list.at(1).ts >= list.at(2).ts);
        }

        // Remove leaves a TOMBSTONE, so a peer holding the row cannot resurrect it on merge.
        VocabularyStore::remove(aen.id);
        CHECK(VocabularyStore::count() == 2);
        CHECK(VocabularyStore::byId(aen.id).id.isEmpty());
        CHECK(tombTs(aen.id) > 0);
        // Looking a removed word up again brings it back: the stale tombstone is cleared, so it does not
        // self-suppress on the next merge.
        const VocabularyStore::Word again2 = VocabularyStore::add(en);
        CHECK(again2.id == aen.id);
        CHECK(tombTs(aen.id) == 0);
        CHECK(VocabularyStore::count() == 3);

        // THE SYNC CLASSIFICATION. A word you had to look up is a fact about the READER, not about the
        // machine they were holding — so it is per-item-synced and NOT device-local. Asserted BOTH ways, so a
        // mistaken filing in either table turns one of them red.
        CHECK(CloudSync::isPerItemStoreKey(VocabularyStore::itemsKey()) == true);
        CHECK(CloudSync::isDeviceLocalKey(VocabularyStore::itemsKey()) == false);
        CHECK(CloudSync::isPerItemStoreKey(QStringLiteral("vocabulary/default/items")) == true);
        // The prefix is "vocabulary/" WITH the slash: a key that merely starts with the letters is not ours.
        CHECK(CloudSync::isPerItemStoreKey(QStringLiteral("vocabularylist/items")) == false);

        // The CLOUDMERGE ROUND TRIP is in probe_cloudmerge (section 24e), where the highlights one lives
        // for the same reason: CloudMerge.cpp reaches into forty other stores, and linking all of them here
        // would make this probe fail for everyone else's reasons.
    }

    // ---- 7. cancel -------------------------------------------------------------------------------------
    std::printf("== 7. cancel ==\n");
    {
        // THE PAGE-TURN PATH. EbookView::cancelLookup() is called from nextPage/prevPage/loadChapter/hideEvent
        // and calls exactly this. A cancelled lookup produces NOTHING: not a late card, not a vocabulary row.
        stub.delayMs = 1500;
        const int seenBefore = int(stub.seen.size());
        LookupClient client;
        bool fired = false;
        client.start(LookupRequest::Verb::Translate, QStringLiteral("ineffable"), QStringLiteral("en"),
                     QStringLiteral("fr"), endpoint,
                     [&](LookupRequest::Outcome) { fired = true; });
        CHECK(client.busy());
        // Let the request actually LEAVE first — cancelling before the socket has written anything would
        // assert nothing about the interesting case, which is a page turned while an answer is in flight.
        {
            QDeadlineTimer sent(3000);
            while (int(stub.seen.size()) == seenBefore && !sent.hasExpired())
                QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        }
        CHECK(stub.seen.size() > seenBefore);   // the request DID go out — it is the ANSWER that is dropped
        CHECK(client.busy());                   // …and the server is still holding it back
        // …the reader turns the page.
        client.cancel();
        CHECK(!client.busy());
        // Give the answer every chance to arrive anyway.
        QDeadlineTimer dl(2500);
        while (!dl.hasExpired()) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        CHECK(!fired);
        stub.delayMs = 0;

        // Starting a SECOND lookup cancels the first for the same reason: the previous question is no longer
        // the one being asked, and two answers racing onto one card is how the wrong word gets recorded.
        stub.delayMs = 800;
        bool first = false;
        client.start(LookupRequest::Verb::Translate, QStringLiteral("one"), QStringLiteral("en"),
                     QStringLiteral("fr"), endpoint, [&](LookupRequest::Outcome) { first = true; });
        stub.delayMs = 0;
        bool second = false;
        LookupRequest::Outcome got;
        client.start(LookupRequest::Verb::Translate, QStringLiteral("two"), QStringLiteral("en"),
                     QStringLiteral("fr"), endpoint,
                     [&](LookupRequest::Outcome out) { got = out; second = true; });
        CHECK(spin(second));
        QDeadlineTimer dl2(1500);
        while (!dl2.hasExpired()) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        CHECK(!first);          // the abandoned question never answers
        CHECK(second);
        CHECK(got.text == QLatin1String("indicible"));
    }

    // ---- 8. nothing is requested until a verb is pressed ------------------------------------------------
    std::printf("== 8. nothing until a verb ==\n");
    {
        // The privacy line in Settings ▸ Reading promises a lookup is a network call carrying the selected
        // text, and that nothing is looked up without an explicit verb press — never on mere selection. The
        // headless form of that: building every request SHAPE for a selection sends nothing at all. Only
        // start() opens a socket.
        const int before = int(stub.seen.size());
        const QString selected = QStringLiteral("ineffable");
        (void)LookupRequest::defineUrl(selected, QStringLiteral("en"));
        (void)LookupRequest::summaryUrl(selected, QStringLiteral("en"));
        (void)LookupRequest::translateUrl(endpoint);
        (void)LookupRequest::translateBody(selected, QStringLiteral("en"), QStringLiteral("fr"));
        (void)LookupRequest::isLookupable(selected);
        (void)LookupRequest::translateConfigured(endpoint);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        CHECK(stub.seen.size() == before);

        LookupClient client;
        bool done = false;
        client.start(LookupRequest::Verb::Translate, selected, QStringLiteral("en"), QStringLiteral("fr"),
                     endpoint, [&](LookupRequest::Outcome) { done = true; });
        CHECK(spin(done));
        CHECK(stub.seen.size() == before + 1);   // exactly one request, and only once a verb ran
    }

    if (g_fail == 0) std::printf("LOOKUP-OK\n");
    else             std::printf("PROBE-LOOKUP-FAILED (%d)\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
