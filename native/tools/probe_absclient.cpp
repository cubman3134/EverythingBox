// probe_absclient — the Audiobookshelf client (issue #197, increment 1), driven against a FIXTURE HTTP
// STUB this file starts on a loopback port.
//
// NO REAL SERVER, NO REAL ACCOUNT, AND — the point of saying so — NO REAL CREDENTIAL. The password below is
// the literal string "probe-not-a-real-password" and the token the stub issues is
// "probe-fixture-token-6f3a9c2e", both named so nobody can mistake either for one. The last test in this
// file is a BYTE SCAN for that token across everything the feature writes and everything it prints, which
// is the assertion the whole credential rule reduces to: a token belongs in exactly one device-local ini
// row and in exactly two URLs that are minted and thrown away, and nowhere else.
//
// WHY A REAL SOCKET RATHER THAN A FAKED REPLY. Half of what is under test is not a function of the payload:
// that the token rides an Authorization HEADER rather than a query (so it cannot reach a log line), that
// exactly one PATCH leaves the process for two ticks a second apart, that a seek sends one immediately, and
// that a play session's tracks come back in the server's order and not the array's. None of those can be
// asserted against a hand-built struct; all of them are one small server away.
//
// What is under test, and why each of these:
//
//   1. ID QUALIFICATION — the #160 rule, from the first commit. A round trip; the episode form; the refusal
//      of an id half carrying either separator; and that nothing else in this app's key space (a file path,
//      a http url, a Subsonic-style 0x1F key) can parse as one of ours.
//   2. THE PAYLOAD READERS — libraries, a library listing, series, authors, an expanded item, a podcast's
//      episodes, and a play session, including the two things a naive reader gets wrong: the track ORDER is
//      the server's `index` and not the array's, and a chapter list is over the WHOLE BOOK.
//   3. THE CHAPTER REBASE — the server's book-wide list turned into the list for the FILE that is open,
//      including the chapter that straddles a part boundary.
//   4. THE BOOK'S TIMELINE — a position in part four turned into a position in the book and back, which is
//      the only arithmetic between "what mpv knows" and "what the server wants".
//   5. THE BROWSE LEVELS — every row's type/mime pair, the doors that only appear when their dimension
//      exists, a podcast's episodes newest-first, and a book level that leads with its verb.
//   6. LIVE, AGAINST THE STUB — sign in, browse, open a book, mint a part's link, report progress. Plus the
//      throttle, which is a decision about TIME and is therefore driven over the clock rather than asserted
//      about.
//   7. PROGRESS IS THE SERVER'S — a PlaybackSession carrying one of these ids writes NOTHING into the
//      resume store, and the server's position beats a local mark for the same id.
//   8. THE TOKEN NEVER LANDS — the byte scan described above.
#include "AbsCatalogs.h"
#include "AbsClient.h"
#include "AbsServerStore.h"
#include "AppBrand.h"
#include "AppPaths.h"
#include "Audiobookshelf.h"
#include "PlaybackSession.h"
#include "RemoteAudiobook.h"
#include "ResumeStore.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QUrl>
#include <QUrlQuery>

#include <cstdio>
#include <initializer_list>

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

// The two strings the credential rule is asserted over. Named so they cannot be mistaken for real ones, and
// distinctive enough that a byte scan over a whole directory tree cannot match them by accident.
static const char* kPassword = "probe-not-a-real-password";
static const char* kToken    = "probe-fixture-token-6f3a9c2e";

// ==================================================================================================
// THE FIXTURE SERVER
// ==================================================================================================
// A deliberately small HTTP/1.1 server: one request per connection, Content-Length bodies only, no
// chunking and no keep-alive. It is not pretending to be Audiobookshelf — it answers the seven endpoints
// this client speaks and records what it was asked, which is what the assertions are about.
class AbsStub : public QTcpServer
{
public:
    struct Seen { QString method; QString path; QString auth; QByteArray body; };
    QVector<Seen> seen;
    // Set to make /login answer without a token (the wrong-password case) and to make /api/authorize
    // publish a distinguishing id (the server that HAS one).
    bool    refuseLogin = false;
    QString publishedServerId;

    explicit AbsStub(QObject* parent = nullptr) : QTcpServer(parent) {}

    int countOf(const QString& method, const QString& pathPrefix) const
    {
        int n = 0;
        for (const Seen& s : seen)
            if (s.method == method && s.path.startsWith(pathPrefix)) ++n;
        return n;
    }
    const Seen* lastOf(const QString& method, const QString& pathPrefix) const
    {
        for (int i = seen.size() - 1; i >= 0; --i)
            if (seen[i].method == method && seen[i].path.startsWith(pathPrefix)) return &seen[i];
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
            QString auth;
            int wantBody = 0;
            for (int i = 1; i < lines.size(); ++i)
            {
                const QByteArray l = lines.at(i).trimmed();
                // CASE-INSENSITIVELY, because Qt 6 lower-cases the header names it puts on the wire —
                // a stub that matched "Authorization:" saw none and would have reported the token as
                // ABSENT from every request, which is a green light for exactly the mistake this checks.
                if (l.toLower().startsWith("authorization:"))
                    auth = QString::fromUtf8(l.mid(14)).trimmed();
                if (l.toLower().startsWith("content-length:")) wantBody = l.mid(15).trimmed().toInt();
            }
            const QByteArray body = buf.mid(headEnd + 4);
            if (body.size() < wantBody) return;    // wait for the rest
            reply(sock, QString::fromUtf8(reqLine.value(0)), QString::fromUtf8(reqLine.value(1)), auth,
                  body.left(wantBody));
        });
    }

