// Headless probe for the subtitle accuracy cores: the OpenSubtitles OSDb hash, the match chain (tier ORDER
// and which tiers are emitted at all), the cache-identifier precedence, and the download cache. Since #81 also
// the transport's search/download split and the built-in API key, against a loopback fake OpenSubtitles.
// Prints SUBS-OK on success; any failure prints SUBS-FAIL <cond> (line) and exits non-zero.
#include "SubtitleHash.h"
#include "SubtitleCache.h"
#include "SubtitleFetcher.h"
#include "BuiltinCredentials.h"   // #81: the built-in key, from the FIXTURE header (tools/fixtures/builtin81)
#include "Settings.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QFile>
#include <QByteArray>
#include <QStringList>
#include <QVector>
#include <cstdio>
#include <functional>
#include <utility>   // std::swap

static int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "SUBS-FAIL %s (line %d)\n", #cond, __LINE__); ++failures; } \
} while (0)

// An INDEPENDENT reference implementation of the OSDb hash, so the probe proves the real one rather than
// merely agreeing with itself: sum the file size with every little-endian quint64 in the two 64 KiB windows.
static QString refHash(const QByteArray& head, const QByteArray& tail, qint64 size)
{
    quint64 h = quint64(size);
    const auto addAll = [&h](const QByteArray& b) {
        for (int i = 0; i + 8 <= b.size(); i += 8) {
            quint64 w = 0;
            for (int k = 7; k >= 0; --k) w = (w << 8) | quint8(b.at(i + k));   // little-endian
            h += w;
        }
    };
    addAll(head); addAll(tail);
    return QStringLiteral("%1").arg(h, 16, 16, QLatin1Char('0'));
}

// ---- #81: a loopback stand-in for OpenSubtitles ---------------------------------------------------------
// NOT OPENSUBTITLES. A deliberately small HTTP/1.1 server on 127.0.0.1: one request per connection,
// Content-Length bodies only. It answers the four request shapes SubtitleFetcher makes (login, search,
// download, and the file the download link names) and RECORDS the method, path and the two headers that
// matter of every request. Header names are matched case-insensitively (Qt 6 lower-cases what it writes).
static const char* const kFixtureOsKey  = "TEST-OPENSUBTITLES-KEY";   // the fixture header's built-in key
static const char* const kUserOsKey     = "user-typed-os-key";
static const char* const kFixtureOsUser = "fixture-os-user";
static const char* const kFixtureOsPass = "fixture-os-password";
static const char* const kStubToken     = "STUB-OS-TOKEN";

class OsStub : public QTcpServer
{
public:
    struct Seen { QByteArray method; QByteArray path; QByteArray apiKey; QByteArray authorization; QByteArray body; };
    QVector<Seen> seen;