private:
    void send(QTcpSocket* sock, int status, const QByteArray& body, const char* type = "application/json")
    {
        QByteArray out = "HTTP/1.1 " + QByteArray::number(status) + (status == 200 ? " OK" : " ERR")
                       + "\r\nContent-Type: " + type
                       + "\r\nContent-Length: " + QByteArray::number(body.size())
                       + "\r\nConnection: close\r\n\r\n" + body;
        sock->write(out);
        sock->flush();
        sock->disconnectFromHost();
    }

    void reply(QTcpSocket* sock, const QString& method, const QString& target, const QString& auth,
               const QByteArray& body)
    {
        const QUrl u(target);
        const QString path = u.path();
        seen.push_back({ method, target, auth, body });

        if (path == QLatin1String("/login"))
        {
            if (refuseLogin) { send(sock, 401, "{\"error\":\"Invalid user or password\"}"); return; }
            send(sock, 200, QByteArray("{\"user\":{\"id\":\"u1\",\"username\":\"reader\",\"token\":\"")
                                + kToken + "\"},\"userDefaultLibraryId\":\"lib_books\"}");
            return;
        }
        if (path == QLatin1String("/api/authorize"))
        {
            const QByteArray sid = publishedServerId.isEmpty() ? QByteArray("server-settings")
                                                               : publishedServerId.toUtf8();
            send(sock, 200, "{\"user\":{\"id\":\"u1\"},\"serverSettings\":{\"id\":\"" + sid + "\"}}");
            return;
        }
        if (path == QLatin1String("/api/libraries"))
        {
            send(sock, 200, R"({"libraries":[
                {"id":"lib_books","name":"Books","mediaType":"book"},
                {"id":"lib_pods","name":"Shows","mediaType":"podcast"},
                {"id":"","name":"Broken","mediaType":"book"}]})");
            return;
        }
        if (path == QLatin1String("/api/libraries/lib_books/items"))
        {
            send(sock, 200, R"({"results":[
              {"id":"li_multi","mediaType":"book","media":{"duration":450,"numTracks":3,
                "coverPath":"/covers/a.jpg",
                "metadata":{"title":"The Long Book","authorName":"A. Writer",
                            "narratorName":"N. Reader","seriesName":"Chronicles","sequence":"2"}}},
              {"id":"li_one","mediaType":"book","media":{"duration":600,"numTracks":1,
                "metadata":{"title":"One File","authorName":"A. Writer"}}},
              {"id":"li_other","mediaType":"book","media":{"duration":300,"numTracks":1,
                "metadata":{"title":"Another","authorName":"B. Author","seriesName":"Chronicles",
                            "sequence":"1"}}}]})");
            return;
        }
        if (path == QLatin1String("/api/libraries/lib_books/series"))
        {
            send(sock, 200, R"({"results":[{"id":"ser_1","name":"Chronicles",
                                            "books":[{"id":"li_multi"},{"id":"li_other"}]}]})");
            return;
        }
        if (path == QLatin1String("/api/libraries/lib_books/authors"))
        {
            send(sock, 200, R"({"authors":[{"id":"aut_1","name":"A. Writer","numBooks":2},
                                           {"id":"aut_2","name":"B. Author","numBooks":1}]})");
            return;
        }
        if (path == QLatin1String("/api/libraries/lib_pods/items"))
        {
            send(sock, 200, R"({"results":[{"id":"li_pod","mediaType":"podcast",
              "media":{"numEpisodes":2,"coverPath":"/covers/p.jpg",
                       "metadata":{"title":"A Show","authorName":"Host"}}}]})");
            return;
        }
        if (path == QLatin1String("/api/items/li_multi"))
        {
            // Tracks fed OUT OF ORDER on purpose, plus one with no contentUrl, plus a zero-length chapter.
            send(sock, 200, R"({"id":"li_multi","mediaType":"book",
              "media":{"duration":450,"numTracks":3,
                "metadata":{"title":"The Long Book","authorName":"A. Writer"},
                "tracks":[
                  {"index":2,"startOffset":100,"duration":200,"title":"02 - Two.mp3",
                   "contentUrl":"/api/items/li_multi/file/af_2","mimeType":"audio/mpeg"},
                  {"index":1,"startOffset":0,"duration":100,"title":"01 - One.mp3",
                   "contentUrl":"/api/items/li_multi/file/af_1","mimeType":"audio/mpeg"},
                  {"index":9,"startOffset":900,"duration":10,"title":"junk"},
                  {"index":3,"startOffset":300,"duration":150,"title":"03 - Three.mp3",
                   "contentUrl":"/api/items/li_multi/file/af_3","mimeType":"audio/mpeg"}],
                "chapters":[
                  {"id":0,"start":0,"end":80,"title":"Chapter One"},
                  {"id":1,"start":80,"end":260,"title":"Chapter Two"},
                  {"id":2,"start":260,"end":260,"title":"Zero"},
                  {"id":3,"start":260,"end":450,"title":"Chapter Three"}]}})");
            return;
        }
        if (path == QLatin1String("/api/items/li_pod"))
        {
            send(sock, 200, R"({"id":"li_pod","mediaType":"podcast",
              "media":{"metadata":{"title":"A Show","authorName":"Host"},
                "episodes":[
                  {"id":"ep_old","title":"The first one","pubDate":"2024-01-02T00:00:00Z",
                   "audioFile":{"duration":1800}},
                  {"id":"ep_new","title":"The latest one","pubDate":"2025-06-01T00:00:00Z",
                   "duration":2400}]}})");
            return;
        }
        if (path.endsWith(QLatin1String("/play")) || path.contains(QLatin1String("/play/")))
        {
            // WITH `libraryItem`, which a real Audiobookshelf sends and which a client re-opening from its
            // own Recents has no other source for the book's name in.
            send(sock, 200, R"({"id":"sess_1","duration":450,"currentTime":250,
              "libraryItem":{"id":"li_multi","media":{"metadata":{"title":"The Long Book"}}},
              "audioTracks":[
                {"index":1,"startOffset":0,"duration":100,"title":"01 - One.mp3",
                 "contentUrl":"/api/items/li_multi/file/af_1","mimeType":"audio/mpeg"},
                {"index":2,"startOffset":100,"duration":200,"title":"02 - Two.mp3",
                 "contentUrl":"/api/items/li_multi/file/af_2","mimeType":"audio/mpeg"},
                {"index":3,"startOffset":300,"duration":150,"title":"03 - Three.mp3",
                 "contentUrl":"/api/items/li_multi/file/af_3","mimeType":"audio/mpeg"}],
              "chapters":[
                {"start":0,"end":80,"title":"Chapter One"},
                {"start":80,"end":260,"title":"Chapter Two"},
                {"start":260,"end":450,"title":"Chapter Three"}]})");
            return;
        }
        if (path.startsWith(QLatin1String("/api/me/progress/")))
        {
            if (method == QLatin1String("PATCH")) { send(sock, 200, "{\"ok\":true}"); return; }
            send(sock, 200, "{\"libraryItemId\":\"li_multi\",\"currentTime\":250,\"duration\":450,"
                            "\"progress\":0.5555,\"isFinished\":false}");
            return;
        }
        if (path.contains(QLatin1String("/cover"))) { send(sock, 200, "JPEGBYTES", "image/jpeg"); return; }
        if (path.contains(QLatin1String("/file/")))  { send(sock, 200, "AUDIOBYTES", "audio/mpeg"); return; }
        send(sock, 404, "{}");
    }
};

// Spin the event loop until `done` or the deadline. Never a QEventLoop with no exit: a probe that hangs on
// a reply that never comes is a probe that hangs CI rather than failing it.
static bool waitFor(const std::function<bool()>& done, int ms = 5000)
{
    QDeadlineTimer dl(ms);
    while (!done() && !dl.hasExpired())
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return done();
}

// ==================================================================================================
// 1. ID QUALIFICATION
// ==================================================================================================
static void testIds()
{
    const QString sid = QStringLiteral("3f2a-server-a");
    const QString oid = QStringLiteral("9c1b-server-b");

    const QString q = Abs::qualify(sid, QStringLiteral("li_multi"));
    CHECK(q == QStringLiteral("abs:3f2a-server-a:li_multi"));
    const Abs::Ref r = Abs::parse(q);
    CHECK(r.ok && r.serverId == sid && r.itemId == QStringLiteral("li_multi") && !r.isEpisode());

    // AN ID FROM SERVER A IS NOT AN ID ON SERVER B — the whole reason the scheme exists.
    CHECK(Abs::serverOf(q) != oid);
    CHECK(Abs::qualify(oid, QStringLiteral("li_multi")) != q);

    // The episode form, and that the item half is still readable out of it.
    const QString e = Abs::qualifyEpisode(sid, QStringLiteral("li_pod"), QStringLiteral("ep_new"));
    CHECK(e == QStringLiteral("abs:3f2a-server-a:li_pod#ep_new"));
    const Abs::Ref er = Abs::parse(e);
    CHECK(er.ok && er.isEpisode() && er.itemId == QStringLiteral("li_pod")
          && er.episodeId == QStringLiteral("ep_new"));
    CHECK(Abs::itemIdOf(e) == Abs::qualify(sid, QStringLiteral("li_pod")));
    CHECK(Abs::itemIdOf(q) == q);   // an item id is its own item id

    // REFUSED rather than escaped: an id half carrying either separator.
    CHECK(Abs::qualify(QStringLiteral("a:b"), QStringLiteral("li")).isEmpty());
    CHECK(Abs::qualify(QStringLiteral("ab"), QStringLiteral("li:1")).isEmpty());
    CHECK(Abs::qualify(QStringLiteral("a#b"), QStringLiteral("li")).isEmpty());
    CHECK(Abs::qualify(QString(), QStringLiteral("li")).isEmpty());
    CHECK(Abs::qualify(sid, QString()).isEmpty());
    CHECK(Abs::qualifyEpisode(sid, QStringLiteral("li"), QString()).isEmpty());

    // NOTHING ELSE IN THIS APP'S KEY SPACE PARSES AS ONE OF OURS. Each of these is a real shape some other
    // feature stores: a Windows path, a POSIX path, a stream url, a Subsonic-style 0x1F key, a part token.
    CHECK(!Abs::isQualified(QStringLiteral("C:/Books/A Book/01.mp3")));
    CHECK(!Abs::isQualified(QStringLiteral("/home/me/books/01.mp3")));
    CHECK(!Abs::isQualified(QStringLiteral("https://example.invalid/stream?id=1")));
    CHECK(!Abs::isQualified(QStringLiteral("sub") + QChar(0x1F) + QStringLiteral("uuid")
                            + QChar(0x1F) + QStringLiteral("track")));
    CHECK(!Abs::isQualified(QStringLiteral("ebaudiobookpart:key") + QChar(0x1F)
                            + QStringLiteral("01.mp3")));
    CHECK(!Abs::isQualified(QStringLiteral("abs:")));
    CHECK(!Abs::isQualified(QStringLiteral("abs:only-a-server")));
    CHECK(!Abs::isQualified(QStringLiteral("abs::li")));           // empty server half
    CHECK(!Abs::isQualified(QStringLiteral("abs:s:a:b")));         // a third colon is somebody else's key

    // A PART TOKEN OVER A QUALIFIED BOOK still names the book, which is what makes an entry in a multi-file
    // queue routable back to the server it came from.
    const QString tok = RemoteAudiobook::partToken(q, QStringLiteral("01 - One.mp3"));
    CHECK(AbsSupply::bookIdOf(tok) == q);
    CHECK(AbsSupply::isAbsEntry(tok));
    CHECK(AbsSupply::bookIdOf(q) == q);                            // a single-file book plays under its id
    CHECK(AbsSupply::bookIdOf(QStringLiteral("C:/x/01.mp3")).isEmpty());
    CHECK(!AbsSupply::isAbsEntry(RemoteAudiobook::partToken(QStringLiteral("some-torrent-release"),
                                                            QStringLiteral("01.mp3"))));
}

// ==================================================================================================
// 2. THE ADDRESS
// ==================================================================================================
static void testUrls()
{
    CHECK(Abs::checkUrl(QStringLiteral("https://books.invalid"), false) == Abs::UrlVerdict::Ok);
    CHECK(Abs::checkUrl(QStringLiteral("http://books.invalid"), false) == Abs::UrlVerdict::InsecureRefused);
    CHECK(Abs::checkUrl(QStringLiteral("http://books.invalid"), true) == Abs::UrlVerdict::Ok);
    CHECK(Abs::checkUrl(QStringLiteral("ftp://books.invalid"), true) == Abs::UrlVerdict::NotHttp);
    CHECK(Abs::checkUrl(QStringLiteral("not a url"), true) == Abs::UrlVerdict::Malformed);
    CHECK(Abs::checkUrl(QString(), true) == Abs::UrlVerdict::Malformed);
    CHECK(Abs::normalizeRoot(QStringLiteral("https://books.invalid///"), false)
          == QStringLiteral("https://books.invalid"));
    CHECK(Abs::normalizeRoot(QStringLiteral("http://books.invalid"), false).isEmpty());

    // The stream url: the server's own path, joined, with the token in the query and nothing else added.
    const QString s = Abs::streamUrl(QStringLiteral("https://books.invalid"),
                                     QStringLiteral("/api/items/li/file/af"), QLatin1String(kToken));
    CHECK(s == QStringLiteral("https://books.invalid/api/items/li/file/af?token=") + QLatin1String(kToken));
    // A server that hands back an ABSOLUTE url is handing us another origin, which this feature never
    // follows — so it mints nothing rather than fetching from somewhere the user never configured.
    CHECK(Abs::streamUrl(QStringLiteral("https://books.invalid"),
                         QStringLiteral("https://elsewhere.invalid/x"), QLatin1String(kToken)).isEmpty());
    CHECK(Abs::streamUrl(QString(), QStringLiteral("/x"), QLatin1String(kToken)).isEmpty());
}

// ==================================================================================================
// 3. THE PAYLOAD READERS
// ==================================================================================================
static void testReaders()
{
    const Abs::Login in = Abs::readLogin(QByteArray("{\"user\":{\"username\":\"reader\",\"token\":\"")
                                         + kToken + "\"}}");
    CHECK(in.ok && in.token == QLatin1String(kToken) && in.username == QStringLiteral("reader"));
    // A root-level token, which some versions answer with instead.
    CHECK(Abs::readLogin(QByteArray("{\"token\":\"") + kToken + "\"}").token == QLatin1String(kToken));
    CHECK(!Abs::readLogin("{\"error\":\"Invalid user or password\"}").ok);

    // "server-settings" is the constant EVERY install answers with, so it identifies nothing and must be
    // refused — otherwise every server in the world would qualify its ids identically.
    CHECK(Abs::serverIdOf("{\"serverSettings\":{\"id\":\"server-settings\"}}").isEmpty());
    CHECK(Abs::serverIdOf("{\"serverSettings\":{\"id\":\"abcd-1234\"}}") == QStringLiteral("abcd-1234"));
    // ...and an id carrying a scheme separator could never qualify anything either.
    CHECK(Abs::serverIdOf("{\"serverSettings\":{\"id\":\"a:b\"}}").isEmpty());

    const QVector<Abs::Library> libs =
        Abs::readLibraries(R"({"libraries":[{"id":"l1","name":"Books","mediaType":"book"},
                                            {"id":"l2","name":"Shows","mediaType":"podcast"},
                                            {"id":"","name":"Broken"}]})");
    CHECK(libs.size() == 2);                      // the id-less row is dropped, not rendered
    CHECK(!libs.at(0).isPodcast() && libs.at(1).isPodcast());

    const QVector<Abs::Item> items = Abs::readLibraryItems(
        R"({"results":[{"id":"li_1","mediaType":"book","media":{"duration":90,"numTracks":4,
             "coverPath":"/c.jpg","metadata":{"title":"T","authorName":"A","narratorName":"N",
             "seriesName":"S","sequence":"3"}}}]})");
    CHECK(items.size() == 1);
    CHECK(items.at(0).title == QStringLiteral("T") && items.at(0).author == QStringLiteral("A")
          && items.at(0).narrator == QStringLiteral("N") && items.at(0).series == QStringLiteral("S")
          && items.at(0).seriesSequence == QStringLiteral("3"));
    CHECK(items.at(0).duration == 90.0 && items.at(0).trackCount == 4 && items.at(0).hasCover);

    CHECK(Abs::readSeries(R"({"results":[{"id":"s1","name":"S","books":[{},{},{}]}]})").at(0).bookCount == 3);
    CHECK(Abs::readAuthors(R"({"authors":[{"id":"a1","name":"A","numBooks":7}]})").at(0).bookCount == 7);

    // THE PLAY SESSION: out-of-order tracks, a track nothing can be minted from, a zero-length chapter.
    const Abs::Session s = Abs::readPlaySession(R"({"id":"x","duration":450,"currentTime":250,
      "audioTracks":[{"index":3,"startOffset":300,"duration":150,"contentUrl":"/c"},
                     {"index":1,"startOffset":0,"duration":100,"contentUrl":"/a"},
                     {"index":9,"startOffset":900,"duration":10},
                     {"index":2,"startOffset":100,"duration":200,"contentUrl":"/b"}],
      "chapters":[{"start":260,"end":450,"title":"Three"},{"start":0,"end":80,"title":"One"},
                  {"start":260,"end":260,"title":"Zero"},{"start":80,"end":260,"title":"Two"}]})");
    CHECK(s.ok && s.currentTime == 250.0 && s.duration == 450.0);
    // THE ITEM'S OWN TITLE, off the reply's `libraryItem`. It looks redundant — browsing to a book means
    // the expanded item is already cached — and that is exactly how it was missed: a re-open from RECENTS
    // reaches /play with nothing else fetched, and without this the book came back titled "01 - One.mp3",
    // i.e. the Recents row renamed itself to its own first track. Found on the first live drive.
    CHECK(Abs::readPlaySession(R"({"id":"x","duration":9,"audioTracks":[{"index":1,"contentUrl":"/a"}],
          "libraryItem":{"media":{"metadata":{"title":"The Long Book"}}}})").title
          == QStringLiteral("The Long Book"));
    // THE SERVER'S ORDER, out of `index` — not the array's, and not a natural sort over names.
    CHECK(s.tracks.size() == 3);
    CHECK(s.tracks.at(0).contentUrl == QStringLiteral("/a"));
    CHECK(s.tracks.at(1).contentUrl == QStringLiteral("/b"));
    CHECK(s.tracks.at(2).contentUrl == QStringLiteral("/c"));
    CHECK(s.chapters.size() == 3);                             // the zero-length one is not a region
    CHECK(s.chapters.at(0).title == QStringLiteral("One")
          && s.chapters.at(2).title == QStringLiteral("Three"));

    // "Never opened" is not "at zero" — the difference decides whether the server wins over a local mark.
    CHECK(!Abs::readProgress("{}").found);
    CHECK(!Abs::readProgress("").found);
    const Abs::Progress p = Abs::readProgress("{\"currentTime\":0,\"duration\":450}");
    CHECK(p.found && p.currentTime == 0.0);

    // The PATCH body.
    const QJsonObject b = Abs::progressBody(225.0, 450.0);
    CHECK(b.value(QStringLiteral("currentTime")).toDouble() == 225.0);
    CHECK(b.value(QStringLiteral("duration")).toDouble() == 450.0);
    CHECK(qAbs(b.value(QStringLiteral("progress")).toDouble() - 0.5) < 1e-9);
    CHECK(b.value(QStringLiteral("isFinished")).toBool() == false);
    CHECK(Abs::progressBody(450.0, 450.0).value(QStringLiteral("isFinished")).toBool() == true);
    // A position past a stale duration must not report a fraction above one: the server stores it verbatim
    // and every client then draws a bar past its own end.
    CHECK(Abs::progressBody(900.0, 450.0).value(QStringLiteral("progress")).toDouble() <= 1.0);
    // An unknown total omits the fraction rather than inventing one.
    CHECK(!Abs::progressBody(10.0, 0.0).contains(QStringLiteral("progress")));
}