    int countOf(const QByteArray& method, const QByteArray& pathPrefix) const
    {
        int n = 0;
        for (const Seen& s : seen) if (s.method == method && s.path.startsWith(pathPrefix)) ++n;
        return n;
    }
    // True when every request of this shape satisfies pred, and there is at least one.
    bool all(const QByteArray& method, const QByteArray& pathPrefix, const std::function<bool(const Seen&)>& pred) const
    {
        int n = 0;
        for (const Seen& s : seen)
        {
            if (s.method != method || !s.path.startsWith(pathPrefix)) continue;
            ++n;
            if (!pred(s)) return false;
        }
        return n > 0;
    }

protected:
    void incomingConnection(qintptr handle) override
    {
        auto* sock = new QTcpSocket(this);
        sock->setSocketDescriptor(handle);
        connect(sock, &QTcpSocket::readyRead, this, [this, sock] {
            sock->setProperty("buf", sock->property("buf").toByteArray() + sock->readAll());
            const QByteArray buf = sock->property("buf").toByteArray();
            const int headEnd = buf.indexOf("\r\n\r\n");
            if (headEnd < 0) return;
            const QList<QByteArray> lines = buf.left(headEnd).split('\n');
            const QList<QByteArray> reqLine = lines.value(0).trimmed().split(' ');
            Seen s;
            s.method = reqLine.value(0);
            s.path = reqLine.value(1);
            int wantBody = 0;
            for (int i = 1; i < lines.size(); ++i)
            {
                const QByteArray l = lines.at(i).trimmed();
                const QByteArray lower = l.toLower();
                if (lower.startsWith("api-key:")) s.apiKey = l.mid(8).trimmed();
                if (lower.startsWith("authorization:")) s.authorization = l.mid(14).trimmed();
                if (lower.startsWith("content-length:")) wantBody = l.mid(15).trimmed().toInt();
            }
            s.body = buf.mid(headEnd + 4);
            if (s.body.size() < wantBody) return;   // the rest of the body is still on its way
            sock->setProperty("buf", QByteArray());
            seen.push_back(s);

            int status = 200;
            QByteArray type = "application/json";
            QByteArray reply = "{}";
            if (s.method == "POST" && s.path == "/api/v1/login")
            {
                if (s.body.contains("refuse-this-password")) { status = 401; reply = R"({"message":"Error, invalid username/password"})"; }
                // base_url names a REAL host on purpose: under a loopback root the fetcher must not follow it.
                else reply = QByteArray(R"({"token":")") + kStubToken + R"(","base_url":"vip-api.opensubtitles.com","status":200})";
            }
            else if (s.method == "GET" && s.path.startsWith("/api/v1/subtitles"))
            {
                reply = R"({"total_count":2,"data":[)"
                        R"({"attributes":{"language":"en","release":"Fixture.Release.720p","download_count":7,)"
                        R"("files":[{"file_id":1002,"file_name":"fixture-720p.srt"}]}},)"
                        R"({"attributes":{"language":"en","release":"Fixture.Release.1080p","download_count":42,)"
                        R"("files":[{"file_id":1001,"file_name":"fixture-1080p.srt"}]}}]})";
            }
            else if (s.method == "POST" && s.path == "/api/v1/download")
            {
                reply = "{\"link\":\"http://127.0.0.1:" + QByteArray::number(serverPort())
                      + "/files/1001.srt\",\"remaining\":19}";
            }
            else if (s.method == "GET" && s.path == "/files/1001.srt")
            {
                type = "text/plain";
                reply = "1\r\n00:00:01,000 --> 00:00:02,000\r\nFIXTURE SUBTITLE LINE\r\n";
            }
            else
                status = 404;

            const QByteArray out = "HTTP/1.1 " + QByteArray::number(status) + " X\r\nContent-Type: " + type
                                 + "\r\nContent-Length: " + QByteArray::number(reply.size())
                                 + "\r\nConnection: close\r\n\r\n" + reply;
            sock->write(out);
            sock->flush();
            sock->disconnectFromHost();
        });
        connect(sock, &QTcpSocket::disconnected, sock, &QObject::deleteLater);
    }
};

static bool waitFor(const std::function<bool()>& done, int ms = 5000)
{
    QElapsedTimer t;
    t.start();
    while (!done() && t.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return done();
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const int W = 65536;

    // --- OSDb hash: pure core matches an independent implementation -------------------------------------
    {
        QByteArray head(W, '\0'), tail(W, '\0');
        for (int i = 0; i < W; ++i) { head[i] = char(i % 251); tail[i] = char((i * 7) % 241); }
        const qint64 size = 734003200;                       // a plausible 700 MiB rip
        CHECK(SubtitleHash::ofBytes(head, tail, size) == refHash(head, tail, size));
        CHECK(SubtitleHash::ofBytes(head, tail, size).size() == 16);           // 16 hex digits, zero-padded
        CHECK(SubtitleHash::ofBytes(head, tail, size) ==
              SubtitleHash::ofBytes(head, tail, size).toLower());              // lowercase
        // Endianness is load-bearing: a byte-swapped window must NOT produce the same hash.
        QByteArray swapped = head;
        for (int i = 0; i + 8 <= swapped.size(); i += 8)
            for (int k = 0; k < 4; ++k) std::swap(swapped[i + k], swapped[i + 7 - k]);
        CHECK(SubtitleHash::ofBytes(swapped, tail, size) != SubtitleHash::ofBytes(head, tail, size));
        // KNOWN-ANSWER vector — the only assertion here that pins the little-endian CONVENTION rather than
        // mere within-word order sensitivity. refHash above is a structural twin of the implementation, so a
        // shared endianness mistake would satisfy every comparison-based check; this one would not. A single
        // word 01..08 with everything else zero must read back LSB-first.
        {
            QByteArray h8(W, '\0'), t0(W, '\0');
            for (int i = 0; i < 8; ++i) h8[i] = char(i + 1);       // bytes 01 02 03 04 05 06 07 08
            // little-endian => 0x0807060504030201 ; a big-endian impl would yield "0102030405060708".
            CHECK(SubtitleHash::ofBytes(h8, t0, 0) == QStringLiteral("0807060504030201"));
        }
        // Size participates.
        CHECK(SubtitleHash::ofBytes(head, tail, size + 1) != SubtitleHash::ofBytes(head, tail, size));
        // Precondition guard: a window that is not exactly 64 KiB has NO valid hash (a short read must not
        // produce a plausible-looking digest that the server can never match).
        CHECK(SubtitleHash::ofBytes(head.left(W - 8), tail, size).isEmpty());
        CHECK(SubtitleHash::ofBytes(head, tail.left(W - 8), size).isEmpty());
        CHECK(SubtitleHash::ofBytes(QByteArray(), QByteArray(), size).isEmpty());
        CHECK(SubtitleHash::ofBytes(head + QByteArray(8, '\0'), tail, size).isEmpty());   // too LONG, too
    }

    // --- OSDb hash: file path wrapper ------------------------------------------------------------------
    // The fixture dir outlives this block: the match-chain and cacheIdentifier sections below need a hashable
    // and an unhashable file on disk to prove tier gating. Hermetic — everything lives under QTemporaryDir.
    QTemporaryDir tmp; CHECK(tmp.isValid());
    const QString smallPath = tmp.path() + QStringLiteral("/small.mkv");
    const QString bigPath   = tmp.path() + QStringLiteral("/big.mkv");
    {
        { QFile f(smallPath); f.open(QIODevice::WriteOnly); f.write(QByteArray(1000, 'x')); f.close(); }
        CHECK(SubtitleHash::ofFile(smallPath).isEmpty());              // < 128 KiB ⇒ no valid hash
        CHECK(SubtitleHash::ofFile(tmp.path() + QStringLiteral("/missing.mkv")).isEmpty());

        QByteArray head(W, '\0'), mid(4096, 'm'), tail(W, '\0');
        for (int i = 0; i < W; ++i) { head[i] = char(i % 251); tail[i] = char((i * 7) % 241); }
        { QFile f(bigPath); f.open(QIODevice::WriteOnly); f.write(head); f.write(mid); f.write(tail); f.close(); }
        const qint64 sz = qint64(W) * 2 + mid.size();
        CHECK(SubtitleHash::ofFile(bigPath) == refHash(head, tail, sz));   // reads only the two windows

        // ofFile memoizes its last result (the same file is hashed twice per open). Two things must hold:
        // a repeat call agrees with the first, AND a file rewritten underneath us re-hashes rather than
        // serving a stale digest — the memo key carries size + mtime, so a different size invalidates it.
        CHECK(SubtitleHash::ofFile(bigPath) == SubtitleHash::ofFile(bigPath));      // stable
        const QString before = SubtitleHash::ofFile(bigPath);
        {
            QByteArray h2(W, '\0'), m2(8192, 'z'), t2(W, '\0');
            for (int i = 0; i < W; ++i) { h2[i] = char((i * 3) % 253); t2[i] = char((i * 11) % 239); }
            QFile f(bigPath); f.open(QIODevice::WriteOnly | QIODevice::Truncate);   // different bytes AND size
            f.write(h2); f.write(m2); f.write(t2); f.close();
            CHECK(SubtitleHash::ofFile(bigPath) == refHash(h2, t2, qint64(W) * 2 + m2.size()));
        }
        CHECK(SubtitleHash::ofFile(bigPath) != before);                // the memo did NOT serve a stale hash
    }

    // --- the match chain: order + gating -----------------------------------------------------------
    // buildQueries IS the accuracy feature: which tiers are emitted, and in what order. Pure (strings in,
    // strings out; the only I/O is hashing the path it's handed), so it's asserted directly here.
    {
        // A stream (no local path): imdb then title, never a hash tier.
        const QStringList s = SubtitleFetcher::buildQueries(QStringLiteral("tt1375666"),
                                  QStringLiteral("Inception"), QStringLiteral("en"), QString());
        CHECK(s.size() == 2);
        CHECK(s.at(0).contains(QStringLiteral("imdb_id=1375666")));
        CHECK(s.at(1).contains(QStringLiteral("query=Inception")));
        for (const QString& q : s) CHECK(q.contains(QStringLiteral("languages=en")));
        // An episode id expands to parent+season+episode.
        const QStringList e = SubtitleFetcher::buildQueries(QStringLiteral("tt0903747:2:5"),
                                  QString(), QStringLiteral("en"), QString());
        CHECK(e.size() == 1);
        CHECK(e.at(0).contains(QStringLiteral("parent_imdb_id=903747")));
        CHECK(e.at(0).contains(QStringLiteral("season_number=2")));
        CHECK(e.at(0).contains(QStringLiteral("episode_number=5")));
        // The API's imdb ids are NUMERIC: the "tt" prefix goes AND the zero padding goes with it. Most
        // pre-2000 titles are zero-padded, so leaving the padding on would blunt the imdb tier for them.
        CHECK(!e.at(0).contains(QStringLiteral("parent_imdb_id=0")));
        // The language filter rides EVERY tier, episodes included — a tier that dropped it would answer with
        // subtitles in some other language and the auto-pick would load them.
        for (const QString& q : e) CHECK(q.contains(QStringLiteral("languages=en")));
        CHECK(SubtitleFetcher::buildQueries(QStringLiteral("tt0110912"), QString(), QStringLiteral("en"),
                  QString()).at(0).contains(QStringLiteral("imdb_id=110912")));
        // Nothing to search on ⇒ no queries at all (no blind, unmatchable request).
        CHECK(SubtitleFetcher::buildQueries(QString(), QString(), QStringLiteral("en"), QString()).isEmpty());
        // A hashable local file puts the moviehash tier FIRST, ahead of imdb — the exact-rip match wins.
        const QStringList h = SubtitleFetcher::buildQueries(QStringLiteral("tt1375666"),
                                  QStringLiteral("Inception"), QStringLiteral("en"), bigPath);
        CHECK(h.size() == 3);
        CHECK(h.at(0).contains(QStringLiteral("moviehash=")));
        CHECK(h.at(1).contains(QStringLiteral("imdb_id=")));
        CHECK(h.at(2).contains(QStringLiteral("query=Inception")));
        // …and on all three tiers of the hashable case, the moviehash tier especially (it is the one built
        // from a different branch, so it is the one that could silently lose the filter).
        for (const QString& q : h) CHECK(q.contains(QStringLiteral("languages=en")));
        // An UNHASHABLE local file (< 128 KiB) must NOT emit a hash tier — a bogus hash matches nothing.
        const QStringList u = SubtitleFetcher::buildQueries(QStringLiteral("tt1375666"),
                                  QStringLiteral("Inception"), QStringLiteral("en"), smallPath);
        CHECK(u.size() == 2);
        CHECK(!u.at(0).contains(QStringLiteral("moviehash=")));
        CHECK(u.at(0).contains(QStringLiteral("imdb_id=")));
    }

    // --- cacheIdentifier precedence ----------------------------------------------------------------
    // The download cache must key on whichever tier will ACTUALLY match, or a hit would replay a subtitle
    // matched by a coarser tier than the one this open would use.
    {
        CHECK(SubtitleFetcher::cacheIdentifier(QStringLiteral("tt1"), QStringLiteral("T"), bigPath)
                  .startsWith(QStringLiteral("hash:")));                        // hashable ⇒ hash wins
        CHECK(SubtitleFetcher::cacheIdentifier(QStringLiteral("tt1"), QStringLiteral("T"), QString())
                  == QStringLiteral("tt1"));                                    // no path ⇒ imdb
        CHECK(SubtitleFetcher::cacheIdentifier(QString(), QStringLiteral("T"), QString())
                  == QStringLiteral("title:T"));                                // neither ⇒ title
        CHECK(SubtitleFetcher::cacheIdentifier(QStringLiteral("tt1"), QStringLiteral("T"), smallPath)
                  == QStringLiteral("tt1"));                                    // unhashable ⇒ falls to imdb
        // The last rung: an unhashable path AND no imdb id ⇒ all the way down to the title. (A "hash:" here
        // would key on a digest no search ever used; an empty key would collide across every such video.)
        CHECK(SubtitleFetcher::cacheIdentifier(QString(), QStringLiteral("T"), smallPath)
                  == QStringLiteral("title:T"));
    }

    // --- SubtitleCache --------------------------------------------------------------------------------
    {
        QTemporaryDir ctmp; CHECK(ctmp.isValid());   // its own dir (the outer `tmp` holds the hash fixtures)
        const QString cachePath = ctmp.path() + QStringLiteral("/subtitles.json");
        const QString srt = ctmp.path() + QStringLiteral("/a.srt");
        { QFile f(srt); f.open(QIODevice::WriteOnly); f.write("1\n"); f.close(); }

        CHECK(SubtitleCache::keyFor(QStringLiteral("tt1375666"), QStringLiteral("en"))
              == QStringLiteral("tt1375666|en"));
        {
            SubtitleCache c(cachePath); c.load();
            CHECK(c.lookup(SubtitleCache::keyFor(QStringLiteral("tt1"), QStringLiteral("en"))).isEmpty());
            c.put(SubtitleCache::keyFor(QStringLiteral("tt1"), QStringLiteral("en")), srt);
            CHECK(c.lookup(SubtitleCache::keyFor(QStringLiteral("tt1"), QStringLiteral("en"))) == srt);
            c.save();
        }
        {
            SubtitleCache c(cachePath); c.load();                       // round-trip
            CHECK(c.lookup(SubtitleCache::keyFor(QStringLiteral("tt1"), QStringLiteral("en"))) == srt);
            // A picker choice OVERWRITES, so the correction sticks on replay.
            const QString srt2 = ctmp.path() + QStringLiteral("/b.srt");
            { QFile f(srt2); f.open(QIODevice::WriteOnly); f.write("2\n"); f.close(); }
            c.put(SubtitleCache::keyFor(QStringLiteral("tt1"), QStringLiteral("en")), srt2);
            CHECK(c.lookup(SubtitleCache::keyFor(QStringLiteral("tt1"), QStringLiteral("en"))) == srt2);
            // A recorded file deleted behind our back reads as a MISS (self-healing ⇒ re-fetch).
            QFile::remove(srt2);
            CHECK(c.lookup(SubtitleCache::keyFor(QStringLiteral("tt1"), QStringLiteral("en"))).isEmpty());
            c.clear();
            CHECK(c.lookup(SubtitleCache::keyFor(QStringLiteral("tt1"), QStringLiteral("en"))).isEmpty());
        }
    }

    // --- #81: search with an API key alone, download behind a login, and the built-in key -------------------
    // Everything below talks to OsStub, a fake OpenSubtitles on 127.0.0.1 that this probe opens itself. The key
    // is the FIXTURE's (tools/fixtures/builtin81/BuiltinSecrets.h: "TEST-OPENSUBTITLES-KEY"), the login is made
    // up, and nothing here is printed: a recorded header holds the key and a recorded body the password.
    {
        using Result = SubtitleFetcher::DownloadResult;
        QStandardPaths::setTestModeEnabled(true);   // the downloaded .srt goes to a test cache, not the user's

        // 1. Where requests go. Only a loopback http root, and only under EB_UITEST, replaces production.
        const QString prod = QStringLiteral("https://api.opensubtitles.com/api/v1");
        CHECK(SubtitleFetcher::apiRootFor(false, QStringLiteral("http://127.0.0.1:8123")) == prod);
        CHECK(SubtitleFetcher::apiRootFor(true, QString()) == prod);
        CHECK(SubtitleFetcher::apiRootFor(true, QStringLiteral("http://example.com:8123")) == prod);
        CHECK(SubtitleFetcher::apiRootFor(true, QStringLiteral("https://127.0.0.1:8123")) == prod);
        CHECK(SubtitleFetcher::apiRootFor(true, QStringLiteral("http://u:p@127.0.0.1:8123")) == prod);
        CHECK(SubtitleFetcher::apiRootFor(true, QStringLiteral("http://127.0.0.1:8123/"))
              == QStringLiteral("http://127.0.0.1:8123/api/v1"));

        // 2. The key: the fixture's built-in one decodes exactly, and is used when the user has none.
        CHECK(BuiltinCredentials::builtinOpenSubtitles().id == QLatin1String(kFixtureOsKey));
        Settings::setOpenSubApiKey(QString());
        Settings::setOpenSubUsername(QString());
        Settings::setOpenSubPassword(QString());
        CHECK(SubtitleFetcher::apiKey().id == QLatin1String(kFixtureOsKey));
        CHECK(SubtitleFetcher::usingBuiltinKey());
        CHECK(SubtitleFetcher::canSearch());       // an API key is all a search needs...
        CHECK(!SubtitleFetcher::canDownload());    // ...and a download needs a login too
        CHECK(Settings::openSubApiKey().isEmpty()); // resolving never writes the built-in into the user's row

        OsStub stub;
        CHECK(stub.listen(QHostAddress::LocalHost));
        const QByteArray base = "http://127.0.0.1:" + QByteArray::number(stub.serverPort());
        qputenv("EB_UITEST", "1");
        qputenv("EB_UITEST_OPENSUBTITLES_BASE", base);
        CHECK(SubtitleFetcher::apiRoot() == QString::fromLatin1(base) + QStringLiteral("/api/v1"));

        QStringList logLines;
        {
            SubtitleFetcher f;
            QObject::connect(&f, &SubtitleFetcher::log, [&](const QString& l) { logLines << l; });

            // 3. SEARCH WITH THE KEY ONLY sends no login request, and returns the fake's rows.
            bool got = false;
            QVector<SubtitleCandidate> rows;
            f.searchList(QStringLiteral("tt0133093"), QStringLiteral("The Matrix"), QStringLiteral("eng"), QString(),
                         [&](const QVector<SubtitleCandidate>& l) { rows = l; got = true; });
            CHECK(waitFor([&] { return got; }));
            CHECK(rows.size() == 2);
            CHECK(!rows.isEmpty() && rows.first().fileId == 1001);          // most-downloaded first
            CHECK(stub.countOf("GET", "/api/v1/subtitles") == 1);
            CHECK(stub.countOf("POST", "/api/v1/login") == 0);               // no login for a search
            CHECK(stub.all("GET", "/api/v1/subtitles",
                           [](const OsStub::Seen& s) { return s.apiKey == kFixtureOsKey; }));  // the built-in key
            CHECK(stub.all("GET", "/api/v1/subtitles",
                           [](const OsStub::Seen& s) { return s.authorization.isEmpty(); }));  // and no bearer

            // 4. DOWNLOAD WITHOUT A LOGIN reports NeedsLogin — the window's prompt path — and sends NOTHING.
            const int before = stub.seen.size();
            Result r = Result::Ok;
            QString srt;
            got = false;
            f.downloadChoice(1001, QStringLiteral("eng"), [&](const QString& p, Result res) { srt = p; r = res; got = true; });
            CHECK(waitFor([&] { return got; }));
            CHECK(r == Result::NeedsLogin);
            CHECK(srt.isEmpty());
            CHECK(stub.seen.size() == before);

            // ...and the automatic on-open fetch, which downloads without asking, does nothing at all.
            got = false;
            f.fetch(QStringLiteral("tt0133093"), QStringLiteral("The Matrix"), QStringLiteral("eng"),
                    [&](const QString& p) { srt = p; got = true; });
            CHECK(waitFor([&] { return got; }));
            CHECK(srt.isEmpty());
            CHECK(stub.seen.size() == before);

            // 5. DOWNLOAD WITH A LOGIN works: one login, then /download with its bearer, then the file.
            Settings::setOpenSubUsername(QString::fromLatin1(kFixtureOsUser));
            Settings::setOpenSubPassword(QString::fromLatin1(kFixtureOsPass));
            CHECK(SubtitleFetcher::canDownload());
            got = false;
            f.downloadChoice(1001, QStringLiteral("eng"), [&](const QString& p, Result res) { srt = p; r = res; got = true; });
            CHECK(waitFor([&] { return got; }));
            CHECK(r == Result::Ok);
            CHECK(!srt.isEmpty());
            {
                QFile sf(srt);
                CHECK(sf.open(QIODevice::ReadOnly));
                CHECK(sf.readAll().contains("FIXTURE SUBTITLE LINE"));
            }
            QFile::remove(srt);
            CHECK(stub.countOf("POST", "/api/v1/login") == 1);
            CHECK(stub.countOf("POST", "/api/v1/download") == 1);
            CHECK(stub.countOf("GET", "/files/1001.srt") == 1);
            CHECK(stub.all("POST", "/api/v1/download",
                           [](const OsStub::Seen& s) { return s.authorization == "Bearer " + QByteArray(kStubToken); }));
            // The login's base_url names a real host; under the loopback root it is ignored, so everything above
            // reached the stub. (Had it been followed, /download would have gone to the internet instead.)

            // A search made while a login IS stored still makes no login request of its own.
            const int logins = stub.countOf("POST", "/api/v1/login");
            got = false;
            f.searchList(QStringLiteral("tt0133093"), QString(), QStringLiteral("eng"), QString(),
                         [&](const QVector<SubtitleCandidate>& l) { rows = l; got = true; });
            CHECK(waitFor([&] { return got; }));
            CHECK(stub.countOf("POST", "/api/v1/login") == logins);
        }

        // 6. A login OpenSubtitles refuses is LoginRefused, not "no subtitle" (a fresh fetcher: no cached token).
        {
            SubtitleFetcher f;
            QObject::connect(&f, &SubtitleFetcher::log, [&](const QString& l) { logLines << l; });
            Settings::setOpenSubPassword(QStringLiteral("refuse-this-password"));
            Result r = Result::Ok;
            bool got = false;
            f.downloadChoice(1001, QStringLiteral("eng"), [&](const QString&, Result res) { r = res; got = true; });
            CHECK(waitFor([&] { return got; }));
            CHECK(r == Result::LoginRefused);
            CHECK(stub.countOf("POST", "/api/v1/download") == 1);   // no download after a refused login
        }

        // 7. THE USER'S KEY WINS over the built-in one, on the wire.
        {
            Settings::setOpenSubApiKey(QString::fromLatin1(kUserOsKey));
            CHECK(SubtitleFetcher::apiKey().id == QLatin1String(kUserOsKey));
            CHECK(!SubtitleFetcher::usingBuiltinKey());
            SubtitleFetcher f;
            bool got = false;
            const int n = stub.countOf("GET", "/api/v1/subtitles");
            f.searchList(QStringLiteral("tt0133093"), QString(), QStringLiteral("eng"), QString(),
                         [&](const QVector<SubtitleCandidate>&) { got = true; });
            CHECK(waitFor([&] { return got; }));
            CHECK(stub.countOf("GET", "/api/v1/subtitles") == n + 1);
            CHECK(stub.seen.last().apiKey == kUserOsKey);
            // Clearing the user's key falls back to the built-in one again; nothing was overwritten.
            Settings::setOpenSubApiKey(QString());
            CHECK(SubtitleFetcher::apiKey().id == QLatin1String(kFixtureOsKey));
        }

        // 8. With neither key there is nothing to search with (the one rule, asked directly: this build
        // embeds the fixture, so "no built-in" cannot be staged through the real slot).
        {
            const BuiltinSecret::Resolved none = BuiltinSecret::resolve(QString(), QString());
            CHECK(!none.usable());
            CHECK(none.source == BuiltinSecret::Source::None);
            CHECK(BuiltinSecret::resolve(QStringLiteral("   "), QString()).source == BuiltinSecret::Source::None);
        }

        // 9. Nothing the fetcher logged carries the key, the password or the token.
        for (const QString& l : logLines)
        {
            CHECK(!l.contains(QLatin1String(kFixtureOsKey)));
            CHECK(!l.contains(QLatin1String(kFixtureOsPass)));
            CHECK(!l.contains(QStringLiteral("refuse-this-password")));
            CHECK(!l.contains(QLatin1String(kStubToken)));
        }

        qunsetenv("EB_UITEST_OPENSUBTITLES_BASE");
        qunsetenv("EB_UITEST");
        Settings::setOpenSubUsername(QString());
        Settings::setOpenSubPassword(QString());
    }

    if (failures == 0) { std::puts("SUBS-OK"); return 0; }
    std::fprintf(stderr, "SUBS: %d check(s) failed\n", failures);
    return 1;
}