// ==================================================================================================
// 4. THE CHAPTER REBASE, AND THE BOOK'S TIMELINE
// ==================================================================================================
static QVector<Abs::Track> fixtureTracks()
{
    QVector<Abs::Track> t;
    t.push_back({ 1, QStringLiteral("01"), QStringLiteral("/a"),   0.0, 100.0, {} });
    t.push_back({ 2, QStringLiteral("02"), QStringLiteral("/b"), 100.0, 200.0, {} });
    t.push_back({ 3, QStringLiteral("03"), QStringLiteral("/c"), 300.0, 150.0, {} });
    return t;
}

static void testTimeline()
{
    const QVector<Abs::Track> t = fixtureTracks();

    CHECK(Abs::absoluteTime(t, 0, 30.0) == 30.0);
    CHECK(Abs::absoluteTime(t, 1, 150.0) == 250.0);      // part two, 150 in -> 250 in the book
    CHECK(Abs::absoluteTime(t, 2, 10.0) == 310.0);
    CHECK(Abs::absoluteTime(t, 99, 5.0) == 5.0);         // out of range: the position, unmoved

    CHECK(Abs::trackAtTime(t, 0.0) == 0);
    CHECK(Abs::trackAtTime(t, 99.0) == 0);
    CHECK(Abs::trackAtTime(t, 100.0) == 1);
    CHECK(Abs::trackAtTime(t, 250.0) == 1);
    CHECK(Abs::trackAtTime(t, 300.0) == 2);
    CHECK(Abs::trackAtTime(t, -5.0) == 0);
    CHECK(Abs::trackAtTime({}, 10.0) == -1);
    CHECK(Abs::trackAtTime(t, 99999.0) == 2);            // past the end: the LAST part, never nothing

    CHECK(Abs::offsetWithinTrack(t, 250.0) == 150.0);
    CHECK(Abs::offsetWithinTrack(t, 0.0) == 0.0);
    // Past the end of the track it lands in: clamped INSIDE it. Seeking past the end of the file mpv is
    // holding is not recoverable; landing a second early is.
    CHECK(Abs::offsetWithinTrack(t, 99999.0) <= 149.0);
    CHECK(Abs::offsetWithinTrack(t, 99999.0) > 0.0);

    // THE REBASE. The book's chapters are 0-80, 80-260, 260-450; part two is 100..300.
    QVector<Abs::Chapter> book;
    book.push_back({   0.0,  80.0, QStringLiteral("One") });
    book.push_back({  80.0, 260.0, QStringLiteral("Two") });
    book.push_back({ 260.0, 450.0, QStringLiteral("Three") });

    const QVector<Abs::Chapter> p1 = Abs::chaptersForTrack(book, 0.0, 100.0);
    CHECK(p1.size() == 2);                                // One entire, Two clipped at the part's end
    CHECK(p1.at(0).start == 0.0 && p1.at(0).end == 80.0);
    CHECK(p1.at(1).start == 80.0 && p1.at(1).end == 100.0);

    const QVector<Abs::Chapter> p2 = Abs::chaptersForTrack(book, 100.0, 200.0);
    CHECK(p2.size() == 2);
    // THE STRADDLING CHAPTER IS KEPT, clamped to 0 — the region the listener is standing in has to be in
    // the list or "end of chapter" cannot find it, and a chapter that began in the previous file is still
    // the chapter they are listening to.
    CHECK(p2.at(0).title == QStringLiteral("Two") && p2.at(0).start == 0.0);
    CHECK(qAbs(p2.at(0).end - 160.0) < 1e-9);             // 260 - 100
    CHECK(p2.at(1).title == QStringLiteral("Three") && qAbs(p2.at(1).start - 160.0) < 1e-9);

    const QVector<Abs::Chapter> p3 = Abs::chaptersForTrack(book, 300.0, 150.0);
    CHECK(p3.size() == 1 && p3.at(0).title == QStringLiteral("Three") && p3.at(0).start == 0.0);

    CHECK(Abs::chaptersForTrack(book, 0.0, 0.0).isEmpty());       // a part with no length has no regions
    CHECK(Abs::chaptersForTrack({}, 0.0, 100.0).isEmpty());
}

// ==================================================================================================
// 5. WHEN TO TELL THE SERVER
// ==================================================================================================
static void testThrottle()
{
    // Nothing sent yet -> send. A listener who plays ten seconds and closes the app must not lose them.
    CHECK(Abs::shouldReport(false, 0.0, 0, 3.0, 1000));
    // Ordinary listening, inside the interval -> silence.
    CHECK(!Abs::shouldReport(true, 10.0, 1000, 15.0, 5000));
    // ...and past it -> send.
    CHECK(Abs::shouldReport(true, 10.0, 1000, 15.0, 1000 + Abs::kReportIntervalMs));
    // A SEEK is news immediately, in either direction: a position that moved further than playback could
    // have moved is a user action whose whole point is that the position changed.
    CHECK(Abs::shouldReport(true, 10.0, 1000, 10.0 + Abs::kSeekJumpS, 1100));
    CHECK(Abs::shouldReport(true, 600.0, 1000, 600.0 - Abs::kSeekJumpS, 1100));
    CHECK(!Abs::shouldReport(true, 10.0, 1000, 10.0 + Abs::kSeekJumpS - 1.0, 1100));
}

// ==================================================================================================
// 6. THE BROWSE LEVELS
// ==================================================================================================
static const MediaItem* rowOfType(const MediaCatalog& c, const char* type)
{
    for (const MediaItem& it : c.items)
        if (it.type == QString::fromLatin1(type)) return &it;
    return nullptr;
}

static void testLevels()
{
    const QString sid = QStringLiteral("srv-1");
    const QString lib = Abs::qualify(sid, QStringLiteral("lib_books"));

    // The door on the Audiobooks root.
    const MediaItem door = browse::absServersRow(2);
    CHECK(door.type == QLatin1String(browse::kAbsServersType) && door.expandable);
    CHECK(door.mime == QLatin1String(browse::kAbsServersPrefix));

    // The server list. The ADD row is always last and always present, which is what makes the first server
    // addable at all; a disabled server is still listed and still openable.
    MediaCatalog servers = browse::absServersCatalog({ sid }, { QStringLiteral("Home") },
                                                     { QStringLiteral("https://books.invalid") },
                                                     { false });
    CHECK(servers.items.size() == 2);
    CHECK(servers.items.at(0).type == QLatin1String(browse::kAbsServerType));
    CHECK(browse::absKeyOf(servers.items.at(0).mime, browse::kAbsServerPrefix) == sid);
    CHECK(servers.items.at(0).subtitle.contains(QStringLiteral("https://books.invalid")));
    CHECK(servers.items.at(1).type == QLatin1String(browse::kAbsAddServerType));
    // ...and it says nothing about the account. A row that named the user would put an account name on a
    // TV in a living room to no purpose, and there is obviously no rendering of the token.
    CHECK(!servers.items.at(0).subtitle.contains(QStringLiteral("reader")));
    CHECK(browse::absServersCatalog({}, {}, {}, {}).items.size() == 1);

    // The libraries.
    QVector<Abs::Library> libs;
    libs.push_back({ QStringLiteral("lib_books"), QStringLiteral("Books"), QStringLiteral("book") });
    libs.push_back({ QStringLiteral("lib_pods"),  QStringLiteral("Shows"), QStringLiteral("podcast") });
    const MediaCatalog ls = browse::absLibrariesCatalog(sid, QStringLiteral("Home"), libs);
    CHECK(ls.items.size() == 2);
    CHECK(browse::absKeyOf(ls.items.at(0).mime, browse::kAbsLibraryPrefix) == lib);

    // A BOOK library's doors, each only when its dimension exists — the compatibility rule this whole
    // feature rests on: a library whose books carry no series gets a plain list of books.
    const MediaCatalog withBoth = browse::absLibraryCatalog(lib, QStringLiteral("Books"), false, 3, 4, 9, {});
    CHECK(rowOfType(withBoth, browse::kAbsSeriesListType) != nullptr);
    CHECK(rowOfType(withBoth, browse::kAbsAuthorsType) != nullptr);
    CHECK(rowOfType(withBoth, browse::kAbsBooksType) != nullptr);
    const MediaCatalog noSeries = browse::absLibraryCatalog(lib, QStringLiteral("Books"), false, 0, 4, 9, {});
    CHECK(rowOfType(noSeries, browse::kAbsSeriesListType) == nullptr);
    CHECK(rowOfType(noSeries, browse::kAbsBooksType) != nullptr);
    const MediaCatalog bare = browse::absLibraryCatalog(lib, QStringLiteral("Books"), false, 0, 0, 9, {});
    CHECK(bare.items.size() == 1 && rowOfType(bare, browse::kAbsBooksType) != nullptr);

    // A PODCAST library is its shows, straight away — there are no dimensions to offer.
    QVector<Abs::Item> pods;
    Abs::Item pod; pod.id = QStringLiteral("li_pod"); pod.title = QStringLiteral("A Show");
    pod.isPodcast = true; pod.episodeCount = 12;
    pods.push_back(pod);
    const MediaCatalog pl = browse::absLibraryCatalog(lib, QStringLiteral("Shows"), true, 0, 0, 1, pods);
    CHECK(pl.items.size() == 1 && pl.items.at(0).type == QLatin1String(browse::kAbsPodcastType));
    CHECK(browse::absKeyOf(pl.items.at(0).mime, browse::kAbsPodcastPrefix)
          == Abs::qualify(sid, QStringLiteral("li_pod")));

    // The two dimension lists, whose keys are COMPOSITE (the library plus the bucket) and must survive the
    // ':' every qualified id carries.
    QVector<Abs::SeriesRow> series; series.push_back({ QStringLiteral("ser_1"),
                                                       QStringLiteral("Chronicles"), 2 });
    const MediaCatalog sl = browse::absSeriesListCatalog(lib, QStringLiteral("Books"), series);
    const QString skey = browse::absKeyOf(sl.items.at(0).mime, browse::kAbsSeriesPrefix);
    CHECK(browse::absKeyHead(skey) == lib);
    // THE BUCKET IS THE NAME, not "ser_1". A library listing joins to a series by `seriesName` and does not
    // carry the id /series is keyed by, so a row keyed by the id opens a level that filters the listing by
    // a string none of its books has — an empty shelf over a library that plainly has the books. That is
    // what the first live drive of this feature showed; this line is why it cannot come back.
    CHECK(browse::absKeyTail(skey) == QStringLiteral("Chronicles"));
    // ...and the filter is over the same string, so the two halves cannot drift apart.
    {
        QVector<Abs::Item> lib3;
        Abs::Item m; m.id = QStringLiteral("li_multi"); m.series = QStringLiteral("Chronicles");
        m.author = QStringLiteral("A. Writer");
        Abs::Item o; o.id = QStringLiteral("li_other"); o.series = QStringLiteral("Chronicles");
        o.author = QStringLiteral("B. Author");
        Abs::Item u; u.id = QStringLiteral("li_one");   u.author = QStringLiteral("A. Writer");
        lib3 << m << o << u;
        int inSeries = 0;
        for (const Abs::Item& b : lib3) if (b.series == browse::absKeyTail(skey)) ++inSeries;
        CHECK(inSeries == 2);
    }
    // A bucket with NO name cannot be joined, so it is not offered rather than offered and empty.
    QVector<Abs::SeriesRow> nameless; nameless.push_back({ QStringLiteral("ser_2"), QString(), 1 });
    CHECK(browse::absSeriesListCatalog(lib, QStringLiteral("Books"), nameless).items.isEmpty());

    QVector<Abs::AuthorRow> authors; authors.push_back({ QStringLiteral("aut_1"),
                                                         QStringLiteral("A. Writer"), 2 });
    const MediaCatalog al = browse::absAuthorsCatalog(lib, QStringLiteral("Books"), authors);
    const QString akey = browse::absKeyOf(al.items.at(0).mime, browse::kAbsAuthorPrefix);
    CHECK(browse::absKeyHead(akey) == lib);
    CHECK(browse::absKeyTail(akey) == QStringLiteral("A. Writer"));   // the NAME, for the same reason

    // A list of books.
    QVector<Abs::Item> books;
    Abs::Item b; b.id = QStringLiteral("li_multi"); b.title = QStringLiteral("The Long Book");
    b.author = QStringLiteral("A. Writer"); b.trackCount = 3; b.duration = 450.0;
    b.seriesSequence = QStringLiteral("2");
    books.push_back(b);
    const MediaCatalog bl = browse::absBooksCatalog(QStringLiteral("Chronicles"), sid, books);
    CHECK(bl.items.size() == 1 && bl.items.at(0).type == QLatin1String(browse::kAbsBookType));
    CHECK(browse::absKeyOf(bl.items.at(0).mime, browse::kAbsBookPrefix)
          == Abs::qualify(sid, QStringLiteral("li_multi")));
    CHECK(bl.items.at(0).subtitle.contains(QStringLiteral("A. Writer")));
    // A book the scheme cannot qualify is DROPPED rather than rendered unroutable.
    QVector<Abs::Item> bad; Abs::Item x; x.id = QStringLiteral("li:bad"); x.title = QStringLiteral("X");
    bad.push_back(x);
    CHECK(browse::absBooksCatalog(QStringLiteral("T"), sid, bad).items.isEmpty());

    // ONE BOOK: the verb first, then the parts, keyed by their PLACE in the book.
    const QString bookKey = Abs::qualify(sid, QStringLiteral("li_multi"));
    const MediaCatalog bc = browse::absBookCatalog(bookKey, b, fixtureTracks(), 3);
    CHECK(bc.items.size() == 4);
    CHECK(bc.items.at(0).type == QLatin1String(browse::kAbsPlayBookType));
    CHECK(browse::absKeyOf(bc.items.at(0).mime, browse::kAbsPlayBookPrefix) == bookKey);
    CHECK(bc.items.at(0).subtitle.contains(QStringLiteral("3")));    // the chapter count is on the verb
    CHECK(bc.items.at(1).type == QLatin1String(browse::kAbsPartType));
    const QString pkey = browse::absKeyOf(bc.items.at(1).mime, browse::kAbsPartPrefix);
    CHECK(browse::absKeyHead(pkey) == bookKey && browse::absKeyTail(pkey) == QStringLiteral("0"));
    CHECK(browse::absKeyTail(browse::absKeyOf(bc.items.at(3).mime, browse::kAbsPartPrefix))
          == QStringLiteral("2"));
    // A book with NO chapter list says nothing rather than "0 chapters", which reads as a broken file.
    CHECK(!browse::absBookCatalog(bookKey, b, fixtureTracks(), 0).items.at(0).subtitle
               .contains(QStringLiteral("chapter")));

    // A PODCAST'S EPISODES, newest first — decided once, here, so the two layouts cannot disagree about
    // which episode is at the top of a show.
    QVector<Abs::Episode> eps;
    eps.push_back({ QStringLiteral("ep_old"), QStringLiteral("The first one"), {},
                    QStringLiteral("2024-01-02T00:00:00Z"), 1800.0 });
    eps.push_back({ QStringLiteral("ep_new"), QStringLiteral("The latest one"), {},
                    QStringLiteral("2025-06-01T00:00:00Z"), 2400.0 });
    const MediaCatalog ec = browse::absEpisodesCatalog(Abs::qualify(sid, QStringLiteral("li_pod")), pod, eps);
    CHECK(ec.items.size() == 2);
    CHECK(ec.items.at(0).title == QStringLiteral("The latest one"));
    CHECK(browse::absKeyOf(ec.items.at(0).mime, browse::kAbsEpisodePrefix)
          == Abs::qualifyEpisode(sid, QStringLiteral("li_pod"), QStringLiteral("ep_new")));

    // EVERY TYPE THIS FEATURE MINTS STARTS WITH '_', which is what makes it DRILL on the themed layouts
    // rather than open the per-leaf chooser over a row no player could be handed.
    const std::initializer_list<const MediaCatalog*> all =
        { &servers, &ls, &withBoth, &pl, &sl, &al, &bl, &bc, &ec };
    for (const MediaCatalog* cat : all)
        for (const MediaItem& it : cat->items)
            CHECK(browse::isAbsType(it.type) || it.type == QStringLiteral("info"));

    // The empty-level note is a SENTENCE, never a blank shelf, and is non-actionable.
    const MediaCatalog note = browse::absNoteCatalog(QStringLiteral("Books"), QStringLiteral("Nothing."));
    CHECK(note.items.size() == 1 && note.items.at(0).type == QStringLiteral("info"));
    CHECK(browse::absNoteCatalog(QStringLiteral("Books"), QString()).items.isEmpty());
}

// ==================================================================================================
// 7. THE STORE
// ==================================================================================================
static void testStore()
{
    AbsServer s;
    s.name = QStringLiteral("Home");
    s.url  = QStringLiteral("https://books.invalid");
    s.username = QStringLiteral("reader");
    s.token = QLatin1String(kToken);
    const QString id = AbsServerStore::add(s);
    CHECK(!id.isEmpty() && !id.contains(QLatin1Char(':')) && !id.contains(QLatin1Char('#')));
    CHECK(AbsServerStore::hasServers());

    AbsServer got;
    CHECK(AbsServerStore::get(id, got));
    CHECK(got.token == QLatin1String(kToken) && got.username == QStringLiteral("reader"));
    CHECK(got.enabled);                              // absent means ON: an upgrade must not look like a loss
    CHECK(AbsServerStore::enabledList().size() == 1);
    got.enabled = false;
    AbsServerStore::update(got);
    CHECK(AbsServerStore::enabledList().isEmpty() && AbsServerStore::list().size() == 1);
    got.enabled = true; AbsServerStore::update(got);

    // A re-add carrying an existing id UPDATES rather than duplicating — an id must never be reused, and
    // two rows sharing one would be two servers whose ids qualified identically.
    got.name = QStringLiteral("Renamed");
    CHECK(AbsServerStore::add(got) == id);
    CHECK(AbsServerStore::list().size() == 1);

    // THE KEY IS DEVICE-LOCAL BY PREFIX. probe_cloudmerge pins that CloudSync carves this prefix out; what
    // is pinned here is the other half — that this store writes under it and nowhere else.
    QSettings ini(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                  QSettings::IniFormat);
    int rows = 0;
    for (const QString& k : ini.allKeys())
        if (ini.value(k).toString().contains(QLatin1String(kToken)))
        {
            ++rows;
            CHECK(k.startsWith(QStringLiteral("audiobookshelf/")));
        }
    CHECK(rows == 1);   // exactly one row holds it: not a second copy under a synced key

    AbsServerStore::remove(id);
    CHECK(!AbsServerStore::hasServers());
    // ...and removing it takes the token with it.
    QSettings ini2(AppPaths::dataDir() + QStringLiteral("/") + QLatin1String(AppBrand::kIniFile),
                   QSettings::IniFormat);
    for (const QString& k : ini2.allKeys())
        CHECK(!ini2.value(k).toString().contains(QLatin1String(kToken)));
}

// ==================================================================================================
// 8. LIVE, AGAINST THE FIXTURE STUB
// ==================================================================================================
// `serverId` is filled in with the id the sign-in settled on, so the tests after this one can qualify with
// the same thing the app would.
static QString g_serverId;

static void testLive(AbsStub& stub, quint16 port)
{
    const QString root = QStringLiteral("http://127.0.0.1:%1").arg(port);

    // ---- Sign in ------------------------------------------------------------------------------------
    bool done = false; bool ok = false; AbsServer saved;
    AbsClient::instance().login(root, QStringLiteral("reader"), QLatin1String(kPassword),
                                /*allowPlainHttp*/ true, QStringLiteral("Fixture"),
                                [&](const AbsClient::Result& r, const AbsServer& s) {
        done = true; ok = r.ok; saved = s;
    });
    CHECK(waitFor([&] { return done; }));
    CHECK(ok && saved.token == QLatin1String(kToken));
    // THE PASSWORD WAS POSTED AND DROPPED. What comes back has a token and no field a password could be in.
    const AbsStub::Seen* login = stub.lastOf(QStringLiteral("POST"), QStringLiteral("/login"));
    CHECK(login && login->body.contains(kPassword));      // it went in the BODY, once
    CHECK(login && login->auth.isEmpty());                // ...and not in a header on the un-authed call
    // This server publishes only "server-settings", so the id stays the one the store mints.
    CHECK(saved.id.isEmpty());
    g_serverId = AbsServerStore::add(saved);
    CHECK(!g_serverId.isEmpty());

    // A server that DOES publish an id is adopted, which is the behaviour the scheme is written for even
    // though no Audiobookshelf release has one yet.
    stub.publishedServerId = QStringLiteral("real-instance-id");
    done = false; AbsServer withId;
    AbsClient::instance().login(root, QStringLiteral("reader"), QLatin1String(kPassword), true,
                                QStringLiteral("Fixture2"),
                                [&](const AbsClient::Result&, const AbsServer& s) { done = true; withId = s; });
    CHECK(waitFor([&] { return done; }));
    CHECK(withId.id == QStringLiteral("real-instance-id"));
    stub.publishedServerId.clear();

    // A refused sign-in SAVES NOTHING and says so. A row written first and repaired later is a permanently
    // broken server if the app closes in between — and one whose id has already been minted.
    stub.refuseLogin = true;
    done = false; ok = true; QString why;
    AbsClient::instance().login(root, QStringLiteral("reader"), QLatin1String(kPassword), true,
                                QStringLiteral("Bad"),
                                [&](const AbsClient::Result& r, const AbsServer&) {
        done = true; ok = r.ok; why = r.message;
    });
    CHECK(waitFor([&] { return done; }));
    CHECK(!ok && !why.isEmpty());
    // ...and the message is OUR sentence or the server's, never a request. Qt's errorString() embeds the
    // url, which is the exact route a credential takes into a status bar.
    CHECK(!why.contains(QLatin1String(kToken)) && !why.contains(QStringLiteral("127.0.0.1")));
    stub.refuseLogin = false;

    // ---- Browse -------------------------------------------------------------------------------------
    AbsClient& c = AbsClient::instance();
    done = false;
    c.fetchLibraries(g_serverId, [&](const AbsClient::Result& r) { done = true; ok = r.ok; });
    CHECK(waitFor([&] { return done; }));
    CHECK(ok && c.librariesLoaded(g_serverId) && c.libraries(g_serverId).size() == 2);

    const QString libBooks = Abs::qualify(g_serverId, QStringLiteral("lib_books"));
    done = false;
    c.fetchLibrary(libBooks, [&](const AbsClient::Result& r) { done = true; ok = r.ok; });
    CHECK(waitFor([&] { return done; }));
    CHECK(ok && c.libraryLoaded(libBooks));
    CHECK(c.libraryItems(libBooks).size() == 3);
    CHECK(waitFor([&] { return !c.series(libBooks).isEmpty() && !c.authors(libBooks).isEmpty(); }));
    CHECK(c.libraryNameOf(libBooks) == QStringLiteral("Books"));
    // A series' and an author's books come out of the listing that is already cached — no request of their
    // own, which is why opening one is instant.
    const int before = stub.seen.size();
    // BY THE KEY THE BROWSE LEVEL ACTUALLY MINTS, read back out of the row rather than spelled here — a
    // hand-written "Chronicles" would pass even if the level keyed its rows by "ser_1", which is exactly
    // the bug the first live drive of this feature found.
    const MediaCatalog liveSeries = browse::absSeriesListCatalog(libBooks, QStringLiteral("Books"),
                                                                 c.series(libBooks));
    const QString liveSeriesKey =
        browse::absKeyTail(browse::absKeyOf(liveSeries.items.at(0).mime, browse::kAbsSeriesPrefix));
    CHECK(c.seriesBooks(libBooks, liveSeriesKey).size() == 2);
    const MediaCatalog liveAuthors = browse::absAuthorsCatalog(libBooks, QStringLiteral("Books"),
                                                               c.authors(libBooks));
    const QString liveAuthorKey =
        browse::absKeyTail(browse::absKeyOf(liveAuthors.items.at(0).mime, browse::kAbsAuthorPrefix));
    CHECK(c.authorBooks(libBooks, liveAuthorKey).size() == 2);
    CHECK(stub.seen.size() == before);

    // A coalesced fetch: two callers, one request, both answered.
    const int reqs = stub.countOf(QStringLiteral("GET"), QStringLiteral("/api/libraries/lib_pods/items"));
    const QString libPods = Abs::qualify(g_serverId, QStringLiteral("lib_pods"));
    int answered = 0;
    c.fetchLibrary(libPods, [&](const AbsClient::Result&) { ++answered; });
    c.fetchLibrary(libPods, [&](const AbsClient::Result&) { ++answered; });
    CHECK(waitFor([&] { return answered == 2; }));
    CHECK(stub.countOf(QStringLiteral("GET"), QStringLiteral("/api/libraries/lib_pods/items")) == reqs + 1);

    // ---- One item -----------------------------------------------------------------------------------
    const QString book = Abs::qualify(g_serverId, QStringLiteral("li_multi"));
    done = false;
    c.fetchItem(book, [&](const AbsClient::Result& r) { done = true; ok = r.ok; });
    CHECK(waitFor([&] { return done; }));
    CHECK(ok && c.itemLoaded(book));
    const Abs::ItemDetail d = c.item(book);
    CHECK(d.ok && d.tracks.size() == 3 && d.chapters.size() == 3);
    CHECK(d.tracks.at(0).title == QStringLiteral("01 - One.mp3"));

    const QString show = Abs::qualify(g_serverId, QStringLiteral("li_pod"));
    done = false;
    c.fetchItem(show, [&](const AbsClient::Result& r) { done = true; });
    CHECK(waitFor([&] { return done; }));
    CHECK(c.item(show).episodes.size() == 2);

    // ---- Open, and mint -----------------------------------------------------------------------------
    done = false; Abs::Session sess;
    c.openSession(book, [&](const AbsClient::Result& r, const Abs::Session& s) {
        done = true; ok = r.ok; sess = s;
    });
    CHECK(waitFor([&] { return done; }));
    CHECK(ok && sess.ok && sess.tracks.size() == 3 && sess.currentTime == 250.0);
    CHECK(sess.title == QStringLiteral("The Long Book"));   // ...live, off the stub's own /play reply
    // THE SERVER'S POSITION ARRIVES ON THE SAME REPLY, which is why seeding a resume costs no request.
    CHECK(Abs::trackAtTime(sess.tracks, sess.currentTime) == 1);
    CHECK(Abs::offsetWithinTrack(sess.tracks, sess.currentTime) == 150.0);

    const QString url = c.partStreamUrl(book, 1);
    CHECK(url.startsWith(root + QStringLiteral("/api/items/li_multi/file/af_2")));
    CHECK(QUrlQuery(QUrl(url).query()).queryItemValue(QStringLiteral("token"))
          == QLatin1String(kToken));
    CHECK(c.partStreamUrl(book, 99).isEmpty());
    CHECK(c.partStreamUrl(QStringLiteral("abs:nope:li"), 0).isEmpty());

    // ---- The token is a HEADER on every API request --------------------------------------------------
    int apiCalls = 0;
    for (const AbsStub::Seen& s : stub.seen)
    {
        if (!s.path.startsWith(QStringLiteral("/api/"))) continue;
        ++apiCalls;
        CHECK(s.auth == QStringLiteral("Bearer ") + QLatin1String(kToken));
        // ...and NEVER in the url. That is the whole reason it is a header: a url reaches errorString(),
        // the status bar and stream_debug.log, and a header does not.
        CHECK(!s.path.contains(QLatin1String(kToken)));
    }
    CHECK(apiCalls > 5);

    // ---- Progress ------------------------------------------------------------------------------------
    const int patches0 = stub.countOf(QStringLiteral("PATCH"), QStringLiteral("/api/me/progress/"));
    c.reportProgress(book, 250.0, 450.0);                 // nothing sent yet -> sends
    CHECK(waitFor([&] { return stub.countOf(QStringLiteral("PATCH"),
                                            QStringLiteral("/api/me/progress/")) == patches0 + 1; }));
    const AbsStub::Seen* patch = stub.lastOf(QStringLiteral("PATCH"), QStringLiteral("/api/me/progress/"));
    CHECK(patch && patch->path == QStringLiteral("/api/me/progress/li_multi"));
    const QJsonObject sent = QJsonDocument::fromJson(patch->body).object();
    CHECK(sent.value(QStringLiteral("currentTime")).toDouble() == 250.0);
    CHECK(sent.value(QStringLiteral("duration")).toDouble() == 450.0);
    CHECK(qAbs(sent.value(QStringLiteral("progress")).toDouble() - (250.0 / 450.0)) < 1e-6);

    // THE THROTTLE. A tick five seconds of position later, immediately after, sends NOTHING: the hook it
    // rides fires on a 5-second position change, which is right for an ini write and far too chatty for a
    // round trip to somebody's Raspberry Pi.
    c.reportProgress(book, 255.0, 450.0);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 60);
    CHECK(stub.countOf(QStringLiteral("PATCH"), QStringLiteral("/api/me/progress/")) == patches0 + 1);
    // A SEEK is news immediately.
    c.reportProgress(book, 250.0 + Abs::kSeekJumpS + 1.0, 450.0);
    CHECK(waitFor([&] { return stub.countOf(QStringLiteral("PATCH"),
                                            QStringLiteral("/api/me/progress/")) == patches0 + 2; }));
    // ...and `force` is the way out for the LAST report a book makes, which must not be swallowed.
    c.reportProgress(book, 285.0, 450.0, /*force*/ true);
    CHECK(waitFor([&] { return stub.countOf(QStringLiteral("PATCH"),
                                            QStringLiteral("/api/me/progress/")) == patches0 + 3; }));

    // An EPISODE reports under both ids, which is how Audiobookshelf keys a podcast's position.
    const QString ep = Abs::qualifyEpisode(g_serverId, QStringLiteral("li_pod"), QStringLiteral("ep_new"));
    c.reportProgress(ep, 60.0, 2400.0);
    CHECK(waitFor([&] {
        const AbsStub::Seen* p = stub.lastOf(QStringLiteral("PATCH"), QStringLiteral("/api/me/progress/"));
        return p && p->path == QStringLiteral("/api/me/progress/li_pod/ep_new");
    }));

    // Reading it back.
    done = false; Abs::Progress got;
    c.fetchProgress(book, [&](const AbsClient::Result& r, const Abs::Progress& p) { done = true; got = p; });
    CHECK(waitFor([&] { return done; }));
    CHECK(got.found && got.currentTime == 250.0);
}

// ==================================================================================================
// 9. PROGRESS IS THE SERVER'S — the PlaybackSession seams
// ==================================================================================================
// Driven over the real PlaybackSession, with the two hooks MainWindow installs, because the claim is about
// what that object DOES NOT WRITE and a fake would prove only that a fake writes nothing.
static void testSessionHooks(const QString& scratchIni)
{
    const QString book = Abs::qualify(g_serverId.isEmpty() ? QStringLiteral("srv") : g_serverId,
                                      QStringLiteral("li_multi"));
    const QVector<Abs::Track> tracks = fixtureTracks();
    QStringList queue;
    for (int i = 0; i < tracks.size(); ++i)
        queue << RemoteAudiobook::partToken(book, QStringLiteral("%1.mp3").arg(i));

    // A LOCAL MARK FOR ONE OF THESE IDS, planted before anything opens: the server's answer has to beat it.
    // It could only ever be a leftover from before this feature existed, and reading it would put the
    // listener somewhere no other client of that server agrees with.
    {
        QSettings s(scratchIni, QSettings::IniFormat);
        // Through ResumeStore::groupFor, because a resume key is HASHED — a hand-spelled key would be a
        // mark nothing reads, and this test would then pass by writing a row nobody was ever going to
        // consult. That is the "harness reports success without working" family, and this is the one line
        // that would put this file in it.
        s.setValue(ResumeStore::groupFor(queue.at(1)) + QStringLiteral("/pos"), 42.0);
        s.sync();
    }

    PlaybackSession session(scratchIni);
    QVector<QPair<QString, double>> reported;
    bool sawLeaving = false;
    session.setRemoteProgress([&](const QString& id, double pos, double dur, bool leaving) {
        const QString b = AbsSupply::bookIdOf(id);
        if (b.isEmpty()) return false;
        int idx = -1;
        for (int i = 0; i < queue.size(); ++i) if (queue.at(i) == id) idx = i;
        reported.push_back({ b, idx >= 0 ? Abs::absoluteTime(tracks, idx, pos) : pos });
        if (leaving) sawLeaving = true;
        Q_UNUSED(dur);
        return true;
    });
    // The seed the app computes from the play session's currentTime: 250 -> part 1, 150 in.
    int seedPart = Abs::trackAtTime(tracks, 250.0);
    const double seedWithin = Abs::offsetWithinTrack(tracks, 250.0);
    session.setRemoteResume([&](const QString& id) -> double {
        if (AbsSupply::bookIdOf(id).isEmpty()) return -1.0;
        int idx = -1;
        for (int i = 0; i < queue.size(); ++i) if (queue.at(i) == id) idx = i;
        if (idx != seedPart) return 0.0;
        seedPart = -1;
        return seedWithin;
    });

    session.setQueue(queue, /*startIndex*/ 1, { QStringLiteral("a"), QStringLiteral("b"),
                                                QStringLiteral("c") });
    // THE SERVER'S POSITION WON. 150, not the 42 planted on disk.
    CHECK(session.takeResumeSeek() == 150.0);

    session.setPosition(30.0);
    session.persistResume();
    CHECK(reported.size() >= 1);
    // ...AND IT WAS REPORTED IN BOOK TIME. 30 seconds into part two is 130 seconds into the book; reported
    // bare it would send every other client of that server back two minutes.
    CHECK(reported.last().first == book);
    CHECK(reported.last().second == 130.0);

    // NOTHING WAS WRITTEN INTO THE RESUME STORE FOR IT. One owner per position, and for a server's own item
    // the owner is the server — a duplicate here would be merged across devices by OUR newest-wins rule
    // beside the server's, and the two would disagree the first time a phone and a TV listened out of order.
    {
        QSettings s(scratchIni, QSettings::IniFormat);
        for (const QString& k : s.allKeys())
            if (k.startsWith(QStringLiteral("resume/")) && k.endsWith(QStringLiteral("/pos")))
                CHECK(s.value(k).toDouble() == 42.0);   // only the planted one, untouched
    }

    // Leaving the media forces the last report through, whatever the far side's throttle would have said.
    session.setPosition(45.0);
    session.clearQueue();
    CHECK(sawLeaving);

    // A LOCAL QUEUE IS UNTOUCHED — the compatibility claim. The hooks are installed, the entries are not
    // ours, and the resume row is written exactly as it always was.
    const QString localFile = QStringLiteral("C:/Books/Something/01.mp3");
    session.setQueue({ localFile }, 0);
    session.setPosition(12.0);
    session.persistResume();
    {
        QSettings s(scratchIni, QSettings::IniFormat);
        bool found = false;
        for (const QString& k : s.allKeys())
            if (k.startsWith(QStringLiteral("resume/")) && k.endsWith(QStringLiteral("/pos"))
                && s.value(k).toDouble() == 12.0) found = true;
        CHECK(found);
    }
    session.clearQueue();
}

// ==================================================================================================
// 10. THE TOKEN NEVER LANDS
// ==================================================================================================
// A byte scan for the fixture token over everything this process wrote and everything it recorded. The
// files under the probe's own (isolated) data dir are every store this app has: the ini, the metadata
// cache, the recents/resume/stats documents. The one legitimate hit is the device-local server row, which
// testStore already pins by key; here the claim is that there is no SECOND copy anywhere.
static void testTokenNeverLands(const AbsStub& stub, const QString& scratchIni)
{
    const QByteArray needle = QByteArray(kToken);

    QStringList offenders;
    QDirIterator it(AppPaths::dataDir(), QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
    {
        const QString path = it.next();
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) continue;
        const QByteArray bytes = f.readAll();
        if (!bytes.contains(needle)) continue;
        // The ONE legitimate holder: the device-local server row in the shared ini.
        if (QFileInfo(path).fileName() == QLatin1String(AppBrand::kIniFile))
        {
            QSettings ini(path, QSettings::IniFormat);
            for (const QString& k : ini.allKeys())
                if (ini.value(k).toString().contains(QLatin1String(kToken)))
                    CHECK(k.startsWith(QStringLiteral("audiobookshelf/")));
            continue;
        }
        offenders << path;
    }
    for (const QString& o : offenders) std::fprintf(stderr, "FAIL token found in %s\n", qPrintable(o));
    CHECK(offenders.isEmpty());

    // The scratch store the queue tests used: the queue holds part TOKENS, and a part token is the book key
    // plus a file name — there is no url in it to carry a credential, which is the property #214 built it
    // for and the property this asserts is still true when the book key is a server's.
    QFile scratch(scratchIni);
    if (scratch.open(QIODevice::ReadOnly)) CHECK(!scratch.readAll().contains(needle));

    // NOTHING THE STUB WAS ASKED CARRIES IT IN ITS PATH except the two urls that are minted and thrown
    // away — the stream and the cover. Both are documented exceptions (mpv is handed a url; MetaCache
    // fetches one), and neither is ever written down.
    for (const AbsStub::Seen& s : stub.seen)
    {
        if (!s.path.contains(QLatin1String(kToken))) continue;
        CHECK(s.path.contains(QStringLiteral("/file/")) || s.path.contains(QStringLiteral("/cover")));
    }
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    AbsStub stub;
    if (!stub.listen(QHostAddress::LocalHost, 0))
    { std::fprintf(stderr, "FAIL: the fixture stub could not listen\n"); return 1; }
    const quint16 port = stub.serverPort();

    QTemporaryDir tmp;
    if (!tmp.isValid()) { std::fprintf(stderr, "FAIL: no temp dir\n"); return 1; }
    const QString scratchIni = tmp.path() + QStringLiteral("/queue.ini");

    testIds();
    testUrls();
    testReaders();
    testTimeline();
    testThrottle();
    testLevels();
    testStore();
    testLive(stub, port);
    testSessionHooks(scratchIni);
    testTokenNeverLands(stub, scratchIni);

    // The saved server goes at the end rather than in a destructor: the scan above has to run while the row
    // (and therefore the token) is still on disk, or it would be asserting over an empty directory.
    if (!g_serverId.isEmpty()) AbsServerStore::remove(g_serverId);

    if (g_fail) { std::fprintf(stderr, "%d check(s) failed\n", g_fail); return 1; }
    std::printf("ABSCLIENT-OK\n");
    return 0;
}
